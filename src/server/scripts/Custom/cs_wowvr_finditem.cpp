/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * wowVR `.finditem`: a player-friendly item search (SEC_PLAYER).
 *
 *   .finditem <words> [q:<quality>] [slot:<slot>] [ilvl:<min>] [usable] [p:<page>]
 *
 * Every word must appear inside a word of the item name, in any order
 * ("frost" finds Frostmourne, "sword" finds Greatsword). Results are ranked
 * (exact name, then names starting with the query, then the rest; within each,
 * best quality and item level first), test/placeholder items are hidden, and
 * each line is a clickable link that can be pasted into `.getitem`.
 *
 * Speed: each locale has a word index, stored in a file and memory-mapped:
 *   - every item (id, lower-cased name, quality, item level, slot);
 *   - every distinct word of the names, with the items containing it;
 *   - every suffix of those words, sorted, so a binary search finds all words
 *     containing a query word.
 * A query is limited to MAX_QUERY_WORDS distinct words of at least
 * MIN_WORD_LENGTH characters; only the longest one goes through the index and
 * the rest are checked on its candidates, so a search costs one index lookup
 * plus work proportional to its matches. Like the other wowVR commands it is
 * also subject to the per-account cooldown in cs_wowvr.cpp.
 *
 * The file carries a fingerprint of the item data it was built from: on
 * startup it is reused when the items are unchanged and rebuilt otherwise.
 * Directory: WowVR.FindItem.CacheDir in worldserver.conf (default:
 * <DataDir>/wowvr-finditem-cache). It must be owned by the server user and not
 * writable by group or others, and index files must be regular files owned by
 * the server user (symlinks are refused), so other local users cannot plant or
 * redirect them. If that is not the case, the index is kept in memory.
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "Util.h"
#include "World.h"
#include "WorldScript.h"
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>

#if AC_PLATFORM != AC_PLATFORM_WINDOWS
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace Acore::ChatCommands;

namespace
{
    constexpr std::size_t RESULTS_PER_PAGE = 25;
    constexpr std::size_t MAX_QUERY_WORDS = 5;
    constexpr std::size_t MIN_WORD_LENGTH = 2; // characters, not bytes

    struct NamedValue
    {
        std::string_view name;
        uint32 value;
    };

    constexpr std::array<NamedValue, 14> QUALITY_NAMES =
    { {
        { "poor", ITEM_QUALITY_POOR }, { "grey", ITEM_QUALITY_POOR }, { "gray", ITEM_QUALITY_POOR },
        { "common", ITEM_QUALITY_NORMAL }, { "white", ITEM_QUALITY_NORMAL },
        { "uncommon", ITEM_QUALITY_UNCOMMON }, { "green", ITEM_QUALITY_UNCOMMON },
        { "rare", ITEM_QUALITY_RARE }, { "blue", ITEM_QUALITY_RARE },
        { "epic", ITEM_QUALITY_EPIC }, { "purple", ITEM_QUALITY_EPIC },
        { "legendary", ITEM_QUALITY_LEGENDARY }, { "orange", ITEM_QUALITY_LEGENDARY },
        { "heirloom", ITEM_QUALITY_HEIRLOOM }
    } };

