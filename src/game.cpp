/*

   Copyright [2008] [Trevor Hogan]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   CODE PORTED FROM THE ORIGINAL GHOST PROJECT: http://ghost.pwner.org/

*/

#include "game.h"
#include "bnet.h"
#include "config.h"
#include "gameplayer.h"
#include "gameprotocol.h"
#include "ghost.h"
#include "ghostdb.h"
#include "language.h"
#include "map.h"
#include "packed.h"
#include "savegame.h"
#include "socket.h"
#include "stats.h"
#include "statsdota.h"
#include "statsw3mmd.h"
#include "util.h"

#include <cmath>
#include <string>
#include <time.h>

//
// sorting classes
//

class CGamePlayerSortAscByPing
{
public:
    bool operator()(CGamePlayer *Player1, CGamePlayer *Player2) const
    {
        return Player1->GetPing(false) < Player2->GetPing(false);
    }
};

class CGamePlayerSortDescByPing
{
public:
    bool operator()(CGamePlayer *Player1, CGamePlayer *Player2) const
    {
        return Player1->GetPing(false) > Player2->GetPing(false);
    }
};

//
// CGame
//

CGame ::CGame(CGHost *nGHost, CMap *nMap, CSaveGame *nSaveGame, uint16_t nHostPort, unsigned char nGameState, std::string nGameName, std::string nOwnerName, std::string nCreatorName, std::string nCreatorServer) : CBaseGame(nGHost, nMap, nSaveGame, nHostPort, nGameState, nGameName, nOwnerName, nCreatorName, nCreatorServer)
{
    m_DBBanLast = NULL;
    m_DBGame    = new CDBGame(0, std::string(), m_Map->GetMapPath(), std::string(), std::string(), std::string(), 0);

    if (m_Map->GetMapType() == "w3mmd")
        m_Stats = new CStatsW3MMD(this, m_Map->GetMapStatsW3MMDCategory());
    else if (m_Map->GetMapType() == "dota")
        m_Stats = new CStatsDOTA(this);
    else
        m_Stats = NULL;

    m_CallableGameAdd = NULL;
}

CGame ::~CGame()
{
    if (m_CallableGameAdd && m_CallableGameAdd->GetReady())
    {
        if (m_CallableGameAdd->GetResult() > 0)
        {
            CONSOLE_Print("[GAME: " + m_GameName + "] saving player/stats data to database");

            // store the CDBGamePlayers in the database

            for (std::vector<CDBGamePlayer *>::iterator i = m_DBGamePlayers.begin(); i != m_DBGamePlayers.end(); i++)
                m_GHost->m_Callables.push_back(m_GHost->m_DB->ThreadedGamePlayerAdd(m_CallableGameAdd->GetResult(), (*i)->GetName(), (*i)->GetIP(), (*i)->GetSpoofed(), (*i)->GetSpoofedRealm(), (*i)->GetReserved(), (*i)->GetLoadingTime(), (*i)->GetLeft(), (*i)->GetLeftReason(), (*i)->GetTeam(), (*i)->GetColour()));

            // store the stats in the database

            if (m_Stats)
                m_Stats->Save(m_GHost, m_GHost->m_DB, m_CallableGameAdd->GetResult());
        }
        else
            CONSOLE_Print("[GAME: " + m_GameName + "] unable to save player/stats data to database");

        m_GHost->m_DB->RecoverCallable(m_CallableGameAdd);
        delete m_CallableGameAdd;
        m_CallableGameAdd = NULL;
    }

    for (std::vector<PairedBanCheck>::iterator i = m_PairedBanChecks.begin(); i != m_PairedBanChecks.end(); i++)
        m_GHost->m_Callables.push_back(i->second);

    for (std::vector<PairedBanAdd>::iterator i = m_PairedBanAdds.begin(); i != m_PairedBanAdds.end(); i++)
        m_GHost->m_Callables.push_back(i->second);

    for (std::vector<PairedGPSCheck>::iterator i = m_PairedGPSChecks.begin(); i != m_PairedGPSChecks.end(); i++)
        m_GHost->m_Callables.push_back(i->second);

    for (std::vector<PairedDPSCheck>::iterator i = m_PairedDPSChecks.begin(); i != m_PairedDPSChecks.end(); i++)
        m_GHost->m_Callables.push_back(i->second);

    for (std::vector<CDBBan *>::iterator i = m_DBBans.begin(); i != m_DBBans.end(); i++)
        delete *i;

    delete m_DBGame;

    for (std::vector<CDBGamePlayer *>::iterator i = m_DBGamePlayers.begin(); i != m_DBGamePlayers.end(); i++)
        delete *i;

    delete m_Stats;

    // it's a "bad thing" if m_CallableGameAdd is non NULL here
    // it means the game is being deleted after m_CallableGameAdd was created (the first step to saving the game data) but before the associated thread terminated
    // rather than failing horribly we choose to allow the thread to complete in the orphaned callables list but step 2 will never be completed
    // so this will create a game entry in the database without any gameplayers and/or DotA stats

    if (m_CallableGameAdd)
    {
        CONSOLE_Print("[GAME: " + m_GameName + "] game is being deleted before all game data was saved, game data has been lost");
        m_GHost->m_Callables.push_back(m_CallableGameAdd);
    }
}

bool CGame ::Update(void *fd, void *send_fd)
{
    // update callables

    for (std::vector<PairedBanCheck>::iterator i = m_PairedBanChecks.begin(); i != m_PairedBanChecks.end();)
    {
        if (i->second->GetReady())
        {
            CDBBan *Ban = i->second->GetResult();

            if (Ban)
                SendAllChat(m_GHost->m_Language->UserWasBannedOnByBecause(i->second->GetServer(), i->second->GetUser(), Ban->GetDate(), Ban->GetAdmin(), Ban->GetReason()));
            else
                SendAllChat(m_GHost->m_Language->UserIsNotBanned(i->second->GetServer(), i->second->GetUser()));

            m_GHost->m_DB->RecoverCallable(i->second);
            delete i->second;
            i = m_PairedBanChecks.erase(i);
        }
        else
            i++;
    }

    for (std::vector<PairedBanAdd>::iterator i = m_PairedBanAdds.begin(); i != m_PairedBanAdds.end();)
    {
        if (i->second->GetReady())
        {
            if (i->second->GetResult())
            {
                for (std::vector<CBNET *>::iterator j = m_GHost->m_BNETs.begin(); j != m_GHost->m_BNETs.end(); j++)
                {
                    if ((*j)->GetServer() == i->second->GetServer())
                        (*j)->AddBan(i->second->GetUser(), i->second->GetIP(), i->second->GetGameName(), i->second->GetAdmin(), i->second->GetReason());
                }

                SendAllChat(m_GHost->m_Language->PlayerWasBannedByPlayer(i->second->GetServer(), i->second->GetUser(), i->first));
            }

            m_GHost->m_DB->RecoverCallable(i->second);
            delete i->second;
            i = m_PairedBanAdds.erase(i);
        }
        else
            i++;
    }

    for (std::vector<PairedBanRemove>::iterator i = m_PairedBanRemoves.begin(); i != m_PairedBanRemoves.end();)
    {
        if (i->second->GetReady())
        {
            if (i->second->GetResult())
            {
                for (std::vector<CBNET *>::iterator j = m_GHost->m_BNETs.begin(); j != m_GHost->m_BNETs.end(); j++)
                {
                    if ((*j)->GetServer() == i->second->GetServer())
                        (*j)->RemoveBan(i->second->GetUser());
                }
            }

            CGamePlayer *Player = GetPlayerFromName(i->first, true);

            if (Player)
            {
                if (i->second->GetResult())
                    SendChat(Player, m_GHost->m_Language->UnbannedUser(i->second->GetUser()));
                else
                    SendChat(Player, m_GHost->m_Language->ErrorUnbanningUser(i->second->GetUser()));
            }

            m_GHost->m_DB->RecoverCallable(i->second);
            delete i->second;
            i = m_PairedBanRemoves.erase(i);
        }
        else
            i++;
    }

    for (std::vector<PairedGPSCheck>::iterator i = m_PairedGPSChecks.begin(); i != m_PairedGPSChecks.end();)
    {
        if (i->second->GetReady())
        {
            CDBGamePlayerSummary *GamePlayerSummary = i->second->GetResult();

            if (GamePlayerSummary)
            {
                if (i->first.empty())
                    SendAllChat(m_GHost->m_Language->HasPlayedGamesWithThisBot(i->second->GetName(), GamePlayerSummary->GetFirstGameDateTime(), GamePlayerSummary->GetLastGameDateTime(), UTIL_ToString(GamePlayerSummary->GetTotalGames()), UTIL_ToString((float)GamePlayerSummary->GetAvgLoadingTime() / 1000, 2), UTIL_ToString(GamePlayerSummary->GetAvgLeftPercent())));
                else
                {
                    CGamePlayer *Player = GetPlayerFromName(i->first, true);

                    if (Player)
                        SendChat(Player, m_GHost->m_Language->HasPlayedGamesWithThisBot(i->second->GetName(), GamePlayerSummary->GetFirstGameDateTime(), GamePlayerSummary->GetLastGameDateTime(), UTIL_ToString(GamePlayerSummary->GetTotalGames()), UTIL_ToString((float)GamePlayerSummary->GetAvgLoadingTime() / 1000, 2), UTIL_ToString(GamePlayerSummary->GetAvgLeftPercent())));
                }
            }
            else
            {
                if (i->first.empty())
                    SendAllChat(m_GHost->m_Language->HasntPlayedGamesWithThisBot(i->second->GetName()));
                else
                {
                    CGamePlayer *Player = GetPlayerFromName(i->first, true);

                    if (Player)
                        SendChat(Player, m_GHost->m_Language->HasntPlayedGamesWithThisBot(i->second->GetName()));
                }
            }

            m_GHost->m_DB->RecoverCallable(i->second);
            delete i->second;
            i = m_PairedGPSChecks.erase(i);
        }
        else
            i++;
    }

    for (std::vector<PairedDPSCheck>::iterator i = m_PairedDPSChecks.begin(); i != m_PairedDPSChecks.end();)
    {
        if (i->second->GetReady())
        {
            CDBDotAPlayerSummary *DotAPlayerSummary = i->second->GetResult();

            if (DotAPlayerSummary)
            {
                std::string Summary = m_GHost->m_Language->HasPlayedDotAGamesWithThisBot(i->second->GetName(),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetTotalGames()),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetTotalWins()),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetTotalLosses()),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetTotalKills()),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetTotalDeaths()),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetTotalCreepKills()),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetTotalCreepDenies()),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetTotalAssists()),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetTotalNeutralKills()),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetTotalTowerKills()),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetTotalRaxKills()),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetTotalCourierKills()),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetAvgKills(), 2),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetAvgDeaths(), 2),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetAvgCreepKills(), 2),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetAvgCreepDenies(), 2),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetAvgAssists(), 2),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetAvgNeutralKills(), 2),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetAvgTowerKills(), 2),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetAvgRaxKills(), 2),
                                                                                         UTIL_ToString(DotAPlayerSummary->GetAvgCourierKills(), 2));

                if (i->first.empty())
                    SendAllChat(Summary);
                else
                {
                    CGamePlayer *Player = GetPlayerFromName(i->first, true);

                    if (Player)
                        SendChat(Player, Summary);
                }
            }
            else
            {
                if (i->first.empty())
                    SendAllChat(m_GHost->m_Language->HasntPlayedDotAGamesWithThisBot(i->second->GetName()));
                else
                {
                    CGamePlayer *Player = GetPlayerFromName(i->first, true);

                    if (Player)
                        SendChat(Player, m_GHost->m_Language->HasntPlayedDotAGamesWithThisBot(i->second->GetName()));
                }
            }

            m_GHost->m_DB->RecoverCallable(i->second);
            delete i->second;
            i = m_PairedDPSChecks.erase(i);
        }
        else
            i++;
    }

    return CBaseGame ::Update(fd, send_fd);
}

