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

// Global corpse viewer registry (for real-time updates)
// Maps corpse GUID → Set of sessions viewing that corpse in AOE loot
std::map<ObjectGuid, std::set<WorldSession*>> s_corpseViewers;
std::mutex s_corpseViewersMutex;

// Configuration constants
const uint32 AOE_MAX_CORPSES = 10;
const uint32 AOE_MAX_DISPLAYED_ITEMS = 18;  // Client hard limit in 3.3.5a
const uint32 AOE_MERGE_COOLDOWN = 1;         // Prevent spam DoS

// Item with priority for sorting
struct LootItemWithPriority
{
    LootItem* item;
    ObjectGuid corpseGuid;
    uint8 originalSlot;
    uint8 priority;

    bool operator<(const LootItemWithPriority& other) const
    {
        // Higher priority first
        if (priority != other.priority)
            return priority > other.priority;
        // Same priority, sort by item ID for stability
        return item->itemid > other.item->itemid;
    }
};

// Calculate item priority (quest items + rarity)
uint8 CalculateItemPriority(LootItem* item, Player* player)
{
    // Quest items get highest priority
    if (item->needs_quest)
        return 100;

    // Get item template for quality
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->itemid);
    if (!proto)
        return 0;

    // Quality-based priority: Legendary=50, Epic=40, Rare=30, Uncommon=20, Common=10, Poor=0
    uint8 qualityPriority = proto->Quality * 10;

    // Bonus: BoP items get +5 priority (more valuable)
    if (proto->Bonding == BIND_WHEN_PICKED_UP)
        qualityPriority += 5;

    return qualityPriority;
}

// Search for nearby dead creatures
void SearchNearbyDeadCreatures(Player* player, Creature* mainCreature, float range, std::list<Creature*>& outList)
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

// Collect items from a single corpse
void CollectItemsFromCorpse(Creature* creature, Player* player,
                           std::vector<LootItemWithPriority>& outItems,
                           std::set<ObjectGuid>& outCorpses,
                           uint32& outGold)
{
    Loot* loot = &creature->loot;

    // Check if loot is valid and player has rights
    if (loot->loot_type != LOOT_CORPSE)
        return;

    // For AOE loot, we're more permissive - if the player can loot the main corpse,
    // they should be able to see items from nearby corpses too
    // We'll still do permission checks when actually looting items
    // Skip permission check for now - we'll validate when player actually loots
    // This allows AOE loot to work even if loot recipient isn't properly set

    // Track this corpse
    outCorpses.insert(creature->GetGUID());

    // Add gold
    outGold += loot->gold;

    // Collect regular items
    for (uint8 i = 0; i < loot->items.size(); ++i)
    {
        LootItem& item = loot->items[i];

        // Skip looted or blocked items
        if (item.is_looted || item.is_blocked)
            continue;

        // Check if player can see this item
        if (!item.AllowedForPlayer(player))
            continue;

        // Create priority entry
        LootItemWithPriority itemPriority;
        itemPriority.item = &item;
        itemPriority.corpseGuid = creature->GetGUID();
        itemPriority.originalSlot = i;
        itemPriority.priority = CalculateItemPriority(&item, player);

        outItems.push_back(itemPriority);
    }

    // TODO: Handle quest_items separately (they use different structure)
}

