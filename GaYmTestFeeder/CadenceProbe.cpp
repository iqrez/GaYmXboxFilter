#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "DeviceHelper.h"

namespace {

struct ProbeConfig {
    long DurationSeconds = 10;
    long IntervalMs = 500;
    int DeviceIndex = 0;
    GaYmControlTarget Target = GaYmControlTarget::Lower;
};

static bool ParseLongArgument(const char* text, long minValue, long maxValue, long* value)
{
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < minValue || parsed > maxValue) {
        return false;
    }

    *value = parsed;
    return true;
}

static const char* ControlTargetName(GaYmControlTarget target)
{
    switch (target) {
    case GaYmControlTarget::Upper:
        return "upper";
    case GaYmControlTarget::Lower:
        return "lower";
    case GaYmControlTarget::Legacy:
        return "legacy";
    case GaYmControlTarget::Auto:
    default:
        return "auto";
    }
}

static bool ParseControlTargetArgument(const char* text, GaYmControlTarget* target)
{
    if (_stricmp(text, "upper") == 0) {
        *target = GaYmControlTarget::Upper;
        return true;
    }
    if (_stricmp(text, "lower") == 0) {
        *target = GaYmControlTarget::Lower;
        return true;
    }
    if (_stricmp(text, "legacy") == 0) {
        *target = GaYmControlTarget::Legacy;
        return true;
    }
    if (_stricmp(text, "auto") == 0) {
        *target = GaYmControlTarget::Auto;
        return true;
    }

    return false;
}

static void PrintUsage()
{
    std::printf("CadenceProbe.exe [seconds] [interval_ms] [target] [device_index]\n");
    std::printf("  seconds      : 1-3600 (default 10)\n");
    std::printf("  interval_ms  : 50-5000 (default 500)\n");
    std::printf("  target       : lower|upper|auto|legacy (default lower)\n");
    std::printf("  device_index : 0-64 (default 0)\n");
}

static bool ParseArgs(int argc, char* argv[], ProbeConfig* config)
{
    if (argc >= 2 && !ParseLongArgument(argv[1], 1, 3600, &config->DurationSeconds)) {
        return false;
    }
    if (argc >= 3 && !ParseLongArgument(argv[2], 50, 5000, &config->IntervalMs)) {
        return false;
    }
    if (argc >= 4 && !ParseControlTargetArgument(argv[3], &config->Target)) {
        return false;
    }
    if (argc >= 5) {
        long deviceIndex = 0;
        if (!ParseLongArgument(argv[4], 0, 64, &deviceIndex)) {
            return false;
        }
        config->DeviceIndex = static_cast<int>(deviceIndex);
    }

    return true;
}

static bool QueryProbeState(
    HANDLE device,
    GAYM_QUERY_DEVICE_SUMMARY_RESPONSE* summary,
    GAYM_QUERY_RUNTIME_COUNTERS_RESPONSE* runtime,
    GAYM_QUERY_LAST_IO_RESPONSE* lastIo)
{
    return QueryDeviceSummary(device, summary) &&
        QueryRuntimeCounters(device, runtime) &&
        QueryLastIoSnapshot(device, lastIo);
}

static void PrintHeader(
    const ProbeConfig& config,
    const GAYM_QUERY_DEVICE_SUMMARY_RESPONSE& summary)
{
    std::printf(
        "Cadence probe target=%s device=%d duration=%ld s interval=%ld ms\n",
        ControlTargetName(config.Target),
        config.DeviceIndex,
        config.DurationSeconds,
        config.IntervalMs);
    std::printf(
        "Device: %s VID:%04X PID:%04X override=%s reports=%lu\n\n",
        DeviceTypeName(summary.Payload.DeviceType),
        summary.Payload.VendorId,
        summary.Payload.ProductId,
        summary.Payload.OverrideActive ? "ON" : "OFF",
        summary.Payload.ReportsSent);
}

