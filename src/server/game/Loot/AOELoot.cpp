/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "AOELoot.h"
#include "Creature.h"
#include "Player.h"
#include "WorldSession.h"
#include "Loot.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "World.h"
#include "Log.h"
#include <algorithm>

// Tracks which sessions are viewing which corpses for real-time updates
std::map<ObjectGuid, std::set<WorldSession*>> s_corpseViewers;
std::mutex s_corpseViewersMutex;

// AOE loot configuration
const uint32 AOE_MAX_CORPSES = 10;
const uint32 AOE_MAX_DISPLAYED_ITEMS = 18;  // 3.3.5a client limit
const uint32 AOE_MERGE_COOLDOWN = 1;        // Seconds between merges (anti-spam)

struct LootItemWithPriority
{
    LootItem* item;
    ObjectGuid corpseGuid;
    uint8 originalSlot;
    bool isQuestItem;
    uint8 priority;

    bool operator<(const LootItemWithPriority& other) const
    {
        if (priority != other.priority)
            return priority > other.priority;
        return item->itemid > other.item->itemid;
    }
};

// Calculate display priority: quest items > rarity > binding
uint8 CalculateItemPriority(LootItem* item, Player* /*player*/)
{
    if (item->needs_quest)
        return 100;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->itemid);
    if (!proto)
        return 0;

    uint8 qualityPriority = proto->Quality * 10;
    if (proto->Bonding == BIND_WHEN_PICKED_UP)
        qualityPriority += 5;

    return qualityPriority;
}

void SearchNearbyDeadCreatures(Player* player, Creature* /*mainCreature*/, float range, std::list<Creature*>& outList)
{
    struct DeadCreatureCheck
    {
        DeadCreatureCheck(WorldObject const* obj, float range) : _obj(obj), _range(range) {}
        bool operator()(Creature* u)
        {
            return !u->IsAlive() && _obj->IsWithinDist(u, _range, false);
        }
        WorldObject const* _obj;
        float _range;
    };

    DeadCreatureCheck check(player, range);
    Trinity::CreatureListSearcher<DeadCreatureCheck> searcher(player, outList, check);
    Cell::VisitGridObjects(player, searcher, range);
}

// Collects regular items from a single corpse for merging (quest items handled separately)
void CollectItemsFromCorpse(Creature* creature, Player* player,
                           std::vector<LootItemWithPriority>& outItems,
                           std::set<ObjectGuid>& outCorpses,
                           uint32& outGold)
{
    Loot* loot = &creature->loot;

    if (loot->loot_type != LOOT_CORPSE)
        return;

    outCorpses.insert(creature->GetGUID());
    outGold += loot->gold;

    // Collect only regular items (quest items stay on real corpses)
    for (uint8 i = 0; i < loot->items.size(); ++i)
    {
        LootItem& item = loot->items[i];

        if (item.is_looted || item.is_blocked)
            continue;

        if (!item.AllowedForPlayer(player))
            continue;

        LootItemWithPriority itemPriority;
        itemPriority.item = &item;
        itemPriority.corpseGuid = creature->GetGUID();
        itemPriority.originalSlot = i;
        itemPriority.isQuestItem = false;
        itemPriority.priority = CalculateItemPriority(&item, player);

        outItems.push_back(itemPriority);
    }
}