    constexpr std::array<NamedValue, 37> SLOT_NAMES =
    { {
        { "head", INVTYPE_HEAD }, { "helm", INVTYPE_HEAD },
        { "neck", INVTYPE_NECK }, { "necklace", INVTYPE_NECK },
        { "shoulder", INVTYPE_SHOULDERS }, { "shoulders", INVTYPE_SHOULDERS },
        { "shirt", INVTYPE_BODY },
        { "chest", INVTYPE_CHEST }, { "chest", INVTYPE_ROBE }, { "robe", INVTYPE_ROBE },
        { "waist", INVTYPE_WAIST }, { "belt", INVTYPE_WAIST },
        { "legs", INVTYPE_LEGS }, { "pants", INVTYPE_LEGS },
        { "feet", INVTYPE_FEET }, { "boots", INVTYPE_FEET },
        { "wrist", INVTYPE_WRISTS }, { "bracers", INVTYPE_WRISTS },
        { "hands", INVTYPE_HANDS }, { "gloves", INVTYPE_HANDS },
        { "finger", INVTYPE_FINGER }, { "ring", INVTYPE_FINGER },
        { "trinket", INVTYPE_TRINKET },
        { "back", INVTYPE_CLOAK }, { "cloak", INVTYPE_CLOAK },
        { "onehand", INVTYPE_WEAPON }, { "mainhand", INVTYPE_WEAPON }, { "mainhand", INVTYPE_WEAPONMAINHAND },
        { "offhand", INVTYPE_WEAPONOFFHAND }, { "offhand", INVTYPE_HOLDABLE }, { "offhand", INVTYPE_SHIELD },
        { "shield", INVTYPE_SHIELD },
        { "twohand", INVTYPE_2HWEAPON },
        { "ranged", INVTYPE_RANGED }, { "ranged", INVTYPE_RANGEDRIGHT }, { "ranged", INVTYPE_THROWN },
        { "relic", INVTYPE_RELIC }
    } };

    // Whole words that mark developer, test and placeholder items.
    constexpr std::array<std::wstring_view, 10> JUNK_WORDS =
    {
        L"test", L"deprecated", L"deptecated", L"unused", L"obsolete", L"[ph]", L"(old)", L"(test)", L"qa", L"zzold"
    };

    bool HasWholeWord(std::wstring_view text, std::wstring_view word)
    {
        for (std::size_t pos = text.find(word); pos != std::wstring_view::npos; pos = text.find(word, pos + 1))
        {
            bool const startOk = pos == 0 || !std::iswalnum(text[pos - 1]);
            std::size_t const end = pos + word.size();
            bool const endOk = end == text.size() || !std::iswalnum(text[end]);
            if (startOk && endOk)
                return true;
        }
        return false;
    }

    // Placeholder items are marked in capitals ("OLD Warrior's Belt"), so that
    // check uses the original name; real items like "Old Ironjaw" must stay.
    bool IsJunkName(std::string_view name, std::wstring_view lowerName)
    {
        if (name.starts_with("OLD ") || lowerName.starts_with(L"monster -") || lowerName.starts_with(L"zz"))
            return true;

        return std::any_of(JUNK_WORDS.begin(), JUNK_WORDS.end(),
            [lowerName](std::wstring_view junk) { return HasWholeWord(lowerName, junk); });
    }

    // Lower-cases with Unicode rules and returns UTF-8, so localized names and
    // query words compare consistently.
    bool ToLowerUtf8(std::string_view text, std::wstring& wide, std::string& out)
    {
        if (!Utf8toWStr(text, wide))
            return false;
        wstrToLower(wide);
        return WStrToUtf8(wide, out);
    }

    bool IsUtf8ContinuationByte(char c)
    {
        return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
    }

    std::string const& ItemName(ItemTemplate const& item, int localeIndex)
    {
        if (localeIndex > 0)
            if (ItemLocale const* locale = sObjectMgr->GetItemLocale(item.ItemId))
                if (locale->Name.size() > std::size_t(localeIndex) && !locale->Name[localeIndex].empty())
                    return locale->Name[localeIndex];

        return item.Name1;
    }

    // ---- On-disk index format (native byte order; a version bump invalidates old files) ----

    constexpr std::array<char, 8> INDEX_MAGIC = { 'W', 'V', 'R', 'F', 'I', 'N', 'D', 0 };
    constexpr uint32 INDEX_VERSION = 1;

    struct IndexHeader
    {
        std::array<char, 8> magic;
        uint32 version;
        uint32 locale;
        uint64 fingerprint;
        uint32 itemCount;
        uint32 wordCount;
        uint32 suffixCount;
        uint32 postingCount;
        uint32 textSize;
        uint32 reserved;
    };