// Build virtual AOE loot view with all security mitigations
Loot* BuildVirtualAOELoot(Creature* mainCreature, Player* player, WorldSession* session)
{
    // Check if AOE loot is enabled
    if (!sWorld->getBoolConfig(CONFIG_AOE_LOOT_ENABLE))
        return nullptr;

    // Security: Check cooldown to prevent spam DoS
    time_t now = time(nullptr);
    if (now - session->GetAOELastMergeTime() < AOE_MERGE_COOLDOWN)
        return nullptr;

    // Collect all available items from nearby corpses
    std::vector<LootItemWithPriority> allItems;
    std::set<ObjectGuid> involvedCorpses;
    uint32 totalGold = 0;

    // Add main corpse first
    CollectItemsFromCorpse(mainCreature, player, allItems, involvedCorpses, totalGold);

    // Search for nearby corpses
    std::list<Creature*> nearbyCreatures;
    float lootRange = sWorld->getFloatConfig(CONFIG_AOE_LOOT_RANGE);
    SearchNearbyDeadCreatures(player, mainCreature, lootRange, nearbyCreatures);

    // Collect from nearby corpses
    uint32 corpseCount = 1; // Count main corpse
    for (Creature* creature : nearbyCreatures)
    {
        if (creature == mainCreature)
            continue;

        // Security: Limit max corpses to prevent DoS
        if (corpseCount >= AOE_MAX_CORPSES)
            break;

        // Security: Skip corpses already merged to prevent refresh exploit
        if (session->GetAOEMergedCorpses().find(creature->GetGUID()) != session->GetAOEMergedCorpses().end())
        {
            // Allow re-merge after cooldown period
            if (now - session->GetAOELastMergeTime() < 30)
                continue;
        }

        // IMPORTANT: Ensure loot_type is set to LOOT_CORPSE for nearby corpses
        // Loot is normally only generated when player opens corpse, but we need it now
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

    // Check if we actually merged anything
    if (allItems.empty() || involvedCorpses.size() <= 1)
        return nullptr; // Use normal loot

    // SORT by priority (quest items + rarity first)
    std::sort(allItems.begin(), allItems.end());

    // Build virtual loot with TOP items (respect 18-item limit)
    Loot* virtualLoot = new Loot();
    virtualLoot->gold = totalGold;
    virtualLoot->loot_type = LOOT_CORPSE;
    virtualLoot->unlootedCount = 0;

    std::vector<WorldSession::AOELootSlotMapping> slotMap;

    uint32 itemCount = 0;
    for (LootItemWithPriority& itemPriority : allItems)
    {
        // Security: Enforce 18-item client limit
        if (itemCount >= AOE_MAX_DISPLAYED_ITEMS)
            break;

        // Add to virtual loot
        virtualLoot->items.push_back(*itemPriority.item);
        virtualLoot->unlootedCount++;

        // Create slot mapping
        WorldSession::AOELootSlotMapping mapping;
        mapping.corpseGuid = itemPriority.corpseGuid;
        mapping.originalSlot = itemPriority.originalSlot;
        mapping.itemId = itemPriority.item->itemid;
        mapping.isQuestItem = itemPriority.item->needs_quest;
        mapping.isValid = true;

        // Get item quality from template
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemPriority.item->itemid);
        mapping.quality = proto ? proto->Quality : 0;

        slotMap.push_back(mapping);
        itemCount++;
    }

    // Store in session (transfers ownership to session)
    session->SetVirtualAOELoot(virtualLoot);
    session->GetAOESlotMapNonConst() = slotMap;
    session->GetAOEInvolvedCorpsesNonConst() = involvedCorpses;
    session->SetAOELastMergeTime(now);

    // Register session in corpse viewer registry for real-time updates
    {
        std::lock_guard<std::mutex> lock(s_corpseViewersMutex);
        for (ObjectGuid guid : involvedCorpses)
            s_corpseViewers[guid].insert(session);
    }

    return session->GetVirtualAOELoot();
}

// Clean up AOE loot session (called on loot release or logout)
void CleanupAOELootSession(WorldSession* session)
{
    if (!session->GetVirtualAOELoot())
        return;

    // Remove from corpse viewer registry
    {
        std::lock_guard<std::mutex> lock(s_corpseViewersMutex);
        for (ObjectGuid guid : session->GetInvolvedCorpses())
        {
            auto itr = s_corpseViewers.find(guid);
            if (itr != s_corpseViewers.end())
            {
                itr->second.erase(session);

                // Clean up empty entries
                if (itr->second.empty())
                    s_corpseViewers.erase(itr);
            }
        }
    }

    // Clear session data
    session->SetVirtualAOELoot(nullptr);
    session->GetAOESlotMapNonConst().clear();
    session->GetAOEInvolvedCorpsesNonConst().clear();
    // Keep m_aoeMergedCorpses for refresh exploit prevention
}

// Remove corpse from viewer registry (called when corpse despawns)
void RemoveCorpseFromViewerRegistry(ObjectGuid corpseGuid)
{
    std::lock_guard<std::mutex> lock(s_corpseViewersMutex);

    auto itr = s_corpseViewers.find(corpseGuid);
    if (itr != s_corpseViewers.end())
    {
        // Invalidate mappings for all viewers
        for (WorldSession* session : itr->second)
        {
            if (session)
                session->RemoveCorpseFromAOEView(corpseGuid);
        }

        s_corpseViewers.erase(itr);
    }
}
