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
#if _MSC_VER >= 1932 // Visual Studio 2022 version 17.2+
#    pragma comment(linker, "/alternatename:__imp___std_init_once_complete=__imp_InitOnceComplete")
#    pragma comment(linker, "/alternatename:__imp___std_init_once_begin_initialize=__imp_InitOnceBeginInitialize")
#endif

#include "ghost.h"
#include "bnet.h"
#include "config.h"
#include "crc32.h"
#include "csvparser.h"
#include "game.h"
#include "game_admin.h"
#include "game_base.h"
#include "gameplayer.h"
#include "gameprotocol.h"
#include "gcbiprotocol.h"
#include "ghostdb.h"
#include "ghostdbmysql.h"
#include "ghostdbsqlite.h"
#include "gpsprotocol.h"
#include "language.h"
#include "map.h"
#include "packed.h"
#include "savegame.h"
#include "sha1.h"
#include "socket.h"
#include "statusbroadcaster.h"
#include "userinterface.h"
#include "util.h"

#include <signal.h>
#include <cstdlib>

#ifdef WIN32
#include <ws2tcpip.h> // for WSAIoctl
#endif

#define __STORMLIB_SELF__
#include <StormLib.h>

#include <time.h>

#ifndef WIN32
#include <sys/time.h>
#endif

std::string gCFGFile;
std::string gLogFile;
uint32_t gLogMethod;
std::ofstream *gLog = NULL;
CGHost *gGHost      = NULL;
CCurses *gCurses    = NULL;

uint32_t GetTime()
{
    return GetTicks() / 1000;
}

uint32_t GetTicks()
{
#ifdef WIN32
    // don't use GetTickCount anymore because it's not accurate enough (~16ms resolution)
    // don't use QueryPerformanceCounter anymore because it isn't guaranteed to be strictly increasing on some systems and thus requires "smoothing" code
    // use timeGetTime instead, which typically has a high resolution (5ms or more) but we request a lower resolution on startup

    return timeGetTime();
#else
    uint32_t ticks;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    ticks = t.tv_sec * 1000;
    ticks += t.tv_nsec / 1000000;
    return ticks;
#endif
}

void SignalCatcher2(int s)
{
    CONSOLE_Print("[!!!] caught signal " + UTIL_ToString(s) + ", exiting NOW");

    if (gGHost)
    {
        if (gGHost->m_Exiting)
            exit(1);
        else
            gGHost->m_Exiting = true;
    }
    else
        exit(1);
}

void SignalCatcher(int s)
{
    // signal( SIGABRT, SignalCatcher2 );
    signal(SIGINT, SignalCatcher2);

    CONSOLE_Print("[!!!] caught signal " + UTIL_ToString(s) + ", exiting nicely");

    if (gGHost)
        gGHost->m_ExitingNice = true;
    else
        exit(1);
}

void CONSOLE_Print(std::string message)
{
    CONSOLE_Print(message, 0, true);
}

void CONSOLE_Print(std::string message, bool toMainBuffer)
{
    CONSOLE_Print(message, 0, toMainBuffer);
}

void CONSOLE_Print(std::string message, uint32_t realmId, bool toMainBuffer)
{
    if (gCurses)
        gCurses->Print(message, realmId, toMainBuffer);
    else
    {
        std::cout << message << std::endl;
    }

    // logging

    if (!gLogFile.empty())
    {
        if (gLogMethod == 1)
        {
            std::ofstream Log;
            Log.open(gLogFile.c_str(), std::ios::app);

            if (!Log.fail())
            {
                time_t Now       = time(NULL);
                std::string Time = asctime(localtime(&Now));

                // erase the newline

                Time.erase(Time.size() - 1);
                Log << "[" << Time << "] " << message << std::endl;
                Log.close();
            }
        }
        else if (gLogMethod == 2)
        {
            if (gLog && !gLog->fail())
            {
                time_t Now       = time(NULL);
                std::string Time = asctime(localtime(&Now));

                // erase the newline

                Time.erase(Time.size() - 1);
                *gLog << "[" << Time << "] " << message << std::endl;
                gLog->flush();
            }
        }
    }
}

void DEBUG_Print(std::string message)
{
    std::cout << message << std::endl;
}

void DEBUG_Print(BYTEARRAY b)
{
    std::cout << "{ ";

    for (unsigned int i = 0; i < b.size(); i++)
        std::cout << std::hex << (int)b[i] << " ";

    std::cout << "}" << std::endl;
}

void CONSOLE_ChangeChannel(std::string channel, uint32_t realmId)
{
    if (gCurses)
        gCurses->ChangeChannel(channel, realmId);
}

void CONSOLE_AddChannelUser(std::string name, uint32_t realmId, int flag)
{
    if (gCurses)
        gCurses->AddChannelUser(name, realmId, flag);
}

void CONSOLE_UpdateChannelUser(std::string name, uint32_t realmId, int flag)
{
    if (gCurses)
        gCurses->UpdateChannelUser(name, realmId, flag);
}

void CONSOLE_RemoveChannelUser(std::string name, uint32_t realmId)
{
    if (gCurses)
        gCurses->RemoveChannelUser(name, realmId);
}

void CONSOLE_RemoveChannelUsers(uint32_t realmId)
{
    if (gCurses)
        gCurses->RemoveChannelUsers(realmId);
}

//
// main
//

int main(int argc, char **argv)
{   
    /*
    std::ofstream StatusFile;
    StatusFile.open("online.txt", std::ios::trunc);
    if (!StatusFile.fail())
    {
        StatusFile << "0" << std::endl;
        StatusFile << false << std::endl;
        StatusFile << false << std::endl;
        StatusFile << true << std::endl;
        StatusFile.close();
    }
    */

    gCFGFile = "ghost.cfg";

    if (argc > 1 && argv[1])
        gCFGFile = argv[1];

    // read config file

    CConfig CFG;
    CFG.Read("default.cfg");
    CFG.Read(gCFGFile);
    gLogFile   = CFG.GetString("bot_log", std::string());
    gLogMethod = CFG.GetInt("bot_logmethod", 1);

    if (CFG.GetInt("curses_enabled", 1) == 1)
        gCurses = new CCurses(CFG.GetInt("term_width", 0), CFG.GetInt("term_height", 0), !!CFG.GetInt("curses_splitview", 0), CFG.GetInt("curses_listtype", 0));

    UTIL_Construct_UTF8_Latin1_Map();

    if (!gLogFile.empty())
    {
        if (gLogMethod == 1)
        {
            // log method 1: open, append, and close the log for every message
            // this works well on Linux but poorly on Windows, particularly as the log file grows in size
            // the log file can be edited/moved/deleted while GHost++ is running
        }
        else if (gLogMethod == 2)
        {
            // log method 2: open the log on startup, flush the log for every message, close the log on shutdown
            // the log file CANNOT be edited/moved/deleted while GHost++ is running

            gLog = new std::ofstream();
            gLog->open(gLogFile.c_str(), std::ios::app);
        }
    }

    CONSOLE_Print("[GHOST] starting up");

    if (!gLogFile.empty())
    {
        if (gLogMethod == 1)
            CONSOLE_Print("[GHOST] using log method 1, logging is enabled and [" + gLogFile + "] will not be locked");
        else if (gLogMethod == 2)
        {
            if (gLog->fail())
                CONSOLE_Print("[GHOST] using log method 2 but unable to open [" + gLogFile + "] for appending, logging is disabled");
            else
                CONSOLE_Print("[GHOST] using log method 2, logging is enabled and [" + gLogFile + "] is now locked");
        }
    }
    else
        CONSOLE_Print("[GHOST] no log file specified, logging is disabled");

    // catch SIGABRT and SIGINT

    // signal( SIGABRT, SignalCatcher );
    signal(SIGINT, SignalCatcher);

#ifndef WIN32
    // disable SIGPIPE since some systems like OS X don't define MSG_NOSIGNAL

    signal(SIGPIPE, SIG_IGN);
#endif

#ifdef WIN32
    // initialize timer resolution
    // attempt to set the resolution as low as possible from 1ms to 5ms

    unsigned int TimerResolution = 0;

    for (unsigned int i = 1; i <= 5; i++)
    {
        if (timeBeginPeriod(i) == TIMERR_NOERROR)
        {
            TimerResolution = i;
            break;
        }
        else if (i < 5)
            CONSOLE_Print("[GHOST] error setting Windows timer resolution to " + UTIL_ToString(i) + " milliseconds, trying a higher resolution");
        else
        {
            CONSOLE_Print("[GHOST] error setting Windows timer resolution");
            return 1;
        }
    }

    CONSOLE_Print("[GHOST] using Windows timer with resolution " + UTIL_ToString(TimerResolution) + " milliseconds");
#else
    // print the timer resolution

    struct timespec Resolution;

    if (clock_getres(CLOCK_MONOTONIC, &Resolution) == -1)
        CONSOLE_Print("[GHOST] error getting monotonic timer resolution");
    else
        CONSOLE_Print("[GHOST] using monotonic timer with resolution " + UTIL_ToString((double)(Resolution.tv_nsec / 1000), 2) + " microseconds");
#endif

#ifdef WIN32
    // initialize winsock

    CONSOLE_Print("[GHOST] starting winsock");
    WSADATA wsadata;

    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0)
    {
        CONSOLE_Print("[GHOST] error starting winsock");
        return 1;
    }

    // increase process priority

    CONSOLE_Print("[GHOST] setting process priority to \"above normal\"");
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
#endif

    // initialize ghost

    gGHost = new CGHost(&CFG);

    if (gCurses)
        gCurses->SetGHost(gGHost);

    while (1)
    {
        // block for 50ms on all sockets - if you intend to perform any timed actions more frequently you should change this
        // that said it's likely we'll loop more often than this due to there being data waiting on one of the sockets but there aren't any guarantees

        if (gGHost->Update(50000) || (gCurses && gCurses->Update()))
            break;
    }

    // shutdown ghost

    CONSOLE_Print("[GHOST] shutting down");
    delete gGHost;
    gGHost = NULL;

