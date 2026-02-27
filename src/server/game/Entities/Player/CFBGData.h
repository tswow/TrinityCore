#ifndef _CPLAYER_H
#define _CPLAYER_H

#include "Common.h"
#include "Player.h"

class Player;

class TC_GAME_API CFBGData
{
public:
    CFBGData(Player* player)
    {
        this->player = player;
    }

    bool NativeTeam() { return player->GetTeam() == GetOTeam(); }
    uint8 GetFRace() const { return 5; }
    uint8 GetORace() const { return 1; }
    uint32 GetOFaction() const { return 469; }
    uint32 GetFFaction() const { return 67; }
    uint32 GetOTeam() const { return m_oTeam; }
    void SetCFBGData();
    void ReplaceRacials();
    void ReplaceItems();
    void InitializeCFData();
    void SetRaceDisplayID();

private:
    Player* player;
    uint8 m_fRace;
    uint8 m_oRace;
    uint32 m_fFaction;
    uint32 m_oFaction;
    uint32 m_fTeam;
    uint32 m_oTeam;
};

#endif // _CPLAYER_H
