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

/*
 * Interaction between core and LFGScripts
 */

#include "LFGScripts.h"
#include "Chat.h"
#include "Common.h"
#include "Group.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldSession.h"

namespace lfg
{

LFGPlayerScript::LFGPlayerScript() : PlayerScript("LFGPlayerScript") { }

void LFGPlayerScript::OnLogout(Player* player)
{
    if (!sLFGMgr->isOptionEnabled(LFG_OPTION_ENABLE_DUNGEON_FINDER | LFG_OPTION_ENABLE_RAID_BROWSER))
        return;

    if (!player->GetGroup())
    {
        player->GetSession()->SendLfgLfrList(false);
        sLFGMgr->LeaveLfg(player->GetGUID());
    }
    else if (player->GetSession()->PlayerDisconnected())
        sLFGMgr->LeaveLfg(player->GetGUID(), true);
}

void LFGPlayerScript::OnLogin(Player* player, bool /*loginFirst*/)
{
    if (!sLFGMgr->isOptionEnabled(LFG_OPTION_ENABLE_DUNGEON_FINDER | LFG_OPTION_ENABLE_RAID_BROWSER))
        return;

    // Temporal: Trying to determine when group data and LFG data gets desynched
    ObjectGuid guid = player->GetGUID();
    ObjectGuid gguid = sLFGMgr->GetGroup(guid);

    if (Group const* group = player->GetGroup())
    {
        ObjectGuid gguid2 = group->GetGUID();
        if (gguid != gguid2)
        {
            TC_LOG_ERROR("lfg", "{} on group {} but LFG has group {} saved... Fixing.",
                player->GetSession()->GetPlayerInfo(), gguid2.ToString(), gguid.ToString());
            sLFGMgr->SetupGroupMember(guid, group->GetGUID());
        }
    }

    sLFGMgr->SetTeam(player->GetGUID(), player->GetTeam());

    // Check if player reconnected after completing cross-faction dungeon
    if (Group* group = player->GetGroup())
    {
        if (group->IsCrossFactionLFG() &&
            !sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP))
        {
            ObjectGuid gguid = group->GetGUID();
            LfgState state = sLFGMgr->GetState(guid);

            if (state == LFG_STATE_FINISHED_DUNGEON)
            {
                Map* currentMap = player->GetMap();
                if (!currentMap->IsDungeon() && player->IsAlive())
                {
                    // Check grace period
                    uint32 completionTime = sLFGMgr->GetDungeonCompletionTime(gguid);
                    uint32 currentTime = getMSTime();
                    uint32 timeSinceCompletion = getMSTimeDiff(completionTime, currentTime);

                    if (timeSinceCompletion > 60000)
                    {
                        TC_LOG_INFO("lfg", "Player {} reconnected outside completed cross-faction dungeon, auto-leaving group {}",
                                   player->GetName(), gguid.ToString());

                        // Clean up LFG data
                        sLFGMgr->LeaveLfg(guid);
                        sLFGMgr->SetGroup(guid, ObjectGuid::Empty);
                        sLFGMgr->RemovePlayerFromGroup(gguid, guid);

                        // Remove from group
                        group->RemoveMember(guid, GROUP_REMOVEMETHOD_LEAVE, ObjectGuid::Empty, nullptr);

                        // Notify player
                        ChatHandler(player->GetSession()).PSendSysMessage(
                            "You have left the cross-faction group after completing the dungeon.");
                    }
                }
            }
        }
    }

    /// @todo - Restore LfgPlayerData and send proper status to player if it was in a group
}

