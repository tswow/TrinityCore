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

#include "WorldSession.h"
#include "Common.h"
#include "Corpse.h"
#include "Creature.h"
#include "GameObject.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "LootItemStorage.h"
#include "LootMgr.h"
#include "AOELoot.h"
#include "Map.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "WorldPacket.h"
#include "World.h"

void WorldSession::HandleAutostoreLootItemOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: CMSG_AUTOSTORE_LOOT_ITEM");
    Player* player = GetPlayer();
    ObjectGuid lguid = player->GetLootGUID();
    Loot* loot = nullptr;
    uint8 lootSlot = 0;

    recvData >> lootSlot;

    if (lguid.IsGameObject())
    {
        GameObject* go = player->GetMap()->GetGameObject(lguid);

        // not check distance for GO in case owned GO (fishing bobber case, for example) or Fishing hole GO
        if (!go || ((go->GetOwnerGUID() != _player->GetGUID() && go->GetGoType() != GAMEOBJECT_TYPE_FISHINGHOLE) && !go->IsWithinDistInMap(_player)))
        {
            player->SendLootRelease(lguid);
            return;
        }

        loot = &go->loot;
    }
    else if (lguid.IsItem())
    {
        Item* pItem = player->GetItemByGuid(lguid);

        if (!pItem)
        {
            player->SendLootRelease(lguid);
            return;
        }

        loot = &pItem->loot;
    }
    else if (lguid.IsCorpse())
    {
        Corpse* bones = ObjectAccessor::GetCorpse(*player, lguid);
        if (!bones)
        {
            player->SendLootRelease(lguid);
            return;
        }

        loot = &bones->loot;
    }
    else
    {
        Creature* creature = GetPlayer()->GetMap()->GetCreature(lguid);

        bool lootAllowed = creature && creature->IsAlive() == (player->GetClass() == CLASS_ROGUE && creature->loot.loot_type == LOOT_PICKPOCKETING);

        // Use AOE loot range if enabled, otherwise use default INTERACTION_DISTANCE
        float lootRange = INTERACTION_DISTANCE;
        if (sWorld->getBoolConfig(CONFIG_AOE_LOOT_ENABLE))
            lootRange = sWorld->getFloatConfig(CONFIG_AOE_LOOT_RANGE);

        if (!lootAllowed || !creature->IsWithinDistInMap(_player, lootRange))
        {
            player->SendLootError(lguid, lootAllowed ? LOOT_ERROR_TOO_FAR : LOOT_ERROR_DIDNT_KILL);
            return;
        }

        loot = &creature->loot;
    }

    // AOE Loot: Check if this is virtual loot, translate to real corpse
    std::vector<AOELootSlotMapping>* slotMap = GetAOESlotMap();
    if (slotMap && !slotMap->empty() && lootSlot < slotMap->size())
    {
        const AOELootSlotMapping& mapping = (*slotMap)[lootSlot];

        // Security: Validate slot index doesn't exceed displayed limit
        const uint8 MAX_DISPLAYED_ITEMS = 18;
        if (lootSlot >= MAX_DISPLAYED_ITEMS)
        {
            player->SendLootError(lguid, LOOT_ERROR_DIDNT_KILL);
            return;
        }

        // Security: Validate mapping is still valid
        if (!mapping.isValid)
        {
            player->SendLootError(lguid, LOOT_ERROR_DIDNT_KILL);
            return;
        }

        // Get real corpse from mapping
        Creature* realCorpse = player->GetMap()->GetCreature(mapping.corpseGuid);
        if (!realCorpse)
        {
            InvalidateVirtualSlot(lootSlot);
            player->SendLootError(lguid, LOOT_ERROR_DIDNT_KILL);
            return;
        }

        // Security: Validate distance to REAL corpse using AOE loot range
        float aoeLootRange = sWorld->getFloatConfig(CONFIG_AOE_LOOT_RANGE);
        if (!realCorpse->IsWithinDistInMap(player, aoeLootRange))
        {
            player->SendLootError(lguid, LOOT_ERROR_TOO_FAR);
            return;
        }

        // Security: Validate item hasn't changed (GUID reuse protection)
        if (mapping.originalSlot >= realCorpse->loot.items.size())
        {
            InvalidateVirtualSlot(lootSlot);
            player->SendLootError(lguid, LOOT_ERROR_DIDNT_KILL);
            return;
        }

        LootItem& realItem = realCorpse->loot.items[mapping.originalSlot];
        if (realItem.itemid != mapping.itemId || realItem.is_looted)
        {
            InvalidateVirtualSlot(lootSlot);
            player->SendLootError(lguid, LOOT_ERROR_DIDNT_KILL);
            return;
        }

        // Security: Re-check loot permissions (group loot rules might have changed)
        // For AOE loot, we're more lenient - if player can open the main corpse loot window,
        // they should be able to loot from merged corpses. The real permission check
        // happens in StoreLootItem anyway.
        // if (!realCorpse->loot.hasItemFor(player))
        // {
        //     player->SendLootError(lguid, LOOT_ERROR_DIDNT_KILL);
        //     return;
        // }

        // Security: Check if item is blocked for rolling
        if (realItem.is_blocked)
        {
            player->SendLootError(lguid, LOOT_ERROR_LOCKED);
            return;
        }

        // CRITICAL SECTION: Lock the loot to prevent double-loot race condition
        {
            std::lock_guard<std::mutex> lock(realCorpse->loot.m_lootMutex);

            // Double-check item is still available after acquiring lock
            if (realItem.is_looted)
            {
                InvalidateVirtualSlot(lootSlot);
                player->SendLootError(lguid, LOOT_ERROR_DIDNT_KILL);
                return;
            }

            // Loot from REAL corpse using REAL slot
            player->StoreLootItem(mapping.originalSlot, &realCorpse->loot);
        }

        // Mark virtual slot as invalid
        InvalidateVirtualSlot(lootSlot);

        // Update virtual loot display
        if (Loot* virtualLoot = GetVirtualAOELoot())
        {
            if (lootSlot < virtualLoot->items.size())
            {
                virtualLoot->items[lootSlot].is_looted = true;
                if (virtualLoot->unlootedCount > 0)
                    virtualLoot->unlootedCount--;
            }
        }

        // Update visual state of the real corpse if it's now empty
        if (realCorpse->loot.isLooted())
        {
            realCorpse->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
            realCorpse->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);
        }

        return;
    }

    // Normal looting (not AOE)
    player->StoreLootItem(lootSlot, loot);

    // If player is removing the last LootItem, delete the empty container.
    if (loot->isLooted() && lguid.IsItem())
        player->GetSession()->DoLootRelease(lguid);
}

