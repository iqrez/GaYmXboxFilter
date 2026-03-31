/*
 * NetworkProvider - UDP input receiver.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include "NetworkProvider.h"
#include <cstdio>

#pragma comment(lib, "ws2_32.lib")

NetworkProvider::NetworkProvider(const std::string& bindAddr, int port)
    : bindAddr_(bindAddr), port_(port)
{
    RtlZeroMemory(&lastReport_, sizeof(lastReport_));
    lastReport_.DPad = GAYM_DPAD_NEUTRAL;
}

bool NetworkProvider::Init()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[Network] WSAStartup failed: %d\n", WSAGetLastError());
        return false;
    }
    wsaInitialized_ = true;

    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) {
        fprintf(stderr, "[Network] socket() failed: %d\n", WSAGetLastError());
        return false;
    }

    /* Set non-blocking */
    u_long nonBlocking = 1;
    ioctlsocket(sock_, FIONBIO, &nonBlocking);

    /* Bind */
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u_short)port_);
    inet_pton(AF_INET, bindAddr_.c_str(), &addr.sin_addr);

    if (bind(sock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[Network] bind(%s:%d) failed: %d\n",
            bindAddr_.c_str(), port_, WSAGetLastError());
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        return false;
    }

    printf("[Network] Listening on %s:%d\n", bindAddr_.c_str(), port_);
    return true;
}

void NetworkProvider::Shutdown()
{
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    if (wsaInitialized_) {
        WSACleanup();
        wsaInitialized_ = false;
    }
}

void NetworkProvider::GetReport(GAYM_REPORT* report)
{
    if (sock_ == INVALID_SOCKET) {
        *report = lastReport_;
        return;
    }

    /* Non-blocking receive: drain all pending packets, keep the latest */
    GAYM_REPORT incoming;
    sockaddr_in from;
    int fromLen = sizeof(from);

    while (true) {
        int received = recvfrom(sock_, (char*)&incoming, sizeof(incoming), 0,
                                (sockaddr*)&from, &fromLen);
        if (received == sizeof(GAYM_REPORT)) {
            lastReport_ = incoming;
        } else {
            break;  /* No more packets */
        }
    }

    *report = lastReport_;
}