#ifdef WIN32
    // shutdown winsock

    CONSOLE_Print("[GHOST] shutting down winsock");
    WSACleanup();

    // shutdown timer

    timeEndPeriod(TimerResolution);
#endif

    if (gLog)
    {
        if (!gLog->fail())
            gLog->close();

        delete gLog;
    }

    // shutdown curses

    if (gCurses)
    {
        CONSOLE_Print("[GHOST] shutting down curses");
        delete gCurses;
        gCurses = NULL;
    }

    return 0;
}

//
// CGHost
//

CGHost::CGHost(CConfig *CFG)
{
    m_UDPSocket = new CUDPSocket();
    m_UDPSocket->SetBroadcastTarget(CFG->GetString("udp_broadcasttarget", std::string()));
    m_UDPSocket->SetDontRoute(CFG->GetInt("udp_dontroute", 0) == 0 ? false : true);
    m_ReconnectSocket = NULL;
    m_GPSProtocol     = new CGPSProtocol();
    m_GCBIProtocol    = new CGCBIProtocol();
    m_CRC             = new CCRC32();
    m_CRC->Initialize();
    m_SHA              = new CSHA1();
    m_CurrentGame      = NULL;
    std::string DBType = CFG->GetString("db_type", "sqlite3");
    CONSOLE_Print("[GHOST] opening primary database");

    if (DBType == "mysql")
    {
#ifdef GHOST_MYSQL
        m_DB = new CGHostDBMySQL(CFG);
#else
        CONSOLE_Print("[GHOST] warning - this binary was not compiled with MySQL database support, using SQLite database instead");
        m_DB = new CGHostDBSQLite(CFG);
#endif
    }
    else
        m_DB = new CGHostDBSQLite(CFG);

    CONSOLE_Print("[GHOST] opening secondary (local) database");
    m_DBLocal = new CGHostDBSQLite(CFG);

    // get a list of local IP addresses
    // this list is used elsewhere to determine if a player connecting to the bot is local or not

    CONSOLE_Print("[GHOST] attempting to find local IP addresses");

#ifdef WIN32
    // use a more reliable Windows specific method since the portable method doesn't always work properly on Windows
    // code stolen from: http://tangentsoft.net/wskfaq/examples/getifaces.html

    SOCKET sd = WSASocket(AF_INET, SOCK_DGRAM, 0, 0, 0, 0);

    if (sd == (unsigned int)SOCKET_ERROR)
        CONSOLE_Print("[GHOST] error finding local IP addresses - failed to create socket (error code " + UTIL_ToString(WSAGetLastError()) + ")");
    else
    {
        INTERFACE_INFO InterfaceList[20];
        unsigned long nBytesReturned;

        if (WSAIoctl(sd, SIO_GET_INTERFACE_LIST, 0, 0, &InterfaceList, sizeof(InterfaceList), &nBytesReturned, 0, 0) == SOCKET_ERROR)
            CONSOLE_Print("[GHOST] error finding local IP addresses - WSAIoctl failed (error code " + UTIL_ToString(WSAGetLastError()) + ")");
        else
        {
            int nNumInterfaces = nBytesReturned / sizeof(INTERFACE_INFO);

            for (int i = 0; i < nNumInterfaces; i++)
            {
                sockaddr_in *pAddress;
                pAddress = (sockaddr_in *)&(InterfaceList[i].iiAddress);
                CONSOLE_Print("[GHOST] local IP address #" + UTIL_ToString(i + 1) + " is [" + std::string(inet_ntoa(pAddress->sin_addr)) + "]");
                m_LocalAddresses.push_back(UTIL_CreateByteArray((uint32_t)pAddress->sin_addr.s_addr, false));
            }
        }

        closesocket(sd);
    }
#else
    // use a portable method

    char HostName[255];

    if (gethostname(HostName, 255) == SOCKET_ERROR)
        CONSOLE_Print("[GHOST] error finding local IP addresses - failed to get local hostname");
    else
    {
        CONSOLE_Print("[GHOST] local hostname is [" + std::string(HostName) + "]");
        struct hostent *HostEnt = gethostbyname(HostName);

        if (!HostEnt)
            CONSOLE_Print("[GHOST] error finding local IP addresses - gethostbyname failed");
        else
        {
            for (int i = 0; HostEnt->h_addr_list[i] != NULL; i++)
            {
                struct in_addr Address;
                memcpy(&Address, HostEnt->h_addr_list[i], sizeof(struct in_addr));
                CONSOLE_Print("[GHOST] local IP address #" + UTIL_ToString(i + 1) + " is [" + std::string(inet_ntoa(Address)) + "]");
                m_LocalAddresses.push_back(UTIL_CreateByteArray((uint32_t)Address.s_addr, false));
            }
        }
    }
#endif

    m_Language                 = NULL;
    m_Exiting                  = false;
    m_ExitingNice              = false;
    m_Enabled                  = true;
    m_Version                  = "17.1";
    m_HostCounter              = 1;
    m_AutoHostMaximumGames     = CFG->GetInt("autohost_maxgames", 0);
    m_AutoHostAutoStartPlayers = CFG->GetInt("autohost_startplayers", 0);
    m_AutoHostGameName         = CFG->GetString("autohost_gamename", std::string());
    m_AutoHostOwner            = CFG->GetString("autohost_owner", std::string());
    m_LastAutoHostTime         = GetTime() - m_RehostDelay + 15;
    m_AutoHostMatchMaking      = false;
    m_AutoHostMinimumScore     = 0.0;
    m_AutoHostMaximumScore     = 0.0;
    m_AllGamesFinished         = false;
    m_AllGamesFinishedTime     = 0;
    m_TFT                      = CFG->GetInt("bot_tft", 1) == 0 ? false : true;

    if (m_TFT)
        CONSOLE_Print("[GHOST] acting as Warcraft III: The Frozen Throne");
    else
        CONSOLE_Print("[GHOST] acting as Warcraft III: Reign of Chaos");

    m_HostPort      = CFG->GetInt("bot_hostport", 6112);
    m_Reconnect     = CFG->GetInt("bot_reconnect", 1) == 0 ? false : true;
    m_ReconnectPort = CFG->GetInt("bot_reconnectport", 6114);

    m_TCPStatus = CFG->GetInt("bot_tcpstatus", 1) == 0 ? false : true;
    ;

    m_DefaultMap        = CFG->GetString("bot_defaultmap", "map");
    m_AdminGameCreate   = CFG->GetInt("admingame_create", 0) == 0 ? false : true;
    m_AdminGamePort     = CFG->GetInt("admingame_port", 6113);
    m_AdminGamePassword = CFG->GetString("admingame_password", std::string());
    m_AdminGameMap      = CFG->GetString("admingame_map", std::string());
    m_LANWar3Version    = CFG->GetInt("lan_war3version", 24);
    m_ReplayWar3Version = CFG->GetInt("replay_war3version", 24);
    m_ReplayBuildNumber = CFG->GetInt("replay_buildnumber", 6059);
    SetConfigs(CFG);

    // load the battle.net connections
    // we're just loading the config data and creating the CBNET classes here, the connections are established later (in the Update function)

    for (uint32_t i = 1; i < 10; i++)
    {
        std::string Prefix;

        if (i == 1)
            Prefix = "bnet_";
        else
            Prefix = "bnet" + UTIL_ToString(i) + "_";

        std::string Server        = CFG->GetString(Prefix + "server", std::string());
        std::string ServerAlias   = CFG->GetString(Prefix + "serveralias", std::string());
        std::string CDKeyROC      = CFG->GetString(Prefix + "cdkeyroc", std::string());
        std::string CDKeyTFT      = CFG->GetString(Prefix + "cdkeytft", std::string());
        std::string CountryAbbrev = CFG->GetString(Prefix + "countryabbrev", "USA");
        std::string Country       = CFG->GetString(Prefix + "country", "United States");
        std::string Locale        = CFG->GetString(Prefix + "locale", "system");
        uint32_t LocaleID;

        if (Locale == "system")
        {
#ifdef WIN32
            LocaleID = GetUserDefaultLangID();
#else
            LocaleID = 1033;
#endif
        }
        else
            LocaleID = UTIL_ToUInt32(Locale);

        std::string UserName           = CFG->GetString(Prefix + "username", std::string());
        std::string UserPassword       = CFG->GetString(Prefix + "password", std::string());
        std::string FirstChannel       = CFG->GetString(Prefix + "firstchannel", "The Void");
        std::string RootAdmin          = CFG->GetString(Prefix + "rootadmin", std::string());
        std::string BNETCommandTrigger = CFG->GetString(Prefix + "commandtrigger", "!");

        if (BNETCommandTrigger.empty())
            BNETCommandTrigger = "!";

        bool HoldFriends             = CFG->GetInt(Prefix + "holdfriends", 1) == 0 ? false : true;
        bool HoldClan                = CFG->GetInt(Prefix + "holdclan", 1) == 0 ? false : true;
        bool PublicCommands          = CFG->GetInt(Prefix + "publiccommands", 1) == 0 ? false : true;
        std::string BNLSServer       = CFG->GetString(Prefix + "bnlsserver", std::string());
        int BNLSPort                 = CFG->GetInt(Prefix + "bnlsport", 9367);
        int BNLSWardenCookie         = CFG->GetInt(Prefix + "bnlswardencookie", 0);
        unsigned char War3Version    = CFG->GetInt(Prefix + "custom_war3version", 24);
        BYTEARRAY EXEVersion         = UTIL_ExtractNumbers(CFG->GetString(Prefix + "custom_exeversion", std::string()), 4);
        BYTEARRAY EXEVersionHash     = UTIL_ExtractNumbers(CFG->GetString(Prefix + "custom_exeversionhash", std::string()), 4);
        std::string PasswordHashType = CFG->GetString(Prefix + "custom_passwordhashtype", std::string());
        std::string PVPGNRealmName   = CFG->GetString(Prefix + "custom_pvpgnrealmname", "PvPGN Realm");
        uint32_t MaxMessageLength    = CFG->GetInt(Prefix + "custom_maxmessagelength", 200);

        if (Server.empty())
            break;

        if (CDKeyROC.empty())
        {
            CONSOLE_Print("[GHOST] missing " + Prefix + "cdkeyroc, skipping this battle.net connection");
            continue;
        }

        if (m_TFT && CDKeyTFT.empty())
        {
            CONSOLE_Print("[GHOST] missing " + Prefix + "cdkeytft, skipping this battle.net connection");
            continue;
        }

        if (UserName.empty())
        {
            CONSOLE_Print("[GHOST] missing " + Prefix + "username, skipping this battle.net connection");
            continue;
        }

        if (UserPassword.empty())
        {
            CONSOLE_Print("[GHOST] missing " + Prefix + "password, skipping this battle.net connection");
            continue;
        }

        CONSOLE_Print("[GHOST] found battle.net connection #" + UTIL_ToString(i) + " for server [" + Server + "]");

        if (Locale == "system")
        {
#ifdef WIN32
            CONSOLE_Print("[GHOST] using system locale of " + UTIL_ToString(LocaleID));
#else
            CONSOLE_Print("[GHOST] unable to get system locale, using default locale of 1033");
#endif
        }

#ifdef __PDCURSES__
        PDC_set_title(std::string("GHost++ | " + UserName + " | " + m_AutoHostGameName).c_str());
#endif

        m_BNETs.push_back(new CBNET(this, Server, ServerAlias, BNLSServer, (uint16_t)BNLSPort, (uint32_t)BNLSWardenCookie, CDKeyROC, CDKeyTFT, CountryAbbrev, Country, LocaleID, UserName, UserPassword, FirstChannel, RootAdmin, m_LANRootAdmin, BNETCommandTrigger[0], HoldFriends, HoldClan, PublicCommands, War3Version, EXEVersion, EXEVersionHash, PasswordHashType, PVPGNRealmName, MaxMessageLength, i));
    }

    if (m_BNETs.empty())
        CONSOLE_Print("[GHOST] warning - no battle.net connections found in config file");

    // extract common.j and blizzard.j from War3Patch.mpq if we can
    // these two files are necessary for calculating "map_crc" when loading maps so we make sure to do it before loading the default map
    // see CMap:: Load for more information

    ExtractScripts();

    // load the default maps (note: make sure to run ExtractScripts first)

    if (m_DefaultMap.size() < 4 || m_DefaultMap.substr(m_DefaultMap.size() - 4) != ".cfg")
    {
        m_DefaultMap += ".cfg";
        CONSOLE_Print("[GHOST] adding \".cfg\" to default map -> new default is [" + m_DefaultMap + "]");
    }

    CConfig MapCFG;
    MapCFG.Read(m_MapCFGPath + m_DefaultMap);
    m_Map = new CMap(this, &MapCFG, m_MapCFGPath + m_DefaultMap);

    m_StatusBroadcaster             = new CStatusBroadcaster(CFG->GetString("autohost_gamename", std::string()));
    m_StatusBroadcaster->hostPort   = m_HostPort;
    m_StatusBroadcaster->statusPort = CFG->GetInt("bot_statusport", 6150);
    m_StatusBroadcaster->slotsNum   = m_Map->GetSlots().size();
    m_StatusBroadcaster->statString = CalcStatString(m_Map);

    if (!m_AdminGameMap.empty())
    {
        if (m_AdminGameMap.size() < 4 || m_AdminGameMap.substr(m_AdminGameMap.size() - 4) != ".cfg")
        {
            m_AdminGameMap += ".cfg";
            CONSOLE_Print("[GHOST] adding \".cfg\" to default admin game map -> new default is [" + m_AdminGameMap + "]");
        }

        CONSOLE_Print("[GHOST] trying to load default admin game map");
        CConfig AdminMapCFG;
        AdminMapCFG.Read(m_MapCFGPath + m_AdminGameMap);
        m_AdminMap = new CMap(this, &AdminMapCFG, m_MapCFGPath + m_AdminGameMap);

        if (!m_AdminMap->GetValid())
        {
            CONSOLE_Print("[GHOST] default admin game map isn't valid, using hardcoded admin game map instead");
            delete m_AdminMap;
            m_AdminMap = new CMap(this);
        }
    }
    else
    {
        CONSOLE_Print("[GHOST] using hardcoded admin game map");
        m_AdminMap = new CMap(this);
    }

    m_AutoHostMap = new CMap(*m_Map);
    m_SaveGame    = new CSaveGame();

    // load the iptocountry data

    LoadIPToCountryData();

    // create the admin game

    if (m_AdminGameCreate)
    {
        CONSOLE_Print("[GHOST] creating admin game");
        m_AdminGame = new CAdminGame(this, m_AdminMap, NULL, m_AdminGamePort, 0, "GHost++ Admin Game", m_AdminGamePassword);

        if (m_AdminGamePort == m_HostPort)
            CONSOLE_Print("[GHOST] warning - admingame_port and bot_hostport are set to the same value, you won't be able to host any games");
    }
    else
        m_AdminGame = NULL;

    if (m_BNETs.empty() && !m_AdminGame)
        CONSOLE_Print("[GHOST] warning - no battle.net connections found and no admin game created");

#ifdef GHOST_MYSQL
    CONSOLE_Print("[GHOST] GHost++ Version " + m_Version + " (with MySQL support)");
#else
    CONSOLE_Print("[GHOST] GHost++ Version " + m_Version + " (without MySQL support)");
#endif
}