    struct ItemRecord
    {
        uint32 itemId;
        uint32 quality;
        uint32 itemLevel;
        uint32 inventoryType;
        uint32 nameOffset; // lower-cased name in the text blob
        uint32 nameLength;
    };

    struct WordRecord
    {
        uint32 textOffset;
        uint32 textLength;
        uint32 postingOffset; // first item index in the postings array
        uint32 postingCount;
    };

    struct SuffixRecord
    {
        uint32 wordId;
        uint32 offset; // byte offset of the suffix inside the word
    };

    // Section layout: header, items, words, suffixes, postings, text.
    struct IndexView
    {
        IndexHeader const* header = nullptr;
        ItemRecord const* items = nullptr;
        WordRecord const* words = nullptr;
        SuffixRecord const* suffixes = nullptr;
        uint32 const* postings = nullptr;
        char const* text = nullptr;

        std::string_view Text(uint32 offset, uint32 length) const { return { text + offset, length }; }
        std::string_view Name(ItemRecord const& item) const { return Text(item.nameOffset, item.nameLength); }

        std::string_view Suffix(SuffixRecord const& suffix) const
        {
            WordRecord const& word = words[suffix.wordId];
            return Text(word.textOffset + suffix.offset, word.textLength - suffix.offset);
        }

        // Sets up the section pointers and checks every offset, so a damaged
        // or foreign file is rejected instead of read out of bounds.
        bool Attach(char const* data, std::size_t size)
        {
            if (size < sizeof(IndexHeader))
                return false;

            header = reinterpret_cast<IndexHeader const*>(data);
            if (header->magic != INDEX_MAGIC || header->version != INDEX_VERSION)
                return false;

            uint64 const expected = sizeof(IndexHeader)
                + uint64(header->itemCount) * sizeof(ItemRecord)
                + uint64(header->wordCount) * sizeof(WordRecord)
                + uint64(header->suffixCount) * sizeof(SuffixRecord)
                + uint64(header->postingCount) * sizeof(uint32)
                + header->textSize;
            if (expected != size)
                return false;

            char const* cursor = data + sizeof(IndexHeader);
            items = reinterpret_cast<ItemRecord const*>(cursor);
            cursor += std::size_t(header->itemCount) * sizeof(ItemRecord);
            words = reinterpret_cast<WordRecord const*>(cursor);
            cursor += std::size_t(header->wordCount) * sizeof(WordRecord);
            suffixes = reinterpret_cast<SuffixRecord const*>(cursor);
            cursor += std::size_t(header->suffixCount) * sizeof(SuffixRecord);
            postings = reinterpret_cast<uint32 const*>(cursor);
            cursor += std::size_t(header->postingCount) * sizeof(uint32);
            text = cursor;

            auto const inText = [this](uint64 offset, uint64 length) { return offset + length <= header->textSize; };
            for (uint32 i = 0; i < header->itemCount; ++i)
                if (!inText(items[i].nameOffset, items[i].nameLength) || items[i].quality >= MAX_ITEM_QUALITY)
                    return false;
            for (uint32 i = 0; i < header->wordCount; ++i)
            {
                if (!inText(words[i].textOffset, words[i].textLength)
                    || uint64(words[i].postingOffset) + words[i].postingCount > header->postingCount)
                    return false;
            }
            for (uint32 i = 0; i < header->suffixCount; ++i)
            {
                SuffixRecord const& suffix = suffixes[i];
                if (suffix.wordId >= header->wordCount || suffix.offset >= words[suffix.wordId].textLength)
                    return false;
            }
            for (uint32 i = 0; i < header->postingCount; ++i)
                if (postings[i] >= header->itemCount)
                    return false;

            return true;
        }