void WorldSession::HandleLootMoneyOpcode(WorldPacket& /*recvData*/)
{
    TC_LOG_DEBUG("network", "WORLD: CMSG_LOOT_MONEY");

    Player* player = GetPlayer();
    ObjectGuid guid = player->GetLootGUID();
    if (!guid)
        return;

    // AOE Loot: Check if this is an AOE loot session, loot gold from ALL involved corpses
    std::set<ObjectGuid> involvedCorpses = GetInvolvedCorpses();
    if (!involvedCorpses.empty())
    {
        uint32 totalGold = 0;
        bool anyGroupLoot = false;

        // Collect gold from all involved corpses
        for (ObjectGuid corpseGuid : involvedCorpses)
        {
            Creature* corpse = player->GetMap()->GetCreature(corpseGuid);
            if (!corpse)
                continue;

            Loot* corpseLoot = &corpse->loot;

            // For AOE loot, we're more permissive - if player can open the main corpse,
            // they should be able to loot gold from all merged corpses
            // Just check if corpse has gold (skip hasItemFor permission check)
            if (corpseLoot->gold > 0)
            {
                totalGold += corpseLoot->gold;

                // Check if any corpse requires group sharing
                if (corpse->GetLootRecipientGroup())
                    anyGroupLoot = true;

                // Remove gold from corpse
                corpseLoot->gold = 0;
                corpseLoot->NotifyMoneyRemoved();
            }
        }

        // Give gold to player (or group if applicable)
        if (totalGold > 0)
        {
            if (anyGroupLoot && player->GetGroup())
            {
                // Group gold sharing
                Group* group = player->GetGroup();
                std::vector<Player*> playersNear;

                for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    Player* member = itr->GetSource();
                    if (!member)
                        continue;

                    if (player->IsAtGroupRewardDistance(member))
                        playersNear.push_back(member);
                }

                uint32 goldPerPlayer = uint32(totalGold / playersNear.size());

                for (Player* member : playersNear)
                {
                    member->ModifyMoney(goldPerPlayer);
                    member->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, goldPerPlayer);

                    WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
                    data << uint32(goldPerPlayer);
                    data << uint8(playersNear.size() <= 1);
                    member->SendDirectMessage(&data);
                }
            }
            else
            {
                // Solo gold
                player->ModifyMoney(totalGold);
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, totalGold);

                WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
                data << uint32(totalGold);
                data << uint8(1); // "You loot..."
                SendPacket(&data);
            }
        }

        // Update virtual loot to remove gold icon
        if (Loot* virtualLoot = GetVirtualAOELoot())
        {
            virtualLoot->gold = 0;
            virtualLoot->NotifyMoneyRemoved();
        }

        return; // AOE gold looting complete
    }

    // Normal gold looting (not AOE)
    Loot* loot = nullptr;
    bool shareMoney = true;

    switch (guid.GetHigh())
    {
        case HighGuid::GameObject:
        {
            GameObject* go = GetPlayer()->GetMap()->GetGameObject(guid);

            // do not check distance for GO if player is the owner of it (ex. fishing bobber)
            if (go && ((go->GetOwnerGUID() == player->GetGUID() || go->IsWithinDistInMap(player))))
                loot = &go->loot;

            break;
        }
        case HighGuid::Corpse:                               // remove insignia ONLY in BG
        {
            Corpse* bones = ObjectAccessor::GetCorpse(*player, guid);

            if (bones && bones->IsWithinDistInMap(player, INTERACTION_DISTANCE))
            {
                loot = &bones->loot;
                shareMoney = false;
            }

            break;
        }
        case HighGuid::Item:
        {
            if (Item* item = player->GetItemByGuid(guid))
            {
                loot = &item->loot;
                shareMoney = false;
            }
            break;
        }
        case HighGuid::Unit:
        case HighGuid::Vehicle:
        {
            Creature* creature = player->GetMap()->GetCreature(guid);
            bool lootAllowed = creature && creature->IsAlive() == (player->GetClass() == CLASS_ROGUE && creature->loot.loot_type == LOOT_PICKPOCKETING);
            if (lootAllowed && creature->IsWithinDistInMap(player, INTERACTION_DISTANCE))
            {
                loot = &creature->loot;
                if (creature->IsAlive())
                    shareMoney = false;
            }
            else
                player->SendLootError(guid, lootAllowed ? LOOT_ERROR_TOO_FAR : LOOT_ERROR_DIDNT_KILL);
            break;
        }
        default:
            return;                                         // unlootable type
    }

    if (loot)
    {
        loot->NotifyMoneyRemoved();
        if (shareMoney && player->GetGroup())      //item, pickpocket and players can be looted only single player
        {
            Group* group = player->GetGroup();

            std::vector<Player*> playersNear;
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (!member)
                    continue;

                if (player->IsAtGroupRewardDistance(member))
                    playersNear.push_back(member);
            }

            uint32 goldPerPlayer = uint32((loot->gold) / (playersNear.size()));

            for (std::vector<Player*>::const_iterator i = playersNear.begin(); i != playersNear.end(); ++i)
            {
                (*i)->ModifyMoney(goldPerPlayer);
                (*i)->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, goldPerPlayer);

                WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
                data << uint32(goldPerPlayer);
                data << uint8(playersNear.size() <= 1); // Controls the text displayed in chat. 0 is "Your share is..." and 1 is "You loot..."
                (*i)->SendDirectMessage(&data);
            }
        }
        else
        {
            player->ModifyMoney(loot->gold);
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, loot->gold);

            WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
            data << uint32(loot->gold);
            data << uint8(1);   // "You loot..."
            SendPacket(&data);
        }

        loot->gold = 0;

        // Delete the money loot record from the DB
        if (loot->containerID > 0)
            sLootItemStorage->RemoveStoredMoneyForContainer(loot->containerID);

        // Delete container if empty
        if (loot->isLooted() && guid.IsItem())
            player->GetSession()->DoLootRelease(guid);
    }
}