CGHost::~CGHost()
{
    delete m_UDPSocket;
    delete m_ReconnectSocket;
    delete m_StatusBroadcaster;

    for (std::vector<CTCPSocket *>::iterator i = m_ReconnectSockets.begin(); i != m_ReconnectSockets.end(); i++)
        delete *i;

    delete m_GPSProtocol;
    delete m_GCBIProtocol;
    delete m_CRC;
    delete m_SHA;

    for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
        delete *i;

    delete m_CurrentGame;
    delete m_AdminGame;

    for (std::vector<CBaseGame *>::iterator i = m_Games.begin(); i != m_Games.end(); i++)
        delete *i;

    delete m_DB;
    delete m_DBLocal;

    // warning: we don't delete any entries of m_Callables here because we can't be guaranteed that the associated threads have terminated
    // this is fine if the program is currently exiting because the OS will clean up after us
    // but if you try to recreate the CGHost object within a single session you will probably leak resources!

    if (!m_Callables.empty())
        CONSOLE_Print("[GHOST] warning - " + UTIL_ToString(m_Callables.size()) + " orphaned callables were leaked (this is not an error)");

    delete m_Language;
    delete m_Map;
    delete m_AdminMap;
    delete m_AutoHostMap;
    delete m_SaveGame;
}

bool CGHost::Update(uint32_t usecBlock)
{
    // todotodo: do we really want to shutdown if there's a database error? is there any way to recover from this?

    if (m_DB->HasError())
    {
        CONSOLE_Print("[GHOST] database error - " + m_DB->GetError());
        return true;
    }

    if (m_DBLocal->HasError())
    {
        CONSOLE_Print("[GHOST] local database error - " + m_DBLocal->GetError());
        return true;
    }

    // try to exit nicely if requested to do so

    if (m_ExitingNice)
    {
        if (!m_BNETs.empty())
        {
            CONSOLE_Print("[GHOST] deleting all battle.net connections in preparation for exiting nicely");

            for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
                delete *i;

            m_BNETs.clear();
        }

        if (m_CurrentGame)
        {
            CONSOLE_Print("[GHOST] deleting current game in preparation for exiting nicely");
            delete m_CurrentGame;
            m_CurrentGame = NULL;
        }

        if (m_AdminGame)
        {
            CONSOLE_Print("[GHOST] deleting admin game in preparation for exiting nicely");
            delete m_AdminGame;
            m_AdminGame = NULL;
        }

        if (m_Games.empty())
        {
            if (!m_AllGamesFinished)
            {
                CONSOLE_Print("[GHOST] all games finished, waiting 60 seconds for threads to finish");
                CONSOLE_Print("[GHOST] there are " + UTIL_ToString(m_Callables.size()) + " threads in progress");
                m_AllGamesFinished     = true;
                m_AllGamesFinishedTime = GetTime();
            }
            else
            {
                if (m_Callables.empty())
                {
                    CONSOLE_Print("[GHOST] all threads finished, exiting nicely");
                    m_Exiting = true;
                }
                else if (GetTime() - m_AllGamesFinishedTime >= 60)
                {
                    CONSOLE_Print("[GHOST] waited 60 seconds for threads to finish, exiting anyway");
                    CONSOLE_Print("[GHOST] there are " + UTIL_ToString(m_Callables.size()) + " threads still in progress which will be terminated");
                    m_Exiting = true;
                }
            }
        }
    }

    // update callables

    for (std::vector<CBaseCallable *>::iterator i = m_Callables.begin(); i != m_Callables.end();)
    {
        if ((*i)->GetReady())
        {
            m_DB->RecoverCallable(*i);
            delete *i;
            i = m_Callables.erase(i);
        }
        else
            i++;
    }

    // создание сокета для обновления статуса
    if (m_TCPStatus)
    {
        if (!m_StatusBroadcaster->connectSocket)
        {

            if (!m_StatusBroadcaster->StartListen(m_BindAddress)) //запускаем сокет
            {
                delete m_StatusBroadcaster; //если не стартанул сокет
                m_StatusBroadcaster = NULL;
                m_TCPStatus         = false;
            }
        }
        else if (m_StatusBroadcaster->connectSocket->HasError())
        {
            CONSOLE_Print("[GHOST][STATUS] Gproxy status listener error (" + m_StatusBroadcaster->connectSocket->GetErrorString() + ")");
            delete m_StatusBroadcaster->connectSocket;
            m_StatusBroadcaster->connectSocket = NULL;
            m_TCPStatus                        = false;
        }
    }

    // create the GProxy++ reconnect listener

    if (m_Reconnect)
    {
        if (!m_ReconnectSocket)
        {
            m_ReconnectSocket = new CTCPServer();

            if (m_ReconnectSocket->Listen(m_BindAddress, m_ReconnectPort))
                CONSOLE_Print("[GHOST] listening for GProxy++ reconnects on port " + UTIL_ToString(m_ReconnectPort));
            else
            {
                CONSOLE_Print("[GHOST] error listening for GProxy++ reconnects on port " + UTIL_ToString(m_ReconnectPort));
                delete m_ReconnectSocket;
                m_ReconnectSocket = NULL;
                m_Reconnect       = false;
            }
        }
        else if (m_ReconnectSocket->HasError())
        {
            CONSOLE_Print("[GHOST] GProxy++ reconnect listener error (" + m_ReconnectSocket->GetErrorString() + ")");
            delete m_ReconnectSocket;
            m_ReconnectSocket = NULL;
            m_Reconnect       = false;
        }
    }

    unsigned int NumFDs = 0;

    // take every socket we own and throw it in one giant select statement so we can block on all sockets

    int nfds = 0;
    fd_set fd;
    fd_set send_fd;
    FD_ZERO(&fd);
    FD_ZERO(&send_fd);

    // 1. all battle.net sockets

    for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
        NumFDs += (*i)->SetFD(&fd, &send_fd, &nfds);

    // 2. the current game's server and player sockets

    if (m_CurrentGame)
        NumFDs += m_CurrentGame->SetFD(&fd, &send_fd, &nfds);

    // 3. the admin game's server and player sockets

    if (m_AdminGame)
        NumFDs += m_AdminGame->SetFD(&fd, &send_fd, &nfds);

    // 4. all running games' player sockets

    for (std::vector<CBaseGame *>::iterator i = m_Games.begin(); i != m_Games.end(); i++)
        NumFDs += (*i)->SetFD(&fd, &send_fd, &nfds);

    // 5. the GProxy++ reconnect socket(s)

    if (m_Reconnect && m_ReconnectSocket)
    {
        m_ReconnectSocket->SetFD(&fd, &send_fd, &nfds);
        NumFDs++;
    }

    for (std::vector<CTCPSocket *>::iterator i = m_ReconnectSockets.begin(); i != m_ReconnectSockets.end(); i++)
    {
        (*i)->SetFD(&fd, &send_fd, &nfds);
        NumFDs++;
    }

    // 6. Status sockets

    if (m_TCPStatus && m_StatusBroadcaster->connectSocket)
    {
        m_StatusBroadcaster->connectSocket->SetFD(&fd, &send_fd, &nfds);
        NumFDs++;
    }

    for (std::vector<CTCPStatusBroadcasterSocket *>::iterator i = m_StatusBroadcaster->sockets.begin(); i != m_StatusBroadcaster->sockets.end(); i++)
    {
        (*i)->socket->SetFD(&fd, &send_fd, &nfds);
        NumFDs++;
    }

    m_StatusBroadcaster->set_fd(&fd, &send_fd, &nfds);

    // before we call select we need to determine how long to block for
    // previously we just blocked for a maximum of the passed usecBlock microseconds
    // however, in an effort to make game updates happen closer to the desired latency setting we now use a dynamic block interval
    // note: we still use the passed usecBlock as a hard maximum

    for (std::vector<CBaseGame *>::iterator i = m_Games.begin(); i != m_Games.end(); i++)
    {
        if ((*i)->GetNextTimedActionTicks() * 1000 < usecBlock)
            usecBlock = (*i)->GetNextTimedActionTicks() * 1000;
    }

    // always block for at least 1ms just in case something goes wrong
    // this prevents the bot from sucking up all the available CPU if a game keeps asking for immediate updates
    // it's a bit ridiculous to include this check since, in theory, the bot is programmed well enough to never make this mistake
    // however, considering who programmed it, it's worthwhile to do it anyway

    if (usecBlock < 1000)
        usecBlock = 1000;

    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = usecBlock;

    struct timeval send_tv;
    send_tv.tv_sec  = 0;
    send_tv.tv_usec = 0;

#ifdef WIN32
    select(1, &fd, NULL, NULL, &tv);
    select(1, NULL, &send_fd, NULL, &send_tv);
#else
    select(nfds + 1, &fd, NULL, NULL, &tv);
    select(nfds + 1, NULL, &send_fd, NULL, &send_tv);
#endif

    if (NumFDs == 0)
    {
        // we don't have any sockets (i.e. we aren't connected to battle.net maybe due to a lost connection and there aren't any games running)
        // select will return immediately and we'll chew up the CPU if we let it loop so just sleep for 50ms to kill some time

        MILLISLEEP(50);
    }

    bool AdminExit = false;
    bool BNETExit  = false;

    // update current game

    if (m_CurrentGame)
    {
        if (m_CurrentGame->Update(&fd, &send_fd))
        {
            CONSOLE_Print("[GHOST] deleting current game [" + m_CurrentGame->GetGameName() + "]");
            delete m_CurrentGame;
            m_CurrentGame = NULL;

            for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
            {
                (*i)->QueueGameUncreate();
                (*i)->QueueEnterChat();
            }
        }
        else if (m_CurrentGame)
            m_CurrentGame->UpdatePost(&send_fd);
    }

    // update admin game

    if (m_AdminGame)
    {
        if (m_AdminGame->Update(&fd, &send_fd))
        {
            CONSOLE_Print("[GHOST] deleting admin game");
            delete m_AdminGame;
            m_AdminGame = NULL;
            AdminExit   = true;
        }
        else if (m_AdminGame)
            m_AdminGame->UpdatePost(&send_fd);
    }

    // update running games

    for (std::vector<CBaseGame *>::iterator i = m_Games.begin(); i != m_Games.end();)
    {
        if ((*i)->Update(&fd, &send_fd))
        {
            CONSOLE_Print("[GHOST] deleting game [" + (*i)->GetGameName() + "]");
            EventGameDeleted(*i);
            delete *i;
            i = m_Games.erase(i);
        }
        else
        {
            (*i)->UpdatePost(&send_fd);
            i++;
        }
    }

    // update battle.net connections

    for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
    {
        if ((*i)->Update(&fd, &send_fd))
            BNETExit = true;
    }

    // update GProxy++ reliable reconnect sockets

    if (m_Reconnect && m_ReconnectSocket)
    {
        CTCPSocket *NewSocket = m_ReconnectSocket->Accept(&fd);

        if (NewSocket)
            m_ReconnectSockets.push_back(NewSocket);
    }

    for (std::vector<CTCPSocket *>::iterator i = m_ReconnectSockets.begin(); i != m_ReconnectSockets.end();)
    {
        if ((*i)->HasError() || !(*i)->GetConnected() || GetTime() - (*i)->GetLastRecv() >= 10)
        {
            delete *i;
            i = m_ReconnectSockets.erase(i);
            continue;
        }

        (*i)->DoRecv(&fd);
        std::string *RecvBuffer = (*i)->GetBytes();
        BYTEARRAY Bytes         = UTIL_CreateByteArray((unsigned char *)RecvBuffer->c_str(), RecvBuffer->size());

        // a packet is at least 4 bytes

        if (Bytes.size() >= 4)
        {
            if (Bytes[0] == GPS_HEADER_CONSTANT)
            {
                // bytes 2 and 3 contain the length of the packet

                uint16_t Length = UTIL_ByteArrayToUInt16(Bytes, false, 2);

                if (Length >= 4)
                {
                    if (Bytes.size() >= Length)
                    {
                        if (Bytes[1] == CGPSProtocol::GPS_RECONNECT && Length == 13)
                        {
                            unsigned char PID     = Bytes[4];
                            uint32_t ReconnectKey = UTIL_ByteArrayToUInt32(Bytes, false, 5);
                            uint32_t LastPacket   = UTIL_ByteArrayToUInt32(Bytes, false, 9);

                            // look for a matching player in a running game

                            CGamePlayer *Match = NULL;

                            for (std::vector<CBaseGame *>::iterator j = m_Games.begin(); j != m_Games.end(); j++)
                            {
                                if ((*j)->GetGameLoaded())
                                {
                                    CGamePlayer *Player = (*j)->GetPlayerFromPID(PID);

                                    if (Player && Player->GetGProxy() && Player->GetGProxyReconnectKey() == ReconnectKey)
                                    {
                                        Match = Player;
                                        break;
                                    }
                                }
                            }

                            if (Match)
                            {
                                // reconnect successful!

                                *RecvBuffer = RecvBuffer->substr(Length);
                                Match->EventGProxyReconnect(*i, LastPacket);
                                i = m_ReconnectSockets.erase(i);
                                continue;
                            }
                            else
                            {
                                (*i)->PutBytes(m_GPSProtocol->SEND_GPSS_REJECT(REJECTGPS_NOTFOUND));
                                (*i)->DoSend(&send_fd);
                                delete *i;
                                i = m_ReconnectSockets.erase(i);
                                continue;
                            }
                        }
                        else
                        {
                            (*i)->PutBytes(m_GPSProtocol->SEND_GPSS_REJECT(REJECTGPS_INVALID));
                            (*i)->DoSend(&send_fd);
                            delete *i;
                            i = m_ReconnectSockets.erase(i);
                            continue;
                        }
                    }
                }
                else
                {
                    (*i)->PutBytes(m_GPSProtocol->SEND_GPSS_REJECT(REJECTGPS_INVALID));
                    (*i)->DoSend(&send_fd);
                    delete *i;
                    i = m_ReconnectSockets.erase(i);
                    continue;
                }
            }
            else
            {
                (*i)->PutBytes(m_GPSProtocol->SEND_GPSS_REJECT(REJECTGPS_INVALID));
                (*i)->DoSend(&send_fd);
                delete *i;
                i = m_ReconnectSockets.erase(i);
                continue;
            }
        }

        (*i)->DoSend(&send_fd);
        i++;
    }

    //status socket
    if (m_TCPStatus && m_StatusBroadcaster->connectSocket)
    {
        //новое подключение
        CTCPStatusBroadcasterSocket *NewSocketStatus = new CTCPStatusBroadcasterSocket(m_StatusBroadcaster->connectSocket->Accept(&fd));

        if (NewSocketStatus->socket)
        {
            //NewSocketStatus->socket->SetLogFile("statuslog.txt");
            m_StatusBroadcaster->sockets.push_back(NewSocketStatus);
            NewSocketStatus->socket->SetFD(&fd, &send_fd, &nfds);
            m_StatusBroadcaster->SendGame(GetGame(), NewSocketStatus); // отправялем "GAME" всем новым подключениям
        }

        if (m_StatusBroadcaster->sockets.size() > 0)
        {

            for (std::vector<CTCPStatusBroadcasterSocket *>::iterator i = m_StatusBroadcaster->sockets.begin(); i != m_StatusBroadcaster->sockets.end();)
            {

                if ((*i)->socket->HasError() || !(*i)->socket->GetConnected())
                {
                    delete *i;
                    i = m_StatusBroadcaster->sockets.erase(i);
                    continue;
                }

                (*i)->socket->DoRecv(&fd);
                std::string *RecvBuffer = (*i)->socket->GetBytes();

                BYTEARRAY Bytes = UTIL_CreateByteArray((unsigned char *)RecvBuffer->c_str(), RecvBuffer->size());

                //отправка "GAME" и "SLOT" по запросу на конкретный сокет
                if (Bytes.size() >= 4)
                {
                    if (Bytes[0] == 'G' && Bytes[1] == 'A' && Bytes[2] == 'M' && Bytes[3] == 'E')
                        m_StatusBroadcaster->SendGame(GetGame(), (*i));
                    if (Bytes[0] == 'S' && Bytes[1] == 'L' && Bytes[2] == 'O' && Bytes[3] == 'T')
                        m_StatusBroadcaster->SendSlot(GetGame(), (*i));
                }

                (*i)->socket->ClearRecvBuffer();
                i++;
            }
        }
    }

    // autohost

    /* debug info
    if(m_AutoHostGameName.empty( ))
        CONSOLE_Print("name empty");
    else 
        CONSOLE_Print("name NOT empty");
    CONSOLE_Print("Max games: " + UTIL_ToString(m_AutoHostMaximumGames));
    CONSOLE_Print("Players: " + UTIL_ToString(m_AutoHostAutoStartPlayers));
    CONSOLE_Print("m_RehostDelay: " + UTIL_ToString(m_RehostDelay) );
    CONSOLE_Print("Time: " + UTIL_ToString(GetTime( ) - m_LastAutoHostTime));*/

    if (!m_AutoHostGameName.empty() && m_AutoHostMaximumGames != 0 && m_AutoHostAutoStartPlayers != 0 && GetTime() - m_LastAutoHostTime >= m_RehostDelay)
    {
        // copy all the checks from CGHost:: CreateGame here because we don't want to spam the chat when there's an error
        // instead we fail silently and try again soon

        if (!m_ExitingNice && m_Enabled && !m_CurrentGame && m_Games.size() < m_MaxGames && m_Games.size() < m_AutoHostMaximumGames)
        {
            if (m_AutoHostMap->GetValid())
            {
                // @disturbed_oc
                // try to find gamename with random mode, or fallback on autohost_gamename

                //CONSOLE_Print( "[GHOST] Trying to determine gamename..." );
                std::string GameName = m_AutoHostMap->GetMapGameNameWithRandomMode();

                if (GameName.empty())
                    GameName = m_AutoHostGameName;

                //GameName = GameName + " #" + UTIL_ToString( m_HostCounter );

                // @end

                /* removed when implemented HCL command rotation code
                std::string GameName = m_AutoHostGameName + " #" + UTIL_ToString( m_HostCounter );
                */

                if (GameName.size() <= 31)
                {
                    CreateGame(m_AutoHostMap, GAME_PUBLIC, false, GameName, m_AutoHostOwner, m_AutoHostOwner, m_AutoHostServer, false);

                    if (m_CurrentGame)
                    {
                        m_CurrentGame->SetAutoStartPlayers(m_AutoHostAutoStartPlayers);

                        if (m_AutoHostMatchMaking)
                        {
                            if (!m_Map->GetMapMatchMakingCategory().empty())
                            {
                                if (!(m_Map->GetMapOptions() & MAPOPT_FIXEDPLAYERSETTINGS))
                                    CONSOLE_Print("[GHOST] autohostmm - map_matchmakingcategory [" + m_Map->GetMapMatchMakingCategory() + "] found but matchmaking can only be used with fixed player settings, matchmaking disabled");
                                else
                                {
                                    CONSOLE_Print("[GHOST] autohostmm - map_matchmakingcategory [" + m_Map->GetMapMatchMakingCategory() + "] found, matchmaking enabled");

                                    m_CurrentGame->SetMatchMaking(true);
                                    m_CurrentGame->SetMinimumScore(m_AutoHostMinimumScore);
                                    m_CurrentGame->SetMaximumScore(m_AutoHostMaximumScore);
                                }
                            }
                            else
                                CONSOLE_Print("[GHOST] autohostmm - map_matchmakingcategory not found, matchmaking disabled");
                        }
                    }
                }
                else
                {
                    CONSOLE_Print("[GHOST] stopped auto hosting, next game name [" + GameName + "] is too long (the maximum is 31 characters)");
                    m_AutoHostGameName.clear();
                    m_AutoHostOwner.clear();
                    m_AutoHostServer.clear();
                    m_AutoHostMaximumGames     = 0;
                    m_AutoHostAutoStartPlayers = 0;
                    m_AutoHostMatchMaking      = false;
                    m_AutoHostMinimumScore     = 0.0;
                    m_AutoHostMaximumScore     = 0.0;
                }
            }
            else
            {
                CONSOLE_Print("[GHOST] stopped auto hosting, map config file [" + m_AutoHostMap->GetCFGFile() + "] is invalid");
                m_AutoHostGameName.clear();
                m_AutoHostOwner.clear();
                m_AutoHostServer.clear();
                m_AutoHostMaximumGames     = 0;
                m_AutoHostAutoStartPlayers = 0;
                m_AutoHostMatchMaking      = false;
                m_AutoHostMinimumScore     = 0.0;
                m_AutoHostMaximumScore     = 0.0;
            }
        }

        m_LastAutoHostTime = GetTime();
    }

    return m_Exiting || AdminExit || BNETExit;
}

