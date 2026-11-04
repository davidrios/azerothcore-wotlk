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
#include "SpellMgr.h"
#include "Util.h"
#include "World.h"
#include <algorithm>
#include <array>

using namespace Acore::ChatCommands;

namespace
{
    // Largest stack a single .getitem may ask for; bag space limits it further.
    constexpr uint32 MAX_GETITEM_COUNT = 1000;

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

                    if (trainerSpell.IsCastable())
                        player->CastSpell(player, trainerSpell.SpellId, true);
                    else
                        player->learnSpell(trainerSpell.SpellId, false);

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

        LearnTrainerSpells(player, sObjectMgr->GetClassTrainers(player->getClass()),
            [](Trainer::Spell const&) { return true; });
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
            if (quest->GetRequiredClasses() && player->SatisfyQuestClass(quest, false))
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
        if (player->IsInCombat())
        {
            handler->SendErrorMessage(LANG_YOU_IN_COMBAT);
            return false;
        }

        // Levelling inside a battleground or arena would break its bracket.
        if (player->InBattleground() || player->InArena())
        {
            handler->SendErrorMessage("You cannot use this in a battleground or arena.");
            return false;
        }

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
        if (player->IsInCombat())
        {
            handler->SendErrorMessage(LANG_YOU_IN_COMBAT);
            return false;
        }

        LearnAllTalents(player);
        handler->SendSysMessage("You now know every talent of your class at its highest rank.");
        return true;
    }

    static bool HandleGetItemCommand(ChatHandler* handler, ItemTemplate const* itemTemplate, Optional<uint32> countArg)
    {
        Player* player = handler->GetPlayer();
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

        if (Item* item = player->StoreNewItem(dest, itemId, true))
            player->SendNewItem(item, count, false, true);

        if (noSpaceForCount)
            handler->PSendSysMessage(LANG_ITEM_CANNOT_CREATE, itemId, noSpaceForCount);

        return true;
    }
};

// Prints a guide to the wowVR player commands before the usual `.help` list.
class wowvr_help_guide : public AllCommandScript
{
public:
    wowvr_help_guide() : AllCommandScript("wowvr_help_guide", { ALLCOMMANDHOOK_ON_TRY_EXECUTE_COMMAND }) { }

    bool OnTryExecuteCommand(ChatHandler& handler, std::string_view cmdStr) override
    {
        if (!handler.GetSession() || !StringEqualI(cmdStr, "help"))
            return true;

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

        return true;
    }
};

void AddSC_wowvr_commandscript()
{
    new wowvr_commandscript();
    new wowvr_help_guide();
}