void LFGPlayerScript::OnMapChanged(Player* player)
{
    Map const* map = player->GetMap();

    if (sLFGMgr->inLfgDungeonMap(player->GetGUID(), map->GetId(), map->GetDifficulty()))
    {
        Group* group = player->GetGroup();
        // This function is also called when players log in
        // if for some reason the LFG system recognises the player as being in a LFG dungeon,
        // but the player was loaded without a valid group, we'll teleport to homebind to prevent
        // crashes or other undefined behaviour
        if (!group)
        {
            sLFGMgr->LeaveLfg(player->GetGUID());
            player->RemoveAurasDueToSpell(LFG_SPELL_LUCK_OF_THE_DRAW);
            player->TeleportTo(player->m_homebindMapId, player->m_homebindX, player->m_homebindY, player->m_homebindZ, 0.0f);
            TC_LOG_ERROR("lfg", "LFGPlayerScript::OnMapChanged, Player {} {} is in LFG dungeon map but does not have a valid group! "
                "Teleporting to homebind.", player->GetName(), player->GetGUID().ToString());
            return;
        }

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            if (Player* member = itr->GetSource())
                player->GetSession()->SendNameQueryOpcode(member->GetGUID());

        if (sLFGMgr->selectedRandomLfgDungeon(player->GetGUID()))
            player->CastSpell(player, LFG_SPELL_LUCK_OF_THE_DRAW, true);
    }
    else
    {
        Group* group = player->GetGroup();

        // Auto-leave cross-faction groups after dungeon completion
        if (group && group->IsCrossFactionLFG() &&
            !sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP))
        {
            ObjectGuid guid = player->GetGUID();
            ObjectGuid gguid = group->GetGUID();
            LfgState state = sLFGMgr->GetState(guid);

            if (state == LFG_STATE_FINISHED_DUNGEON && player->IsAlive())
            {
                // Check grace period (60 seconds)
                uint32 completionTime = sLFGMgr->GetDungeonCompletionTime(gguid);
                uint32 currentTime = getMSTime();
                uint32 timeSinceCompletion = getMSTimeDiff(completionTime, currentTime);

                if (timeSinceCompletion > 60000)  // 60 seconds passed
                {
                    TC_LOG_INFO("lfg", "Player {} left completed cross-faction dungeon, auto-leaving group {}",
                               player->GetName(), gguid.ToString());

                    // Clean up LFG data
                    sLFGMgr->LeaveLfg(guid);
                    sLFGMgr->SetGroup(guid, ObjectGuid::Empty);
                    sLFGMgr->RemovePlayerFromGroup(gguid, guid);

                    // Remove from group
                    group->RemoveMember(guid, GROUP_REMOVEMETHOD_LEAVE, ObjectGuid::Empty, nullptr);

                    // Notify player
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "You have left the cross-faction group after completing the dungeon.");

                    player->RemoveAurasDueToSpell(LFG_SPELL_LUCK_OF_THE_DRAW);
                    return;
                }
                else
                {
                    // Within grace period - notify player
                    uint32 secondsRemaining = (60000 - timeSinceCompletion) / 1000;
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "Cross-faction group will disband in %u seconds.", secondsRemaining);
                }
            }
        }

        // Original code: disband if last member
        if (group && group->GetMembersCount() == 1)
        {
            sLFGMgr->LeaveLfg(group->GetGUID());
            group->Disband();
            TC_LOG_DEBUG("lfg", "LFGPlayerScript::OnMapChanged, Player {}({}) is last in the lfggroup so we disband the group.",
                player->GetName(), player->GetGUID().ToString());
        }
        player->RemoveAurasDueToSpell(LFG_SPELL_LUCK_OF_THE_DRAW);
    }
}

LFGGroupScript::LFGGroupScript() : GroupScript("LFGGroupScript") { }

void LFGGroupScript::OnAddMember(Group* group, ObjectGuid guid)
{
    if (!sLFGMgr->isOptionEnabled(LFG_OPTION_ENABLE_DUNGEON_FINDER | LFG_OPTION_ENABLE_RAID_BROWSER))
        return;

    ObjectGuid gguid = group->GetGUID();
    ObjectGuid leader = group->GetLeaderGUID();

    if (leader == guid)
    {
        TC_LOG_DEBUG("lfg", "LFGScripts::OnAddMember [{}]: added [{}] leader [{}]", gguid.ToString(), guid.ToString(), leader.ToString());
        sLFGMgr->SetLeader(gguid, guid);
    }
    else
    {
        LfgState gstate = sLFGMgr->GetState(gguid);
        LfgState state = sLFGMgr->GetState(guid);
        TC_LOG_DEBUG("lfg", "LFGScripts::OnAddMember [{}]: added [{}] leader [{}] gstate: {}, state: {}", gguid.ToString(), guid.ToString(), leader.ToString(), gstate, state);

        if (state == LFG_STATE_QUEUED)
            sLFGMgr->LeaveLfg(guid);

        if (gstate == LFG_STATE_QUEUED)
            sLFGMgr->LeaveLfg(gguid);
    }

    sLFGMgr->SetGroup(guid, gguid);
    sLFGMgr->AddPlayerToGroup(gguid, guid);
}

