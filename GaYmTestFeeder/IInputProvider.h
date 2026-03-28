#pragma once
/*
 * GaYmTestFeeder - Abstract input provider interface.
 *
 * Each provider maps a different input source to GAYM_REPORT:
 *   KeyboardProvider  - WASD + arrows + keys → gamepad
 *   MouseProvider     - Mouse movement → right stick, buttons → triggers
 *   NetworkProvider   - Receives GAYM_REPORT over UDP
 *   MacroProvider     - Plays back recorded input from CSV
 *   ConfigProvider    - Static report from config (testing only)
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winioctl.h>
#include "../ioctl.h"

class IInputProvider {
public:
    /* Fill `report` with current input state. Called every poll cycle. */
    virtual void GetReport(GAYM_REPORT* report) = 0;

    /* Optional: initialize resources (sockets, files, etc.) */
    virtual bool Init() { return true; }

    /* Optional: clean up resources */
    virtual void Shutdown() {}

    /* Human-readable name for this provider */
    virtual const char* Name() const = 0;

    virtual ~IInputProvider() = default;
};
