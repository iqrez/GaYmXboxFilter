#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
    JoySnapshot previous = {};
};

struct LowerSnapshot {
    ULONG rawLength = 0;
    UCHAR rawSample[GAYM_NATIVE_SAMPLE_BYTES] = {};
    ULONG semanticFlags = 0;
    ULONG semanticLength = 0;
    ULONG semanticIoctl = 0;
    ULONG semanticSampleLength = 0;
    UCHAR semanticSample[GAYM_NATIVE_SAMPLE_BYTES] = {};
    GAYM_REPORT semanticReport = {};
};

static volatile LONG g_Running = TRUE;

static BOOL WINAPI ConsoleHandler(DWORD eventType)
{
    if (eventType == CTRL_C_EVENT ||
        eventType == CTRL_BREAK_EVENT ||
        eventType == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_Running, FALSE);
        return TRUE;
    }

    return FALSE;
}

static DWORD ParsePositiveArg(int argc, char* argv[], const char* name, DWORD defaultValue)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (_stricmp(argv[index], name) == 0) {
            const long parsedValue = std::strtol(argv[index + 1], nullptr, 10);
            if (parsedValue > 0) {
                return static_cast<DWORD>(parsedValue);
            }
        }
    }

    return defaultValue;
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
        JOYCAPSW caps = {};
        if (joyGetDevCapsW(id, &caps, sizeof(caps)) != JOYERR_NOERROR) {
            continue;
        }

        JoySnapshot snapshot = {};
        if (!QueryJoystick(id, &snapshot)) {
            continue;
        }

        JoystickCandidate candidate = {};
        candidate.id = id;
        candidate.caps = caps;
        candidate.previous = snapshot;
        candidates.push_back(candidate);
    }

    return candidates;
}

static bool JoySnapshotEqual(const JoySnapshot& left, const JoySnapshot& right)
{
    return left.valid == right.valid &&
        std::memcmp(&left.info, &right.info, sizeof(JOYINFOEX)) == 0;
}

static void PrintHexBytes(const UCHAR* bytes, ULONG length)
{
    for (ULONG index = 0; index < length; ++index) {
        std::printf("%02X", bytes[index]);
    }
}

static void PrintCaptureFlags(ULONG flags)
{
    if (flags == 0) {
        std::printf("none");
        return;
    }

    bool wroteAny = false;
    auto printFlag = [&](ULONG mask, const char* label) {
        if ((flags & mask) == 0) {
            return;
        }

        if (wroteAny) {
            std::printf("|");
        }

        std::printf("%s", label);
        wroteAny = true;
    };

    printFlag(GAYM_CAPTURE_FLAG_VALID, "valid");
    printFlag(GAYM_CAPTURE_FLAG_PARTIAL, "partial");
    printFlag(GAYM_CAPTURE_FLAG_TRIGGERS_COMBINED, "combined-triggers");
    printFlag(GAYM_CAPTURE_FLAG_SOURCE_NATIVE_READ, "native-read");
}

static void PrintReportState(const GAYM_REPORT& report)
{
    std::printf(
        "btn0=0x%02X btn1=0x%02X dpad=%u lt=%u rt=%u lx=%d ly=%d rx=%d ry=%d",
        report.Buttons[0],
        report.Buttons[1],
        report.DPad,
        report.TriggerLeft,
        report.TriggerRight,
        report.ThumbLeftX,
        report.ThumbLeftY,
        report.ThumbRightX,
        report.ThumbRightY);
}

static void PrintJoySnapshot(const JoySnapshot& snapshot)
{
    std::printf(
        "X=%lu Y=%lu Z=%lu R=%lu U=%lu V=%lu POV=%lu Buttons=0x%08lX",
        snapshot.info.dwXpos,
        snapshot.info.dwYpos,
        snapshot.info.dwZpos,
        snapshot.info.dwRpos,
        snapshot.info.dwUpos,
        snapshot.info.dwVpos,
        snapshot.info.dwPOV,
        snapshot.info.dwButtons);
}

static void PrintJoyDiff(const JoySnapshot& previous, const JoySnapshot& current)
{
    bool wroteAny = false;

    auto printField = [&](const char* name, DWORD oldValue, DWORD newValue) {
        if (oldValue == newValue) {
            return;
        }

        if (!wroteAny) {
            std::printf(" diff:");
            wroteAny = true;
        }

        std::printf(" %s=%lu->%lu", name, oldValue, newValue);
    };

    printField("X", previous.info.dwXpos, current.info.dwXpos);
    printField("Y", previous.info.dwYpos, current.info.dwYpos);
    printField("Z", previous.info.dwZpos, current.info.dwZpos);
    printField("R", previous.info.dwRpos, current.info.dwRpos);
    printField("U", previous.info.dwUpos, current.info.dwUpos);
    printField("V", previous.info.dwVpos, current.info.dwVpos);
    printField("POV", previous.info.dwPOV, current.info.dwPOV);
    printField("Buttons", previous.info.dwButtons, current.info.dwButtons);

    if (!wroteAny) {
        std::printf(" diff: none");
    }
}

