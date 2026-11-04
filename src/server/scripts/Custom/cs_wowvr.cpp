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
 * wowVR self-service commands, available to every player (SEC_PLAYER):
 *   .maxme                    level cap + every class spell and weapon skill; talent
 *                             points are left free to spend normally
 *   .maxtalents               every talent of the class at max rank, in all trees
 *   .getitem <item> [count]   add items to your own bags
 *   .finditem <words> ...     item search (cs_wowvr_finditem.cpp)
 *
 * A bare `.help` also prints a short guide to these commands.
 *
 * Abuse limits for player accounts (GMs are exempt), enforced in
 * wowvr_player_command_guard before any of these commands runs:
 *   - one wowVR command per COMMAND_COOLDOWN per account. Chat commands run on
 *     the world thread and the core applies no rate limit to them, so without
 *     this a client could keep the server busy;
 *   - none of them while frozen (GM freeze/jail).
 * .getitem also caps new item stacks per account per hour so item GUIDs cannot
 * be exhausted, and it and .maxtalents are refused in combat, battlegrounds
 * and arenas.
 *
 * They only ever act on the player who types them. `.teleport <location>` and
 * `.lookup item <name>` are opened to players by
 * data/sql/custom/db_world/wowvr_player_commands.sql.
 */

#include "AllCommandScript.h"
#include "Chat.h"
#include "CommandScript.h"
#include "DBCStores.h"
#include "Language.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellMgr.h"
#include "Util.h"
#include "World.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <unordered_map>

using namespace Acore::ChatCommands;

namespace
{
    // Largest stack a single .getitem may ask for; bag space limits it further.
    constexpr uint32 MAX_GETITEM_COUNT = 1000;

    // New item stacks one account may create with .getitem per hour.
    constexpr uint32 MAX_GETITEM_STACKS_PER_HOUR = 1000;

    constexpr std::chrono::milliseconds COMMAND_COOLDOWN{ 1000 };

    constexpr uint32 SPELL_FREEZE = 9454;

    // Commands covered by the cooldown and freeze check. Any abbreviation the
    // command parser accepts (".tele", ".maxt") is matched too.
    constexpr std::array<std::string_view, 6> GUARDED_COMMANDS =
    {
        "maxme", "maxtalents", "getitem", "finditem", "teleport", "lookup"
    };

    using Clock = std::chrono::steady_clock;

    // Returns false (and sends a message) when the player may not use a
    // command that changes their character right now.
    bool CanChangeCharacter(ChatHandler* handler, Player* player)
    {
        if (player->IsInCombat())
        {
            handler->SendErrorMessage(LANG_YOU_IN_COMBAT);
            return false;
        }

        // Levels, talents or spawned items inside PvP would be an unfair advantage
        // (and levelling would break the bracket).
        if (player->InBattleground() || player->InArena())
        {
            handler->SendErrorMessage("You cannot use this in a battleground or arena.");
            return false;
        }

        return true;
    }

    // Reserves `stacks` new item stacks for an account in the current hour.
    bool ReserveItemStacks(uint32 accountId, uint32 stacks)
    {
        static std::mutex mutex;
        static std::unordered_map<uint32, std::pair<Clock::time_point, uint32>> usage;

        std::lock_guard<std::mutex> lock(mutex);
        auto const now = Clock::now();
        auto& [windowStart, used] = usage[accountId];
        if (now - windowStart > std::chrono::hours(1))
        {
            windowStart = now;
            used = 0;
        }

        if (used + stacks > MAX_GETITEM_STACKS_PER_HOUR)
            return false;

        used += stacks;
        return true;
    }

    bool IsWeaponSkillSpell(uint32 spellId)
    {
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            if (SkillLineEntry const* skillLine = sSkillLineStore.LookupEntry(itr->second->SkillLine))
                if (skillLine->categoryId == SKILL_CATEGORY_WEAPON)
                    return true;

        return false;
    }

