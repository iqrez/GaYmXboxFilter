#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "DeviceHelper.h"

#pragma comment(lib, "winmm.lib")

struct JoySnapshot {
    JOYINFOEX info = {};
    bool valid = false;
};

struct JoystickCandidate {
    UINT id = 0;
    JOYCAPSW caps = {};
    JoySnapshot snapshot = {};
};

static void InitNeutralReport(GAYM_REPORT* report)
{
    std::memset(report, 0, sizeof(*report));
    report->DPad = GAYM_DPAD_NEUTRAL;
}

static bool QueryJoystick(UINT id, JoySnapshot* snapshot)
{
    std::memset(snapshot, 0, sizeof(*snapshot));
    snapshot->info.dwSize = sizeof(snapshot->info);
    snapshot->info.dwFlags = JOY_RETURNALL;
    snapshot->valid = joyGetPosEx(id, &snapshot->info) == JOYERR_NOERROR;
    return snapshot->valid;
}

static std::vector<JoystickCandidate> EnumerateJoysticks()
{
    std::vector<JoystickCandidate> candidates;
    const UINT count = joyGetNumDevs();

    for (UINT id = 0; id < count; ++id) {
        JOYCAPSW localCaps = {};
        if (joyGetDevCapsW(id, &localCaps, sizeof(localCaps)) != JOYERR_NOERROR) {
            continue;
        }

        JoySnapshot snapshot = {};
        if (!QueryJoystick(id, &snapshot)) {
            continue;
        }

        JoystickCandidate candidate = {};
        candidate.id = id;
        candidate.caps = localCaps;
        candidate.snapshot = snapshot;
        candidates.push_back(candidate);
    }

    return candidates;
}

static std::vector<JoystickCandidate> WaitForJoysticks(DWORD timeoutMs)
{
    const DWORD start = GetTickCount();
    do {
        std::vector<JoystickCandidate> candidates = EnumerateJoysticks();
        if (!candidates.empty()) {
            return candidates;
        }

        Sleep(50);
    } while (GetTickCount() - start < timeoutMs);

    return {};
}

static void PrintJoystickList(const std::vector<JoystickCandidate>& candidates)
{
    for (const JoystickCandidate& candidate : candidates) {
        std::printf(
            "Joystick %u: %ls  Baseline X=%lu Y=%lu Buttons=0x%08lX\n",
            candidate.id,
            candidate.caps.szPname,
            candidate.snapshot.info.dwXpos,
            candidate.snapshot.info.dwYpos,
            candidate.snapshot.info.dwButtons);
    }
}

static bool TryObserveJoystickState(
    const std::vector<JoystickCandidate>& candidates,
    UINT* matchedJoystickId,
    JoySnapshot* matched)
{
    for (const JoystickCandidate& candidate : candidates) {
        JoySnapshot current = {};
        if (!QueryJoystick(candidate.id, &current)) {
            continue;
        }

        const DWORD xRange = candidate.caps.wXmax > candidate.caps.wXmin
            ? (DWORD)(candidate.caps.wXmax - candidate.caps.wXmin)
            : 0xFFFF;
        const DWORD midpoint = candidate.caps.wXmin + (xRange / 2);
        const DWORD threshold = xRange / 4;
        const bool buttonA = (current.info.dwButtons & 0x1) != 0;
        const bool xMovedRight = current.info.dwXpos > midpoint + threshold;

        if (buttonA && xMovedRight) {
            *matchedJoystickId = candidate.id;
            *matched = current;
            return true;
        }
    }

    return false;
}

