#pragma once
/*
 * NetworkProvider - Receives GAYM_REPORT structs over UDP.
 *
 * Listens on a configurable UDP port. Remote clients send raw
 * GAYM_REPORT packets (48 bytes). Non-blocking receive on each poll.
 *
 * Use case: remote-control input from another machine, phone app, etc.
 */

#include <winsock2.h>
#include "IInputProvider.h"
#include <string>

class NetworkProvider : public IInputProvider {
public:
    NetworkProvider(const std::string& bindAddr = "127.0.0.1", int port = 43210);

    bool Init() override;
    void Shutdown() override;
    void GetReport(GAYM_REPORT* report) override;
    const char* Name() const override { return "Network (UDP)"; }

private:
    std::string bindAddr_;
    int         port_;
    SOCKET      sock_ = INVALID_SOCKET;
    GAYM_REPORT lastReport_ = {};
    bool        wsaInitialized_ = false;
};