    // Teaches every spell of the given trainers that the player may learn,
    // repeating until nothing new (later ranks need the earlier ones).
    template <typename Filter>
    void LearnTrainerSpells(Player* player, std::vector<Trainer::Trainer const*> const& trainers, Filter filter)
    {
        bool hadNew;
        do
        {
            hadNew = false;
            for (Trainer::Trainer const* trainer : trainers)
            {
                if (!trainer->IsTrainerValidForPlayer(player))
                    continue;

                for (Trainer::Spell const& trainerSpell : trainer->GetSpells())
                {
                    if (!filter(trainerSpell) || !trainer->CanTeachSpell(player, &trainerSpell))
                        continue;

                    // Same as Trainer::TeachSpell: learnSpell runs the learn hook itself,
                    // a castable entry is put to the hook first so modules can refuse it.
                    if (!trainerSpell.IsCastable())
                        player->learnSpell(trainerSpell.SpellId, false);
                    else if (sScriptMgr->OnPlayerCanLearnSpell(player, trainerSpell.SpellId))
                        player->CastSpell(player, trainerSpell.SpellId, true);

                    if (!trainer->CanTeachSpell(player, &trainerSpell))
                        hadNew = true;
                }
            }
        } while (hadNew);
    }

    void LearnClassSpells(Player* player)
    {
        if (!sChrClassesStore.LookupEntry(player->getClass()))
            return;

        // Not GetClassTrainers(): it throws for a class without trainer rows.
        std::vector<Trainer::Trainer const*> classTrainers;
        for (auto const& [id, trainer] : sObjectMgr->GetTrainers())
            if (trainer.GetTrainerType() == Trainer::Type::Class
                && trainer.GetTrainerRequirement() == player->getClass())
                classTrainers.push_back(&trainer);

        LearnTrainerSpells(player, classTrainers, [](Trainer::Spell const&) { return true; });
    }

    // Weapon masters are requirement-less trainers; CanTeachSpell keeps only
    // the weapons the player's class and race can use.
    void LearnWeaponSkills(Player* player)
    {
        std::vector<Trainer::Trainer const*> weaponTrainers;
        for (auto const& [id, trainer] : sObjectMgr->GetTrainers())
            if (!trainer.GetTrainerRequirement())
                weaponTrainers.push_back(&trainer);

        LearnTrainerSpells(player, weaponTrainers,
            [](Trainer::Spell const& spell) { return IsWeaponSkillSpell(spell.SpellId); });
    }

    void LearnClassQuestSpells(Player* player)
    {
        for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
            if (quest->GetRequiredClasses() && player->SatisfyQuestClass(quest, false)
                && player->SatisfyQuestRace(quest, false))
                player->learnQuestRewardedSpells(quest);
    }

    // Every talent of the class at its highest rank, like `.learn all my talents`.
    void LearnAllTalents(Player* player)
    {
        uint32 const classMask = player->getClassMask();
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* talentInfo = sTalentStore.LookupEntry(i);
            if (!talentInfo)
                continue;

            TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
            if (!talentTabInfo || !(classMask & talentTabInfo->ClassMask))
                continue;

            for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            {
                uint32 const spellId = talentInfo->RankID[rank];
                if (!spellId)
                    continue;

                SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
                if (spellInfo && SpellMgr::IsSpellValid(spellInfo))
                    player->LearnTalent(talentInfo->TalentID, rank, true);
                break;
            }
        }

        player->SetFreeTalentPoints(0);
        player->SendTalentsInfoData(false);
    }
}

class wowvr_commandscript : public CommandScript
{
public:
    wowvr_commandscript() : CommandScript("wowvr_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "maxme",      HandleMaxMeCommand,      SEC_PLAYER, Console::No },
            { "maxtalents", HandleMaxTalentsCommand, SEC_PLAYER, Console::No },
            { "getitem",    HandleGetItemCommand,    SEC_PLAYER, Console::No }
        };
        return commandTable;
    }

    static bool HandleMaxMeCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!CanChangeCharacter(handler, player))
            return false;

        uint8 const maxLevel = static_cast<uint8>(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
        if (player->GetLevel() < maxLevel)
        {
            player->GiveLevel(maxLevel);
            player->InitTalentForLevel();
            player->SetUInt32Value(PLAYER_XP, 0);
        }

        LearnClassSpells(player);
        LearnWeaponSkills(player);
        LearnClassQuestSpells(player);
        player->UpdateSkillsToMaxSkillsForLevel();

        handler->PSendSysMessage("You are now level {} and know every spell and weapon skill of your class. "
            "You have {} talent points to spend.", player->GetLevel(), player->GetFreeTalentPoints());
        return true;
    }

    static bool HandleMaxTalentsCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!CanChangeCharacter(handler, player))
            return false;

        LearnAllTalents(player);
        handler->SendSysMessage("You now know every talent of your class at its highest rank.");
        return true;
    }

    static bool HandleGetItemCommand(ChatHandler* handler, ItemTemplate const* itemTemplate, Optional<uint32> countArg)
    {
        Player* player = handler->GetPlayer();
        if (!CanChangeCharacter(handler, player))
            return false;

        uint32 const itemId = itemTemplate->ItemId;
        uint32 count = std::clamp<uint32>(countArg.value_or(1), 1, MAX_GETITEM_COUNT);

        uint32 noSpaceForCount = 0;
        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count, &noSpaceForCount) != EQUIP_ERR_OK)
            count -= noSpaceForCount;

        if (!count || dest.empty())
        {
            handler->SendErrorMessage(LANG_ITEM_CANNOT_CREATE, itemId, noSpaceForCount);
            return false;
        }

        // Each destination slot may become a new item (and use an item GUID).
        if (handler->GetSession()->GetSecurity() == SEC_PLAYER
            && !ReserveItemStacks(handler->GetSession()->GetAccountId(), uint32(dest.size())))
        {
            handler->SendErrorMessage("You have created too many items this hour. Try again later.");
            return false;
        }

        if (Item* item = player->StoreNewItem(dest, itemId, true))
            player->SendNewItem(item, count, false, true);

        if (noSpaceForCount)
            handler->PSendSysMessage(LANG_ITEM_CANNOT_CREATE, itemId, noSpaceForCount);

        return true;
    }
};

