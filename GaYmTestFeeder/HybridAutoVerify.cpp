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
    JoySnapshot baseline = {};
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
        candidate.baseline = snapshot;
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
            candidate.baseline.info.dwXpos,
            candidate.baseline.info.dwButtons);
    }
}

static bool TryObservePadState(
    DWORD padIndex,
    const XINPUT_STATE& baselinePad,
    XINPUT_STATE* observedPad)
{
    XINPUT_STATE currentPad = {};
    if (XInputGetState(padIndex, &currentPad) != ERROR_SUCCESS) {
        return false;
    }

    const SHORT deltaLX = (SHORT)(currentPad.Gamepad.sThumbLX - baselinePad.Gamepad.sThumbLX);
    if ((currentPad.Gamepad.wButtons & XINPUT_GAMEPAD_A) == 0 ||
        deltaLX < 20000) {
        return false;
    }

    *observedPad = currentPad;
    return true;
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
        const bool movedFromBaseline =
            (((LONG)currentJoy.info.dwXpos - (LONG)joystick.baseline.info.dwXpos) > (threshold / 2)) ||
            (((currentJoy.info.dwButtons ^ joystick.baseline.info.dwButtons) & 0x1) != 0);
        if (!buttonA || !stickRight || !movedFromBaseline) {
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

int main()
{
    std::printf("GaYm hybrid verifier\n\n");

    HANDLE device = OpenGaYmDeviceForTarget(GaYmControlTarget::Upper, 0);
    if (device == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "ERROR: Cannot open upper control device (error %lu)\n", GetLastError());
        return 1;
    }

    DWORD padIndex = 0;
    XINPUT_STATE baselinePad = {};
    if (!WaitForPad(3000, &padIndex, &baselinePad)) {
        std::fprintf(stderr, "ERROR: No XInput pad found.\n");
        CloseHandle(device);
        return 1;
    }

    const std::vector<JoystickCandidate> joysticks = EnumerateJoysticks();
    if (joysticks.empty()) {
        std::fprintf(stderr, "ERROR: No WinMM joystick found.\n");
        CloseHandle(device);
        return 1;
    }

    for (JoystickCandidate& joystick : const_cast<std::vector<JoystickCandidate>&>(joysticks)) {
        JoySnapshot stable = {};
        if (WaitForStableJoystick(joystick.id, 500, &stable)) {
            joystick.baseline = stable;
        }
    }

    std::printf("XInput pad index: %lu\n", padIndex);
    std::printf("Baseline XInput: packet=%lu LX=%d buttons=0x%04X\n",
        baselinePad.dwPacketNumber,
        baselinePad.Gamepad.sThumbLX,
        baselinePad.Gamepad.wButtons);
    PrintJoystickList(joysticks);
    std::printf("\n");

    GAYM_REPORT neutral = {};
    InitNeutralReport(&neutral);

    bool overrideEnabled = false;
    auto cleanup = [&]() {
        if (overrideEnabled) {
            const DWORD releaseStart = GetTickCount();
            while (GetTickCount() - releaseStart < 80) {
                InjectReport(device, &neutral);
                Sleep(8);
            }
            SendIoctl(device, IOCTL_GAYM_OVERRIDE_OFF);
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

    XINPUT_STATE observedPad = {};
    JoySnapshot observedJoy = {};
    UINT observedJoystickId = 0;
    bool sawXInput = false;
    bool sawJoy = false;

    const DWORD injectStart = GetTickCount();
    while (GetTickCount() - injectStart < 1400) {
        if (!InjectReport(device, &report)) {
            std::fprintf(stderr, "ERROR: InjectReport failed (error %lu)\n", GetLastError());
            cleanup();
            return 1;
        }

        if (!sawXInput) {
            sawXInput = TryObservePadState(padIndex, baselinePad, &observedPad);
        }
        if (!sawJoy) {
            sawJoy = TryObserveJoystickState(joysticks, &observedJoystickId, &observedJoy);
        }
        if (sawXInput && sawJoy) {
            break;
        }

        Sleep(8);
    }

    cleanup();

    const bool overrideOff = VerifyOverrideDisabled();
    const bool pass = sawXInput && sawJoy && overrideOff;

    if (sawXInput) {
        std::printf("Observed XInput: packet=%lu LX=%d buttons=0x%04X\n",
            observedPad.dwPacketNumber,
            observedPad.Gamepad.sThumbLX,
            observedPad.Gamepad.wButtons);
    } else {
        XINPUT_STATE currentPad = {};
        if (XInputGetState(padIndex, &currentPad) == ERROR_SUCCESS) {
            std::printf("Current XInput: packet=%lu LX=%d buttons=0x%04X\n",
                currentPad.dwPacketNumber,
                currentPad.Gamepad.sThumbLX,
                currentPad.Gamepad.wButtons);
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