        // Sorted, distinct item indexes whose name has a word containing `fragment`.
        std::vector<uint32> ItemsContaining(std::string_view fragment) const
        {
            SuffixRecord const* begin = suffixes;
            SuffixRecord const* end = suffixes + header->suffixCount;
            SuffixRecord const* it = std::lower_bound(begin, end, fragment,
                [this](SuffixRecord const& suffix, std::string_view value) { return Suffix(suffix) < value; });

            std::vector<uint32> wordIds;
            for (; it != end && Suffix(*it).starts_with(fragment); ++it)
                wordIds.push_back(it->wordId);

            std::sort(wordIds.begin(), wordIds.end());
            wordIds.erase(std::unique(wordIds.begin(), wordIds.end()), wordIds.end());

            std::vector<uint32> result;
            for (uint32 wordId : wordIds)
            {
                WordRecord const& word = words[wordId];
                uint32 const* first = postings + word.postingOffset;
                result.insert(result.end(), first, first + word.postingCount);
            }

            std::sort(result.begin(), result.end());
            result.erase(std::unique(result.begin(), result.end()), result.end());
            return result;
        }
    };

    // ---- Building ----

    struct SearchableItem
    {
        ItemTemplate const* item;
        std::string const* name;
    };

    std::vector<SearchableItem> CollectItems(int localeIndex)
    {
        std::vector<SearchableItem> result;
        for (auto const& [entry, item] : *sObjectMgr->GetItemTemplateStore())
            if (item.Quality < MAX_ITEM_QUALITY)
                result.push_back({ &item, &ItemName(item, localeIndex) });

        // Item store iteration order is unspecified; sort so the fingerprint is stable.
        std::sort(result.begin(), result.end(),
            [](SearchableItem const& a, SearchableItem const& b) { return a.item->ItemId < b.item->ItemId; });
        return result;
    }

    // FNV-1a over everything the index depends on.
    uint64 Fingerprint(std::vector<SearchableItem> const& items, int localeIndex)
    {
        uint64 hash = 14695981039346656037ULL;
        auto const mix = [&hash](void const* data, std::size_t size)
        {
            for (std::size_t i = 0; i < size; ++i)
            {
                hash ^= static_cast<unsigned char const*>(data)[i];
                hash *= 1099511628211ULL;
            }
        };

        mix(&INDEX_VERSION, sizeof(INDEX_VERSION));
        mix(&localeIndex, sizeof(localeIndex));
        for (SearchableItem const& entry : items)
        {
            uint32 const fields[] = { entry.item->ItemId, entry.item->Quality, entry.item->ItemLevel,
                entry.item->InventoryType, uint32(entry.name->size()) };
            mix(fields, sizeof(fields));
            mix(entry.name->data(), entry.name->size());
        }
        return hash;
    }

