#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "DeviceHelper.h"
#include "ObservationTypes.h"

namespace {

struct CaptureKernelConfig {
    std::filesystem::path eventsPath;
    std::optional<std::filesystem::path> cadencePath;
    uint32_t sampleCount = 16;
    uint32_t dueTimeMs = 8;
};

struct CadenceWindowRow {
    uint64_t windowId = 0;
    uint64_t start = 0;
    uint64_t end = 0;
    double upperHz = 0.0;
};

static bool ParseUnsigned32(const char* text, uint32_t* value)
{
    uint32_t parsed = 0;
    const char* end = text + std::strlen(text);
    const auto result = std::from_chars(text, end, parsed, 10);
    if (result.ec != std::errc() || result.ptr != end) {
        return false;
    }

    *value = parsed;
    return true;
}

static void EnsureParentDirectory(const std::filesystem::path& path)
{
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
}

static void PrintUsage()
{
    std::printf(
        "ObservationCaptureKernel.exe <events_path> [sample_count] [due_time_ms] [cadence_path]\n");
    std::printf("  Captures bounded binary observation rows from \\\\.\\GaYmFilterCtl.\n");
}

static CaptureKernelConfig ParseArgs(int argc, char* argv[])
{
    if (argc < 2) {
        PrintUsage();
        throw std::runtime_error("missing events_path");
    }

    CaptureKernelConfig config = {};
    config.eventsPath = argv[1];

    if (argc >= 3 && !ParseUnsigned32(argv[2], &config.sampleCount)) {
        throw std::runtime_error("invalid sample_count");
    }
    if (argc >= 4 && !ParseUnsigned32(argv[3], &config.dueTimeMs)) {
        throw std::runtime_error("invalid due_time_ms");
    }
    if (argc >= 5) {
        config.cadencePath = std::filesystem::path(argv[4]);
    }

    if (config.sampleCount == 0 ||
        config.sampleCount > GAYM_OBSERVATION_CAPTURE_MAX_SAMPLES) {
        throw std::runtime_error("sample_count is out of range");
    }
    if (config.dueTimeMs == 0) {
        throw std::runtime_error("due_time_ms must be > 0");
    }
    if ((static_cast<uint64_t>(config.sampleCount) * static_cast<uint64_t>(config.dueTimeMs)) >
        GAYM_OBSERVATION_CAPTURE_MAX_TOTAL_DURATION_MS) {
        throw std::runtime_error("requested capture exceeds the bounded session budget");
    }

    return config;
}

static std::string DescribeControlRouteState(const GAYM_CONTROL_ROUTE_STATE& routeState)
{
    char buffer[256] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "flags=0x%08lX bound=%s in_d0=%s vid=%04X pid=%04X type=%lu build=0x%08lX",
        routeState.Flags,
        (routeState.Flags & GAYM_CONTROL_ROUTE_FLAG_BOUND_FILTER_CONTEXT) != 0 ? "yes" : "no",
        (routeState.Flags & GAYM_CONTROL_ROUTE_FLAG_FILTER_IN_D0) != 0 ? "yes" : "no",
        routeState.VendorId,
        routeState.ProductId,
        routeState.DeviceType,
        routeState.DriverBuildStamp);
    return std::string(buffer);
}

static std::optional<GAYM_CONTROL_ROUTE_STATE> TryQueryRouteState(HANDLE device)
{
    GAYM_CONTROL_ROUTE_STATE routeState = {};
    if (QueryControlRouteState(device, &routeState, nullptr)) {
        return routeState;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED) {
        return std::nullopt;
    }

    return std::nullopt;
}