static void CaptureLowerSnapshot(const GAYM_DEVICE_INFO& info, LowerSnapshot* snapshot)
{
    std::memset(snapshot, 0, sizeof(*snapshot));
    snapshot->rawLength = std::min(info.LastRawReadSampleLength, (ULONG)GAYM_NATIVE_SAMPLE_BYTES);
    std::memcpy(snapshot->rawSample, info.LastRawReadSample, snapshot->rawLength);
    snapshot->semanticFlags = info.LastSemanticCaptureFlags;
    snapshot->semanticLength = info.LastSemanticCaptureLength;
    snapshot->semanticIoctl = info.LastSemanticCaptureIoctl;
    snapshot->semanticSampleLength = std::min(info.LastSemanticCaptureSampleLength, (ULONG)GAYM_NATIVE_SAMPLE_BYTES);
    std::memcpy(snapshot->semanticSample, info.LastSemanticCaptureSample, snapshot->semanticSampleLength);
    std::memcpy(&snapshot->semanticReport, &info.LastSemanticCaptureReport, sizeof(snapshot->semanticReport));
}

static bool LowerSnapshotEqual(const LowerSnapshot& left, const LowerSnapshot& right)
{
    return left.rawLength == right.rawLength &&
        left.semanticFlags == right.semanticFlags &&
        left.semanticLength == right.semanticLength &&
        left.semanticIoctl == right.semanticIoctl &&
        left.semanticSampleLength == right.semanticSampleLength &&
        std::memcmp(left.rawSample, right.rawSample, left.rawLength) == 0 &&
        std::memcmp(left.semanticSample, right.semanticSample, left.semanticSampleLength) == 0 &&
        std::memcmp(&left.semanticReport, &right.semanticReport, sizeof(GAYM_REPORT)) == 0;
}

static void PrintRawDiff(const LowerSnapshot& previous, const LowerSnapshot& current)
{
    const ULONG length = std::max(previous.rawLength, current.rawLength);
    bool wroteAny = false;

    for (ULONG index = 0; index < length; ++index) {
        const UCHAR oldValue = (index < previous.rawLength) ? previous.rawSample[index] : 0;
        const UCHAR newValue = (index < current.rawLength) ? current.rawSample[index] : 0;
        if (oldValue == newValue) {
            continue;
        }

        if (!wroteAny) {
            std::printf(" diff:");
            wroteAny = true;
        }

        std::printf(" [%lu]=%02X->%02X", index, oldValue, newValue);
    }

    if (!wroteAny) {
        std::printf(" diff: none");
    }
}

static HANDLE ReopenLowerDevice()
{
    SetEnvironmentVariableW(L"GAYM_CONTROL_TARGET", L"lower");
    return OpenGaYmDevice(0);
}

