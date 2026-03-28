#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "DeviceHelper.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "xinput.lib")

struct JoySnapshot {
    JOYINFOEX info = {};
    bool valid = false;
};

struct JoystickCandidate {
    UINT id = 0;
    JOYCAPSW caps = {};
    JoySnapshot snapshot = {};
};

struct ObservedPadState {
    bool connected = false;
    DWORD packetChanges = 0;
    SHORT minLX = 0;
    SHORT maxLX = 0;
    WORD buttonsOr = 0;
};

static void InitNeutralReport(GAYM_REPORT* report)
{
    std::memset(report, 0, sizeof(*report));
    report->DPad = GAYM_DPAD_NEUTRAL;
}

static bool TryGetPadState(DWORD* index, XINPUT_STATE* state)
{
    for (DWORD candidate = 0; candidate < XUSER_MAX_COUNT; ++candidate) {
        XINPUT_STATE current = {};
        if (XInputGetState(candidate, &current) == ERROR_SUCCESS) {
            *index = candidate;
            *state = current;
            return true;
        }
    }

    return false;
}

static bool WaitForPad(DWORD timeoutMs, DWORD* index, XINPUT_STATE* state)
{
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        if (TryGetPadState(index, state)) {
            return true;
        }
        Sleep(25);
    }

    return false;
}