void LFGGroupScript::OnRemoveMember(Group* group, ObjectGuid guid, RemoveMethod method, ObjectGuid kicker, char const* reason)
{
    if (!sLFGMgr->isOptionEnabled(LFG_OPTION_ENABLE_DUNGEON_FINDER | LFG_OPTION_ENABLE_RAID_BROWSER))
        return;

    ObjectGuid gguid = group->GetGUID();
    TC_LOG_DEBUG("lfg", "LFGScripts::OnRemoveMember [{}]: remove [{}] Method: {} Kicker: [{}] Reason: {}",
        gguid.ToString(), guid.ToString(), method, kicker.ToString(), (reason ? reason : ""));

    bool isLFG = group->isLFGGroup();

    if (isLFG && method == GROUP_REMOVEMETHOD_KICK)        // Player have been kicked
    {
        /// @todo - Update internal kick cooldown of kicker
        std::string str_reason = "";
        if (reason)
            str_reason = std::string(reason);
        sLFGMgr->InitBoot(gguid, kicker, guid, str_reason);
        return;
    }

    LfgState state = sLFGMgr->GetState(gguid);

    // If group is being formed after proposal success do nothing more
    if (state == LFG_STATE_PROPOSAL && method == GROUP_REMOVEMETHOD_DEFAULT)
    {
        // LfgData: Remove player from group
        sLFGMgr->SetGroup(guid, ObjectGuid::Empty);
        sLFGMgr->RemovePlayerFromGroup(gguid, guid);
        return;
    }

    sLFGMgr->LeaveLfg(guid);
    sLFGMgr->SetGroup(guid, ObjectGuid::Empty);
    uint8 players = sLFGMgr->RemovePlayerFromGroup(gguid, guid);

    if (Player* player = ObjectAccessor::FindPlayer(guid))
    {
        if (method == GROUP_REMOVEMETHOD_LEAVE && state == LFG_STATE_DUNGEON &&
            players >= LFG_GROUP_KICK_VOTES_NEEDED)
            player->CastSpell(player, LFG_SPELL_DUNGEON_DESERTER, true);
        else if (method == GROUP_REMOVEMETHOD_KICK_LFG)
            player->RemoveAurasDueToSpell(LFG_SPELL_DUNGEON_COOLDOWN);
        //else if (state == LFG_STATE_BOOT)
            // Update internal kick cooldown of kicked

        player->GetSession()->SendLfgUpdateParty(LfgUpdateData(LFG_UPDATETYPE_LEADER_UNK1));
        if (isLFG && player->GetMap()->IsDungeon())            // Teleport player out the dungeon
            sLFGMgr->TeleportPlayer(player, true);
    }

    if (isLFG && state != LFG_STATE_FINISHED_DUNGEON) // Need more players to finish the dungeon
        if (Player* leader = ObjectAccessor::FindConnectedPlayer(sLFGMgr->GetLeader(gguid)))
            leader->GetSession()->SendLfgOfferContinue(sLFGMgr->GetDungeon(gguid, false));
}

void LFGGroupScript::OnDisband(Group* group)
{
    if (!sLFGMgr->isOptionEnabled(LFG_OPTION_ENABLE_DUNGEON_FINDER | LFG_OPTION_ENABLE_RAID_BROWSER))
        return;

    ObjectGuid gguid = group->GetGUID();
    TC_LOG_DEBUG("lfg", "LFGScripts::OnDisband [{}]", gguid.ToString());

    sLFGMgr->RemoveGroupData(gguid);
}

void LFGGroupScript::OnChangeLeader(Group* group, ObjectGuid newLeaderGuid, ObjectGuid oldLeaderGuid)
{
    if (!sLFGMgr->isOptionEnabled(LFG_OPTION_ENABLE_DUNGEON_FINDER | LFG_OPTION_ENABLE_RAID_BROWSER))
        return;

    ObjectGuid gguid = group->GetGUID();

    TC_LOG_DEBUG("lfg", "LFGScripts::OnChangeLeader [{}]: old [{}] new [{}]",
        gguid.ToString(), newLeaderGuid.ToString(), oldLeaderGuid.ToString());

    sLFGMgr->SetLeader(gguid, newLeaderGuid);
}

void LFGGroupScript::OnInviteMember(Group* group, ObjectGuid guid)
{
    if (!sLFGMgr->isOptionEnabled(LFG_OPTION_ENABLE_DUNGEON_FINDER | LFG_OPTION_ENABLE_RAID_BROWSER))
        return;

    ObjectGuid gguid = group->GetGUID();
    ObjectGuid leader = group->GetLeaderGUID();
    TC_LOG_DEBUG("lfg", "LFGScripts::OnInviteMember [{}]: invite [{}] leader [{}]",
        gguid.ToString(), guid.ToString(), leader.ToString());

    // No gguid ==  new group being formed
    // No leader == after group creation first invite is new leader
    // leader and no gguid == first invite after leader is added to new group (this is the real invite)
    if (leader && !gguid)
        sLFGMgr->LeaveLfg(leader);
}

void AddSC_LFGScripts()
{
    new LFGPlayerScript();
    new LFGGroupScript();
}

} // namespace lfg