int main(int argc, char* argv[])
{
    const DWORD durationMs = ParsePositiveArg(argc, argv, "--duration-ms", 45000);
    const DWORD pollMs = ParsePositiveArg(argc, argv, "--poll-ms", 16);
    const DWORD retryMs = 250;
    const DWORD startTick = GetTickCount();
    DWORD transientFailures = 0;
    DWORD bytesReturned = 0;
    bool sawLowerChange = false;
    bool sawJoyChange = false;

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    SetEnvironmentVariableW(L"GAYM_CONTROL_TARGET", L"lower");

    HANDLE device = ReopenLowerDevice();
    if (device == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "ERROR: Cannot open lower filter control path (error %lu)\n", GetLastError());
        return 1;
    }

    GAYM_DEVICE_INFO baselineInfo = {};
    if (!QueryDeviceInfo(device, &baselineInfo, &bytesReturned)) {
        std::fprintf(stderr, "ERROR: QueryDeviceInfo failed (error %lu)\n", GetLastError());
        CloseHandle(device);
        return 1;
    }

    LowerSnapshot baselineLower = {};
    CaptureLowerSnapshot(baselineInfo, &baselineLower);

    std::vector<JoystickCandidate> joysticks = EnumerateJoysticks();

    std::printf("GaYm WinMM packet sniffer\n");
    std::printf(
        "Lower device: %s VID:%04X PID:%04X QueryBytes:%lu Layout:%lu Build:0x%08lX\n",
        DeviceTypeName(baselineInfo.DeviceType),
        baselineInfo.VendorId,
        baselineInfo.ProductId,
        bytesReturned,
        baselineInfo.QueryLayoutVersion,
        baselineInfo.DriverBuildStamp);

    std::printf("[lower baseline] rawLen=%lu raw=", baselineLower.rawLength);
    PrintHexBytes(baselineLower.rawSample, baselineLower.rawLength);
    std::printf("\n");
    std::printf("[lower baseline] flags=");
    PrintCaptureFlags(baselineLower.semanticFlags);
    std::printf(" ioctl=0x%08lX semLen=%lu ", baselineLower.semanticIoctl, baselineLower.semanticLength);
    PrintReportState(baselineLower.semanticReport);
    std::printf("\n");
    if (baselineLower.semanticSampleLength != 0) {
        std::printf("[lower baseline] semantic=");
        PrintHexBytes(baselineLower.semanticSample, baselineLower.semanticSampleLength);
        std::printf("\n");
    }

    if (joysticks.empty()) {
        std::printf("No joystick visible via joyGetPosEx at start.\n");
    } else {
        for (const JoystickCandidate& joystick : joysticks) {
            std::printf("[joy baseline] id=%u name=%ls ", joystick.id, joystick.caps.szPname);
            PrintJoySnapshot(joystick.previous);
            std::printf("\n");
        }
    }

    std::printf("\nListening for %lu ms. Move one physical control at a time.\n", durationMs);

    while (InterlockedCompareExchange(&g_Running, TRUE, TRUE) != FALSE &&
           GetTickCount() - startTick < durationMs) {
        Sleep(pollMs);

        GAYM_DEVICE_INFO currentInfo = {};
        if (!QueryDeviceInfo(device, &currentInfo, &bytesReturned)) {
            const DWORD error = GetLastError();
            transientFailures++;
            std::fprintf(stderr, "[warn] QueryDeviceInfo failed (error %lu), retry %lu\n", error, transientFailures);

            CloseHandle(device);
            device = INVALID_HANDLE_VALUE;
            Sleep(retryMs);
            device = ReopenLowerDevice();
            if (device == INVALID_HANDLE_VALUE) {
                if (transientFailures >= 10) {
                    std::fprintf(stderr, "ERROR: Lower control path did not recover.\n");
                    return 1;
                }
                continue;
            }

            if (!QueryDeviceInfo(device, &currentInfo, &bytesReturned)) {
                if (transientFailures >= 10) {
                    std::fprintf(stderr, "ERROR: Lower query did not recover.\n");
                    CloseHandle(device);
                    return 1;
                }
                continue;
            }
        }

        transientFailures = 0;

        LowerSnapshot currentLower = {};
        CaptureLowerSnapshot(currentInfo, &currentLower);
        if (!LowerSnapshotEqual(baselineLower, currentLower)) {
            sawLowerChange = true;
            std::printf("[%6lu ms] lower rawLen=%lu raw=", GetTickCount() - startTick, currentLower.rawLength);
            PrintHexBytes(currentLower.rawSample, currentLower.rawLength);
            std::printf("\n");
            std::printf("          ");
            PrintRawDiff(baselineLower, currentLower);
            std::printf("\n");
            std::printf("          flags=");
            PrintCaptureFlags(currentLower.semanticFlags);
            std::printf(" ioctl=0x%08lX semLen=%lu ", currentLower.semanticIoctl, currentLower.semanticLength);
            PrintReportState(currentLower.semanticReport);
            std::printf("\n");
            if (currentLower.semanticSampleLength != 0) {
                std::printf("          semantic=");
                PrintHexBytes(currentLower.semanticSample, currentLower.semanticSampleLength);
                std::printf("\n");
            }

            baselineLower = currentLower;
        }

        for (JoystickCandidate& joystick : joysticks) {
            JoySnapshot currentJoy = {};
            if (!QueryJoystick(joystick.id, &currentJoy)) {
                continue;
            }

            if (JoySnapshotEqual(joystick.previous, currentJoy)) {
                continue;
            }

            sawJoyChange = true;
            std::printf("[%6lu ms] joy id=%u ", GetTickCount() - startTick, joystick.id);
            PrintJoySnapshot(currentJoy);
            PrintJoyDiff(joystick.previous, currentJoy);
            std::printf("\n");
            joystick.previous = currentJoy;
        }
    }

    if (device != INVALID_HANDLE_VALUE) {
        CloseHandle(device);
    }

    if (!sawLowerChange) {
        std::printf("No lower-packet changes observed.\n");
    }

    if (!sawJoyChange) {
        std::printf("No WinMM joystick changes observed.\n");
    }

    return (sawLowerChange || sawJoyChange) ? 0 : 2;
}