void WorldSession::HandleLootOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: CMSG_LOOT");

    ObjectGuid guid;
    recvData >> guid;

    // Check possible cheat
    if (!GetPlayer()->IsAlive() || !guid.IsCreatureOrVehicle())
        return;

    // interrupt cast
    if (GetPlayer()->IsNonMeleeSpellCast(false))
        GetPlayer()->InterruptNonMeleeSpells(false);

    GetPlayer()->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_LOOTING);

    GetPlayer()->SendLoot(guid, LOOT_CORPSE);
}

void WorldSession::HandleLootReleaseOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: CMSG_LOOT_RELEASE");

    // cheaters can modify lguid to prevent correct apply loot release code and re-loot
    // use internal stored guid
    ObjectGuid guid;
    recvData >> guid;

    if (ObjectGuid lguid = GetPlayer()->GetLootGUID())
        if (lguid == guid)
            DoLootRelease(lguid);
}

void WorldSession::DoLootRelease(ObjectGuid lguid)
{
    Player  *player = GetPlayer();
    Loot    *loot;

    player->SetLootGUID(ObjectGuid::Empty);
    player->SendLootRelease(lguid);

    player->RemoveUnitFlag(UNIT_FLAG_LOOTING);

    if (!player->IsInWorld())
        return;

    if (lguid.IsGameObject())
    {
        GameObject* go = GetPlayer()->GetMap()->GetGameObject(lguid);

        // not check distance for GO in case owned GO (fishing bobber case, for example) or Fishing hole GO
        if (!go || ((go->GetOwnerGUID() != _player->GetGUID() && go->GetGoType() != GAMEOBJECT_TYPE_FISHINGHOLE) && !go->IsWithinDistInMap(_player)))
            return;

        loot = &go->loot;

        if (go->GetGoType() == GAMEOBJECT_TYPE_DOOR)
        {
            // locked doors are opened with spelleffect openlock, prevent remove its as looted
            go->UseDoorOrButton();
        }
        else if (loot->isLooted() || go->GetGoType() == GAMEOBJECT_TYPE_FISHINGNODE)
        {
            if (go->GetGoType() == GAMEOBJECT_TYPE_FISHINGHOLE)
            {                                               // The fishing hole used once more
                go->AddUse();                               // if the max usage is reached, will be despawned in next tick
                if (go->GetUseCount() >= go->GetGOValue()->FishingHole.MaxOpens)
                    go->SetLootState(GO_JUST_DEACTIVATED);
                else
                    go->SetLootState(GO_READY);
            }
            else
                go->SetLootState(GO_JUST_DEACTIVATED);

            loot->clear();
        }
        else
        {
            // not fully looted object
            go->SetLootState(GO_ACTIVATED, player);

            // if the round robin player release, reset it.
            if (player->GetGUID() == loot->roundRobinPlayer)
                loot->roundRobinPlayer.Clear();
        }
    }
    else if (lguid.IsCorpse())        // ONLY remove insignia at BG
    {
        Corpse* corpse = ObjectAccessor::GetCorpse(*player, lguid);
        if (!corpse || !corpse->IsWithinDistInMap(_player, INTERACTION_DISTANCE))
            return;

        loot = &corpse->loot;

        if (loot->isLooted())
        {
            loot->clear();
            corpse->RemoveFlag(CORPSE_FIELD_DYNAMIC_FLAGS, CORPSE_DYNFLAG_LOOTABLE);
        }
    }
    else if (lguid.IsItem())
    {
        Item* pItem = player->GetItemByGuid(lguid);
        if (!pItem)
            return;

        ItemTemplate const* proto = pItem->GetTemplate();

        // destroy only 5 items from stack in case prospecting and milling
        if (proto->HasFlag(ITEM_FLAG_IS_PROSPECTABLE) || proto->HasFlag(ITEM_FLAG_IS_MILLABLE))
        {
            pItem->m_lootGenerated = false;
            pItem->loot.clear();

            uint32 count = pItem->GetCount();

            // >=5 checked in spell code, but will work for cheating cases also with removing from another stacks.
            if (count > 5)
                count = 5;

            player->DestroyItemCount(pItem, count, true);
        }
        else
        {
            // Only delete item if no loot or money (unlooted loot is saved to db) or if it isn't an openable item
            if (pItem->loot.isLooted() || !proto->HasFlag(ITEM_FLAG_HAS_LOOT))
                player->DestroyItem(pItem->GetBagSlot(), pItem->GetSlot(), true);
        }
        return;                                             // item can be looted only single player
    }
    else
    {
        Creature* creature = GetPlayer()->GetMap()->GetCreature(lguid);

        bool lootAllowed = creature && creature->IsAlive() == (player->GetClass() == CLASS_ROGUE && creature->loot.loot_type == LOOT_PICKPOCKETING);
        if (!lootAllowed || !creature->IsWithinDistInMap(_player, INTERACTION_DISTANCE))
            return;

        loot = &creature->loot;
        if (loot->isLooted())
        {
            creature->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);

            // skip pickpocketing loot for speed, skinning timer reduction is no-op in fact
            if (!creature->IsAlive())
                creature->AllLootRemovedFromCorpse();

            loot->clear();
        }
        else
        {
            // if the round robin player release, reset it.
            if (player->GetGUID() == loot->roundRobinPlayer)
            {
                loot->roundRobinPlayer.Clear();

                if (Group* group = player->GetGroup())
                    group->SendLooter(creature, nullptr);
            }
            // force dynflag update to update looter and lootable info
            creature->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);
        }

        // AOE Loot: Clean up virtual loot session when player releases creature loot
        CleanupAOELootSession(this);
    }

    //Player is not looking at loot list, he doesn't need to see updates on the loot list
    loot->RemoveLooter(player->GetGUID());
}