// Runs before every chat command: rate-limits and freeze-checks the wowVR
// player commands, and prints a guide before the usual `.help` list.
class wowvr_player_command_guard : public AllCommandScript
{
public:
    wowvr_player_command_guard() : AllCommandScript("wowvr_player_command_guard",
        { ALLCOMMANDHOOK_ON_TRY_EXECUTE_COMMAND }) { }

    bool OnTryExecuteCommand(ChatHandler& handler, std::string_view cmdStr) override
    {
        WorldSession* session = handler.GetSession();
        if (!session)
            return true;

        if (StringEqualI(cmdStr, "help"))
        {
            SendGuide(handler);
            return true;
        }

        if (session->GetSecurity() != SEC_PLAYER || !IsGuardedCommand(cmdStr))
            return true;

        if (Player* player = session->GetPlayer(); player && player->HasAura(SPELL_FREEZE))
        {
            handler.SendSysMessage("You cannot use commands while frozen.");
            return false;
        }

        if (OnCooldown(session->GetAccountId()))
        {
            handler.SendSysMessage("Please wait a moment before using that command again.");
            return false;
        }

        return true;
    }

private:
    static bool IsGuardedCommand(std::string_view cmdStr)
    {
        std::string_view const first = cmdStr.substr(0, cmdStr.find(' '));
        if (first.empty())
            return false;

        return std::any_of(GUARDED_COMMANDS.begin(), GUARDED_COMMANDS.end(),
            [first](std::string_view name) { return StringStartsWithI(name, first); });
    }

    static bool OnCooldown(uint32 accountId)
    {
        static std::mutex mutex;
        static std::unordered_map<uint32, Clock::time_point> lastUse;

        std::lock_guard<std::mutex> lock(mutex);
        auto const now = Clock::now();
        if (lastUse.size() > 10000)
            std::erase_if(lastUse, [now](auto const& entry) { return now - entry.second > COMMAND_COOLDOWN; });

        auto [it, inserted] = lastUse.try_emplace(accountId, now);
        if (!inserted && now - it->second < COMMAND_COOLDOWN)
            return true;

        it->second = now;
        return false;
    }

    static void SendGuide(ChatHandler& handler)
    {

        static constexpr std::array<std::string_view, 10> guide =
        {
            "|cffffd100wowVR player commands|r",
            "|cff00ccff.maxme|r - reach the level cap and learn all class spells and weapon skills",
            "|cff00ccff.maxtalents|r - learn every talent of your class, in all three trees",
            "|cff00ccff.finditem|r <words> - search items, e.g. |cff00ccff.finditem frost sword q:epic usable|r",
            "    filters: q:<quality> slot:<slot> ilvl:<min> usable p:<page>",
            "|cff00ccff.getitem|r <item link or id> [count] - put an item in your bags",
            "    tip: shift-click a link from .finditem into the chat box after .getitem",
            "|cff00ccff.teleport|r <place> - teleport yourself, e.g. |cff00ccff.teleport stormwind|r",
            "|cff00ccff.lookup item|r <name> - plain item search",
            "Type |cff00ccff.help <command>|r for details on any command. All commands:"
        };
        for (std::string_view line : guide)
            handler.SendSysMessage(line);
    }
};

void AddSC_wowvr_commandscript()
{
    new wowvr_commandscript();
    new wowvr_player_command_guard();
}