void CGame ::EventPlayerDeleted(CGamePlayer *player)
{
    CBaseGame ::EventPlayerDeleted(player);

    // record everything we need to know about the player for storing in the database later
    // since we haven't stored the game yet (it's not over yet!) we can't link the gameplayer to the game
    // see the destructor for where these CDBGamePlayers are stored in the database
    // we could have inserted an incomplete record on creation and updated it later but this makes for a cleaner interface

    if (m_GameLoading || m_GameLoaded)
    {
        // todotodo: since we store players that crash during loading it's possible that the stats classes could have no information on them
        // that could result in a DBGamePlayer without a corresponding DBDotAPlayer - just be aware of the possibility

        unsigned char SID    = GetSIDFromPID(player->GetPID());
        unsigned char Team   = 255;
        unsigned char Colour = 255;

        if (SID < m_Slots.size())
        {
            Team   = m_Slots[SID].GetTeam();
            Colour = m_Slots[SID].GetColour();
        }

        m_DBGamePlayers.push_back(new CDBGamePlayer(0, 0, player->GetName(), player->GetExternalIPString(), player->GetSpoofed() ? 1 : 0, player->GetSpoofedRealm(), player->GetReserved() ? 1 : 0, player->GetFinishedLoading() ? player->GetFinishedLoadingTicks() - m_StartedLoadingTicks : 0, m_GameTicks / 1000, player->GetLeftReason(), Team, Colour));

        // also keep track of the last player to leave for the !banlast command

        for (std::vector<CDBBan *>::iterator i = m_DBBans.begin(); i != m_DBBans.end(); i++)
        {
            if ((*i)->GetName() == player->GetName())
                m_DBBanLast = *i;
        }
    }
}

void CGame ::EventPlayerAction(CGamePlayer *player, CIncomingAction *action)
{
    CBaseGame ::EventPlayerAction(player, action);

    // give the stats class a chance to process the action

    if (m_Stats && m_Stats->ProcessAction(action) && m_GameOverTime == 0)
    {
        CONSOLE_Print("[GAME: " + m_GameName + "] gameover timer started (stats class reported game over)");
        SendEndMessage();
        m_GameOverTime = GetTime();
    }
}