void CGHost::EventBNETConnecting(CBNET *bnet)
{
    if (m_AdminGame)
        m_AdminGame->SendAllChat(m_Language->ConnectingToBNET(bnet->GetServer()));

    if (m_CurrentGame)
        m_CurrentGame->SendAllChat(m_Language->ConnectingToBNET(bnet->GetServer()));
}

void CGHost::EventBNETConnected(CBNET *bnet)
{
    if (m_AdminGame)
        m_AdminGame->SendAllChat(m_Language->ConnectedToBNET(bnet->GetServer()));

    if (m_CurrentGame)
        m_CurrentGame->SendAllChat(m_Language->ConnectedToBNET(bnet->GetServer()));
}

void CGHost::EventBNETDisconnected(CBNET *bnet)
{
    if (m_AdminGame)
        m_AdminGame->SendAllChat(m_Language->DisconnectedFromBNET(bnet->GetServer()));

    if (m_CurrentGame)
        m_CurrentGame->SendAllChat(m_Language->DisconnectedFromBNET(bnet->GetServer()));
}

void CGHost::EventBNETLoggedIn(CBNET *bnet)
{
    if (m_AdminGame)
        m_AdminGame->SendAllChat(m_Language->LoggedInToBNET(bnet->GetServer()));

    if (m_CurrentGame)
        m_CurrentGame->SendAllChat(m_Language->LoggedInToBNET(bnet->GetServer()));
}