    std::vector<char> BuildIndex(std::vector<SearchableItem> const& source, int localeIndex, uint64 fingerprint)
    {
        std::vector<ItemRecord> items;
        std::string text;
        std::unordered_map<std::string, uint32> wordIds;
        std::vector<std::string> words;
        std::vector<std::vector<uint32>> wordItems;

        std::wstring wide;
        std::string lower;
        for (SearchableItem const& entry : source)
        {
            if (entry.name->empty() || !ToLowerUtf8(*entry.name, wide, lower) || IsJunkName(*entry.name, wide))
                continue;

            uint32 const itemIndex = items.size();
            items.push_back({ entry.item->ItemId, entry.item->Quality, entry.item->ItemLevel,
                entry.item->InventoryType, uint32(text.size()), uint32(lower.size()) });
            text += lower;

            for (std::string_view word : Acore::Tokenize(lower, ' ', false))
            {
                auto [it, isNew] = wordIds.try_emplace(std::string(word), uint32(words.size()));
                if (isNew)
                {
                    words.emplace_back(word);
                    wordItems.emplace_back();
                }

                std::vector<uint32>& list = wordItems[it->second];
                if (list.empty() || list.back() != itemIndex) // a word repeated in one name
                    list.push_back(itemIndex);
            }
        }

        std::vector<WordRecord> wordRecords;
        std::vector<uint32> postings;
        std::vector<SuffixRecord> suffixes;
        for (uint32 id = 0; id < words.size(); ++id)
        {
            wordRecords.push_back({ uint32(text.size()), uint32(words[id].size()), uint32(postings.size()),
                uint32(wordItems[id].size()) });
            text += words[id];
            postings.insert(postings.end(), wordItems[id].begin(), wordItems[id].end());

            for (uint32 pos = 0; pos < words[id].size(); ++pos)
                if (!IsUtf8ContinuationByte(words[id][pos]))
                    suffixes.push_back({ id, pos });
        }

        auto const suffixText = [&](SuffixRecord const& s)
        {
            return std::string_view(words[s.wordId]).substr(s.offset);
        };
        std::sort(suffixes.begin(), suffixes.end(),
            [&](SuffixRecord const& a, SuffixRecord const& b) { return suffixText(a) < suffixText(b); });

        IndexHeader header{};
        header.magic = INDEX_MAGIC;
        header.version = INDEX_VERSION;
        header.locale = uint32(localeIndex);
        header.fingerprint = fingerprint;
        header.itemCount = items.size();
        header.wordCount = wordRecords.size();
        header.suffixCount = suffixes.size();
        header.postingCount = postings.size();
        header.textSize = text.size();

        std::vector<char> blob;
        auto const append = [&blob](void const* data, std::size_t size)
        {
            char const* bytes = static_cast<char const*>(data);
            blob.insert(blob.end(), bytes, bytes + size);
        };
        append(&header, sizeof(header));
        append(items.data(), items.size() * sizeof(ItemRecord));
        append(wordRecords.data(), wordRecords.size() * sizeof(WordRecord));
        append(suffixes.data(), suffixes.size() * sizeof(SuffixRecord));
        append(postings.data(), postings.size() * sizeof(uint32));
        append(text.data(), text.size());
        return blob;
    }

    // ---- Loading: memory-mapped file, or an in-memory blob as fallback ----

    // A cache entry is trusted only if it is not a symlink, has the expected
    // type and belongs to the server user; a directory must also not be
    // writable by anyone else, so nobody can swap files inside it.
    bool IsTrustedEntry(std::filesystem::path const& path, bool directory)
    {
#if AC_PLATFORM != AC_PLATFORM_WINDOWS
        struct stat st;
        if (lstat(path.c_str(), &st) != 0)
            return false;

        if (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))
            return false;

        if (st.st_uid != geteuid())
            return false;

        return !directory || !(st.st_mode & (S_IWGRP | S_IWOTH));
#else
        std::error_code error;
        std::filesystem::file_status const status = std::filesystem::symlink_status(path, error);
        if (error || std::filesystem::is_symlink(status))
            return false;

        return directory ? std::filesystem::is_directory(status) : std::filesystem::is_regular_file(status);
#endif
    }

    bool PrepareCacheDirectory(std::filesystem::path const& dir)
    {
        std::error_code error;
        if (std::filesystem::create_directories(dir, error))
            std::filesystem::permissions(dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace,
                error);

        return IsTrustedEntry(dir, true);
    }

    // Creates `path` exclusively (never following or reusing an existing entry)
    // with owner-only permissions, and writes `blob` to it.
    bool WriteNewFile(std::filesystem::path const& path, std::vector<char> const& blob)
    {
#if AC_PLATFORM != AC_PLATFORM_WINDOWS
        int const fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0)
            return false;

        std::size_t written = 0;
        while (written < blob.size())
        {
            ssize_t const n = write(fd, blob.data() + written, blob.size() - written);
            if (n <= 0)
            {
                close(fd);
                return false;
            }
            written += std::size_t(n);
        }
        return close(fd) == 0;
#else
        std::error_code error;
        if (std::filesystem::exists(std::filesystem::symlink_status(path, error)))
            return false;

        std::ofstream out(path, std::ios::binary);
        return bool(out.write(blob.data(), std::streamsize(blob.size())));
