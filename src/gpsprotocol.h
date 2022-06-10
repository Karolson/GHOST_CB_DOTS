/*

   Copyright 2010 Trevor Hogan

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

*/

#pragma once

#include "includes.h"

//
// CGameProtocol
//

#define GPS_HEADER_CONSTANT 248

#define REJECTGPS_INVALID 1
#define REJECTGPS_NOTFOUND 2

class CGPSProtocol
{
public:
    enum Protocol
    {
        GPS_INIT      = 1,
        GPS_RECONNECT = 2,
        GPS_ACK       = 3,
        GPS_REJECT    = 4,
		GPS_DISCORD_PRESENCE_STATE = 5,
		GPS_DISCORD_PRESENCE_DETAILS = 6,
		GPS_DISCORD_PRESENCE_STARTTIMESTAMP = 7,
		GPS_DISCORD_PRESENCE_ENDTIMESTAMP = 8,
		GPS_DISCORD_PRESENCE_LARGEIMAGEKEY = 9,
		GPS_DISCORD_PRESENCE_LARGEIMAGETEXT = 10,
		GPS_DISCORD_PRESENCE_SMALLIMAGEKEY = 11,
		GPS_DISCORD_PRESENCE_SMALLIMAGETEXT = 12,
		GPS_DISCORD_PRESENCE_PARTYSIZE = 13,
		GPS_DISCORD_PRESENCE_PARTYMAX = 14,
    };

    CGPSProtocol();
    ~CGPSProtocol();

    // receive functions

    // send functions

    BYTEARRAY SEND_GPSC_INIT(uint32_t version);
    BYTEARRAY SEND_GPSC_RECONNECT(unsigned char PID, uint32_t reconnectKey, uint32_t lastPacket);
    BYTEARRAY SEND_GPSC_ACK(uint32_t lastPacket);

    BYTEARRAY SEND_GPSS_INIT(uint16_t reconnectPort, unsigned char PID, uint32_t reconnectKey, unsigned char numEmptyActions);
    BYTEARRAY SEND_GPSS_RECONNECT(uint32_t lastPacket);
    BYTEARRAY SEND_GPSS_ACK(uint32_t lastPacket);
    BYTEARRAY SEND_GPSS_REJECT(uint32_t reason);
    
    BYTEARRAY SEND_GPSS_DISCORD_PRESENCE_STATE(std::string state);
    BYTEARRAY SEND_GPSS_DISCORD_PRESENCE_DETAILS(std::string details);
    BYTEARRAY SEND_GPSS_DISCORD_PRESENCE_STARTTIMESTAMP(uint64_t startTimestamp);
    BYTEARRAY SEND_GPSS_DISCORD_PRESENCE_ENDTIMESTAMP(uint64_t endTimestamp);
    BYTEARRAY SEND_GPSS_DISCORD_PRESENCE_LARGEIMAGEKEY(std::string largeImageKey);
    BYTEARRAY SEND_GPSS_DISCORD_PRESENCE_LARGEIMAGETEXT(std::string largeImageText);
    BYTEARRAY SEND_GPSS_DISCORD_PRESENCE_SMALLIMAGEKEY(std::string smallImageKey);
    BYTEARRAY SEND_GPSS_DISCORD_PRESENCE_SMALLIMAGETEXT(std::string smallImageText);
    BYTEARRAY SEND_GPSS_DISCORD_PRESENCE_PARTYSIZE(uint32_t partySize);
    BYTEARRAY SEND_GPSS_DISCORD_PRESENCE_PARTYMAX(uint32_t partyMax);

    // other functions

private:
    bool AssignLength(BYTEARRAY &content);
    bool ValidateLength(BYTEARRAY &content);
};