// Builds virtual merged loot from multiple corpses (dual-path: regular items virtualized, quest items on real corpses)
Loot* BuildVirtualAOELoot(Creature* mainCreature, Player* player, WorldSession* session)
{
    if (!sWorld->getBoolConfig(CONFIG_AOE_LOOT_ENABLE))
        return nullptr;

    time_t now = time(nullptr);
    if (now - session->GetAOELastMergeTime() < AOE_MERGE_COOLDOWN)
        return nullptr;

    std::vector<LootItemWithPriority> allItems;
    std::set<ObjectGuid> involvedCorpses;
    uint32 totalGold = 0;

    CollectItemsFromCorpse(mainCreature, player, allItems, involvedCorpses, totalGold);

    std::list<Creature*> nearbyCreatures;
    float lootRange = sWorld->getFloatConfig(CONFIG_AOE_LOOT_RANGE);
    SearchNearbyDeadCreatures(player, mainCreature, lootRange, nearbyCreatures);

    uint32 corpseCount = 1;
    for (Creature* creature : nearbyCreatures)
    {
        if (creature == mainCreature)
            continue;

        if (corpseCount >= AOE_MAX_CORPSES)
            break;

        if (session->GetAOEMergedCorpses().find(creature->GetGUID()) != session->GetAOEMergedCorpses().end())
        {
            if (now - session->GetAOELastMergeTime() < 30)
                continue;
        }

        if (creature->loot.loot_type == LOOT_NONE)
            creature->loot.loot_type = LOOT_CORPSE;

        size_t beforeSize = involvedCorpses.size();
        CollectItemsFromCorpse(creature, player, allItems, involvedCorpses, totalGold);

        if (involvedCorpses.size() > beforeSize)
        {
            corpseCount++;
            session->GetAOEMergedCorpses().insert(creature->GetGUID());
        }
    }

    if (allItems.empty() || involvedCorpses.size() <= 1)
        return nullptr;

    std::sort(allItems.begin(), allItems.end());

    Loot* virtualLoot = new Loot();
    virtualLoot->gold = totalGold;
    virtualLoot->loot_type = LOOT_CORPSE;
    virtualLoot->unlootedCount = 0;

    if (Player* recipient = mainCreature->GetLootRecipient())
        virtualLoot->lootOwnerGUID = recipient->GetGUID();
    else if (Group* recipientGroup = mainCreature->GetLootRecipientGroup())
        virtualLoot->lootOwnerGUID = recipientGroup->GetLeaderGUID();

    std::vector<WorldSession::AOELootSlotMapping> slotMap;

    // Add regular items to virtual loot
    uint32 itemCount = 0;
    for (LootItemWithPriority& itemPriority : allItems)
    {
        if (itemCount >= AOE_MAX_DISPLAYED_ITEMS)
            break;

        virtualLoot->items.push_back(*itemPriority.item);
        virtualLoot->unlootedCount++;

        WorldSession::AOELootSlotMapping mapping;
        mapping.corpseGuid = itemPriority.corpseGuid;
        mapping.originalSlot = itemPriority.originalSlot;
        mapping.itemId = itemPriority.item->itemid;
        mapping.isQuestItem = false;
        mapping.isValid = true;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemPriority.item->itemid);
        mapping.quality = proto ? proto->Quality : 0;

        slotMap.push_back(mapping);
        itemCount++;
    }

    virtualLoot->FillNotNormalLootFor(player, true);

    // Prepare real corpses for quest items and copy them to virtual loot for display
    std::vector<WorldSession::AOELootSlotMapping> questItemMappings;

    for (ObjectGuid corpseGuid : involvedCorpses)
    {
        Creature* corpse = player->GetMap()->GetCreature(corpseGuid);
        if (!corpse)
            continue;

        corpse->loot.FillNotNormalLootFor(player, true);

        NotNormalLootItemMap& questItemsMap = corpse->loot.GetPlayerQuestItemsNonConst();
        auto itr = questItemsMap.find(player->GetGUID());
        if (itr != questItemsMap.end() && itr->second)
        {
            NotNormalLootItemList* playerQuestItems = itr->second;

            for (NotNormalLootItem& questItemSlot : *playerQuestItems)
            {
                if (questItemSlot.index >= corpse->loot.quest_items.size())
                    continue;

                LootItem& questItem = corpse->loot.quest_items[questItemSlot.index];
                if (questItem.is_looted || questItemSlot.is_looted)
                    continue;

                virtualLoot->quest_items.push_back(questItem);
                virtualLoot->unlootedCount++;

                WorldSession::AOELootSlotMapping questMapping;
                questMapping.corpseGuid = corpseGuid;
                questMapping.originalSlot = questItemSlot.index;
                questMapping.itemId = questItem.itemid;
                questMapping.isQuestItem = true;
                questMapping.isValid = true;

                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(questItem.itemid);
                questMapping.quality = proto ? proto->Quality : 0;

                questItemMappings.push_back(questMapping);
            }
        }
    }

    // Populate quest items map for client display
    if (!virtualLoot->quest_items.empty())
    {
        NotNormalLootItemMap& virtualQuestMap = virtualLoot->GetPlayerQuestItemsNonConst();
        NotNormalLootItemList* virtualPlayerQuestList = new NotNormalLootItemList();

        for (uint8 i = 0; i < virtualLoot->quest_items.size(); ++i)
        {
            virtualPlayerQuestList->push_back(NotNormalLootItem(i, false));
        }

        virtualQuestMap[player->GetGUID()] = virtualPlayerQuestList;
    }

    for (auto& questMapping : questItemMappings)
    {
        slotMap.push_back(questMapping);
    }

    session->SetVirtualAOELoot(virtualLoot);
    session->GetAOESlotMapNonConst() = slotMap;
    session->GetAOEInvolvedCorpsesNonConst() = involvedCorpses;
    session->SetAOELastMergeTime(now);

    {
        std::lock_guard<std::mutex> lock(s_corpseViewersMutex);
        for (ObjectGuid guid : involvedCorpses)
            s_corpseViewers[guid].insert(session);
    }

    return session->GetVirtualAOELoot();
}

void CleanupAOELootSession(WorldSession* session)
{
    if (!session->GetVirtualAOELoot())
        return;

    Player* player = session->GetPlayer();
    if (player)
    {
        // Remove sparkles from corpses that have no lootable items
        for (ObjectGuid corpseGuid : session->GetInvolvedCorpses())
        {
            Creature* corpse = player->GetMap()->GetCreature(corpseGuid);
            if (!corpse)
                continue;

            // Check if corpse is fully looted (all items marked as looted, no gold left)
            bool hasItems = false;

            // Check regular items
            for (LootItem& item : corpse->loot.items)
            {
                if (!item.is_looted)
                {
                    hasItems = true;
                    break;
                }
            }

            // Check quest items
            if (!hasItems)
            {
                for (LootItem& questItem : corpse->loot.quest_items)
                {
                    if (!questItem.is_looted)
                    {
                        hasItems = true;
                        break;
                    }
                }
            }

            // Check gold
            if (!hasItems && corpse->loot.gold > 0)
                hasItems = true;

            if (!hasItems)
            {
                corpse->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(s_corpseViewersMutex);
        for (ObjectGuid guid : session->GetInvolvedCorpses())
        {
            auto itr = s_corpseViewers.find(guid);
            if (itr != s_corpseViewers.end())
            {
                itr->second.erase(session);
                if (itr->second.empty())
                    s_corpseViewers.erase(itr);
            }
        }
    }

    session->SetVirtualAOELoot(nullptr);
    session->GetAOESlotMapNonConst().clear();
    session->GetAOEInvolvedCorpsesNonConst().clear();
}

void RemoveCorpseFromViewerRegistry(ObjectGuid corpseGuid)
{
    std::lock_guard<std::mutex> lock(s_corpseViewersMutex);

    auto itr = s_corpseViewers.find(corpseGuid);
    if (itr != s_corpseViewers.end())
    {
        for (WorldSession* session : itr->second)
        {
            if (session)
                session->RemoveCorpseFromAOEView(corpseGuid);
        }

        s_corpseViewers.erase(itr);
    }
}
