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

#ifndef AOELOOT_H
#define AOELOOT_H

#include "Define.h"
#include "ObjectGuid.h"
#include <map>
#include <set>
#include <mutex>

class Creature;
class Player;
class WorldSession;
struct Loot;

// Build virtual AOE loot view from nearby corpses
// Returns virtual loot or nullptr if AOE loot shouldn't be used
TC_GAME_API Loot* BuildVirtualAOELoot(Creature* mainCreature, Player* player, WorldSession* session);

// Clean up AOE loot session
TC_GAME_API void CleanupAOELootSession(WorldSession* session);

// Remove corpse from viewer registry (called when corpse despawns)
TC_GAME_API void RemoveCorpseFromViewerRegistry(ObjectGuid corpseGuid);

// Global corpse viewer registry (for real-time updates)
// Declared here for access from Loot.cpp
extern std::map<ObjectGuid, std::set<WorldSession*>> s_corpseViewers;
extern std::mutex s_corpseViewersMutex;

#endif // AOELOOT_H
