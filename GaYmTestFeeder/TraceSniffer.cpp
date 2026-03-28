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

struct SampleSnapshot {
    UCHAR bytes[GAYM_NATIVE_SAMPLE_BYTES] = {};
    ULONG length = 0;
    ULONG sequence = 0;
    const char* source = "none";
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

static void PrintHexBytes(const UCHAR* bytes, ULONG length)
{
    for (ULONG index = 0; index < length; ++index) {
        std::printf("%02X", bytes[index]);
    }
}

static void PrintSampleDiff(const SampleSnapshot& previous, const SampleSnapshot& current)
{
    const ULONG maxLength = std::max(previous.length, current.length);
    bool printedAny = false;

    for (ULONG index = 0; index < maxLength; ++index) {
        const UCHAR oldValue = index < previous.length ? previous.bytes[index] : 0;
        const UCHAR newValue = index < current.length ? current.bytes[index] : 0;

        if (index < previous.length && index < current.length && oldValue == newValue) {
            continue;
        }

        if (!printedAny) {
            std::printf("  diff:");
            printedAny = true;
        }

        std::printf(" [%lu]=%02X->%02X", index, oldValue, newValue);
    }

    if (!printedAny) {
        std::printf("  diff: none");
    }

    std::printf("\n");
}

static bool SampleSnapshotsEqual(const SampleSnapshot& left, const SampleSnapshot& right)
{
    return left.length == right.length &&
        left.sequence == right.sequence &&
        std::memcmp(left.bytes, right.bytes, left.length) == 0;
}

static bool TryGetLatestReadCompletionSample(
    const GAYM_DEVICE_INFO& deviceInfo,
    SampleSnapshot* snapshot)
{
    const GAYM_TRACE_ENTRY* latestEntry = nullptr;

    for (ULONG index = 0; index < deviceInfo.TraceCount; ++index) {
        const GAYM_TRACE_ENTRY& entry = deviceInfo.Trace[index];
        if (entry.Phase != GAYM_TRACE_PHASE_COMPLETION ||
            entry.RequestType != GAYM_TRACE_REQUEST_READ ||
            entry.SampleLength == 0) {
            continue;
        }

        if (latestEntry == nullptr || entry.Sequence > latestEntry->Sequence) {
            latestEntry = &entry;
        }
    }

    if (latestEntry == nullptr) {
        return false;
    }

    std::memset(snapshot, 0, sizeof(*snapshot));
    snapshot->length = latestEntry->SampleLength;
    snapshot->sequence = latestEntry->Sequence;
    snapshot->source = "trace/read";
    std::memcpy(snapshot->bytes, latestEntry->Sample, latestEntry->SampleLength);
    return true;
}

static bool TryGetBestSample(const GAYM_DEVICE_INFO& deviceInfo, SampleSnapshot* snapshot)
{
    if (TryGetLatestReadCompletionSample(deviceInfo, snapshot)) {
        return true;
    }

    if (deviceInfo.LastRawReadSampleLength != 0) {
        std::memset(snapshot, 0, sizeof(*snapshot));
        snapshot->length = std::min<ULONG>(deviceInfo.LastRawReadSampleLength, GAYM_NATIVE_SAMPLE_BYTES);
        snapshot->sequence = deviceInfo.TraceSequence;
        snapshot->source = "raw";
        std::memcpy(snapshot->bytes, deviceInfo.LastRawReadSample, snapshot->length);
        return true;
    }

    if (deviceInfo.LastPatchedReadSampleLength != 0) {
        std::memset(snapshot, 0, sizeof(*snapshot));
        snapshot->length = std::min<ULONG>(deviceInfo.LastPatchedReadSampleLength, GAYM_NATIVE_SAMPLE_BYTES);
        snapshot->sequence = deviceInfo.TraceSequence;
        snapshot->source = "patched";
        std::memcpy(snapshot->bytes, deviceInfo.LastPatchedReadSample, snapshot->length);
        return true;
    }

    return false;
}

static ULONG ParsePositiveArg(int argc, char* argv[], const char* name, ULONG defaultValue)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (_stricmp(argv[index], name) == 0) {
            const long parsedValue = std::strtol(argv[index + 1], nullptr, 10);
            if (parsedValue > 0) {
                return static_cast<ULONG>(parsedValue);
            }
        }
    }

    return defaultValue;
}