int main()
{
    std::printf("GaYm joyGetPosEx verifier\n\n");

    HANDLE device = OpenGaYmDevice(0);
    if (device == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "ERROR: Cannot open GaYm device (error %lu)\n", GetLastError());
        return 1;
    }

    GAYM_REPORT neutral = {};
    InitNeutralReport(&neutral);

    bool overrideEnabled = false;
    auto cleanup = [&]() {
        if (overrideEnabled) {
            HANDLE cleanupDevice = device != INVALID_HANDLE_VALUE ? device : OpenGaYmDevice(0);
            const DWORD releaseStart = GetTickCount();
            while (GetTickCount() - releaseStart < 80) {
                if (cleanupDevice != INVALID_HANDLE_VALUE) {
                    InjectReport(cleanupDevice, &neutral);
                }
                Sleep(8);
            }

            if (cleanupDevice != INVALID_HANDLE_VALUE) {
                SendIoctlRaw(cleanupDevice, IOCTL_GAYM_OVERRIDE_OFF, nullptr, 0, nullptr, 0);
                GaYmMirrorIoctlToLower(IOCTL_GAYM_OVERRIDE_OFF);
            } else {
                GaYmMirrorIoctlToLower(IOCTL_GAYM_OVERRIDE_OFF);
            }

            if (cleanupDevice != INVALID_HANDLE_VALUE && cleanupDevice != device) {
                CloseHandle(cleanupDevice);
            }
            overrideEnabled = false;
        }
        CloseHandle(device);
    };

    if (!SendIoctl(device, IOCTL_GAYM_OVERRIDE_ON)) {
        std::fprintf(stderr, "ERROR: Failed to enable override (error %lu)\n", GetLastError());
        CloseHandle(device);
        return 1;
    }
    overrideEnabled = true;

    GAYM_REPORT report = {};
    InitNeutralReport(&report);
    report.Buttons[0] = GAYM_BTN_A;
    report.ThumbLeftX = 32767;

    // Prime the native consumer path before opening WinMM handles.
    const DWORD warmupStart = GetTickCount();
    while (GetTickCount() - warmupStart < 250) {
        if (!InjectReport(device, &report)) {
            std::fprintf(stderr, "ERROR: InjectReport failed during warmup (error %lu)\n", GetLastError());
            cleanup();
            return 1;
        }
        Sleep(8);
    }

    std::vector<JoystickCandidate> joysticks = WaitForJoysticks(2000);
    if (joysticks.empty()) {
        std::fprintf(stderr, "ERROR: No joystick visible via joyGetPosEx after override activation.\n");
        cleanup();
        return 1;
    }

    PrintJoystickList(joysticks);

    JoySnapshot observed = {};
    UINT observedJoystickId = 0;
    bool pass = false;

    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 2000) {
        if (!InjectReport(device, &report)) {
            std::fprintf(stderr, "ERROR: InjectReport failed (error %lu)\n", GetLastError());
            cleanup();
            return 1;
        }

        if (TryObserveJoystickState(joysticks, &observedJoystickId, &observed)) {
            pass = true;
            break;
        }

        Sleep(8);
    }

    cleanup();

    GAYM_DEVICE_INFO after = {};
    bool haveAfter = false;
    HANDLE verifyDevice = OpenGaYmDevice(0);
    if (verifyDevice != INVALID_HANDLE_VALUE) {
        haveAfter = QueryDeviceInfo(verifyDevice, &after);
        CloseHandle(verifyDevice);
    }

    if (pass) {
        std::printf("Observed joystick %u: X=%lu Y=%lu Buttons=0x%08lX\n",
            observedJoystickId,
            observed.info.dwXpos,
            observed.info.dwYpos,
            observed.info.dwButtons);
    } else {
        for (const JoystickCandidate& joystick : joysticks) {
            JoySnapshot current = {};
            if (QueryJoystick(joystick.id, &current)) {
                std::printf("Current joystick %u: X=%lu Y=%lu Buttons=0x%08lX\n",
                    joystick.id,
                    current.info.dwXpos,
                    current.info.dwYpos,
                    current.info.dwButtons);
            }
        }
    }

    if (haveAfter) {
        std::printf("Driver override after test: %s\n", after.OverrideActive ? "ON" : "OFF");
    }

    std::printf("\nResult: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 2;
}