static bool IsInjectedTestPadState(const XINPUT_STATE& state)
{
    return (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0 &&
        state.Gamepad.sThumbLX > 20000;
}

static void UpdateObservedPadState(
    ObservedPadState* observed,
    const XINPUT_STATE& state,
    const XINPUT_STATE& baseline)
{
    if (!observed->connected) {
        observed->connected = true;
        observed->minLX = state.Gamepad.sThumbLX;
        observed->maxLX = state.Gamepad.sThumbLX;
    } else {
        if (state.Gamepad.sThumbLX < observed->minLX) {
            observed->minLX = state.Gamepad.sThumbLX;
        }
        if (state.Gamepad.sThumbLX > observed->maxLX) {
            observed->maxLX = state.Gamepad.sThumbLX;
        }
    }

    observed->buttonsOr |= state.Gamepad.wButtons;
    if (state.dwPacketNumber != baseline.dwPacketNumber) {
        observed->packetChanges++;
    }
}

static bool WaitForQuiescentPad(DWORD index, DWORD timeoutMs, XINPUT_STATE* state)
{
    XINPUT_STATE previous = {};
    bool havePrevious = false;
    UINT stableCount = 0;
    const DWORD start = GetTickCount();

    while (GetTickCount() - start < timeoutMs) {
        XINPUT_STATE current = {};
        if (XInputGetState(index, &current) != ERROR_SUCCESS) {
            havePrevious = false;
            stableCount = 0;
            Sleep(25);
            continue;
        }

        if (IsInjectedTestPadState(current)) {
            havePrevious = false;
            stableCount = 0;
            Sleep(25);
            continue;
        }

        if (havePrevious &&
            previous.dwPacketNumber == current.dwPacketNumber &&
            std::memcmp(&previous.Gamepad, &current.Gamepad, sizeof(current.Gamepad)) == 0) {
            ++stableCount;
        } else {
            previous = current;
            havePrevious = true;
            stableCount = 1;
        }

        if (stableCount >= 3) {
            *state = current;
            return true;
        }

        Sleep(25);
    }

    if (havePrevious) {
        *state = previous;
        return true;
    }

    return false;
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
        JoySnapshot snapshot = {};

        if (joyGetDevCapsW(id, &localCaps, sizeof(localCaps)) != JOYERR_NOERROR) {
            continue;
        }

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

static bool WaitForStableJoystick(UINT id, DWORD timeoutMs, JoySnapshot* snapshot)
{
    JoySnapshot previous = {};
    JoySnapshot current = {};
    UINT stableCount = 0;
    const DWORD start = GetTickCount();

    while (GetTickCount() - start < timeoutMs) {
        if (!QueryJoystick(id, &current)) {
            stableCount = 0;
            Sleep(25);
            continue;
        }

        if (stableCount != 0 &&
            previous.valid == current.valid &&
            std::memcmp(&previous.info, &current.info, sizeof(JOYINFOEX)) == 0) {
            ++stableCount;
        } else {
            stableCount = 1;
            previous = current;
        }

        if (stableCount >= 3) {
            *snapshot = current;
            return true;
        }

        Sleep(25);
    }

    if (current.valid) {
        *snapshot = current;
        return true;
    }

    return false;
}

static void PrintJoystickList(const std::vector<JoystickCandidate>& candidates)
{
    for (const JoystickCandidate& candidate : candidates) {
        std::printf(
            "Joystick %u: %ls  Baseline X=%lu Buttons=0x%08lX\n",
            candidate.id,
            candidate.caps.szPname,
            candidate.snapshot.info.dwXpos,
            candidate.snapshot.info.dwButtons);
    }
}

static bool TryObserveJoystickState(
    const std::vector<JoystickCandidate>& joysticks,
    UINT* observedJoystickId,
    JoySnapshot* observedJoy)
{
    for (const JoystickCandidate& joystick : joysticks) {
        JoySnapshot currentJoy = {};
        if (!QueryJoystick(joystick.id, &currentJoy)) {
            continue;
        }

        const LONG joyRange = joystick.caps.wXmax > joystick.caps.wXmin
            ? (LONG)(joystick.caps.wXmax - joystick.caps.wXmin)
            : 0xFFFF;
        const LONG midpoint = joystick.caps.wXmin + (joyRange / 2);
        const LONG threshold = joyRange / 4;
        const bool buttonA = (currentJoy.info.dwButtons & 0x1) != 0;
        const bool stickRight = (LONG)currentJoy.info.dwXpos > midpoint + threshold;
        if (!buttonA || !stickRight) {
            continue;
        }

        *observedJoystickId = joystick.id;
        *observedJoy = currentJoy;
        return true;
    }

    return false;
}

static bool VerifyOverrideDisabled()
{
    HANDLE device = OpenGaYmDeviceForTarget(GaYmControlTarget::Upper, 0);
    if (device == INVALID_HANDLE_VALUE) {
        return false;
    }

    GAYM_DEVICE_INFO info = {};
    const bool success = QueryDeviceInfo(device, &info) && !info.OverrideActive;
    CloseHandle(device);
    return success;
}

static void DisableOverride(HANDLE device, const GAYM_REPORT& neutral)
{
    const DWORD releaseStart = GetTickCount();
    while (GetTickCount() - releaseStart < 80) {
        InjectReport(device, &neutral);
        Sleep(8);
    }
    SendIoctl(device, IOCTL_GAYM_OVERRIDE_OFF);
}

static void SendNeutralBurst(HANDLE device, const GAYM_REPORT& neutral, DWORD durationMs)
{
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < durationMs) {
        InjectReport(device, &neutral);
        Sleep(8);
    }
}

int main()
{
    std::printf("GaYm hybrid verifier\n\n");

    HANDLE device = OpenGaYmDeviceForTarget(GaYmControlTarget::Upper, 0);
    if (device == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "ERROR: Cannot open upper control device (error %lu)\n", GetLastError());
        return 1;
    }

    GAYM_REPORT neutral = {};
    InitNeutralReport(&neutral);
    SendIoctl(device, IOCTL_GAYM_OVERRIDE_OFF);
    Sleep(100);

    DWORD padIndex = 0;
    XINPUT_STATE baselinePad = {};
    if (!WaitForPad(3000, &padIndex, &baselinePad)) {
        std::fprintf(stderr, "ERROR: No XInput pad found.\n");
        CloseHandle(device);
        return 1;
    }

    if (!WaitForQuiescentPad(padIndex, 1500, &baselinePad)) {
        std::fprintf(stderr, "ERROR: XInput pad did not settle to a quiescent baseline.\n");
        CloseHandle(device);
        return 1;
    }

    std::printf("XInput pad index: %lu\n", padIndex);
    std::printf("Baseline XInput: packet=%lu LX=%d buttons=0x%04X\n",
        baselinePad.dwPacketNumber,
        baselinePad.Gamepad.sThumbLX,
        baselinePad.Gamepad.wButtons);

    GAYM_REPORT report = {};
    InitNeutralReport(&report);
    report.Buttons[0] = GAYM_BTN_A;
    report.ThumbLeftX = 32767;

    ObservedPadState observedPad = {};
    bool sawXInput = false;

    if (!SendIoctl(device, IOCTL_GAYM_OVERRIDE_ON)) {
        std::fprintf(stderr, "ERROR: Failed to enable XInput override (error %lu)\n", GetLastError());
        CloseHandle(device);
        return 1;
    }

    SendNeutralBurst(device, neutral, 250);
    Sleep(80);
    if (!WaitForQuiescentPad(padIndex, 1000, &baselinePad)) {
        std::fprintf(stderr, "ERROR: XInput pad did not settle after neutral warmup.\n");
        DisableOverride(device, neutral);
        CloseHandle(device);
        return 1;
    }

    const DWORD xinputStart = GetTickCount();
    while (GetTickCount() - xinputStart < 1500) {
        if (!InjectReport(device, &report)) {
            std::fprintf(stderr, "ERROR: InjectReport failed during XInput phase (error %lu)\n", GetLastError());
            DisableOverride(device, neutral);
            CloseHandle(device);
            return 1;
        }

        XINPUT_STATE currentPad = {};
        if (XInputGetState(padIndex, &currentPad) == ERROR_SUCCESS) {
            UpdateObservedPadState(&observedPad, currentPad, baselinePad);
            if ((observedPad.buttonsOr & XINPUT_GAMEPAD_A) != 0 &&
                observedPad.maxLX - baselinePad.Gamepad.sThumbLX >= 20000) {
                sawXInput = true;
                break;
            }
        }

        Sleep(8);
    }

    DisableOverride(device, neutral);
    Sleep(150);

    JoySnapshot observedJoy = {};
    UINT observedJoystickId = 0;
    bool sawJoy = false;

    if (!SendIoctl(device, IOCTL_GAYM_OVERRIDE_ON)) {
        std::fprintf(stderr, "ERROR: Failed to enable WinMM override (error %lu)\n", GetLastError());
        CloseHandle(device);
        return 1;
    }

    const DWORD warmupStart = GetTickCount();
    while (GetTickCount() - warmupStart < 250) {
        if (!InjectReport(device, &report)) {
            std::fprintf(stderr, "ERROR: InjectReport failed during WinMM warmup (error %lu)\n", GetLastError());
            DisableOverride(device, neutral);
            CloseHandle(device);
            return 1;
        }
        Sleep(8);
    }

    std::vector<JoystickCandidate> joysticks = EnumerateJoysticks();
    if (joysticks.empty()) {
        std::fprintf(stderr, "ERROR: No WinMM joystick found after override activation.\n");
        DisableOverride(device, neutral);
        CloseHandle(device);
        return 1;
    }

    for (JoystickCandidate& joystick : joysticks) {
        JoySnapshot stable = {};
        if (WaitForStableJoystick(joystick.id, 500, &stable)) {
            joystick.snapshot = stable;
        }
    }

    PrintJoystickList(joysticks);
    std::printf("\n");

    const DWORD joyStart = GetTickCount();
    while (GetTickCount() - joyStart < 2000) {
        if (!InjectReport(device, &report)) {
            std::fprintf(stderr, "ERROR: InjectReport failed during WinMM phase (error %lu)\n", GetLastError());
            DisableOverride(device, neutral);
            CloseHandle(device);
            return 1;
        }

        if (TryObserveJoystickState(joysticks, &observedJoystickId, &observedJoy)) {
            sawJoy = true;
            break;
        }

        Sleep(8);
    }

    DisableOverride(device, neutral);
    CloseHandle(device);

    const bool overrideOff = VerifyOverrideDisabled();
    const bool pass = sawXInput && sawJoy && overrideOff;

    if (sawXInput) {
        std::printf("Observed XInput: pkt=%lu LX=[%d,%d] buttons=0x%04X\n",
            observedPad.packetChanges,
            observedPad.minLX,
            observedPad.maxLX,
            observedPad.buttonsOr);
    } else {
        XINPUT_STATE currentPad = {};
        if (XInputGetState(padIndex, &currentPad) == ERROR_SUCCESS) {
            std::printf("Current XInput: packet=%lu LX=%d buttons=0x%04X observedPkt=%lu observedLX=[%d,%d] observedBtns=0x%04X\n",
                currentPad.dwPacketNumber,
                currentPad.Gamepad.sThumbLX,
                currentPad.Gamepad.wButtons,
                observedPad.packetChanges,
                observedPad.minLX,
                observedPad.maxLX,
                observedPad.buttonsOr);
        }
    }

    if (sawJoy) {
        std::printf("Observed Joy joystick %u: X=%lu Buttons=0x%08lX\n",
            observedJoystickId,
            observedJoy.info.dwXpos,
            observedJoy.info.dwButtons);
    } else {
        for (const JoystickCandidate& joystick : joysticks) {
            JoySnapshot currentJoy = {};
            if (QueryJoystick(joystick.id, &currentJoy)) {
                std::printf("Current Joy joystick %u: X=%lu Buttons=0x%08lX\n",
                    joystick.id,
                    currentJoy.info.dwXpos,
                    currentJoy.info.dwButtons);
            }
        }
    }

    std::printf("Driver override after test: %s\n", overrideOff ? "OFF" : "UNKNOWN/ON");
    std::printf("\nResult: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 2;
}