int main(int argc, char* argv[])
{
    const ULONG durationMs = ParsePositiveArg(argc, argv, "--duration-ms", 30000);
    const ULONG pollMs = ParsePositiveArg(argc, argv, "--poll-ms", 15);
    DWORD bytesReturned = 0;
    GAYM_DEVICE_INFO deviceInfo = {};
    GAYM_DEVICE_INFO latestInfo = {};
    SampleSnapshot baseline = {};
    SampleSnapshot previous = {};
    bool haveBaseline = false;
    bool observedChange = false;

    SetEnvironmentVariableW(L"GAYM_CONTROL_TARGET", L"lower");
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    HANDLE device = OpenGaYmDevice(0);
    if (device == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "ERROR: Cannot open lower filter control path (error %lu)\n", GetLastError());
        return 1;
    }

    if (!QueryDeviceInfo(device, &deviceInfo, &bytesReturned)) {
        std::fprintf(stderr, "ERROR: QueryDeviceInfo failed (error %lu)\n", GetLastError());
        CloseHandle(device);
        return 1;
    }

    std::printf("GaYm lower-path trace sniffer\n");
    std::printf("Device: %s VID:%04X PID:%04X QueryBytes:%lu Layout:%lu Build:0x%08lX\n",
        DeviceTypeName(deviceInfo.DeviceType),
        deviceInfo.VendorId,
        deviceInfo.ProductId,
        bytesReturned,
        deviceInfo.QueryLayoutVersion,
        deviceInfo.DriverBuildStamp);
    std::printf("Listening for physical input changes on the lower HID path for %lu ms.\n", durationMs);
    std::printf("Spam a button, trigger, or stick now.\n\n");

    if (TryGetBestSample(deviceInfo, &baseline)) {
        haveBaseline = true;
        previous = baseline;
        std::printf("[baseline] seq=%lu source=%s len=%lu sample=", baseline.sequence, baseline.source, baseline.length);
        PrintHexBytes(baseline.bytes, baseline.length);
        std::printf("\n");
    } else {
        std::printf("[baseline] no read sample available yet\n");
    }

    const DWORD startTick = GetTickCount();
    while (InterlockedCompareExchange(&g_Running, TRUE, TRUE) != FALSE &&
           GetTickCount() - startTick < durationMs) {
        Sleep(pollMs);

        if (!QueryDeviceInfo(device, &latestInfo, &bytesReturned)) {
            std::fprintf(stderr, "ERROR: QueryDeviceInfo failed during capture (error %lu)\n", GetLastError());
            CloseHandle(device);
            return 1;
        }

        SampleSnapshot current = {};
        if (!TryGetBestSample(latestInfo, &current)) {
            continue;
        }

        if (!haveBaseline) {
            baseline = current;
            previous = current;
            haveBaseline = true;
            std::printf("[baseline] seq=%lu source=%s len=%lu sample=", current.sequence, current.source, current.length);
            PrintHexBytes(current.bytes, current.length);
            std::printf("\n");
            continue;
        }

        if (SampleSnapshotsEqual(previous, current)) {
            continue;
        }

        observedChange = true;
        std::printf(
            "[%6lu ms] seq=%lu source=%s len=%lu read=%lu devctl=%lu pending=%lu done=%lu sample=",
            GetTickCount() - startTick,
            current.sequence,
            current.source,
            current.length,
            latestInfo.ReadRequestsSeen,
            latestInfo.DeviceControlRequestsSeen,
            latestInfo.PendingInputRequests,
            latestInfo.CompletedInputRequests);
        PrintHexBytes(current.bytes, current.length);
        std::printf("\n");
        PrintSampleDiff(previous, current);
        previous = current;
    }

    CloseHandle(device);

    if (!observedChange) {
        std::printf("\nNo sample changes observed.\n");
        return 2;
    }

    std::printf("\nCapture complete.\n");
    return 0;
}