void CGHost::EventBNETGameRefreshed(CBNET *bnet)
{
    if (m_AdminGame)
        m_AdminGame->SendAllChat(m_Language->BNETGameHostingSucceeded(bnet->GetServer()));

    if (m_CurrentGame)
        m_CurrentGame->EventGameRefreshed(bnet->GetServer());
}

void CGHost::EventBNETGameRefreshFailed(CBNET *bnet)
{
    if (m_CurrentGame)
    {
        for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
        {
            (*i)->QueueChatCommand(m_Language->UnableToCreateGameTryAnotherName(bnet->GetServer(), m_CurrentGame->GetGameName()));

            if ((*i)->GetServer() == m_CurrentGame->GetCreatorServer())
                (*i)->QueueChatCommand(m_Language->UnableToCreateGameTryAnotherName(bnet->GetServer(), m_CurrentGame->GetGameName()), m_CurrentGame->GetCreatorName(), true, false);
        }

        if (m_AdminGame)
            m_AdminGame->SendAllChat(m_Language->BNETGameHostingFailed(bnet->GetServer(), m_CurrentGame->GetGameName()));

        m_CurrentGame->SendAllChat(m_Language->UnableToCreateGameTryAnotherName(bnet->GetServer(), m_CurrentGame->GetGameName()));

        // we take the easy route and simply close the lobby if a refresh fails
        // it's possible at least one refresh succeeded and therefore the game is still joinable on at least one battle.net (plus on the local network) but we don't keep track of that
        // we only close the game if it has no players since we support game rehosting (via !priv and !pub in the lobby)

        if (m_CurrentGame->GetNumHumanPlayers() == 0)
            m_CurrentGame->SetExiting(true);

        m_CurrentGame->SetRefreshError(true);
    }
}