#endif
    }

    class LocaleIndex
    {
    public:
        IndexView const& View() const { return _view; }

        void Load(int localeIndex, std::filesystem::path const& cacheDir)
        {
            std::vector<SearchableItem> const source = CollectItems(localeIndex);
            uint64 const fingerprint = Fingerprint(source, localeIndex);
            std::filesystem::path const path = cacheDir / Acore::StringFormat("finditem-{}.idx", localeIndex);
            bool const cacheUsable = PrepareCacheDirectory(cacheDir);

            if (cacheUsable && MapFile(path) && _view.header->fingerprint == fingerprint
                && _view.header->locale == uint32(localeIndex))
            {
                LOG_INFO("server.loading", ">> .finditem: using cached index {} ({} items)", path.string(),
                    _view.header->itemCount);
                return;
            }
            _region.reset();

            std::vector<char> blob = BuildIndex(source, localeIndex, fingerprint);
            if (cacheUsable && WriteFile(path, blob) && MapFile(path))
            {
                LOG_INFO("server.loading", ">> .finditem: built index {} ({} items)", path.string(),
                    _view.header->itemCount);
                return;
            }

            LOG_WARN("server.loading", ">> .finditem: cache directory {} is not writable or not owned by the server "
                "user (or is writable by others); keeping the index in memory", cacheDir.string());
            _region.reset();
            _memory = std::move(blob);
            _view.Attach(_memory.data(), _memory.size());
        }

    private:
        bool MapFile(std::filesystem::path const& path)
        {
            namespace bip = boost::interprocess;
            try
            {
                if (!IsTrustedEntry(path, false))
                    return false;

                bip::file_mapping file(path.string().c_str(), bip::read_only);
                _region = std::make_unique<bip::mapped_region>(file, bip::read_only);
                return _view.Attach(static_cast<char const*>(_region->get_address()), _region->get_size());
            }
            catch (std::exception const&)
            {
                _region.reset();
                return false;
            }
        }

        // Writes to a fresh, uniquely named temporary file and renames it over
        // the index, so readers never see a partial index. rename() replaces the
        // directory entry itself and never follows a symlink at `path`.
        static bool WriteFile(std::filesystem::path const& path, std::vector<char> const& blob)
        {
            std::filesystem::path const tmp = Acore::StringFormat("{}.{:08x}{:08x}.tmp", path.string(),
                urand(0, 0xFFFFFFFF), urand(0, 0xFFFFFFFF));
            if (!WriteNewFile(tmp, blob))
                return false;

            std::error_code error;
            std::filesystem::rename(tmp, path, error);
            if (error)
            {
                std::filesystem::remove(tmp, error);
                return false;
            }
            return true;
        }

        IndexView _view;
        std::unique_ptr<boost::interprocess::mapped_region> _region;
        std::vector<char> _memory;
    };

    std::filesystem::path CacheDirectory()
    {
        std::string const configured = sConfigMgr->GetOption<std::string>("WowVR.FindItem.CacheDir", "", false);
        if (!configured.empty())
            return configured;

        // Never a shared temp directory: other local users could plant files there.
        return std::filesystem::path(sWorld->GetDataPath()) / "wowvr-finditem-cache";
    }

    IndexView const& IndexForLocale(int localeIndex)
    {
        static std::array<std::once_flag, TOTAL_LOCALES> loaded;
        static std::array<LocaleIndex, TOTAL_LOCALES> indexes;

        std::size_t const slot = (localeIndex > 0 && localeIndex < TOTAL_LOCALES) ? std::size_t(localeIndex) : 0;
        std::call_once(loaded[slot], [slot] { indexes[slot].Load(int(slot), CacheDirectory()); });
        return indexes[slot].View();
    }

    // ---- Searching ----

    std::size_t Utf8Length(std::string_view text)
    {
        return std::count_if(text.begin(), text.end(), [](char c) { return !IsUtf8ContinuationByte(c); });
    }

    struct SearchQuery
    {
        std::vector<std::string> words; // lower-cased UTF-8, distinct, longest first
        std::string phrase;             // the words joined, for exact/prefix ranking
        uint32 minQuality = 0;
        std::vector<uint32> slots;
        uint32 minItemLevel = 0;
        bool usableOnly = false;
        std::size_t page = 1;
    };

    struct Match
    {
        ItemRecord const* item;
        std::string_view lowerName;
        uint8 rank; // 0 exact, 1 prefix, 2 contains
    };

    // Returns an error message, or an empty string when the query is valid.
    std::string ParseQuery(std::string_view input, SearchQuery& query)
    {
        std::wstring wide;
        for (std::string_view token : Acore::Tokenize(input, ' ', false))
        {
            std::string lower;
            if (!ToLowerUtf8(token, wide, lower) || lower.empty())
                continue;

            auto const value = [&lower](std::string_view prefix) -> Optional<std::string_view>
            {
                if (lower.size() > prefix.size() && lower.starts_with(prefix))
                    return std::string_view(lower).substr(prefix.size());
                return {};
            };

            if (lower == "usable")
                query.usableOnly = true;
            else if (auto q = value("q:"))
            {
                auto it = std::find_if(QUALITY_NAMES.begin(), QUALITY_NAMES.end(),
                    [&](NamedValue const& n) { return n.name == *q; });
                if (it == QUALITY_NAMES.end())
                    return Acore::StringFormat("Unknown quality '{}'. Use poor, common, uncommon, rare, epic, "
                        "legendary or heirloom.", *q);
                query.minQuality = it->value;
            }
            else if (auto slot = value("slot:"))
            {
                for (NamedValue const& n : SLOT_NAMES)
                    if (n.name == *slot)
                        query.slots.push_back(n.value);
                if (query.slots.empty())
                    return Acore::StringFormat("Unknown slot '{}'. Try head, neck, shoulder, back, chest, wrist, "
                        "hands, waist, legs, feet, ring, trinket, onehand, twohand, mainhand, offhand, shield, "
                        "ranged or relic.", *slot);
            }
            else if (auto ilvl = value("ilvl:"))
            {
                Optional<uint32> level = Acore::StringTo<uint32>(*ilvl);
                if (!level)
                    return "ilvl: needs a number, e.g. ilvl:200.";
                query.minItemLevel = *level;
            }
            else if (auto page = value("p:"))
            {
                Optional<uint32> number = Acore::StringTo<uint32>(*page);
                if (!number || !*number)
                    return "p: needs a page number, e.g. p:2.";
                query.page = *number;
            }
            else
            {
                if (Utf8Length(lower) < MIN_WORD_LENGTH)
                    return Acore::StringFormat("Search words need at least {} letters.", MIN_WORD_LENGTH);

                if (!query.phrase.empty())
                    query.phrase += ' ';
                query.phrase += lower;

                if (std::find(query.words.begin(), query.words.end(), lower) == query.words.end())
                {
                    if (query.words.size() == MAX_QUERY_WORDS)
                        return Acore::StringFormat("Use at most {} words.", MAX_QUERY_WORDS);
                    query.words.push_back(std::move(lower));
                }
            }
        }

        if (query.words.empty())
            return "Give at least one word of the item name, e.g. .finditem frost sword";

        // The longest word is usually the most selective: it alone uses the index.
        std::stable_sort(query.words.begin(), query.words.end(),
            [](std::string const& a, std::string const& b) { return a.size() > b.size(); });
        return {};
    }

    std::vector<Match> FindItems(SearchQuery const& query, Player const* player, IndexView const& index)
    {
        // One index lookup for the longest word; the other words are checked on
        // those candidates. Every query word lies inside a single name word, so
        // a substring test of the whole name is equivalent.
        std::vector<Match> matches;
        for (uint32 itemIndex : index.ItemsContaining(query.words.front()))
        {
            ItemRecord const& item = index.items[itemIndex];
            std::string_view const name = index.Name(item);
            if (!std::all_of(query.words.begin() + 1, query.words.end(),
                [name](std::string const& word) { return name.find(word) != std::string_view::npos; }))
                continue;

            if (item.quality < query.minQuality || item.itemLevel < query.minItemLevel)
                continue;

            if (!query.slots.empty()
                && std::find(query.slots.begin(), query.slots.end(), item.inventoryType) == query.slots.end())
                continue;

            if (query.usableOnly)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.itemId);
                if (!proto || player->CanUseItem(proto) != EQUIP_ERR_OK)
                    continue;
            }

            uint8 rank = 2;
            if (name == query.phrase)
                rank = 0;
            else if (name.starts_with(query.phrase))
                rank = 1;

            matches.push_back({ &item, name, rank });
        }
        return matches;
    }

    bool BetterMatch(Match const& a, Match const& b)
    {
        if (a.rank != b.rank)
            return a.rank < b.rank;
        if (a.item->quality != b.item->quality)
            return a.item->quality > b.item->quality;
        if (a.item->itemLevel != b.item->itemLevel)
            return a.item->itemLevel > b.item->itemLevel;
        return a.lowerName < b.lowerName;
    }
}