bool CGame ::EventPlayerBotCommand(CGamePlayer *player, std::string command, std::string payload)
{
    bool HideCommand = CBaseGame ::EventPlayerBotCommand(player, command, payload);
    bool spoofed;

    // todotodo: don't be lazy

    std::string User    = player->GetName();
    std::string Command = command;
    std::string Payload = payload;

    bool AdminCheck = false;

    for (std::vector<CBNET *>::iterator i = m_GHost->m_BNETs.begin(); i != m_GHost->m_BNETs.end(); i++)
    {
        if ((*i)->GetServer() == player->GetSpoofedRealm() && (*i)->IsAdmin(User))
        {
            AdminCheck = true;
            break;
        }
        //GHost++ Custom Build Addition start
        else if ((UTIL_IsLanIP(player->GetExternalIP()) || UTIL_IsLocalIP(player->GetExternalIP(), m_GHost->m_LocalAddresses)) && m_GHost->m_LANAdmins != 0)
        {
            if (m_GHost->m_LANAdmins == 1 || m_GHost->m_LANAdmins == 3)
                if ((m_GHost->m_GetLANRootAdmins == 1 && ((*i)->IsLANRootAdmin(User) || (*i)->IsAdmin(User))) || m_GHost->m_GetLANRootAdmins != 1)
                    AdminCheck = true;
            break;
        }
        //GHost++ Custom Build Addition end
    }

    //blue is owner start

    bool BluePlayer = false;

    CGamePlayer *p = NULL;
    unsigned char Nrt;
    unsigned char Nr = 255;
    for (std::vector<CGamePlayer *>::iterator i = m_Players.begin(); i != m_Players.end(); i++)
    {
        Nrt = GetSIDFromPID((*i)->GetPID());
        if (Nrt < Nr)
        {
            Nr = Nrt;
            p  = (*i);
        }
    }

    // this is blue player
    if (p)
        if (p->GetPID() == player->GetPID())
            BluePlayer = true;

    spoofed = false;
    if (BluePlayer)
    {
        AdminCheck = true;
        spoofed    = true; //force spoof
                           //AdminAccess = m_GHost->CMDAccessAddOwner(0);
    }

    //blue is owner end

    //проверки на рут админку
    bool RootAdminCheck = false;

    for (std::vector<CBNET *>::iterator i = m_GHost->m_BNETs.begin(); i != m_GHost->m_BNETs.end(); i++)
    {
        if ((*i)->GetServer() == player->GetSpoofedRealm() && (*i)->IsRootAdmin(User))
        {
            RootAdminCheck = true;
            break;
        }
        //GHost++ Custom Build Addition start
        else if ((UTIL_IsLanIP(player->GetExternalIP()) || UTIL_IsLocalIP(player->GetExternalIP(), m_GHost->m_LocalAddresses)) && m_GHost->m_LANAdmins != 0)
        {
            if (m_GHost->m_LANAdmins == 2)
                if ((m_GHost->m_GetLANRootAdmins == 1 && (*i)->IsLANRootAdmin(User)) || m_GHost->m_GetLANRootAdmins != 1)
                    RootAdminCheck = true;
            if (m_GHost->m_LANAdmins == 3)
                if ((m_GHost->m_GetLANRootAdmins == 1 && (*i)->IsLANRootAdmin(User)))
                    RootAdminCheck = true;
            break;
        }
        //GHost++ Custom Build Addition end
    }

    //tmp root
    if (IsTmpRootAdmin(User))
    {
        RootAdminCheck = true;
        spoofed        = true;
    }

    //---------------------------------------------

    if ((player->GetSpoofed() || spoofed) && (AdminCheck || RootAdminCheck || IsOwner(User)))
    {
        CONSOLE_Print("[GAME: " + m_GameName + "] admin [" + User + "] sent command [" + Command + "] with payload [" + Payload + "]");

        if (!m_Locked || RootAdminCheck || IsOwner(User))
        {
            /*****************
            * ADMIN COMMANDS *
            ******************/

            //
            // !ABORT (abort countdown)
            // !A
            //

            // we use "!a" as an alias for abort because you don't have much time to abort the countdown so it's useful for the abort command to be easy to type

            if ((Command == "abort" || Command == "a") && m_CountDownStarted && !m_GameLoading && !m_GameLoaded)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                SendAllChat(m_GHost->m_Language->CountDownAborted());
                m_CountDownStarted = false;
                m_AutoStartPlayers = 0;
                m_UsingStart       = false;
            }

            //
            // !ADDBAN
            // !BAN
            //

            if ((Command == "addban" || Command == "ban") && !Payload.empty() && !m_GHost->m_BNETs.empty())
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                // extract the victim and the reason
                // e.g. "Varlock leaver after dying" -> victim: "Varlock", reason: "leaver after dying"

                std::string Victim;
                std::string Reason;
                std::stringstream SS;
                SS << Payload;
                SS >> Victim;

                if (!SS.eof())
                {
                    getline(SS, Reason);
                    std::string ::size_type Start = Reason.find_first_not_of(" ");

                    if (Start != std::string ::npos)
                        Reason = Reason.substr(Start);
                }

                if (m_GameLoaded)
                {
                    std::string VictimLower = Victim;
                    std::transform(VictimLower.begin(), VictimLower.end(), VictimLower.begin(), (int (*)(int))tolower);
                    uint32_t Matches  = 0;
                    CDBBan *LastMatch = NULL;

                    // try to match each player with the passed std::string (e.g. "Varlock" would be matched with "lock")
                    // we use the m_DBBans std::vector for this in case the player already left and thus isn't in the m_Players std::vector anymore

                    for (std::vector<CDBBan *>::iterator i = m_DBBans.begin(); i != m_DBBans.end(); i++)
                    {
                        std::string TestName = (*i)->GetName();
                        std::transform(TestName.begin(), TestName.end(), TestName.begin(), (int (*)(int))tolower);

                        if (TestName.find(VictimLower) != std::string ::npos)
                        {
                            Matches++;
                            LastMatch = *i;

                            // if the name matches exactly stop any further matching

                            if (TestName == VictimLower)
                            {
                                Matches = 1;
                                break;
                            }
                        }
                    }

                    if (Matches == 0)
                        SendAllChat(m_GHost->m_Language->UnableToBanNoMatchesFound(Victim));
                    else if (Matches == 1)
                        m_PairedBanAdds.push_back(PairedBanAdd(User, m_GHost->m_DB->ThreadedBanAdd(LastMatch->GetServer(), LastMatch->GetName(), LastMatch->GetIP(), m_GameName, User, Reason)));
                    else
                        SendAllChat(m_GHost->m_Language->UnableToBanFoundMoreThanOneMatch(Victim));
                }
                else
                {
                    CGamePlayer *LastMatch = NULL;
                    uint32_t Matches       = GetPlayerFromNamePartial(Victim, &LastMatch);

                    if (Matches == 0)
                        SendAllChat(m_GHost->m_Language->UnableToBanNoMatchesFound(Victim));
                    else if (Matches == 1)
                        m_PairedBanAdds.push_back(PairedBanAdd(User, m_GHost->m_DB->ThreadedBanAdd(LastMatch->GetJoinedRealm(), LastMatch->GetName(), LastMatch->GetExternalIPString(), m_GameName, User, Reason)));
                    else
                        SendAllChat(m_GHost->m_Language->UnableToBanFoundMoreThanOneMatch(Victim));
                }
            }

            //
            // !DELBAN
            // !UNBAN
            //

            if ((Command == "delban" || Command == "unban") && !Payload.empty())
                m_PairedBanRemoves.push_back(PairedBanRemove(player->GetName(), m_GHost->m_DB->ThreadedBanRemove(Payload)));

            //
            // !ANNOUNCE
            //

            if (Command == "announce" && !m_CountDownStarted && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                if (Payload.empty() || Payload == "off")
                {
                    SendAllChat(m_GHost->m_Language->AnnounceMessageDisabled());
                    SetAnnounce(0, std::string());
                }
                else
                {
                    // extract the interval and the message
                    // e.g. "30 hello everyone" -> interval: "30", message: "hello everyone"

                    uint32_t Interval;
                    std::string Message;
                    std::stringstream SS;
                    SS << Payload;
                    SS >> Interval;

                    if (SS.fail() || Interval == 0)
                        CONSOLE_Print("[GAME: " + m_GameName + "] bad input #1 to announce command");
                    else
                    {
                        if (SS.eof())
                            CONSOLE_Print("[GAME: " + m_GameName + "] missing input #2 to announce command");
                        else
                        {
                            getline(SS, Message);
                            std::string ::size_type Start = Message.find_first_not_of(" ");

                            if (Start != std::string ::npos)
                                Message = Message.substr(Start);

                            SendAllChat(m_GHost->m_Language->AnnounceMessageEnabled());
                            SetAnnounce(Interval, Message);
                        }
                    }
                }
            }

            //
            // !AUTOSAVE
            //

            if (Command == "autosave")
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                if (Payload == "on")
                {
                    SendAllChat(m_GHost->m_Language->AutoSaveEnabled());
                    m_AutoSave = true;
                }
                else if (Payload == "off")
                {
                    SendAllChat(m_GHost->m_Language->AutoSaveDisabled());
                    m_AutoSave = false;
                }
            }

            //
            // !AUTOSTART
            //

            if (Command == "autostart" && !m_CountDownStarted && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                if (Payload.empty() || Payload == "off")
                {
                    SendAllChat(m_GHost->m_Language->AutoStartDisabled());
                    m_AutoStartPlayers = 0;
                    if (m_UsingStart == true)
                        m_UsingStart = false;
                }
                else
                {
                    uint32_t AutoStartPlayers = UTIL_ToUInt32(Payload);

                    if (AutoStartPlayers != 0)
                    {
                        SendAllChat(m_GHost->m_Language->AutoStartEnabled(UTIL_ToString(AutoStartPlayers)));
                        m_AutoStartPlayers = AutoStartPlayers;
                    }
                }
            }

            //
            // !BANLAST
            //

            if (Command == "banlast" && m_GameLoaded && !m_GHost->m_BNETs.empty() && m_DBBanLast && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                m_PairedBanAdds.push_back(PairedBanAdd(User, m_GHost->m_DB->ThreadedBanAdd(m_DBBanLast->GetServer(), m_DBBanLast->GetName(), m_DBBanLast->GetIP(), m_GameName, User, Payload)));
            }

            //
            // !CHECK
            //

            if (Command == "check")
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                if (!Payload.empty())
                {
                    CGamePlayer *LastMatch = NULL;
                    uint32_t Matches       = GetPlayerFromNamePartial(Payload, &LastMatch);

                    if (Matches == 0)
                        SendAllChat(m_GHost->m_Language->UnableToCheckPlayerNoMatchesFound(Payload));
                    else if (Matches == 1)
                    {
                        bool LastMatchAdminCheck = false;

                        for (std::vector<CBNET *>::iterator i = m_GHost->m_BNETs.begin(); i != m_GHost->m_BNETs.end(); i++)
                        {
                            if ((*i)->GetServer() == LastMatch->GetSpoofedRealm() && (*i)->IsAdmin(LastMatch->GetName()))
                            {
                                LastMatchAdminCheck = true;
                                break;
                            }
                        }

                        bool LastMatchRootAdminCheck = false;

                        for (std::vector<CBNET *>::iterator i = m_GHost->m_BNETs.begin(); i != m_GHost->m_BNETs.end(); i++)
                        {
                            if ((*i)->GetServer() == LastMatch->GetSpoofedRealm() && (*i)->IsRootAdmin(LastMatch->GetName()))
                            {
                                LastMatchRootAdminCheck = true;
                                break;
                            }
                        }

                        SendAllChat(m_GHost->m_Language->CheckedPlayer(LastMatch->GetName(), LastMatch->GetNumPings() > 0 ? UTIL_ToString(LastMatch->GetPing(m_GHost->m_LCPings)) + "ms" : "N/A", m_GHost->m_DBLocal->FromCheck(UTIL_ByteArrayToUInt32(LastMatch->GetExternalIP(), true)), LastMatchAdminCheck || LastMatchRootAdminCheck ? "Yes" : "No", IsOwner(LastMatch->GetName()) ? "Yes" : "No", LastMatch->GetSpoofed() ? "Yes" : "No", LastMatch->GetSpoofedRealm().empty() ? "N/A" : LastMatch->GetSpoofedRealm(), LastMatch->GetReserved() ? "Yes" : "No"));
                    }
                    else
                        SendAllChat(m_GHost->m_Language->UnableToCheckPlayerFoundMoreThanOneMatch(Payload));
                }
                else
                    SendAllChat(m_GHost->m_Language->CheckedPlayer(User, player->GetNumPings() > 0 ? UTIL_ToString(player->GetPing(m_GHost->m_LCPings)) + "ms" : "N/A", m_GHost->m_DBLocal->FromCheck(UTIL_ByteArrayToUInt32(player->GetExternalIP(), true)), AdminCheck || RootAdminCheck ? "Yes" : "No", IsOwner(User) ? "Yes" : "No", player->GetSpoofed() ? "Yes" : "No", player->GetSpoofedRealm().empty() ? "N/A" : player->GetSpoofedRealm(), player->GetReserved() ? "Yes" : "No"));
            }

            //
            // !CHECKBAN
            //

            if (Command == "checkban" && !Payload.empty() && !m_GHost->m_BNETs.empty())
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                for (std::vector<CBNET *>::iterator i = m_GHost->m_BNETs.begin(); i != m_GHost->m_BNETs.end(); i++)
                    m_PairedBanChecks.push_back(PairedBanCheck(User, m_GHost->m_DB->ThreadedBanCheck((*i)->GetServer(), Payload, std::string())));
            }

            //
            // !CLEARHCL
            //

            if (Command == "clearhcl" && !m_CountDownStarted)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                m_HCLCommandString.clear();
                SendAllChat(m_GHost->m_Language->ClearingHCL());
            }

            //
            // !CLOSE (close slot)
            //

            if (Command == "close" && !Payload.empty() && !m_GameLoading && !m_GameLoaded && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                // close as many slots as specified, e.g. "5 10" closes slots 5 and 10

                std::stringstream SS;
                SS << Payload;

                while (!SS.eof())
                {
                    uint32_t SID;
                    SS >> SID;

                    if (SS.fail())
                    {
                        CONSOLE_Print("[GAME: " + m_GameName + "] bad input to close command");
                        break;
                    }
                    else
                        CloseSlot((unsigned char)(SID - 1), true);
                }
            }

            //
            // !CLOSEALL
            //

            if (Command == "closeall" && !m_GameLoading && !m_GameLoaded && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                CloseAllSlots();
            }

            //
            // !COMP (computer slot)
            //

            if (Command == "comp" && !Payload.empty() && !m_GameLoading && !m_GameLoaded && !m_SaveGame && (RootAdminCheck || m_GHost->m_AddCompsAllowed))
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                // extract the slot and the skill
                // e.g. "1 2" -> slot: "1", skill: "2"

                uint32_t Slot;
                uint32_t Skill = 1;
                std::stringstream SS;
                SS << Payload;
                SS >> Slot;

                if (SS.fail())
                    CONSOLE_Print("[GAME: " + m_GameName + "] bad input #1 to comp command");
                else
                {
                    if (!SS.eof())
                        SS >> Skill;

                    if (SS.fail())
                        CONSOLE_Print("[GAME: " + m_GameName + "] bad input #2 to comp command");
                    else
                        ComputerSlot((unsigned char)(Slot - 1), (unsigned char)Skill, true);
                }
            }

            //
            // !COLOUR (computer colour change)
            //

            if (Command == "colour" && !Payload.empty() && !m_GameLoading && !m_GameLoaded && !m_SaveGame)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                // extract the slot and the colour
                // e.g. "1 2" -> slot: "1", colour: "2"

                uint32_t Slot;
                uint32_t Colour;
                std::stringstream SS;
                SS << Payload;
                SS >> Slot;

                if (SS.fail())
                    CONSOLE_Print("[GAME: " + m_GameName + "] bad input #1 to compcolour command");
                else
                {
                    if (SS.eof())
                        CONSOLE_Print("[GAME: " + m_GameName + "] missing input #2 to compcolour command");
                    else
                    {
                        SS >> Colour;

                        if (SS.fail())
                            CONSOLE_Print("[GAME: " + m_GameName + "] bad input #2 to compcolour command");
                        else
                        {
                            unsigned char SID = (unsigned char)(Slot - 1);

                            if (Colour < 12 && SID < m_Slots.size())
                            {
                                //if( m_Slots[SID].GetSlotStatus( ) == SLOTSTATUS_OCCUPIED && m_Slots[SID].GetComputer( ) == 1 )
                                ColourSlot(SID, Colour);
                            }
                        }
                    }
                }
            }

            //
            // !HANDICAP
            //

            if (Command == "handicap" && !Payload.empty() && !m_GameLoading && !m_GameLoaded && !m_SaveGame)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                // extract the slot and the handicap
                // e.g. "1 50" -> slot: "1", handicap: "50"

                uint32_t Slot;
                uint32_t Handicap;
                std::stringstream SS;
                SS << Payload;
                SS >> Slot;

                if (SS.fail())
                    CONSOLE_Print("[GAME: " + m_GameName + "] bad input #1 to comphandicap command");
                else
                {
                    if (SS.eof())
                        CONSOLE_Print("[GAME: " + m_GameName + "] missing input #2 to comphandicap command");
                    else
                    {
                        SS >> Handicap;

                        if (SS.fail())
                            CONSOLE_Print("[GAME: " + m_GameName + "] bad input #2 to comphandicap command");
                        else
                        {
                            unsigned char SID = (unsigned char)(Slot - 1);

                            //if( !( m_Map->GetMapOptions( ) & MAPOPT_FIXEDPLAYERSETTINGS ) && ( Handicap == 50 || Handicap == 60 || Handicap == 70 || Handicap == 80 || Handicap == 90 || Handicap == 100 ) && SID < m_Slots.size( ) )
                            if ((Handicap >= 1 && Handicap <= 255) && SID < m_Slots.size())
                            {
                                //if( m_Slots[SID].GetSlotStatus( ) == SLOTSTATUS_OCCUPIED && m_Slots[SID].GetComputer( ) == 1 )
                                //{
                                m_Slots[SID].SetHandicap((unsigned char)Handicap);
                                SendAllSlotInfo();
                                //}
                            }
                        }
                    }
                }
            }

            //
            // !COMPRACE (computer race change)
            //

            if (Command == "comprace" && !Payload.empty() && !m_GameLoading && !m_GameLoaded && !m_SaveGame)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                // extract the slot and the race
                // e.g. "1 human" -> slot: "1", race: "human"

                uint32_t Slot;
                std::string Race;
                std::stringstream SS;
                SS << Payload;
                SS >> Slot;

                if (SS.fail())
                    CONSOLE_Print("[GAME: " + m_GameName + "] bad input #1 to comprace command");
                else
                {
                    if (SS.eof())
                        CONSOLE_Print("[GAME: " + m_GameName + "] missing input #2 to comprace command");
                    else
                    {
                        getline(SS, Race);
                        std::string ::size_type Start = Race.find_first_not_of(" ");

                        if (Start != std::string ::npos)
                            Race = Race.substr(Start);

                        std::transform(Race.begin(), Race.end(), Race.begin(), (int (*)(int))tolower);
                        unsigned char SID = (unsigned char)(Slot - 1);

                        if (!(m_Map->GetMapOptions() & MAPOPT_FIXEDPLAYERSETTINGS) && !(m_Map->GetMapFlags() & MAPFLAG_RANDOMRACES) && SID < m_Slots.size())
                        {
                            if (m_Slots[SID].GetSlotStatus() == SLOTSTATUS_OCCUPIED && m_Slots[SID].GetComputer() == 1)
                            {
                                if (Race == "human")
                                {
                                    m_Slots[SID].SetRace(SLOTRACE_HUMAN | SLOTRACE_SELECTABLE);
                                    SendAllSlotInfo();
                                }
                                else if (Race == "orc")
                                {
                                    m_Slots[SID].SetRace(SLOTRACE_ORC | SLOTRACE_SELECTABLE);
                                    SendAllSlotInfo();
                                }
                                else if (Race == "night elf")
                                {
                                    m_Slots[SID].SetRace(SLOTRACE_NIGHTELF | SLOTRACE_SELECTABLE);
                                    SendAllSlotInfo();
                                }
                                else if (Race == "undead")
                                {
                                    m_Slots[SID].SetRace(SLOTRACE_UNDEAD | SLOTRACE_SELECTABLE);
                                    SendAllSlotInfo();
                                }
                                else if (Race == "random")
                                {
                                    m_Slots[SID].SetRace(SLOTRACE_RANDOM | SLOTRACE_SELECTABLE);
                                    SendAllSlotInfo();
                                }
                                else
                                    CONSOLE_Print("[GAME: " + m_GameName + "] unknown race [" + Race + "] sent to comprace command");
                            }
                        }
                    }
                }
            }

            //
            // !COMPTEAM (computer team change)
            //

            if (Command == "compteam" && !Payload.empty() && !m_GameLoading && !m_GameLoaded && !m_SaveGame)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                // extract the slot and the team
                // e.g. "1 2" -> slot: "1", team: "2"

                uint32_t Slot;
                uint32_t Team;
                std::stringstream SS;
                SS << Payload;
                SS >> Slot;

                if (SS.fail())
                    CONSOLE_Print("[GAME: " + m_GameName + "] bad input #1 to compteam command");
                else
                {
                    if (SS.eof())
                        CONSOLE_Print("[GAME: " + m_GameName + "] missing input #2 to compteam command");
                    else
                    {
                        SS >> Team;

                        if (SS.fail())
                            CONSOLE_Print("[GAME: " + m_GameName + "] bad input #2 to compteam command");
                        else
                        {
                            unsigned char SID = (unsigned char)(Slot - 1);

                            if (!(m_Map->GetMapOptions() & MAPOPT_FIXEDPLAYERSETTINGS) && Team < 12 && SID < m_Slots.size())
                            {
                                if (m_Slots[SID].GetSlotStatus() == SLOTSTATUS_OCCUPIED && m_Slots[SID].GetComputer() == 1)
                                {
                                    m_Slots[SID].SetTeam((unsigned char)(Team - 1));
                                    SendAllSlotInfo();
                                }
                            }
                        }
                    }
                }
            }

            //
            // !DBSTATUS
            //

            if (Command == "dbstatus")
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                SendAllChat(m_GHost->m_DB->GetStatus());
            }

            //
            // !DOWNLOAD
            // !DL
            //

            if ((Command == "download" || Command == "dl") && !Payload.empty() && !m_GameLoading && !m_GameLoaded)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                CGamePlayer *LastMatch = NULL;
                uint32_t Matches       = GetPlayerFromNamePartial(Payload, &LastMatch);

                if (Matches == 0)
                    SendAllChat(m_GHost->m_Language->UnableToStartDownloadNoMatchesFound(Payload));
                else if (Matches == 1)
                {
                    if (!LastMatch->GetDownloadStarted() && !LastMatch->GetDownloadFinished())
                    {
                        unsigned char SID = GetSIDFromPID(LastMatch->GetPID());

                        if (SID < m_Slots.size() && m_Slots[SID].GetDownloadStatus() != 100)
                        {
                            // inform the client that we are willing to send the map

                            CONSOLE_Print("[GAME: " + m_GameName + "] map download started for player [" + LastMatch->GetName() + "]");
                            Send(LastMatch, m_Protocol->SEND_W3GS_STARTDOWNLOAD(GetHostPID()));
                            LastMatch->SetDownloadAllowed(true);
                            LastMatch->SetDownloadStarted(true);
                            LastMatch->SetStartedDownloadingTicks(GetTicks());
                        }
                    }
                }
                else
                    SendAllChat(m_GHost->m_Language->UnableToStartDownloadFoundMoreThanOneMatch(Payload));
            }

            //
            // !DROP
            //

            if (Command == "drop" && m_GameLoaded)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                StopLaggers("lagged out (dropped by admin)");
            }

            //
            // !END
            //

            if (Command == "end" && m_GameLoaded)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                CONSOLE_Print("[GAME: " + m_GameName + "] is over (admin ended game)");
                StopPlayers("was disconnected (admin ended game)");
            }

            //
            // !FAKEPLAYER
            //

            if (Command == "fakeplayer" && !m_CountDownStarted && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                if (m_FakePlayerPID == 255)
                    CreateFakePlayer();
                else
                    DeleteFakePlayer();
            }

            //
            // !FPPAUSE
            //

            if (Command == "fppause" && m_FakePlayerPID != 255 && m_GameLoaded && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                BYTEARRAY CRC;
                BYTEARRAY Action;
                Action.push_back(1);
                m_Actions.push(new CIncomingAction(m_FakePlayerPID, CRC, Action));
            }

            //
            // !FPRESUME
            //

            if (Command == "fpresume" && m_FakePlayerPID != 255 && m_GameLoaded && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                BYTEARRAY CRC;
                BYTEARRAY Action;
                Action.push_back(2);
                m_Actions.push(new CIncomingAction(m_FakePlayerPID, CRC, Action));
            }

            //
            // !FROM
            //

            if (Command == "from")
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                std::string Froms;

                for (std::vector<CGamePlayer *>::iterator i = m_Players.begin(); i != m_Players.end(); i++)
                {
                    // we reverse the byte order on the IP because it's stored in network byte order

                    Froms += (*i)->GetNameTerminated();
                    Froms += ": (";
                    Froms += m_GHost->m_DBLocal->FromCheck(UTIL_ByteArrayToUInt32((*i)->GetExternalIP(), true));
                    Froms += ")";

                    if (i != m_Players.end() - 1)
                        Froms += ", ";

                    if ((m_GameLoading || m_GameLoaded) && Froms.size() > 100)
                    {
                        // cut the text into multiple lines ingame

                        SendAllChat(Froms);
                        Froms.clear();
                    }
                }

                if (!Froms.empty())
                    SendAllChat(Froms);
            }

            //
            // !HCL
            //

            if (Command == "hcl" && !m_CountDownStarted)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                if (!Payload.empty())
                {
                    if (Payload.size() <= m_Slots.size())
                    {
                        std::string HCLChars = "abcdefghijklmnopqrstuvwxyz0123456789 -=,.";

                        if (Payload.find_first_not_of(HCLChars) == std::string ::npos)
                        {
                            m_HCLCommandString = Payload;
                            SendAllChat(m_GHost->m_Language->SettingHCL(m_HCLCommandString));
                            // @disturbed_oc
                            m_HCLOverride = true;
                            // @end
                        }
                        else
                            SendAllChat(m_GHost->m_Language->UnableToSetHCLInvalid());
                    }
                    else
                        SendAllChat(m_GHost->m_Language->UnableToSetHCLTooLong());
                }
                else
                    SendAllChat(m_GHost->m_Language->TheHCLIs(m_HCLCommandString));
            }

            //
            // !HOLD (hold a slot for someone)
            //

            if (Command == "hold" && !Payload.empty() && !m_GameLoading && !m_GameLoaded && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                // hold as many players as specified, e.g. "Varlock Kilranin" holds players "Varlock" and "Kilranin"

                std::stringstream SS;
                SS << Payload;

                while (!SS.eof())
                {
                    std::string HoldName;
                    SS >> HoldName;

                    if (SS.fail())
                    {
                        CONSOLE_Print("[GAME: " + m_GameName + "] bad input to hold command");
                        break;
                    }
                    else
                    {
                        SendAllChat(m_GHost->m_Language->AddedPlayerToTheHoldList(HoldName));
                        AddToReserved(HoldName);
                    }
                }
            }

            //
            // !KICK (kick a player)
            //

            if (Command == "kick" && !Payload.empty() && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                CGamePlayer *LastMatch = NULL;
                uint32_t Matches       = GetPlayerFromNamePartial(Payload, &LastMatch);

                if (Matches == 0)
                    SendAllChat(m_GHost->m_Language->UnableToKickNoMatchesFound(Payload));
                else if (Matches == 1)
                {
                    LastMatch->SetDeleteMe(true);
                    LastMatch->SetLeftReason(m_GHost->m_Language->WasKickedByPlayer(User));

                    if (!m_GameLoading && !m_GameLoaded)
                        LastMatch->SetLeftCode(PLAYERLEAVE_LOBBY);
                    else
                        LastMatch->SetLeftCode(PLAYERLEAVE_LOST);

                    if (!m_GameLoading && !m_GameLoaded)
                        OpenSlot(GetSIDFromPID(LastMatch->GetPID()), false);
                }
                else
                    SendAllChat(m_GHost->m_Language->UnableToKickFoundMoreThanOneMatch(Payload));
            }

            //
            // !LATENCY (set game latency)
            //

            if (Command == "latency")
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                if (Payload.empty())
                    SendAllChat(m_GHost->m_Language->LatencyIs(UTIL_ToString(m_Latency)));
                else
                {
                    m_Latency = UTIL_ToUInt32(Payload);

                    if (m_Latency <= 20)
                    {
                        m_Latency = 20;
                        SendAllChat(m_GHost->m_Language->SettingLatencyToMinimum("20"));
                    }
                    else if (m_Latency >= 500)
                    {
                        m_Latency = 500;
                        SendAllChat(m_GHost->m_Language->SettingLatencyToMaximum("500"));
                    }
                    else
                        SendAllChat(m_GHost->m_Language->SettingLatencyTo(UTIL_ToString(m_Latency)));
                }
            }

            //
            // !LOCK
            //

            if (Command == "lock" && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                SendAllChat(m_GHost->m_Language->GameLocked());
                m_Locked = true;
            }

            //
            // !MESSAGES
            //

            if (Command == "messages")
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                if (Payload == "on")
                {
                    SendAllChat(m_GHost->m_Language->LocalAdminMessagesEnabled());
                    m_LocalAdminMessages = true;
                }
                else if (Payload == "off")
                {
                    SendAllChat(m_GHost->m_Language->LocalAdminMessagesDisabled());
                    m_LocalAdminMessages = false;
                }
            }

            //
            // !MUTE
            //

            if (Command == "mute")
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                CGamePlayer *LastMatch = NULL;
                uint32_t Matches       = GetPlayerFromNamePartial(Payload, &LastMatch);

                if (Matches == 0)
                    SendAllChat(m_GHost->m_Language->UnableToMuteNoMatchesFound(Payload));
                else if (Matches == 1)
                {
                    SendAllChat(m_GHost->m_Language->MutedPlayer(LastMatch->GetName(), User));
                    LastMatch->SetMuted(true);
                }
                else
                    SendAllChat(m_GHost->m_Language->UnableToMuteFoundMoreThanOneMatch(Payload));
            }

            //
            // !MUTEALL
            //

            if (Command == "muteall" && m_GameLoaded)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                SendAllChat(m_GHost->m_Language->GlobalChatMuted());
                m_MuteAll = true;
            }

            //
            // !OPEN (open slot)
            //

            if (Command == "open" && !Payload.empty() && !m_GameLoading && !m_GameLoaded && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                // open as many slots as specified, e.g. "5 10" opens slots 5 and 10

                std::stringstream SS;
                SS << Payload;

                while (!SS.eof())
                {
                    uint32_t SID;
                    SS >> SID;

                    if (SS.fail())
                    {
                        CONSOLE_Print("[GAME: " + m_GameName + "] bad input to open command");
                        break;
                    }
                    else
                        OpenSlot((unsigned char)(SID - 1), true);
                }
            }

            //
            // !OPENALL
            //

            if (Command == "openall" && !m_GameLoading && !m_GameLoaded && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                OpenAllSlots();
            }

            //
            // !OWNER (set game owner)
            //

            if (Command == "owner" && RootAdminCheck)
            {
                if (RootAdminCheck || IsOwner(User) || !GetPlayerFromName(m_OwnerName, false))
                {
                    if (m_GHost->m_HideCommands)
                        HideCommand = true;

                    if (!Payload.empty())
                    {
                        SendAllChat(m_GHost->m_Language->SettingGameOwnerTo(Payload));
                        m_OwnerName = Payload;
                    }
                    else
                    {
                        SendAllChat(m_GHost->m_Language->SettingGameOwnerTo(User));
                        m_OwnerName = User;
                    }
                }
                else
                    SendAllChat(m_GHost->m_Language->UnableToSetGameOwner(m_OwnerName));
            }

            //
            // !PING KICK
            //

            if (Command == "pingk" || Command == "pingkick")
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                // kick players with ping higher than payload if payload isn't empty
                // we only do this if the game hasn't started since we don't want to kick players from a game in progress

                uint32_t Kicked   = 0;
                uint32_t KickPing = 0;

                if (!m_GameLoading && !m_GameLoaded && !Payload.empty())
                    KickPing = UTIL_ToUInt32(Payload);

                // copy the m_Players std::vector so we can sort by descending ping so it's easier to find players with high pings

                std::vector<CGamePlayer *> SortedPlayers = m_Players;
                sort(SortedPlayers.begin(), SortedPlayers.end(), CGamePlayerSortDescByPing());
                std::string Pings;

                for (std::vector<CGamePlayer *>::iterator i = SortedPlayers.begin(); i != SortedPlayers.end(); i++)
                {
                    Pings += (*i)->GetNameTerminated();
                    Pings += ": ";

                    if ((*i)->GetNumPings() > 0)
                    {
                        Pings += UTIL_ToString((*i)->GetPing(m_GHost->m_LCPings));

                        if (!m_GameLoading && !m_GameLoaded && !(*i)->GetReserved() && KickPing > 0 && (*i)->GetPing(m_GHost->m_LCPings) > KickPing)
                        {
                            (*i)->SetDeleteMe(true);
                            (*i)->SetLeftReason("was kicked for excessive ping " + UTIL_ToString((*i)->GetPing(m_GHost->m_LCPings)) + " > " + UTIL_ToString(KickPing));
                            (*i)->SetLeftCode(PLAYERLEAVE_LOBBY);
                            OpenSlot(GetSIDFromPID((*i)->GetPID()), false);
                            Kicked++;
                        }

                        Pings += "ms";
                    }
                    else
                        Pings += "N/A";

                    if (i != SortedPlayers.end() - 1)
                        Pings += ", ";

                    if ((m_GameLoading || m_GameLoaded) && Pings.size() > 100)
                    {
                        // cut the text into multiple lines ingame

                            if (HideCommand)
                                SendChat(player, Pings);
                            else
                                SendAllChat(Pings);
                        Pings.clear();
                    }
                }

                if (!Pings.empty())
                {
                    if (HideCommand)
                        SendChat(player, Pings);
                    else
                        SendAllChat(Pings);
                }

                if (Kicked > 0)
                    SendAllChat(m_GHost->m_Language->KickingPlayersWithPingsGreaterThan(UTIL_ToString(Kicked), UTIL_ToString(KickPing)));
            }

            //
            // !PRIV (rehost as private game)
            //

            if (Command == "priv" && !Payload.empty() && !m_CountDownStarted && !m_SaveGame && (!BluePlayer || RootAdminCheck))
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;
                if (Payload.length() < 31)
                {
                    CONSOLE_Print("[GAME: " + m_GameName + "] trying to rehost as private game [" + Payload + "]");
                    SendAllChat(m_GHost->m_Language->TryingToRehostAsPrivateGame(Payload));
                    m_GameState       = GAME_PRIVATE;
                    m_LastGameName    = m_GameName;
                    m_GameName        = Payload;
                    m_HostCounter     = m_GHost->m_HostCounter++;
                    m_RefreshError    = false;
                    m_RefreshRehosted = true;

                    for (std::vector<CBNET *>::iterator i = m_GHost->m_BNETs.begin(); i != m_GHost->m_BNETs.end(); i++)
                    {
                        // unqueue any existing game refreshes because we're going to assume the next successful game refresh indicates that the rehost worked
                        // this ignores the fact that it's possible a game refresh was just sent and no response has been received yet
                        // we assume this won't happen very often since the only downside is a potential false positive

                        (*i)->UnqueueGameRefreshes();
                        (*i)->QueueGameUncreate();
                        (*i)->QueueEnterChat();

                        // we need to send the game creation message now because private games are not refreshed

                        (*i)->QueueGameCreate(m_GameState, m_GameName, std::string(), m_Map, NULL, m_HostCounter);

                        if ((*i)->GetPasswordHashType() != "pvpgn")
                            (*i)->QueueEnterChat();
                    }

                    m_CreationTime    = GetTime();
                    m_LastRefreshTime = GetTime();
                }
                else
                    SendAllChat(m_GHost->m_Language->UnableToCreateGameNameTooLong(Payload));

                // auto set HCL if map_defaulthcl is not empty
                AutoSetHCL();
            }
            else if (Command == "priv")
                SendAllChat("anus sebe !priv");

            //
            // !PUB (rehost as public game)
            //

            if (Command == "pub" && !Payload.empty() && !m_CountDownStarted && !m_SaveGame && (!BluePlayer || RootAdminCheck))
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;
                if (Payload.length() < 31)
                {
                    CONSOLE_Print("[GAME: " + m_GameName + "] trying to rehost as public game [" + Payload + "]");
                    SendAllChat(m_GHost->m_Language->TryingToRehostAsPublicGame(Payload));
                    m_GameState       = GAME_PUBLIC;
                    m_LastGameName    = m_GameName;
                    m_GameName        = Payload;
                    m_HostCounter     = m_GHost->m_HostCounter++;
                    m_RefreshError    = false;
                    m_RefreshRehosted = true;

                    for (std::vector<CBNET *>::iterator i = m_GHost->m_BNETs.begin(); i != m_GHost->m_BNETs.end(); i++)
                    {
                        // unqueue any existing game refreshes because we're going to assume the next successful game refresh indicates that the rehost worked
                        // this ignores the fact that it's possible a game refresh was just sent and no response has been received yet
                        // we assume this won't happen very often since the only downside is a potential false positive

                        (*i)->UnqueueGameRefreshes();
                        (*i)->QueueGameUncreate();
                        (*i)->QueueEnterChat();

                        // the game creation message will be sent on the next refresh
                    }

                    m_CreationTime    = GetTime();
                    m_LastRefreshTime = GetTime();
                }
                else
                    SendAllChat(m_GHost->m_Language->UnableToCreateGameNameTooLong(Payload));
                // auto set HCL if map_defaulthcl is not empty
                AutoSetHCL();
            }
            else if (Command == "pub")
                SendAllChat("anus sebe !pub");

            //
            // !REFRESH (turn on or off refresh messages)
            //

            if (Command == "refresh" && !m_CountDownStarted)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                if (Payload == "on")
                {
                    SendAllChat(m_GHost->m_Language->RefreshMessagesEnabled());
                    m_RefreshMessages = true;
                }
                else if (Payload == "off")
                {
                    SendAllChat(m_GHost->m_Language->RefreshMessagesDisabled());
                    m_RefreshMessages = false;
                }
            }

            //
            // !SAY
            //

            if ((Command == "say" || Command == "s") && !Payload.empty())
            {
                HideCommand = true;

                if (Payload.find("/") != std::string ::npos)
                {
                    if (RootAdminCheck)
                        for (std::vector<CBNET *>::iterator i = m_GHost->m_BNETs.begin(); i != m_GHost->m_BNETs.end(); i++)
                            SendAllChat(Payload);
                }
                else
                    for (std::vector<CBNET *>::iterator i = m_GHost->m_BNETs.begin(); i != m_GHost->m_BNETs.end(); i++)
                        SendAllChat(Payload);
            }

            //
            // !SENDLAN
            //

            if (Command == "sendlan" && !Payload.empty() && !m_CountDownStarted)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                // extract the ip and the port
                // e.g. "1.2.3.4 6112" -> ip: "1.2.3.4", port: "6112"

                std::string IP;
                uint32_t Port = 6112;
                std::stringstream SS;
                SS << Payload;
                SS >> IP;

                if (!SS.eof())
                    SS >> Port;

                if (SS.fail())
                    CONSOLE_Print("[GAME: " + m_GameName + "] bad inputs to sendlan command");
                else
                {
                    // we send 12 for SlotsTotal because this determines how many PID's Warcraft 3 allocates
                    // we need to make sure Warcraft 3 allocates at least SlotsTotal + 1 but at most 12 PID's
                    // this is because we need an extra PID for the virtual host player (but we always delete the virtual host player when the 12th person joins)
                    // however, we can't send 13 for SlotsTotal because this causes Warcraft 3 to crash when sharing control of units
                    // nor can we send SlotsTotal because then Warcraft 3 crashes when playing maps with less than 12 PID's (because of the virtual host player taking an extra PID)
                    // we also send 12 for SlotsOpen because Warcraft 3 assumes there's always at least one player in the game (the host)
                    // so if we try to send accurate numbers it'll always be off by one and results in Warcraft 3 assuming the game is full when it still needs one more player
                    // the easiest solution is to simply send 12 for both so the game will always show up as (1/12) players

                    if (m_SaveGame)
                    {
                        // note: the PrivateGame flag is not set when broadcasting to LAN (as you might expect)

                        uint32_t MapGameType = MAPGAMETYPE_SAVEDGAME;
                        BYTEARRAY MapWidth;
                        MapWidth.push_back(0);
                        MapWidth.push_back(0);
                        BYTEARRAY MapHeight;
                        MapHeight.push_back(0);
                        MapHeight.push_back(0);
                        m_GHost->m_UDPSocket->SendTo(IP, Port, m_Protocol->SEND_W3GS_GAMEINFO(m_GHost->m_TFT, m_GHost->m_LANWar3Version, UTIL_CreateByteArray(MapGameType, false), m_Map->GetMapGameFlags(), MapWidth, MapHeight, m_GameName, "beef", GetTime() - m_CreationTime, "Save\\Multiplayer\\" + m_SaveGame->GetFileNameNoPath(), m_SaveGame->GetMagicNumber(), 12, 12, m_HostPort, m_HostCounter));
                    }
                    else
                    {
                        // note: the PrivateGame flag is not set when broadcasting to LAN (as you might expect)
                        // note: we do not use m_Map->GetMapGameType because none of the filters are set when broadcasting to LAN (also as you might expect)

                        uint32_t MapGameType = MAPGAMETYPE_UNKNOWN0;
                        m_GHost->m_UDPSocket->SendTo(IP, Port, m_Protocol->SEND_W3GS_GAMEINFO(m_GHost->m_TFT, m_GHost->m_LANWar3Version, UTIL_CreateByteArray(MapGameType, false), m_Map->GetMapGameFlags(), m_Map->GetMapWidth(), m_Map->GetMapHeight(), m_GameName, "beef", GetTime() - m_CreationTime, m_Map->GetMapPath(), m_Map->GetMapCRC(), 12, 12, m_HostPort, m_HostCounter));
                    }
                }
            }

            //
            // !SP
            //

            if (Command == "sp" && !m_CountDownStarted)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                SendAllChat(m_GHost->m_Language->ShufflingPlayers());
                ShuffleSlots();
            }

            //
            // !START
            //

            if (Command == "start" && !m_CountDownStarted && m_Players.size() > 1)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                // if the player sent "!start force" skip the checks and start the countdown
                // otherwise check if a player left recently then start the autostart
                // *(GCBC)*
                if (Payload.empty())
                {
                    if (GetTicks() - m_LastPlayerLeaveTicks >= 2000)
                    {
                        uint32_t AutoStartPlayers = GetNumHumanPlayers();

                        if (AutoStartPlayers != 0)
                        {
                            SendAllChat(m_GHost->m_Language->AutoStartEnabled(UTIL_ToString(AutoStartPlayers)));
                            m_AutoStartPlayers = AutoStartPlayers;
                            m_UsingStart       = true;
                        }
                    }
                    else
                        SendAllChat(m_GHost->m_Language->CountDownAbortedSomeoneLeftRecently());
                }

                // *(GCBC)*
                //if( Payload == "now")
                //{
                // skip checks and start the game right now
                //	m_CountDownStarted = true;
                //	m_CountDownCounter = 0;
                //}
            }
            else if (Command == "start")
            {
                if (Payload == "force" && RootAdminCheck)
                    StartCountDown(true);
                else
                    SendAllChat("Need one more player for start");
            }

            // *(GCBC)*
            // !STARTN
            //

            if (Command == "startn" && !m_CountDownStarted && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                // skip checks and start the game right now
                m_CountDownStarted = true;
                m_CountDownCounter = 0;
            }
            else if (Command == "startn" && m_Players.size() < 2)
                SendAllChat("Need one more player for start");
            else if (Command == "startn" && !RootAdminCheck)
                SendAllChat("Need root rights for !startn");

            //
            // !SWAP (swap slots)
            //

            if (Command == "swap" && !Payload.empty() && !m_GameLoading && !m_GameLoaded)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                bool badObserverSwap = false;

                int SID1;
                int SID2;
                std::stringstream SS;
                SS << Payload;
                SS >> SID1;

                if (SS.fail() || SID1 < 1 || SID1 > 12)
                    CONSOLE_Print("[GAME: " + m_GameName + "] bad input #1 to swap command");
                else
                {
                    if (SS.eof())
                        CONSOLE_Print("[GAME: " + m_GameName + "] missing input #2 to swap command");
                    else
                    {
                        SS >> SID2;

                        if (SS.fail() || SID2 < 1 || SID2 > 12)
                            CONSOLE_Print("[GAME: " + m_GameName + "] bad input #2 to swap command");
                        else
                        {
                            std::vector<int> observerSlots;
                            int n;
                            std::stringstream stream(m_GHost->m_ObserverSlots);
                            while (stream >> n)
                                observerSlots.push_back(n);

                            if (!RootAdminCheck) // рут админ может свапать как хочет, скипаем проверки
                            {
                                if (m_Slots[SID1 - 1].GetSlotStatus() == SLOTSTATUS_OCCUPIED &&
                                    m_Slots[SID2 - 1].GetSlotStatus() == SLOTSTATUS_OCCUPIED)
                                {
                                    for (auto obsSlot = observerSlots.begin(); obsSlot != observerSlots.end(); ++obsSlot)
                                        if (*obsSlot == SID1 - 1 || *obsSlot == SID2 - 1) //попытка помен€ть двух игроков местами, из которых один обс
                                        {
                                            badObserverSwap = true; //«јѕ–≈“»“№
                                            SendAllChat("Forbidden swap (obs <> slot)");
                                        }
                                }
                                if (m_Slots[SID1 - 1].GetSlotStatus() == SLOTSTATUS_OCCUPIED && m_Slots[SID2 - 1].GetSlotStatus() == SLOTSTATUS_OPEN)
                                    for (auto obsSlot = observerSlots.begin(); obsSlot != observerSlots.end(); ++obsSlot)
                                        if (*obsSlot == SID2 - 1) //«јѕ–≈“»“№
                                        {
                                            SendAllChat("Forbidden swap (> obs)");
                                            badObserverSwap = true;
                                        }
                                if (m_Slots[SID2 - 1].GetSlotStatus() == SLOTSTATUS_OCCUPIED && m_Slots[SID1 - 1].GetSlotStatus() == SLOTSTATUS_OPEN)
                                    for (auto obsSlot = observerSlots.begin(); obsSlot != observerSlots.end(); ++obsSlot)
                                        if (*obsSlot == SID1 - 1) ///«јѕ–≈“»“№
                                        {
                                            SendAllChat("Forbidden swap (> obs)");
                                            badObserverSwap = true;
                                        }
                            }
                            if (badObserverSwap == false)
                                SwapSlots((unsigned char)(SID1 - 1), (unsigned char)(SID2 - 1));
                        }
                    }
                }
            }

            //
            //!DANCE
            //

            if (Command == "dance" && !m_GameLoading && !m_GameLoaded && m_StartedDanceTime == 0)
            {
                m_StartedDanceTime = GetTime();
            }

            //
            //!DESYNC
            //
            if (Command == "desync")
            {
                if (Payload.empty() || Payload != "on" || Payload != "ON" || Payload != "off" || Payload != "OFF")
                    SendChat(player, "usage: !desync <on | off>");
                if (Payload == "on" || Payload == "ON")
                {
                    m_GHost->m_DesyncKick = true;
                    SendAllChat("Desync kick enabled");
                }
                if (Payload == "off" || Payload == "OFF")
                {
                    m_GHost->m_DesyncKick = false;
                    SendAllChat("Desync kick disabled");
                }
            }

            //
            // !SYNCLIMIT
            //

            if (Command == "synclimit")
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                if (Payload.empty())
                    SendAllChat(m_GHost->m_Language->SyncLimitIs(UTIL_ToString(m_SyncLimit)));
                else
                {
                    m_SyncLimit = UTIL_ToUInt32(Payload);

                    if (m_SyncLimit <= 10)
                    {
                        m_SyncLimit = 10;
                        SendAllChat(m_GHost->m_Language->SettingSyncLimitToMinimum("10"));
                    }
                    else if (m_SyncLimit >= 10000)
                    {
                        m_SyncLimit = 10000;
                        SendAllChat(m_GHost->m_Language->SettingSyncLimitToMaximum("10000"));
                    }
                    else
                        SendAllChat(m_GHost->m_Language->SettingSyncLimitTo(UTIL_ToString(m_SyncLimit)));
                }
            }

            //
            // !UNHOST
            //

            if (Command == "unhost" && !m_CountDownStarted && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                m_Exiting = true;
            }

            //
            // !UNLOCK
            //

            if (Command == "unlock" && (RootAdminCheck || IsOwner(User)))
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                SendAllChat(m_GHost->m_Language->GameUnlocked());
                m_Locked = false;
            }

            //
            // !UNMUTE
            //

            if (Command == "unmute")
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                CGamePlayer *LastMatch = NULL;
                uint32_t Matches       = GetPlayerFromNamePartial(Payload, &LastMatch);

                if (Matches == 0)
                    SendAllChat(m_GHost->m_Language->UnableToMuteNoMatchesFound(Payload));
                else if (Matches == 1)
                {
                    SendAllChat(m_GHost->m_Language->UnmutedPlayer(LastMatch->GetName(), User));
                    LastMatch->SetMuted(false);
                }
                else
                    SendAllChat(m_GHost->m_Language->UnableToMuteFoundMoreThanOneMatch(Payload));
            }

            //
            // !UNMUTEALL
            //

            if (Command == "unmuteall" && m_GameLoaded)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                SendAllChat(m_GHost->m_Language->GlobalChatUnmuted());
                m_MuteAll = false;
            }

            //
            // !VIRTUALHOST
            //

            if (Command == "virtualhost" && !Payload.empty() && Payload.size() <= 15 && !m_CountDownStarted && RootAdminCheck)
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                DeleteVirtualHost();
                m_VirtualHostName = Payload;
            }

            //
            // !VOTECANCEL
            //

            if (Command == "votecancel" && !m_KickVotePlayer.empty())
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                SendAllChat(m_GHost->m_Language->VoteKickCancelled(m_KickVotePlayer));
                m_KickVotePlayer.clear();
                m_StartedKickVoteTime = 0;
            }

            //
            // !W
            //

            if (Command == "w" && !Payload.empty())
            {
                // extract the name and the message
                // e.g. "Varlock hello there!" -> name: "Varlock", message: "hello there!"

                std::string Name;
                std::string Message;
                std::string ::size_type MessageStart = Payload.find(" ");

                if (MessageStart != std::string ::npos)
                {
                    Name    = Payload.substr(0, MessageStart);
                    Message = Payload.substr(MessageStart + 1);

                    for (std::vector<CBNET *>::iterator i = m_GHost->m_BNETs.begin(); i != m_GHost->m_BNETs.end(); i++)
                        (*i)->QueueChatCommand(Message, Name, true, false);
                }

                HideCommand = true;
            }

            //
            //
            // !normalcountdown
            //

            if (Command == "normalcountdown" && !Payload.empty())
            {
                if (m_GHost->m_HideCommands)
                    HideCommand = true;

                if (Payload == "on")
                {
                    m_GHost->m_UseNormalCountDown = true;
                    SendAllChat("Normal WC3 countdown enabled");
                }
                else if (Payload == "off")
                {
                    m_GHost->m_UseNormalCountDown = false;
                    SendAllChat("Normal WC3 countdown disabled");
                }
            }
        }
        else
        {
            CONSOLE_Print("[GAME: " + m_GameName + "] admin command ignored, the game is locked");
            SendChat(player, m_GHost->m_Language->TheGameIsLocked());
        }
    }
    else
    {
        if ((!player->GetSpoofed() && !spoofed) && (AdminCheck || RootAdminCheck))
        {
            CONSOLE_Print("[GAME: " + m_GameName + "] non-spoofchecked user [" + User + "] sent command [" + Command + "] with payload [" + Payload + "]");
            SendAllChat("non-spoofchecked user [" + User + "] sent command [" + Command + "]. Wait please.");
        }
        else
            CONSOLE_Print("[GAME: " + m_GameName + "] non-admin [" + User + "] sent command [" + Command + "] with payload [" + Payload + "]");
    }

    /*********************
    * NON ADMIN COMMANDS *
    *********************/

    //!p
    //root admin

    if (Command == "p" && !Payload.empty())
    {
        HideCommand = true;

        if (IsTmpRootAdmin(User))
        {
            SendChat(player, "Allready tmp root admin.");
            SendChat(player, "Tmp root admins: " + GetTmpRootAdmins());
        }
        else
        {
            if (Payload == m_GHost->m_TMProotPassword)
            {
                {
                    AddTmpRootAdmin(User);
                    AddToReserved(User);
                    SendChat(player, "Success.");
                    SendChat(player, "Tmp root admins: " + GetTmpRootAdmins());
                }
            }
            else
                SendChat(player, "Wrong pass.");
        }
    }

    //
    // !CHECKME
    //

    if (Command == "checkme")
        SendChat(player, m_GHost->m_Language->CheckedPlayer(User, player->GetNumPings() > 0 ? UTIL_ToString(player->GetPing(m_GHost->m_LCPings)) + "ms" : "N/A", m_GHost->m_DBLocal->FromCheck(UTIL_ByteArrayToUInt32(player->GetExternalIP(), true)), AdminCheck || RootAdminCheck ? "Yes" : "No", IsOwner(User) ? "Yes" : "No", player->GetSpoofed() ? "Yes" : "No", player->GetSpoofedRealm().empty() ? "N/A" : player->GetSpoofedRealm(), player->GetReserved() ? "Yes" : "No"));

    //!ping

    if (Command == "ping")
    {
        //if (m_GHost->m_HideCommands)
        HideCommand = true;

        std::vector<CGamePlayer *> SortedPlayers = m_Players;
        sort(SortedPlayers.begin(), SortedPlayers.end(), CGamePlayerSortDescByPing());
        std::string Pings;

        for (std::vector<CGamePlayer *>::iterator i = SortedPlayers.begin(); i != SortedPlayers.end(); i++)
        {
            Pings += (*i)->GetNameTerminated();
            Pings += ": ";

            if ((*i)->GetNumPings() > 0)
            {
                Pings += UTIL_ToString((*i)->GetPing(m_GHost->m_LCPings));
                Pings += "ms";
            }
            else
                Pings += "N/A";

            if (i != SortedPlayers.end() - 1)
                Pings += ", ";

            if ((m_GameLoading || m_GameLoaded) && Pings.size() > 100)
            {
                // cut the text into multiple lines ingame

                SendAllChat(Pings);
                Pings.clear();
            }
        }

        if (!Pings.empty())
            SendAllChat(Pings);
    }

    //!DESYNCCHECK
    if (Command == "desynccheck" || Command == "checkdesync" || Command == "cd" || Command == "dc")
    {
        HideCommand = true;
        SendChat(player, "Desync kick: " + std::string(m_GHost->m_DesyncKick ? "ON (will kick on desync)" : "OFF (will not kick on desync)"));
    }

    //!HANDICAPCHECK

    if (Command == "handicapcheck" || Command == "handicapc" || Command == "hc" || Command == "checkhandicap" || Command == "ch")
    {
        HideCommand = true;

        for (std::size_t i = 1; i <= m_Slots.size(); i++)
        {
            if (i == 12)
            {
                SendChat(player, "MORIYA: [" + UTIL_ToString(m_Slots[i - 1].GetHandicap()) + "]");
                break;
            }
            if (i == 6)
            {
                SendChat(player, "HAKUREI: [" + UTIL_ToString(m_Slots[i - 1].GetHandicap()) + "]");
                SendChat(player, "--------------------");
                continue;
            }

            std::string slotName = "";
            switch (m_Slots[i - 1].GetSlotStatus())
            {
            case (SLOTSTATUS_OCCUPIED):
                if (m_Slots[i - 1].GetComputer() == 0)
                    slotName = GetPlayerFromSID(i - 1)->GetName();
                else
                    slotName = "COMP";
                break;
            case (SLOTSTATUS_OPEN):
                slotName = "OPEN";
                break;
            case (SLOTSTATUS_CLOSED):
                slotName = "CLOSED";
                break;
            default:
                slotName = "??????????";
            }

            SendChat(player, UTIL_ToString(i) + ". " + slotName + ": [" + UTIL_ToString(m_Slots[i - 1].GetHandicap()) + "]");
        }
    }

    //
    // !STATS
    //

    if (Command == "stats" && GetTime() - player->GetStatsSentTime() >= 5)
    {
        std::string StatsUser = User;

        if (!Payload.empty())
            StatsUser = Payload;

        if (player->GetSpoofed() && (AdminCheck || RootAdminCheck || IsOwner(User)))
            m_PairedGPSChecks.push_back(PairedGPSCheck(std::string(), m_GHost->m_DB->ThreadedGamePlayerSummaryCheck(StatsUser)));
        else
            m_PairedGPSChecks.push_back(PairedGPSCheck(User, m_GHost->m_DB->ThreadedGamePlayerSummaryCheck(StatsUser)));

        player->SetStatsSentTime(GetTime());
    }

    //
    // !STATSDOTA
    //

    if (Command == "statsdota" && GetTime() - player->GetStatsDotASentTime() >= 5)
    {
        std::string StatsUser = User;

        if (!Payload.empty())
            StatsUser = Payload;

        if (player->GetSpoofed() && (AdminCheck || RootAdminCheck || IsOwner(User)))
            m_PairedDPSChecks.push_back(PairedDPSCheck(std::string(), m_GHost->m_DB->ThreadedDotAPlayerSummaryCheck(StatsUser)));
        else
            m_PairedDPSChecks.push_back(PairedDPSCheck(User, m_GHost->m_DB->ThreadedDotAPlayerSummaryCheck(StatsUser)));

        player->SetStatsDotASentTime(GetTime());
    }

    //
    // !VERSION
    //

    if (Command == "version")
    {
        if (player->GetSpoofed() && (AdminCheck || RootAdminCheck || IsOwner(User)))
            SendChat(player, m_GHost->m_Language->VersionAdmin(m_GHost->m_Version));
        else
            SendChat(player, m_GHost->m_Language->VersionNotAdmin(m_GHost->m_Version));
    }

    //
    // !VOTEKICK
    //

    if (Command == "votekick" && m_GHost->m_VoteKickAllowed && !Payload.empty())
    {
        if (!m_KickVotePlayer.empty())
            SendChat(player, m_GHost->m_Language->UnableToVoteKickAlreadyInProgress());
        else if (m_Players.size() == 2)
            SendChat(player, m_GHost->m_Language->UnableToVoteKickNotEnoughPlayers());
        else
        {
            CGamePlayer *LastMatch = NULL;
            uint32_t Matches       = GetPlayerFromNamePartial(Payload, &LastMatch);

            if (Matches == 0)
                SendChat(player, m_GHost->m_Language->UnableToVoteKickNoMatchesFound(Payload));
            else if (Matches == 1)
            {
                if (LastMatch->GetReserved())
                    SendChat(player, m_GHost->m_Language->UnableToVoteKickPlayerIsReserved(LastMatch->GetName()));
                else
                {
                    m_KickVotePlayer      = LastMatch->GetName();
                    m_StartedKickVoteTime = GetTime();

                    for (std::vector<CGamePlayer *>::iterator i = m_Players.begin(); i != m_Players.end(); i++)
                        (*i)->SetKickVote(false);

                    player->SetKickVote(true);
                    CONSOLE_Print("[GAME: " + m_GameName + "] votekick against player [" + m_KickVotePlayer + "] started by player [" + User + "]");
                    SendAllChat(m_GHost->m_Language->StartedVoteKick(LastMatch->GetName(), User, UTIL_ToString((uint32_t)ceil((GetNumHumanPlayers() - 1) * (float)m_GHost->m_VoteKickPercentage / 100) - 1)));
                    SendAllChat(m_GHost->m_Language->TypeYesToVote(std::string(1, m_GHost->m_CommandTrigger)));
                }
            }
            else
                SendChat(player, m_GHost->m_Language->UnableToVoteKickFoundMoreThanOneMatch(Payload));
        }
    }

    //
    //AUTH
    //

    if (Command == "dots")
    {
        HideCommand = true;
        player->SetAuthenticated(true);
        CONSOLE_Print("[GAME: " + m_GameName + "][" + User + "] has been authenticated");
        SendAllChat("[" + User + "] has been authenticated");
    }

    //
    //CHECKAUTH
    //

    if (Command == "ca")
    {
        HideCommand = true;
        if (player->GetAuthenticated())
            SendChat(player, "[" + User + "] true");
        else
            SendChat(player, "[" + User + "] false");
    }

    //
    // !YES
    //

    if (Command == "yes" && !m_KickVotePlayer.empty() && player->GetName() != m_KickVotePlayer && !player->GetKickVote())
    {
        player->SetKickVote(true);
        uint32_t VotesNeeded = (uint32_t)ceil((GetNumHumanPlayers() - 1) * (float)m_GHost->m_VoteKickPercentage / 100);
        uint32_t Votes       = 0;

        for (std::vector<CGamePlayer *>::iterator i = m_Players.begin(); i != m_Players.end(); i++)
        {
            if ((*i)->GetKickVote())
                Votes++;
        }

        if (Votes >= VotesNeeded)
        {
            CGamePlayer *Victim = GetPlayerFromName(m_KickVotePlayer, true);

            if (Victim)
            {
                Victim->SetDeleteMe(true);
                Victim->SetLeftReason(m_GHost->m_Language->WasKickedByVote());

                if (!m_GameLoading && !m_GameLoaded)
                    Victim->SetLeftCode(PLAYERLEAVE_LOBBY);
                else
                    Victim->SetLeftCode(PLAYERLEAVE_LOST);

                if (!m_GameLoading && !m_GameLoaded)
                    OpenSlot(GetSIDFromPID(Victim->GetPID()), false);

                CONSOLE_Print("[GAME: " + m_GameName + "] votekick against player [" + m_KickVotePlayer + "] passed with " + UTIL_ToString(Votes) + "/" + UTIL_ToString(GetNumHumanPlayers()) + " votes");
                SendAllChat(m_GHost->m_Language->VoteKickPassed(m_KickVotePlayer));
            }
            else
                SendAllChat(m_GHost->m_Language->ErrorVoteKickingPlayer(m_KickVotePlayer));

            m_KickVotePlayer.clear();
            m_StartedKickVoteTime = 0;
        }
        else
            SendAllChat(m_GHost->m_Language->VoteKickAcceptedNeedMoreVotes(m_KickVotePlayer, User, UTIL_ToString(VotesNeeded - Votes)));
    }

    return HideCommand;
}