void CGHost::EventBNETConnectTimedOut(CBNET *bnet)
{
    if (m_AdminGame)
        m_AdminGame->SendAllChat(m_Language->ConnectingToBNETTimedOut(bnet->GetServer()));

    if (m_CurrentGame)
        m_CurrentGame->SendAllChat(m_Language->ConnectingToBNETTimedOut(bnet->GetServer()));
}

void CGHost::EventBNETWhisper(CBNET *bnet, std::string user, std::string message)
{
    if (m_AdminGame)
    {
        m_AdminGame->SendAdminChat("[WHISPER: " + bnet->GetServerAlias() + "] [" + user + "] " + message);

        if (m_CurrentGame)
            m_CurrentGame->SendLocalAdminChat("[WHISPER: " + bnet->GetServerAlias() + "] [" + user + "] " + message);

        for (std::vector<CBaseGame *>::iterator i = m_Games.begin(); i != m_Games.end(); i++)
            (*i)->SendLocalAdminChat("[WHISPER: " + bnet->GetServerAlias() + "] [" + user + "] " + message);
    }
}

void CGHost::EventBNETChat(CBNET *bnet, std::string user, std::string message)
{
    if (m_AdminGame)
    {
        m_AdminGame->SendAdminChat("[LOCAL: " + bnet->GetServerAlias() + "] [" + user + "] " + message);

        if (m_CurrentGame)
            m_CurrentGame->SendLocalAdminChat("[LOCAL: " + bnet->GetServerAlias() + "] [" + user + "] " + message);

        for (std::vector<CBaseGame *>::iterator i = m_Games.begin(); i != m_Games.end(); i++)
            (*i)->SendLocalAdminChat("[LOCAL: " + bnet->GetServerAlias() + "] [" + user + "] " + message);
    }
}

void CGHost::EventBNETBROADCAST(CBNET *bnet, std::string user, std::string message)
{
    if (m_AdminGame)
    {
        m_AdminGame->SendAdminChat("[BROADCAST: " + bnet->GetServerAlias() + "] " + message);

        if (m_CurrentGame)
            m_CurrentGame->SendLocalAdminChat("[BROADCAST: " + bnet->GetServerAlias() + "] " + message);

        for (std::vector<CBaseGame *>::iterator i = m_Games.begin(); i != m_Games.end(); i++)
            (*i)->SendLocalAdminChat("[BROADCAST: " + bnet->GetServerAlias() + "] " + message);
    }
}

void CGHost::EventBNETCHANNEL(CBNET *bnet, std::string user, std::string message)
{
    if (m_AdminGame)
    {
        m_AdminGame->SendAdminChat("[BNET: " + bnet->GetServerAlias() + "] joined channel [" + message + "]");

        if (m_CurrentGame)
            m_CurrentGame->SendLocalAdminChat("[BNET: " + bnet->GetServerAlias() + "] joined channel [" + message + "]");

        for (std::vector<CBaseGame *>::iterator i = m_Games.begin(); i != m_Games.end(); i++)
            (*i)->SendLocalAdminChat("[BNET: " + bnet->GetServerAlias() + "] joined channel [" + message + "]");
    }
}

void CGHost::EventBNETWHISPERSENT(CBNET *bnet, std::string user, std::string message)
{
    if (m_AdminGame)
    {
        m_AdminGame->SendAdminChat("[WHISPERED: " + bnet->GetServerAlias() + "] [" + user + "] " + message);

        if (m_CurrentGame)
            m_CurrentGame->SendLocalAdminChat("[WHISPERED: " + bnet->GetServerAlias() + "] [" + user + "] " + message);

        for (std::vector<CBaseGame *>::iterator i = m_Games.begin(); i != m_Games.end(); i++)
            (*i)->SendLocalAdminChat("[WHISPERED: " + bnet->GetServerAlias() + "] [" + user + "] " + message);
    }
}

void CGHost::EventBNETCHANNELFULL(CBNET *bnet, std::string user, std::string message)
{
    if (m_AdminGame)
    {
        m_AdminGame->SendAdminChat("[BNET: " + bnet->GetServerAlias() + "] channel is full");

        if (m_CurrentGame)
            m_CurrentGame->SendLocalAdminChat("[BNET: " + bnet->GetServerAlias() + "] channel is full");

        for (std::vector<CBaseGame *>::iterator i = m_Games.begin(); i != m_Games.end(); i++)
            (*i)->SendLocalAdminChat("[BNET: " + bnet->GetServerAlias() + "] channel is full");
    }
}

void CGHost::EventBNETCHANNELDOESNOTEXIST(CBNET *bnet, std::string user, std::string message)
{
    if (m_AdminGame)
    {
        m_AdminGame->SendAdminChat("[BNET: " + bnet->GetServerAlias() + "] channel does not exist");

        if (m_CurrentGame)
            m_CurrentGame->SendLocalAdminChat("[BNET: " + bnet->GetServerAlias() + "] channel does not exist");

        for (std::vector<CBaseGame *>::iterator i = m_Games.begin(); i != m_Games.end(); i++)
            (*i)->SendLocalAdminChat("[BNET: " + bnet->GetServerAlias() + "] channel does not exist");
    }
}

void CGHost::EventBNETCHANNELRESTRICTED(CBNET *bnet, std::string user, std::string message)
{
    if (m_AdminGame)
    {
        m_AdminGame->SendAdminChat("[BNET: " + bnet->GetServerAlias() + "] channel restricted");

        if (m_CurrentGame)
            m_CurrentGame->SendLocalAdminChat("[BNET: " + bnet->GetServerAlias() + "] channel restricted");

        for (std::vector<CBaseGame *>::iterator i = m_Games.begin(); i != m_Games.end(); i++)
            (*i)->SendLocalAdminChat("[BNET: " + bnet->GetServerAlias() + "] channel restricted");
    }
}

void CGHost::EventBNETError(CBNET *bnet, std::string user, std::string message)
{
    if (m_AdminGame)
    {
        m_AdminGame->SendAdminChat("[ERROR: " + bnet->GetServerAlias() + "] " + message);

        if (m_CurrentGame)
            m_CurrentGame->SendLocalAdminChat("[ERROR: " + bnet->GetServerAlias() + "] " + message);

        for (std::vector<CBaseGame *>::iterator i = m_Games.begin(); i != m_Games.end(); i++)
            (*i)->SendLocalAdminChat("[ERROR: " + bnet->GetServerAlias() + "] " + message);
    }
}

void CGHost::EventBNETEmote(CBNET *bnet, std::string user, std::string message)
{
    if (m_AdminGame)
    {
        m_AdminGame->SendAdminChat("[E: " + bnet->GetServerAlias() + "] [" + user + "] " + message);

        if (m_CurrentGame)
            m_CurrentGame->SendLocalAdminChat("[E: " + bnet->GetServerAlias() + "] [" + user + "] " + message);

        for (std::vector<CBaseGame *>::iterator i = m_Games.begin(); i != m_Games.end(); i++)
            (*i)->SendLocalAdminChat("[E: " + bnet->GetServerAlias() + "] [" + user + "] " + message);
    }
}

void CGHost::EventGameDeleted(CBaseGame *game)
{
    for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
    {
        (*i)->QueueChatCommand(m_Language->GameIsOver(game->GetDescription()));

        if ((*i)->GetServer() == game->GetCreatorServer())
            (*i)->QueueChatCommand(m_Language->GameIsOver(game->GetDescription()), game->GetCreatorName(), true, false);
    }
}

void CGHost::EventBNETInfo(CBNET *bnet, std::string user, std::string message)
{
    if (m_AdminGame)
    {
        m_AdminGame->SendAdminChat("[INFO: " + bnet->GetServerAlias() + "] " + message);

        if (m_CurrentGame)
            m_CurrentGame->SendLocalAdminChat("[INFO: " + bnet->GetServerAlias() + "] " + message);

        for (std::vector<CBaseGame *>::iterator i = m_Games.begin(); i != m_Games.end(); i++)
            (*i)->SendLocalAdminChat("[INFO: " + bnet->GetServerAlias() + "] " + message);
    }
}

void CGHost::ReloadConfigs()
{
    CConfig CFG;
    CFG.Read("default.cfg");
    CFG.Read(gCFGFile);
    SetConfigs(&CFG);
}