class wowvr_finditem_commandscript : public CommandScript
{
public:
    wowvr_finditem_commandscript() : CommandScript("wowvr_finditem_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "finditem", HandleFindItemCommand, SEC_PLAYER, Console::No }
        };
        return commandTable;
    }

    static bool HandleFindItemCommand(ChatHandler* handler, Tail input)
    {
        SearchQuery query;
        if (std::string const error = ParseQuery(input, query); !error.empty())
        {
            handler->SendErrorMessage("{}", error);
            return false;
        }

        int const localeIndex = handler->GetSessionDbLocaleIndex();
        std::vector<Match> matches = FindItems(query, handler->GetPlayer(), IndexForLocale(localeIndex));
        if (matches.empty())
        {
            handler->SendSysMessage("No items found. Try fewer words or remove a filter.");
            return true;
        }

        std::size_t const pages = (matches.size() + RESULTS_PER_PAGE - 1) / RESULTS_PER_PAGE;
        std::size_t const page = std::min(query.page, pages);
        std::size_t const first = (page - 1) * RESULTS_PER_PAGE;
        std::size_t const last = std::min(first + RESULTS_PER_PAGE, matches.size());

        // Only the requested page needs to be in order.
        std::partial_sort(matches.begin(), matches.begin() + last, matches.end(), BetterMatch);

        handler->PSendSysMessage("Found {} item(s), page {} of {}. Shift-click a link into .getitem to get it.",
            matches.size(), page, pages);

        for (std::size_t i = first; i < last; ++i)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(matches[i].item->itemId);
            if (!proto)
                continue;

            handler->PSendSysMessage("{} - |c{:08x}|Hitem:{}:0:0:0:0:0:0:0:0|h[{}]|h|r  ilvl {}",
                proto->ItemId, ItemQualityColors[proto->Quality], proto->ItemId, ItemName(*proto, localeIndex),
                proto->ItemLevel);
        }

        if (page < pages)
            handler->PSendSysMessage("More results: add p:{} to your search, or more words to narrow it down.",
                page + 1);

        return true;
    }
};

// Loads (or builds) the default-locale index once item templates are loaded,
// so the first search does not pay for it.
class wowvr_finditem_worldscript : public WorldScript
{
public:
    wowvr_finditem_worldscript() : WorldScript("wowvr_finditem_worldscript", { WORLDHOOK_ON_STARTUP }) { }

    void OnStartup() override
    {
        IndexForLocale(LOCALE_enUS);
    }
};

void AddSC_wowvr_finditem_commandscript()
{
    new wowvr_finditem_commandscript();
    new wowvr_finditem_worldscript();
}
