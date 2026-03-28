#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "DeviceHelper.h"

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

static bool SemanticStateEqual(const GAYM_DEVICE_INFO& left, const GAYM_DEVICE_INFO& right)
{
    const ULONG leftSampleLength = std::min(left.LastSemanticCaptureSampleLength, (ULONG)GAYM_NATIVE_SAMPLE_BYTES);
    const ULONG rightSampleLength = std::min(right.LastSemanticCaptureSampleLength, (ULONG)GAYM_NATIVE_SAMPLE_BYTES);

    return left.LastSemanticCaptureFlags == right.LastSemanticCaptureFlags &&
        left.LastSemanticCaptureLength == right.LastSemanticCaptureLength &&
        left.LastSemanticCaptureIoctl == right.LastSemanticCaptureIoctl &&
        leftSampleLength == rightSampleLength &&
        std::memcmp(&left.LastSemanticCaptureReport, &right.LastSemanticCaptureReport, sizeof(GAYM_REPORT)) == 0 &&
        std::memcmp(left.LastSemanticCaptureSample, right.LastSemanticCaptureSample, leftSampleLength) == 0;
}

static HANDLE ReopenLowerDevice()
{
    SetEnvironmentVariableW(L"GAYM_CONTROL_TARGET", L"lower");
    return OpenGaYmDevice(0);
}

int main(int argc, char* argv[])
{
    const DWORD durationMs = ParsePositiveArg(argc, argv, "--duration-ms", 45000);
    const DWORD pollMs = ParsePositiveArg(argc, argv, "--poll-ms", 15);
    const DWORD retryMs = 250;
    GAYM_DEVICE_INFO baseline = {};
    GAYM_DEVICE_INFO current = {};
    DWORD bytesReturned = 0;
    bool sawChange = false;
    DWORD transientFailures = 0;

    SetEnvironmentVariableW(L"GAYM_CONTROL_TARGET", L"lower");
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    HANDLE device = ReopenLowerDevice();
    if (device == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "ERROR: Cannot open lower filter control path (error %lu)\n", GetLastError());
        return 1;
    }

    if (!QueryDeviceInfo(device, &baseline, &bytesReturned)) {
        std::fprintf(stderr, "ERROR: QueryDeviceInfo failed (error %lu)\n", GetLastError());
        CloseHandle(device);
        return 1;
    }

    std::printf("GaYm lower semantic sniffer\n");
    std::printf("Device: %s VID:%04X PID:%04X QueryBytes:%lu Layout:%lu Build:0x%08lX\n",
        DeviceTypeName(baseline.DeviceType),
        baseline.VendorId,
        baseline.ProductId,
        bytesReturned,
        baseline.QueryLayoutVersion,
        baseline.DriverBuildStamp);
    std::printf("Listening for %lu ms. Spam one control at a time now.\n\n", durationMs);

    std::printf("[baseline] flags=");
    PrintCaptureFlags(baseline.LastSemanticCaptureFlags);
    std::printf(" ioctl=0x%08lX len=%lu ", baseline.LastSemanticCaptureIoctl, baseline.LastSemanticCaptureLength);
    PrintReportState(baseline.LastSemanticCaptureReport);
    std::printf("\n");
    if (baseline.LastSemanticCaptureSampleLength != 0) {
        std::printf("          sample=");
        PrintHexBytes(
            baseline.LastSemanticCaptureSample,
            std::min(baseline.LastSemanticCaptureSampleLength, (ULONG)GAYM_NATIVE_SAMPLE_BYTES));
        std::printf("\n");
    }

    const DWORD startTick = GetTickCount();
    while (InterlockedCompareExchange(&g_Running, TRUE, TRUE) != FALSE &&
           GetTickCount() - startTick < durationMs) {
        Sleep(pollMs);

        if (!QueryDeviceInfo(device, &current, &bytesReturned)) {
            const DWORD error = GetLastError();

            transientFailures++;
            std::fprintf(stderr,
                "[warn] QueryDeviceInfo failed during capture (error %lu), retry %lu\n",
                error,
                transientFailures);

            CloseHandle(device);
            device = INVALID_HANDLE_VALUE;
            Sleep(retryMs);

            device = ReopenLowerDevice();
            if (device == INVALID_HANDLE_VALUE) {
                if (transientFailures >= 10) {
                    std::fprintf(stderr, "ERROR: Lower filter control path did not recover.\n");
                    return 1;
                }
                continue;
            }

            if (!QueryDeviceInfo(device, &current, &bytesReturned)) {
                if (transientFailures >= 10) {
                    std::fprintf(stderr, "ERROR: Lower filter query did not recover.\n");
                    CloseHandle(device);
                    return 1;
                }
                continue;
            }
        }

        transientFailures = 0;

        if (device == INVALID_HANDLE_VALUE) {
            continue;
        }

        if (SemanticStateEqual(baseline, current)) {
            continue;
        }

        sawChange = true;
        std::printf("[%6lu ms] flags=", GetTickCount() - startTick);
        PrintCaptureFlags(current.LastSemanticCaptureFlags);
        std::printf(" ioctl=0x%08lX len=%lu ", current.LastSemanticCaptureIoctl, current.LastSemanticCaptureLength);
        PrintReportState(current.LastSemanticCaptureReport);
        std::printf("\n");

        if (current.LastSemanticCaptureSampleLength != 0) {
            std::printf("          sample=");
            PrintHexBytes(
                current.LastSemanticCaptureSample,
                std::min(current.LastSemanticCaptureSampleLength, (ULONG)GAYM_NATIVE_SAMPLE_BYTES));
            std::printf("\n");
        }

        baseline = current;
    }

    CloseHandle(device);

    if (!sawChange) {
        std::printf("No semantic capture changes observed.\n");
        return 2;
    }

    std::printf("Capture complete.\n");
    return 0;
}