void CGHost::SetConfigs(CConfig *CFG)
{
    // this doesn't set EVERY config value since that would potentially require reconfiguring the battle.net connections
    // it just set the easily reloadable values

    m_LanguageFile = CFG->GetString("bot_language", "language.cfg");
    delete m_Language;
    m_Language                    = new CLanguage(m_LanguageFile);
    m_Warcraft3Path               = UTIL_AddPathSeperator(CFG->GetString("bot_war3path", "C:\\Program Files\\Warcraft III\\"));
    m_BindAddress                 = CFG->GetString("bot_bindaddress", std::string());
    m_ReconnectWaitTime           = CFG->GetInt("bot_reconnectwaittime", 3);
    m_MaxGames                    = CFG->GetInt("bot_maxgames", 5);
    std::string BotCommandTrigger = CFG->GetString("bot_commandtrigger", "!");

    if (BotCommandTrigger.empty())
        BotCommandTrigger = "!";

    m_CommandTrigger       = BotCommandTrigger[0];
    m_MapCFGPath           = UTIL_AddPathSeperator(CFG->GetString("bot_mapcfgpath", std::string()));
    m_SaveGamePath         = UTIL_AddPathSeperator(CFG->GetString("bot_savegamepath", std::string()));
    m_MapPath              = UTIL_AddPathSeperator(CFG->GetString("bot_mappath", std::string()));
    m_SaveReplays          = CFG->GetInt("bot_savereplays", 0) == 0 ? false : true;
    m_ReplayPath           = UTIL_AddPathSeperator(CFG->GetString("bot_replaypath", std::string()));
    m_VirtualHostName      = CFG->GetString("bot_virtualhostname", "|cFF4080C0GHost");
    m_HideIPAddresses      = CFG->GetInt("bot_hideipaddresses", 0) == 0 ? false : true;
    m_CheckMultipleIPUsage = CFG->GetInt("bot_checkmultipleipusage", 1) == 0 ? false : true;

    if (m_VirtualHostName.size() > 15)
    {
        m_VirtualHostName = "|cFF4080C0GHost";
        CONSOLE_Print("[GHOST] warning - bot_virtualhostname is longer than 15 characters, using default virtual host name");
    }

    m_SpoofChecks         = CFG->GetInt("bot_spoofchecks", 2);
    m_RequireSpoofChecks  = CFG->GetInt("bot_requirespoofchecks", 0) == 0 ? false : true;
    m_ReserveAdmins       = CFG->GetInt("bot_reserveadmins", 1) == 0 ? false : true;
    m_RefreshMessages     = CFG->GetInt("bot_refreshmessages", 0) == 0 ? false : true;
    m_AutoLock            = CFG->GetInt("bot_autolock", 0) == 0 ? false : true;
    m_AutoSave            = CFG->GetInt("bot_autosave", 0) == 0 ? false : true;
    m_AllowDownloads      = CFG->GetInt("bot_allowdownloads", 0);
    m_PingDuringDownloads = CFG->GetInt("bot_pingduringdownloads", 0) == 0 ? false : true;
    m_MaxDownloaders      = CFG->GetInt("bot_maxdownloaders", 3);
    m_MaxDownloadSpeed    = CFG->GetInt("bot_maxdownloadspeed", 100);
    m_LCPings             = CFG->GetInt("bot_lcpings", 1) == 0 ? false : true;
    m_AutoKickPing        = CFG->GetInt("bot_autokickping", 400);
    m_BanMethod           = CFG->GetInt("bot_banmethod", 1);
    m_IPBlackListFile     = CFG->GetString("bot_ipblacklistfile", "ipblacklist.txt");
    m_LobbyTimeLimit      = CFG->GetInt("bot_lobbytimelimit", 10);
    m_Latency             = CFG->GetInt("bot_latency", 100);
    m_SyncLimit           = CFG->GetInt("bot_synclimit", 50);
    m_VoteKickAllowed     = CFG->GetInt("bot_votekickallowed", 1) == 0 ? false : true;
    m_VoteKickPercentage  = CFG->GetInt("bot_votekickpercentage", 100);

    if (m_VoteKickPercentage > 100)
    {
        m_VoteKickPercentage = 100;
        CONSOLE_Print("[GHOST] warning - bot_votekickpercentage is greater than 100, using 100 instead");
    }

    m_MOTDFile           = CFG->GetString("bot_motdfile", "motd.txt");
    m_GameLoadedFile     = CFG->GetString("bot_gameloadedfile", "gameloaded.txt");
    m_GameOverFile       = CFG->GetString("bot_gameoverfile", "gameover.txt");
    m_LocalAdminMessages = CFG->GetInt("bot_localadminmessages", 1) == 0 ? false : true;
    m_TCPNoDelay         = CFG->GetInt("tcp_nodelay", 0) == 0 ? false : true;
    m_MatchMakingMethod  = CFG->GetInt("bot_matchmakingmethod", 1);

    //GHost Custom Reloadable CFG Values
    m_ApprovedCountries      = CFG->GetString("bot_approvedcountries", std::string());
    m_LANAdmins              = CFG->GetInt("lan_admins", 0);
    m_UseNormalCountDown     = CFG->GetInt("bot_usenormalcountdown", 0) == 0 ? false : true;
    m_GetLANRootAdmins       = CFG->GetInt("lan_getrootadmins", 1) == 0 ? false : true;
    m_LANRootAdmin           = CFG->GetString("lan_rootadmins", std::string());
    m_ResetDownloads         = CFG->GetInt("bot_resetdownloads", 0) == 0 ? false : true;
    m_AllowDownloads2        = m_AllowDownloads;
    m_HideCommands           = CFG->GetInt("bot_hideadmincommands", 0) == 0 ? false : true;
    m_WhisperResponses       = CFG->GetInt("bot_whisperresponses", 0) == 0 ? false : true;
    m_ForceLoadInGame        = CFG->GetInt("bot_forceloadingame", 0) == 0 ? false : true;
    m_HCLCommandFromGameName = CFG->GetInt("bot_hclfromgamename", 0) == 0 ? false : true;

    //

    m_gameoverminpercent = CFG->GetInt("bot_gameoverminpercent", 0);
    m_RehostDelay        = CFG->GetInt("bot_rehostdelay", 15);
    m_ObserverSlots      = CFG->GetString("bot_observer_slots", "5 11");
    m_AddCompsAllowed    = CFG->GetInt("bot_addcompsallowed", 0) == 0 ? false : true;
    m_DesyncKick         = CFG->GetInt("bot_desynckick", 0) == 0 ? false : true;
    m_TMProotPassword    = CFG->GetString("bot_tmprootpassword", "777777777777");

    //
}

void CGHost::ExtractScripts()
{
    std::string PatchMPQFileName = m_Warcraft3Path + "War3Patch.mpq";
    HANDLE PatchMPQ;

    if (SFileOpenArchive(PatchMPQFileName.c_str(), 0, MPQ_OPEN_FORCE_MPQ_V1, &PatchMPQ))
    {
        CONSOLE_Print("[GHOST] loading MPQ file [" + PatchMPQFileName + "]");
        HANDLE SubFile;

        // common.j

        if (SFileOpenFileEx(PatchMPQ, "Scripts\\common.j", 0, &SubFile))
        {
            uint32_t FileLength = SFileGetFileSize(SubFile, NULL);

            if (FileLength > 0 && FileLength != 0xFFFFFFFF)
            {
                char *SubFileData = new char[FileLength];
                DWORD BytesRead   = 0;

                if (SFileReadFile(SubFile, SubFileData, FileLength, &BytesRead, NULL))
                {
                    CONSOLE_Print("[GHOST] extracting Scripts\\common.j from MPQ file to [" + m_MapCFGPath + "common.j]");
                    UTIL_FileWrite(m_MapCFGPath + "common.j", (unsigned char *)SubFileData, BytesRead);
                }
                else
                    CONSOLE_Print("[GHOST] warning - unable to extract Scripts\\common.j from MPQ file");

                delete[] SubFileData;
            }

            SFileCloseFile(SubFile);
        }
        else
            CONSOLE_Print("[GHOST] couldn't find Scripts\\common.j in MPQ file");

        // blizzard.j

        if (SFileOpenFileEx(PatchMPQ, "Scripts\\blizzard.j", 0, &SubFile))
        {
            uint32_t FileLength = SFileGetFileSize(SubFile, NULL);

            if (FileLength > 0 && FileLength != 0xFFFFFFFF)
            {
                char *SubFileData = new char[FileLength];
                DWORD BytesRead   = 0;

                if (SFileReadFile(SubFile, SubFileData, FileLength, &BytesRead, NULL))
                {
                    CONSOLE_Print("[GHOST] extracting Scripts\\blizzard.j from MPQ file to [" + m_MapCFGPath + "blizzard.j]");
                    UTIL_FileWrite(m_MapCFGPath + "blizzard.j", (unsigned char *)SubFileData, BytesRead);
                }
                else
                    CONSOLE_Print("[GHOST] warning - unable to extract Scripts\\blizzard.j from MPQ file");

                delete[] SubFileData;
            }

            SFileCloseFile(SubFile);
        }
        else
            CONSOLE_Print("[GHOST] couldn't find Scripts\\blizzard.j in MPQ file");

        SFileCloseArchive(PatchMPQ);
    }
    else
        CONSOLE_Print("[GHOST] warning - unable to load MPQ file [" + PatchMPQFileName + "] - error code " + UTIL_ToString(GetLastError()));
}

void CGHost::LoadIPToCountryData()
{
    std::ifstream in;
    in.open("ip-to-country.csv");

    if (in.fail())
        CONSOLE_Print("[GHOST] warning - unable to read file [ip-to-country.csv], iptocountry data not loaded");
    else
    {
        CONSOLE_Print("[GHOST] started loading [ip-to-country.csv]");

        // the begin and commit statements are optimizations
        // we're about to insert ~4 MB of data into the database so if we allow the database to treat each insert as a transaction it will take a LONG time
        // todotodo: handle begin/commit failures a bit more gracefully

        if (!m_DBLocal->Begin())
            CONSOLE_Print("[GHOST] warning - failed to begin local database transaction, iptocountry data not loaded");
        else
        {
            unsigned char Percent = 0;
            std::string Line;
            std::string IP1;
            std::string IP2;
            std::string Country;
            CSVParser parser;

            // get length of file for the progress meter

            in.seekg(0, std::ios::end);
            uint32_t FileLength = in.tellg();
            in.seekg(0, std::ios::beg);

            while (!in.eof())
            {
                getline(in, Line);

                if (Line.empty())
                    continue;

                parser << Line;
                parser >> IP1;
                parser >> IP2;
                parser >> Country;
                m_DBLocal->FromAdd(UTIL_ToUInt32(IP1), UTIL_ToUInt32(IP2), Country);

                // it's probably going to take awhile to load the iptocountry data (~10 seconds on my 3.2 GHz P4 when using SQLite3)
                // so let's print a progress meter just to keep the user from getting worried

                unsigned char NewPercent = (unsigned char)((float)in.tellg() / FileLength * 100);

                if (NewPercent != Percent && NewPercent % 10 == 0)
                {
                    Percent = NewPercent;
                    CONSOLE_Print("[GHOST] iptocountry data: " + UTIL_ToString(Percent) + "% loaded");
                }
            }

            if (!m_DBLocal->Commit())
                CONSOLE_Print("[GHOST] warning - failed to commit local database transaction, iptocountry data not loaded");
            else
                CONSOLE_Print("[GHOST] finished loading [ip-to-country.csv]");
        }

        in.close();
    }
}

