#include "CFBGQueue.h"

#include "BattlegroundMgr.h"
#include "World.h"

class GroupList : public std::list<GroupQueueInfo*>
{
public:
    void AddGroups(std::list<GroupQueueInfo*> list)
    {
        insert(end(), list.begin(), list.end());
    }

    void Sort()
    {
        sort([](GroupQueueInfo* a, GroupQueueInfo* b) { return a->JoinTime < b->JoinTime; });
    }
};

bool CFBGQueue::CheckMixedMatch(BattlegroundQueue* queue, Battleground* bg_template, BattlegroundBracketId bracket_id, uint32 minPlayers, uint32 maxPlayers)
{
    return CFBGGroupInserter(queue, bg_template, bracket_id, maxPlayers, maxPlayers, minPlayers);
}

bool CFBGQueue::MixPlayersToBG(BattlegroundQueue* queue, Battleground* bg, BattlegroundBracketId bracket_id)
{
    return CFBGGroupInserter(queue, bg, bracket_id, bg->GetFreeSlotsForTeam(ALLIANCE), bg->GetFreeSlotsForTeam(HORDE), 0);
}
bool CFBGQueue::CFBGGroupInserter(BattlegroundQueue* queue, Battleground* bg, BattlegroundBracketId bracket_id, uint32 AllyFree, uint32 HordeFree, uint32 MinPlayers)
{
    if (!bg->isBattleground())
        return false;

    // MinPlayers is 0 when filling an existing BG, not starting a new one
    bool Filling = MinPlayers == 0;

    uint32 MaxAlly = AllyFree;
    uint32 MaxHorde = HordeFree;

    queue->m_SelectionPools[TEAM_ALLIANCE].Init();
    queue->m_SelectionPools[TEAM_HORDE].Init();

    // Randomize initial group order to avoid bias
    bool AllyFirst = urand(0, 1);
    GroupList Groups;

    Groups.AddGroups(queue->m_QueuedGroups[bracket_id][AllyFirst ? BG_QUEUE_NORMAL_ALLIANCE : BG_QUEUE_NORMAL_HORDE]);
    Groups.AddGroups(queue->m_QueuedGroups[bracket_id][AllyFirst ? BG_QUEUE_NORMAL_HORDE : BG_QUEUE_NORMAL_ALLIANCE]);
    Groups.Sort();

    bool startable = false;

    for (auto& ginfo : Groups)
    {
        if (!ginfo->IsInvitedToBGInstanceGUID)
        {
            // Choose the team with more available slots (forces team splitting)
            bool AddAsAlly = AllyFree > HordeFree;

            // Assign the team
            ginfo->Team = AddAsAlly ? ALLIANCE : HORDE;

            // Try to add group to the selected team
            if (queue->m_SelectionPools[AddAsAlly ? TEAM_ALLIANCE : TEAM_HORDE].AddGroup(ginfo, AddAsAlly ? MaxAlly : MaxHorde))
            {
                if (AddAsAlly)
                    AllyFree -= ginfo->Players.size();
                else
                    HordeFree -= ginfo->Players.size();
            }
            else if (!Filling)
                break;

            // If enough players are present for both teams, and we're not just filling, we're ready to start
            if (!Filling &&
                queue->m_SelectionPools[TEAM_ALLIANCE].GetPlayerCount() >= MinPlayers &&
                queue->m_SelectionPools[TEAM_HORDE].GetPlayerCount() >= MinPlayers)
            {
                startable = true;
            }
        }
    }

    if (startable)
        return true;

    // Allow 1-player starts in test mode
    if (sBattlegroundMgr->isTesting() &&
        (queue->m_SelectionPools[TEAM_ALLIANCE].GetPlayerCount() + queue->m_SelectionPools[TEAM_HORDE].GetPlayerCount()) > 0)
        return true;

    // Always return true if just filling an existing BG
    if (Filling)
        return true;

    // Not enough to start — reset pools and try again later
    queue->m_SelectionPools[TEAM_ALLIANCE].Init();
    queue->m_SelectionPools[TEAM_HORDE].Init();
    return false;
}