static HANDLE OpenLowerControlDevice()
{
    return CreateFileW(
        GaYmSecondaryControlDevicePath(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
}

static void WriteCadenceWindows(
    const std::filesystem::path& path,
    const std::vector<CadenceWindowRow>& windows)
{
    EnsureParentDirectory(path);

    std::ofstream output(path);
    if (!output.is_open()) {
        throw std::runtime_error("failed to open cadence output: " + path.string());
    }

    output << "window_id,start,end,upper_hz\n";
    for (const CadenceWindowRow& window : windows) {
        output
            << window.windowId << ","
            << window.start << ","
            << window.end << ","
            << window.upperHz << "\n";
    }
}

static std::vector<CadenceWindowRow> BuildCadenceWindows(
    const std::vector<UsbXhciProbeEventRecord>& events,
    uint64_t qpcFrequency)
{
    std::vector<CadenceWindowRow> windows;
    uint64_t previousExitTimestamp = 0;
    uint64_t nextWindowId = 1;

    for (const UsbXhciProbeEventRecord& event : events) {
        if (event.ProbePoint != static_cast<uint16_t>(UsbXhciProbePoint::KeWaitForSingleObjectExit)) {
            continue;
        }

        if (previousExitTimestamp != 0) {
            const uint64_t delta = event.TimestampQpcLike - previousExitTimestamp;
            const double upperHz = delta != 0
                ? static_cast<double>(qpcFrequency) / static_cast<double>(delta)
                : 0.0;
            windows.push_back(CadenceWindowRow{
                nextWindowId++,
                previousExitTimestamp,
                event.TimestampQpcLike,
                upperHz
            });
        }

        previousExitTimestamp = event.TimestampQpcLike;
    }

    return windows;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const CaptureKernelConfig config = ParseArgs(argc, argv);
        EnsureParentDirectory(config.eventsPath);

        HANDLE device = OpenLowerControlDevice();
        if (device == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("failed to open \\\\.\\GaYmFilterCtl (run elevated and ensure a lower probe is active)");
        }

        const std::optional<GAYM_CONTROL_ROUTE_STATE> routeState = TryQueryRouteState(device);
        if (routeState.has_value()) {
            const bool hasBoundFilter =
                (routeState->Flags & GAYM_CONTROL_ROUTE_FLAG_BOUND_FILTER_CONTEXT) != 0;
            const bool filterInD0 =
                (routeState->Flags & GAYM_CONTROL_ROUTE_FLAG_FILTER_IN_D0) != 0;
            if (!hasBoundFilter || !filterInD0) {
                CloseHandle(device);
                throw std::runtime_error(
                    "control route is not ready for observation capture: " +
                    DescribeControlRouteState(*routeState));
            }
        }

        UsbXhciObservationCaptureConfig captureConfig = {};
        captureConfig.SampleCount = config.sampleCount;
        captureConfig.DueTimeMs = config.dueTimeMs;

        std::vector<UsbXhciProbeEventRecord> events(
            static_cast<size_t>(config.sampleCount) * GAYM_OBSERVATION_CAPTURE_EVENTS_PER_SAMPLE);
        DWORD bytesReturned = 0;

        const bool success = CaptureObservation(
            device,
            &captureConfig,
            reinterpret_cast<PGAYM_OBSERVATION_EVENT_RECORD>(events.data()),
            static_cast<DWORD>(events.size() * sizeof(events[0])),
            &bytesReturned);
        const DWORD error = success ? ERROR_SUCCESS : GetLastError();
        const std::optional<GAYM_CONTROL_ROUTE_STATE> routeStateAfterFailure =
            success ? std::nullopt : TryQueryRouteState(device);
        CloseHandle(device);

        if (!success) {
            std::string message =
                "IOCTL_GAYM_CAPTURE_OBSERVATION failed with error " + std::to_string(error);
            if (routeStateAfterFailure.has_value()) {
                message += " (" + DescribeControlRouteState(*routeStateAfterFailure) + ")";
            }
            throw std::runtime_error(message);
        }

        if ((bytesReturned % sizeof(events[0])) != 0) {
            throw std::runtime_error("driver returned a partial observation record stream");
        }

        events.resize(bytesReturned / sizeof(events[0]));

        std::ofstream output(config.eventsPath, std::ios::binary);
        if (!output.is_open()) {
            throw std::runtime_error("failed to open events output: " + config.eventsPath.string());
        }

        output.write(
            reinterpret_cast<const char*>(events.data()),
            static_cast<std::streamsize>(events.size() * sizeof(events[0])));
        output.flush();
        output.close();
        if (!output) {
            throw std::runtime_error("failed while writing events output");
        }

        LARGE_INTEGER qpcFrequency = {};
        QueryPerformanceFrequency(&qpcFrequency);

        if (config.cadencePath.has_value()) {
            WriteCadenceWindows(
                *config.cadencePath,
                BuildCadenceWindows(events, static_cast<uint64_t>(qpcFrequency.QuadPart)));
        }

        std::printf("Lower control path : %ws\n", GaYmSecondaryControlDevicePath());
        if (routeState.has_value()) {
            std::printf("Route state       : %s\n", DescribeControlRouteState(*routeState).c_str());
        }
        std::printf("Wrote binary events: %s\n", config.eventsPath.string().c_str());
        if (config.cadencePath.has_value()) {
            std::printf("Wrote cadence file : %s\n", config.cadencePath->string().c_str());
        }
        std::printf("Samples requested  : %u\n", config.sampleCount);
        std::printf("Rows returned      : %zu\n", events.size());
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "ObservationCaptureKernel failed: %s\n", exception.what());
        return 1;
    }
}
