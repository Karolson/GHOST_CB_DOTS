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

#include "gpsprotocol.h"
#include "ghost.h"
#include "util.h"

//
// CGPSProtocol
//

CGPSProtocol::CGPSProtocol()
{
}

CGPSProtocol::~CGPSProtocol()
{
}

///////////////////////
// RECEIVE FUNCTIONS //
///////////////////////

////////////////////
// SEND FUNCTIONS //
////////////////////

BYTEARRAY CGPSProtocol::SEND_GPSC_INIT(uint32_t version)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_INIT);
    packet.push_back(0);
    packet.push_back(0);
    UTIL_AppendByteArray(packet, version, false);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSC_RECONNECT(unsigned char PID, uint32_t reconnectKey, uint32_t lastPacket)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_RECONNECT);
    packet.push_back(0);
    packet.push_back(0);
    packet.push_back(PID);
    UTIL_AppendByteArray(packet, reconnectKey, false);
    UTIL_AppendByteArray(packet, lastPacket, false);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSC_ACK(uint32_t lastPacket)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_ACK);
    packet.push_back(0);
    packet.push_back(0);
    UTIL_AppendByteArray(packet, lastPacket, false);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSS_INIT(uint16_t reconnectPort, unsigned char PID, uint32_t reconnectKey, unsigned char numEmptyActions)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_INIT);
    packet.push_back(0);
    packet.push_back(0);
    UTIL_AppendByteArray(packet, reconnectPort, false);
    packet.push_back(PID);
    UTIL_AppendByteArray(packet, reconnectKey, false);
    packet.push_back(numEmptyActions);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSS_RECONNECT(uint32_t lastPacket)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_RECONNECT);
    packet.push_back(0);
    packet.push_back(0);
    UTIL_AppendByteArray(packet, lastPacket, false);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSS_ACK(uint32_t lastPacket)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_ACK);
    packet.push_back(0);
    packet.push_back(0);
    UTIL_AppendByteArray(packet, lastPacket, false);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSS_REJECT(uint32_t reason)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_REJECT);
    packet.push_back(0);
    packet.push_back(0);
    UTIL_AppendByteArray(packet, reason, false);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSS_DISCORD_PRESENCE_STATE(std::string state)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_DISCORD_PRESENCE_STATE);
    packet.push_back(0);
    packet.push_back(0);
    packet.push_back(state.size());
    UTIL_AppendByteArrayFast(packet, state, false);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSS_DISCORD_PRESENCE_DETAILS(std::string details)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_DISCORD_PRESENCE_DETAILS);
    packet.push_back(0);
    packet.push_back(0);
    packet.push_back(details.size());
    UTIL_AppendByteArrayFast(packet, details, false);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSS_DISCORD_PRESENCE_STARTTIMESTAMP(uint64_t startTimestamp)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_DISCORD_PRESENCE_STARTTIMESTAMP);
    packet.push_back(0);
    packet.push_back(0);
    UTIL_AppendByteArray(packet, startTimestamp, false);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSS_DISCORD_PRESENCE_ENDTIMESTAMP(uint64_t endTimestamp)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_DISCORD_PRESENCE_ENDTIMESTAMP);
    packet.push_back(0);
    packet.push_back(0);
    UTIL_AppendByteArray(packet, endTimestamp, false);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSS_DISCORD_PRESENCE_LARGEIMAGEKEY(std::string largeImageKey)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_DISCORD_PRESENCE_LARGEIMAGEKEY);
    packet.push_back(0);
    packet.push_back(0);
    packet.push_back(largeImageKey.size());
    UTIL_AppendByteArrayFast(packet, largeImageKey, false);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSS_DISCORD_PRESENCE_LARGEIMAGETEXT(std::string largeImageText)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_DISCORD_PRESENCE_LARGEIMAGETEXT);
    packet.push_back(0);
    packet.push_back(0);
    packet.push_back(largeImageText.size());
    UTIL_AppendByteArrayFast(packet, largeImageText, false);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSS_DISCORD_PRESENCE_SMALLIMAGEKEY(std::string smallImageKey)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_DISCORD_PRESENCE_SMALLIMAGEKEY);
    packet.push_back(0);
    packet.push_back(0);
    packet.push_back(smallImageKey.size());
    UTIL_AppendByteArrayFast(packet, smallImageKey, false);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSS_DISCORD_PRESENCE_SMALLIMAGETEXT(std::string smallImageText)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_DISCORD_PRESENCE_SMALLIMAGETEXT);
    packet.push_back(0);
    packet.push_back(0);
    packet.push_back(smallImageText.size());
    UTIL_AppendByteArrayFast(packet, smallImageText, false);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSS_DISCORD_PRESENCE_PARTYSIZE(uint32_t partySize)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_DISCORD_PRESENCE_PARTYSIZE);
    packet.push_back(0);
    packet.push_back(0);
    UTIL_AppendByteArray(packet, partySize, false);
    AssignLength(packet);
    return packet;
}

BYTEARRAY CGPSProtocol::SEND_GPSS_DISCORD_PRESENCE_PARTYMAX(uint32_t partyMax)
{
    BYTEARRAY packet;
    packet.push_back(GPS_HEADER_CONSTANT);
    packet.push_back(GPS_DISCORD_PRESENCE_PARTYMAX);
    packet.push_back(0);
    packet.push_back(0);
    UTIL_AppendByteArray(packet, partyMax, false);
    AssignLength(packet);
    return packet;
}

/////////////////////
// OTHER FUNCTIONS //
/////////////////////

bool CGPSProtocol::AssignLength(BYTEARRAY &content)
{
    // insert the actual length of the content array into bytes 3 and 4 (indices 2 and 3)

    BYTEARRAY LengthBytes;

    if (content.size() >= 4 && content.size() <= 65535)
    {
        LengthBytes = UTIL_CreateByteArray((uint16_t)content.size(), false);
        content[2]  = LengthBytes[0];
        content[3]  = LengthBytes[1];
        return true;
    }

    return false;
}

bool CGPSProtocol::ValidateLength(BYTEARRAY &content)
{
    // verify that bytes 3 and 4 (indices 2 and 3) of the content array describe the length

    uint16_t Length;
    BYTEARRAY LengthBytes;

    if (content.size() >= 4 && content.size() <= 65535)
    {
        LengthBytes.push_back(content[2]);
        LengthBytes.push_back(content[3]);
        Length = UTIL_ByteArrayToUInt16(LengthBytes, false);

        if (Length == content.size())
            return true;
    }

    return false;
}