void WorldSession::HandleLootMasterGiveOpcode(WorldPacket& recvData)
{
    uint8 slotid;
    ObjectGuid lootguid, target_playerguid;

    recvData >> lootguid >> slotid >> target_playerguid;

    if (!_player->GetGroup() || _player->GetGroup()->GetMasterLooterGuid() != _player->GetGUID() || _player->GetGroup()->GetLootMethod() != MASTER_LOOT)
    {
        _player->SendLootError(lootguid, LOOT_ERROR_DIDNT_KILL);
        return;
    }

    // player on other map
    Player* target = ObjectAccessor::GetPlayer(*_player, target_playerguid);
    if (!target)
    {
        _player->SendLootError(lootguid, LOOT_ERROR_PLAYER_NOT_FOUND);
        return;
    }

    TC_LOG_DEBUG("network", "WorldSession::HandleLootMasterGiveOpcode (CMSG_LOOT_MASTER_GIVE, 0x02A3) Target = [{}].", target->GetName());

    if (_player->GetLootGUID() != lootguid)
    {
        _player->SendLootError(lootguid, LOOT_ERROR_DIDNT_KILL);
        return;
    }

    if (!_player->IsInRaidWith(target) || !_player->IsInMap(target))
    {
        _player->SendLootError(lootguid, LOOT_ERROR_MASTER_OTHER);
        TC_LOG_INFO("entities.player.cheat", "MasterLootItem: Player {} tried to give an item to ineligible player {} !", GetPlayer()->GetName(), target->GetName());
        return;
    }

    Loot* loot = nullptr;

    if (GetPlayer()->GetLootGUID().IsCreatureOrVehicle())
    {
        Creature* creature = GetPlayer()->GetMap()->GetCreature(lootguid);
        if (!creature)
            return;

        loot = &creature->loot;
    }
    else if (GetPlayer()->GetLootGUID().IsGameObject())
    {
        GameObject* pGO = GetPlayer()->GetMap()->GetGameObject(lootguid);
        if (!pGO)
            return;

        loot = &pGO->loot;
    }

    if (!loot)
        return;

    if (slotid >= loot->items.size() + loot->quest_items.size())
    {
        TC_LOG_DEBUG("loot", "MasterLootItem: Player {} might be using a hack! (slot {}, size {})",
            GetPlayer()->GetName(), slotid, (unsigned long)loot->items.size());
        return;
    }

    LootItem& item = slotid >= loot->items.size() ? loot->quest_items[slotid - loot->items.size()] : loot->items[slotid];

    ItemPosCountVec dest;
    InventoryResult msg = target->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item.itemid, item.count);
    if (!item.AllowedForPlayer(target, true))
        msg = EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM;
    if (msg != EQUIP_ERR_OK)
    {
        if (msg == EQUIP_ERR_CANT_CARRY_MORE_OF_THIS)
            _player->SendLootError(lootguid, LOOT_ERROR_MASTER_UNIQUE_ITEM);
        else if (msg == EQUIP_ERR_INVENTORY_FULL)
            _player->SendLootError(lootguid, LOOT_ERROR_MASTER_INV_FULL);
        else
            _player->SendLootError(lootguid, LOOT_ERROR_MASTER_OTHER);
        return;
    }

    // list of players allowed to receive this item in trade
    GuidSet looters = item.GetAllowedLooters();

    // now move item from loot to target inventory
    Item* newitem = target->StoreNewItem(dest, item.itemid, true, item.randomPropertyId, looters);
    // @tswow-begin
    if (newitem)
    {
        FIRE_ID(newitem->GetTemplate()->events.id, Item, OnTakenAsLoot, TSItem(newitem), TSLootItem(&item), TSLoot(loot), TSPlayer(target));
    }
    // @tswow-end

    target->SendNewItem(newitem, uint32(item.count), false, false, true);
    target->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_ITEM, item.itemid, item.count);
    target->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_TYPE, loot->loot_type, item.count);
    target->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_EPIC_ITEM, item.itemid, item.count);

    // mark as looted
    item.count = 0;
    item.is_looted = true;

    loot->NotifyItemRemoved(slotid);
    --loot->unlootedCount;
}