static void PrintSample(
    DWORD elapsedMs,
    const GAYM_RUNTIME_COUNTERS& previousRuntime,
    const GAYM_RUNTIME_COUNTERS& currentRuntime,
    const GAYM_LAST_IO_SNAPSHOT& lastIo)
{
    const ULONG readDelta = currentRuntime.ReadRequestsSeen - previousRuntime.ReadRequestsSeen;
    const ULONG writeDelta = currentRuntime.WriteRequestsSeen - previousRuntime.WriteRequestsSeen;
    const ULONG devctlDelta = currentRuntime.DeviceControlRequestsSeen - previousRuntime.DeviceControlRequestsSeen;
    const ULONG intctlDelta =
        currentRuntime.InternalDeviceControlRequestsSeen -
        previousRuntime.InternalDeviceControlRequestsSeen;
    const ULONG doneDelta =
        currentRuntime.CompletedInputRequests -
        previousRuntime.CompletedInputRequests;

    std::printf(
        "[%5lu ms] read=%-4lu write=%-3lu devctl=%-4lu intctl=%-3lu done=%-4lu "
        "pending=%-3lu lastIoctl=0x%08lX raw=%-3lu semantic=%-3lu flags=0x%08lX\n",
        elapsedMs,
        readDelta,
        writeDelta,
        devctlDelta,
        intctlDelta,
        doneDelta,
        currentRuntime.PendingInputRequests,
        currentRuntime.LastInterceptedIoctl,
        lastIo.LastRawReadSampleLength,
        lastIo.LastSemanticCaptureLength,
        lastIo.LastSemanticCaptureFlags);
}

static int RunProbe(const ProbeConfig& config)
{
    HANDLE device = INVALID_HANDLE_VALUE;
    GAYM_QUERY_DEVICE_SUMMARY_RESPONSE summary = {};
    GAYM_QUERY_RUNTIME_COUNTERS_RESPONSE runtime = {};
    GAYM_QUERY_LAST_IO_RESPONSE lastIo = {};
    GAYM_RUNTIME_COUNTERS previousRuntime = {};
    const DWORD startTick = GetTickCount();
    DWORD nextTick = startTick;
    DWORD lastSampleTick = startTick;

    device = OpenGaYmDeviceForTarget(config.Target, config.DeviceIndex);
    if (device == INVALID_HANDLE_VALUE) {
        std::fprintf(
            stderr,
            "Failed to open %s target device %d (error %lu).\n",
            ControlTargetName(config.Target),
            config.DeviceIndex,
            GetLastError());
        return 1;
    }

    if (!QueryProbeState(device, &summary, &runtime, &lastIo)) {
        std::fprintf(stderr, "Initial probe query failed (error %lu).\n", GetLastError());
        CloseHandle(device);
        return 1;
    }

    PrintHeader(config, summary);
    previousRuntime = runtime.Payload;

    while ((LONG)(GetTickCount() - startTick) < static_cast<LONG>(config.DurationSeconds * 1000)) {
        nextTick += static_cast<DWORD>(config.IntervalMs);
        Sleep(config.IntervalMs);

        if (!QueryProbeState(device, &summary, &runtime, &lastIo)) {
            const DWORD error = GetLastError();
            std::fprintf(stderr, "[transient] query failed (error %lu); reopening device\n", error);
            CloseHandle(device);
            device = OpenGaYmDeviceForTarget(config.Target, config.DeviceIndex);
            if (device == INVALID_HANDLE_VALUE) {
                std::fprintf(stderr, "Reopen failed (error %lu).\n", GetLastError());
                return 1;
            }
            continue;
        }

        PrintSample(
            GetTickCount() - startTick,
            previousRuntime,
            runtime.Payload,
            lastIo.Payload);
        previousRuntime = runtime.Payload;
        lastSampleTick = GetTickCount();
    }

    std::printf(
        "\nFinal totals: read=%lu write=%lu devctl=%lu intctl=%lu done=%lu lastSampleAge=%lu ms\n",
        runtime.Payload.ReadRequestsSeen,
        runtime.Payload.WriteRequestsSeen,
        runtime.Payload.DeviceControlRequestsSeen,
        runtime.Payload.InternalDeviceControlRequestsSeen,
        runtime.Payload.CompletedInputRequests,
        GetTickCount() - lastSampleTick);

    CloseHandle(device);
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    ProbeConfig config = {};

    if (!ParseArgs(argc, argv, &config)) {
        PrintUsage();
        return 1;
    }

    return RunProbe(config);
}