void CGHost::CreateGame(CMap *map, unsigned char gameState, bool saveGame, std::string gameName, std::string ownerName, std::string creatorName, std::string creatorServer, bool whisper)
{
    if (!m_Enabled)
    {
        for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
        {
            if ((*i)->GetServer() == creatorServer)
                (*i)->QueueChatCommand(m_Language->UnableToCreateGameDisabled(gameName), creatorName, whisper, false);
        }

        if (m_AdminGame)
            m_AdminGame->SendAllChat(m_Language->UnableToCreateGameDisabled(gameName));

        return;
    }

    if (gameName.size() > 31)
    {
        for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
        {
            if ((*i)->GetServer() == creatorServer)
                (*i)->QueueChatCommand(m_Language->UnableToCreateGameNameTooLong(gameName), creatorName, whisper, false);
        }

        if (m_AdminGame)
            m_AdminGame->SendAllChat(m_Language->UnableToCreateGameNameTooLong(gameName));

        return;
    }

    if (!map->GetValid())
    {
        for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
        {
            if ((*i)->GetServer() == creatorServer)
                (*i)->QueueChatCommand(m_Language->UnableToCreateGameInvalidMap(gameName), creatorName, whisper, false);
        }

        if (m_AdminGame)
            m_AdminGame->SendAllChat(m_Language->UnableToCreateGameInvalidMap(gameName));

        return;
    }

    if (saveGame)
    {
        if (!m_SaveGame->GetValid())
        {
            for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
            {
                if ((*i)->GetServer() == creatorServer)
                    (*i)->QueueChatCommand(m_Language->UnableToCreateGameInvalidSaveGame(gameName), creatorName, whisper, false);
            }

            if (m_AdminGame)
                m_AdminGame->SendAllChat(m_Language->UnableToCreateGameInvalidSaveGame(gameName));

            return;
        }

        std::string MapPath1 = m_SaveGame->GetMapPath();
        std::string MapPath2 = map->GetMapPath();
        transform(MapPath1.begin(), MapPath1.end(), MapPath1.begin(), (int (*)(int))tolower);
        transform(MapPath2.begin(), MapPath2.end(), MapPath2.begin(), (int (*)(int))tolower);

        if (MapPath1 != MapPath2)
        {
            CONSOLE_Print("[GHOST] path mismatch, saved game path is [" + MapPath1 + "] but map path is [" + MapPath2 + "]");

            for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
            {
                if ((*i)->GetServer() == creatorServer)
                    (*i)->QueueChatCommand(m_Language->UnableToCreateGameSaveGameMapMismatch(gameName), creatorName, whisper, false);
            }

            if (m_AdminGame)
                m_AdminGame->SendAllChat(m_Language->UnableToCreateGameSaveGameMapMismatch(gameName));

            return;
        }

        if (m_EnforcePlayers.empty())
        {
            for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
            {
                if ((*i)->GetServer() == creatorServer)
                    (*i)->QueueChatCommand(m_Language->UnableToCreateGameMustEnforceFirst(gameName), creatorName, whisper, false);
            }

            if (m_AdminGame)
                m_AdminGame->SendAllChat(m_Language->UnableToCreateGameMustEnforceFirst(gameName));

            return;
        }
    }

    if (m_CurrentGame)
    {
        for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
        {
            if ((*i)->GetServer() == creatorServer)
                (*i)->QueueChatCommand(m_Language->UnableToCreateGameAnotherGameInLobby(gameName, m_CurrentGame->GetDescription()), creatorName, whisper, false);
        }

        if (m_AdminGame)
            m_AdminGame->SendAllChat(m_Language->UnableToCreateGameAnotherGameInLobby(gameName, m_CurrentGame->GetDescription()));

        return;
    }

    if (m_Games.size() >= m_MaxGames)
    {
        for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
        {
            if ((*i)->GetServer() == creatorServer)
                (*i)->QueueChatCommand(m_Language->UnableToCreateGameMaxGamesReached(gameName, UTIL_ToString(m_MaxGames)), creatorName, whisper, false);
        }

        if (m_AdminGame)
            m_AdminGame->SendAllChat(m_Language->UnableToCreateGameMaxGamesReached(gameName, UTIL_ToString(m_MaxGames)));

        return;
    }

    CONSOLE_Print("[GHOST] creating game [" + gameName + "]");

    if (saveGame)
        m_CurrentGame = new CGame(this, map, m_SaveGame, m_HostPort, gameState, gameName, ownerName, creatorName, creatorServer);
    else
        m_CurrentGame = new CGame(this, map, NULL, m_HostPort, gameState, gameName, ownerName, creatorName, creatorServer);

    // todotodo: check if listening failed and report the error to the user

    if (m_SaveGame)
    {
        m_CurrentGame->SetEnforcePlayers(m_EnforcePlayers);
        m_EnforcePlayers.clear();
    }

    // auto set HCL if map_defaulthcl is not empty
    m_CurrentGame->AutoSetHCL();

    for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
    {
        if (whisper && (*i)->GetServer() == creatorServer)
        {
            // note that we send this whisper only on the creator server

            if (gameState == GAME_PRIVATE)
                (*i)->QueueChatCommand(m_Language->CreatingPrivateGame(gameName, ownerName), creatorName, whisper, false);
            else if (gameState == GAME_PUBLIC)
                (*i)->QueueChatCommand(m_Language->CreatingPublicGame(gameName, ownerName), creatorName, whisper, false);
        }
        else
        {
            // note that we send this chat message on all other bnet servers

            if (gameState == GAME_PRIVATE)
                (*i)->QueueChatCommand(m_Language->CreatingPrivateGame(gameName, ownerName));
            else if (gameState == GAME_PUBLIC)
                (*i)->QueueChatCommand(m_Language->CreatingPublicGame(gameName, ownerName));
        }

        if (saveGame)
            (*i)->QueueGameCreate(gameState, gameName, std::string(), map, m_SaveGame, m_CurrentGame->GetHostCounter());
        else
            (*i)->QueueGameCreate(gameState, gameName, std::string(), map, NULL, m_CurrentGame->GetHostCounter());
    }

    if (m_AdminGame)
    {
        if (gameState == GAME_PRIVATE)
            m_AdminGame->SendAllChat(m_Language->CreatingPrivateGame(gameName, ownerName));
        else if (gameState == GAME_PUBLIC)
            m_AdminGame->SendAllChat(m_Language->CreatingPublicGame(gameName, ownerName));
    }

    // if we're creating a private game we don't need to send any game refresh messages so we can rejoin the chat immediately
    // unfortunately this doesn't work on PVPGN servers because they consider an enterchat message to be a gameuncreate message when in a game
    // so don't rejoin the chat if we're using PVPGN

    if (gameState == GAME_PRIVATE)
    {
        for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
        {
            if ((*i)->GetPasswordHashType() != "pvpgn")
                (*i)->QueueEnterChat();
        }
    }

    // hold friends and/or clan members

    for (std::vector<CBNET *>::iterator i = m_BNETs.begin(); i != m_BNETs.end(); i++)
    {
        if ((*i)->GetHoldFriends())
            (*i)->HoldFriends(m_CurrentGame);

        if ((*i)->GetHoldClan())
            (*i)->HoldClan(m_CurrentGame);
    }

    //broadcaster
    if (m_TCPStatus && m_StatusBroadcaster != NULL)
    {
        m_StatusBroadcaster->SendGame(GetGame()); // игра создана - рассылаем
    }
}

//текущая игра, лобби или стартанутая
CBaseGame *CGHost::GetGame()
{
    if (m_CurrentGame)
        return m_CurrentGame;
    else if (m_Games.size() > 0)
        return m_Games[0];
    else
        return NULL;
}

std::string CGHost::CalcStatString(CMap *map)
{
    BYTEARRAY StatString;
    UTIL_AppendByteArray(StatString, m_Map->GetMapGameFlags());
    StatString.push_back(0);
    //UTIL_AppendByteArray( StatString, m_Map->GetMapWidth( ) );
    //UTIL_AppendByteArray( StatString, m_Map->GetMapHeight( ) );
    StatString.push_back(0xC0); //fixed size because it needed for gproxy
    StatString.push_back(0x07); //fixed size because it needed for gproxy
    StatString.push_back(0xC0); //fixed size because it needed for gproxy
    StatString.push_back(0x07); //fixed size because it needed for gproxy
    UTIL_AppendByteArray(StatString, m_Map->GetMapCRC());
    UTIL_AppendByteArray(StatString, m_Map->GetMapPath());
    UTIL_AppendByteArray(StatString, "beef(THDOTS.RU)");
    StatString.push_back(0);
    UTIL_AppendByteArray(StatString, m_Map->GetMapSHA1()); // note: in replays generated by Warcraft III it stores 20 zeros for the SHA1 instead of the real thing
    StatString               = UTIL_EncodeStatString(StatString);
    std::string s_StatString = std::string(StatString.begin(), StatString.end());
    return s_StatString;
}