void CGame ::EventGameStarted()
{
    CBaseGame ::EventGameStarted();

    // record everything we need to ban each player in case we decide to do so later
    // this is because when a player leaves the game an admin might want to ban that player
    // but since the player has already left the game we don't have access to their information anymore
    // so we create a "potential ban" for each player and only store it in the database if requested to by an admin

    for (std::vector<CGamePlayer *>::iterator i = m_Players.begin(); i != m_Players.end(); i++)
        m_DBBans.push_back(new CDBBan((*i)->GetJoinedRealm(), (*i)->GetName(), (*i)->GetExternalIPString(), std::string(), std::string(), std::string(), std::string()));
}

bool CGame ::IsGameDataSaved()
{
    return m_CallableGameAdd && m_CallableGameAdd->GetReady();
}

void CGame ::SaveGameData()
{
    CONSOLE_Print("[GAME: " + m_GameName + "] saving game data to database");
    m_CallableGameAdd = m_GHost->m_DB->ThreadedGameAdd(m_GHost->m_BNETs.size() == 1 ? m_GHost->m_BNETs[0]->GetServer() : std::string(), m_DBGame->GetMap(), m_GameName, m_OwnerName, m_GameTicks / 1000, m_GameState, m_CreatorName, m_CreatorServer);
}
