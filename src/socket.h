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

#pragma once

#include "includes.h"

#ifdef WIN32
#include <winsock2.h>
#include <errno.h>

#undef EADDRINUSE
#define EADDRINUSE WSAEADDRINUSE
#undef EADDRNOTAVAIL
#define EADDRNOTAVAIL WSAEADDRNOTAVAIL
#undef EAFNOSUPPORT
#define EAFNOSUPPORT WSAEAFNOSUPPORT
#undef EALREADY
#define EALREADY WSAEALREADY
#undef ECONNABORTED
#define ECONNABORTED WSAECONNABORTED
#undef ECONNREFUSED
#define ECONNREFUSED WSAECONNREFUSED
#undef ECONNRESET
#define ECONNRESET WSAECONNRESET
#undef EDESTADDRREQ
#define EDESTADDRREQ WSAEDESTADDRREQ
#undef EDQUOT
#define EDQUOT WSAEDQUOT
#undef EHOSTDOWN
#define EHOSTDOWN WSAEHOSTDOWN
#undef EHOSTUNREACH
#define EHOSTUNREACH WSAEHOSTUNREACH
#undef EINPROGRESS
#define EINPROGRESS WSAEINPROGRESS
#undef EISCONN
#define EISCONN WSAEISCONN
#undef ELOOP
#define ELOOP WSAELOOP
#undef EMSGSIZE
#define EMSGSIZE WSAEMSGSIZE
// #undef ENAMETOOLONG
// #define ENAMETOOLONG WSAENAMETOOLONG
#undef ENETDOWN
#define ENETDOWN WSAENETDOWN
#undef ENETRESET
#define ENETRESET WSAENETRESET
#undef ENETUNREACH
#define ENETUNREACH WSAENETUNREACH
#undef ENOBUFS
#define ENOBUFS WSAENOBUFS
#undef ENOPROTOOPT
#define ENOPROTOOPT WSAENOPROTOOPT
#undef ENOTCONN
#define ENOTCONN WSAENOTCONN
// #undef ENOTEMPTY
// #define ENOTEMPTY WSAENOTEMPTY
#undef ENOTSOCK
#define ENOTSOCK WSAENOTSOCK
#undef EOPNOTSUPP
#define EOPNOTSUPP WSAEOPNOTSUPP
#undef EPFNOSUPPORT
#define EPFNOSUPPORT WSAEPFNOSUPPORT
#undef EPROTONOSUPPORT
#define EPROTONOSUPPORT WSAEPROTONOSUPPORT
#undef EPROTOTYPE
#define EPROTOTYPE WSAEPROTOTYPE
#undef EREMOTE
#define EREMOTE WSAEREMOTE
#undef ESHUTDOWN
#define ESHUTDOWN WSAESHUTDOWN
#undef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT WSAESOCKTNOSUPPORT
#undef ESTALE
#define ESTALE WSAESTALE
#undef ETIMEDOUT
#define ETIMEDOUT WSAETIMEDOUT
#undef ETOOMANYREFS
#define ETOOMANYREFS WSAETOOMANYREFS
#undef EUSERS
#define EUSERS WSAEUSERS
#undef EWOULDBLOCK
#define EWOULDBLOCK WSAEWOULDBLOCK
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef int SOCKET;

#define INVALID_SOCKET -1
#define SOCKET_ERROR -1

#define closesocket close
#endif

#ifndef INADDR_NONE
#define INADDR_NONE -1
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifdef WIN32
#define SHUT_RDWR 2
#endif

//
// CSocket
//

class CSocket
{
protected:
    SOCKET m_Socket;
    struct sockaddr_in m_SIN;
    bool m_HasError;
    int m_Error;
    std::string m_Name = "";

    ~CSocket();

public:
    CSocket(std::string nName = "");
    CSocket(SOCKET nSocket, struct sockaddr_in nSIN, std::string nName = "");

    virtual BYTEARRAY GetPort();
    virtual BYTEARRAY GetIP();
    virtual std::string GetIPString();
    virtual bool HasError() { return m_HasError; }
    virtual int GetError() { return m_Error; }
    virtual std::string GetErrorString();
    virtual std::string GetName();
    virtual void SetFD(fd_set *fd, fd_set *send_fd, int *nfds);
    virtual void Allocate(int type);
    virtual void Reset();
};

//
// CTCPSocket
//

class CTCPSocket : public CSocket
{
protected:
    bool m_Connected;
    std::string m_LogFile;

private:
    std::string m_RecvBuffer;
    std::string m_SendBuffer;
    uint32_t m_LastRecv;
    uint32_t m_LastSend;

public:
    CTCPSocket(std::string nName = "");
    CTCPSocket(SOCKET nSocket, struct sockaddr_in nSIN, std::string nName = "");
    virtual ~CTCPSocket();

    virtual void Reset();
    virtual bool GetConnected() { return m_Connected; }
    virtual std::string *GetBytes() { return &m_RecvBuffer; }
    virtual void PutBytes(std::string bytes);
    virtual void PutBytes(BYTEARRAY bytes);
    virtual void ClearRecvBuffer() { m_RecvBuffer.clear(); }
    virtual void ClearSendBuffer() { m_SendBuffer.clear(); }
    virtual uint32_t GetLastRecv() { return m_LastRecv; }
    virtual uint32_t GetLastSend() { return m_LastSend; }
    virtual void DoRecv(fd_set *fd);
    virtual void DoSend(fd_set *send_fd);
    virtual void Disconnect();
    virtual void SetNoDelay(bool noDelay);
    virtual void SetLogFile(std::string nLogFile) { m_LogFile = nLogFile; }
};

//
// CTCPClient
//

class CTCPClient : public CTCPSocket
{
protected:
    bool m_Connecting;

public:
    CTCPClient(std::string nName = "");
    virtual ~CTCPClient();

    virtual void Reset();
    virtual void Disconnect();
    virtual bool GetConnecting() { return m_Connecting; }
    virtual void Connect(std::string localaddress, std::string address, uint16_t port);
    virtual bool CheckConnect();
};

//
// CTCPServer
//

class CTCPServer : public CTCPSocket
{
public:
    CTCPServer(std::string nName = "");
    virtual ~CTCPServer();

    virtual bool Listen(std::string address, uint16_t port);
    virtual CTCPSocket *Accept(fd_set *fd);
};

//
// CUDPSocket
//

class CUDPSocket : public CSocket
{
protected:
    struct in_addr m_BroadcastTarget;

public:
    CUDPSocket(std::string nName = "");
    virtual ~CUDPSocket();

    virtual bool SendTo(struct sockaddr_in sin, BYTEARRAY message);
    virtual bool SendTo(std::string address, uint16_t port, BYTEARRAY message);
    virtual bool Broadcast(uint16_t port, BYTEARRAY message);
    virtual void SetBroadcastTarget(std::string subnet);
    virtual void SetDontRoute(bool dontRoute);
};

//
// CUDPServer
//

class CUDPServer : public CUDPSocket
{
public:
    CUDPServer(std::string nName = "");
    virtual ~CUDPServer();

    virtual bool Bind(struct sockaddr_in sin);
    virtual bool Bind(std::string address, uint16_t port);
    virtual void RecvFrom(fd_set *fd, struct sockaddr_in *sin, std::string *message);
};

//обертка для TCP сокета
class CTCPStatusBroadcasterSocket
{
public:
    CTCPStatusBroadcasterSocket(CTCPSocket *socket);
    virtual ~CTCPStatusBroadcasterSocket();

    CTCPSocket *socket;
};
