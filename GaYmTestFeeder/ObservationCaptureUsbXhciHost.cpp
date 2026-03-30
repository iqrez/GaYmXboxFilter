#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <charconv>
#include <cstdint>
#include <cstring>
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

enum class HostEmitterBackend {
    Adapter,
    Import,
};

struct CaptureHostConfig {
    std::filesystem::path eventsPath;
    std::optional<std::filesystem::path> cadencePath;
    std::optional<std::filesystem::path> importEventsPath;
    std::optional<std::filesystem::path> importCadencePath;
    HostEmitterBackend backend = HostEmitterBackend::Adapter;
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

static HostEmitterBackend ParseBackendFromEnvironment()
{
    char value[32] = {};
    const DWORD length = GetEnvironmentVariableA("GAYM_HOST_EMITTER_BACKEND", value, ARRAYSIZE(value));
    if (length == 0 || length >= ARRAYSIZE(value)) {
        return HostEmitterBackend::Adapter;
    }

    if (_stricmp(value, "import") == 0) {
        return HostEmitterBackend::Import;
    }

    return HostEmitterBackend::Adapter;
}

static std::optional<std::filesystem::path> ReadPathFromEnvironment(const char* variableName)
{
    char buffer[MAX_PATH * 4] = {};
    const DWORD length = GetEnvironmentVariableA(variableName, buffer, ARRAYSIZE(buffer));
    if (length == 0 || length >= ARRAYSIZE(buffer)) {
        return std::nullopt;
    }

    return std::filesystem::path(buffer);
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
        "ObservationCaptureUsbXhciHost.exe <events_path> [sample_count] [due_time_ms] [cadence_path]\n");
    std::printf("  Backend is selected by environment:\n");
    std::printf("    GAYM_HOST_EMITTER_BACKEND=adapter|import\n");
    std::printf("    GAYM_HOST_EMITTER_IMPORT_EVENTS=<path> (for import)\n");
    std::printf("    GAYM_HOST_EMITTER_IMPORT_CADENCE=<path> (optional for import)\n");
}

static CaptureHostConfig ParseArgs(int argc, char* argv[])
{
    if (argc < 2) {
        PrintUsage();
        throw std::runtime_error("missing events_path");
    }

    CaptureHostConfig config = {};
    config.eventsPath = argv[1];
    config.backend = ParseBackendFromEnvironment();
    config.importEventsPath = ReadPathFromEnvironment("GAYM_HOST_EMITTER_IMPORT_EVENTS");
    config.importCadencePath = ReadPathFromEnvironment("GAYM_HOST_EMITTER_IMPORT_CADENCE");

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

    if (config.backend == HostEmitterBackend::Import && !config.importEventsPath.has_value()) {
        throw std::runtime_error("import backend requires GAYM_HOST_EMITTER_IMPORT_EVENTS");
    }

    return config;
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

static std::vector<UsbXhciProbeEventRecord> ReadObservationEvents(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open import events: " + path.string());
    }

    const std::streamsize length = input.tellg();
    if (length < 0 || (length % static_cast<std::streamsize>(sizeof(UsbXhciProbeEventRecord))) != 0) {
        throw std::runtime_error("import events file is not aligned to the observation record size");
    }

    input.seekg(0, std::ios::beg);

    std::vector<UsbXhciProbeEventRecord> events(
        static_cast<size_t>(length / static_cast<std::streamsize>(sizeof(UsbXhciProbeEventRecord))));
    if (!events.empty()) {
        input.read(
            reinterpret_cast<char*>(events.data()),
            static_cast<std::streamsize>(events.size() * sizeof(events[0])));
        if (!input) {
            throw std::runtime_error("failed while reading import events: " + path.string());
        }
    }

    return events;
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

static void CopyTextArtifact(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath)
{
    EnsureParentDirectory(destinationPath);
    std::filesystem::copy_file(
        sourcePath,
        destinationPath,
        std::filesystem::copy_options::overwrite_existing);
}

static const char* BackendName(HostEmitterBackend backend)
{
    switch (backend) {
    case HostEmitterBackend::Import:
        return "import";
    case HostEmitterBackend::Adapter:
    default:
        return "adapter";
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const CaptureHostConfig config = ParseArgs(argc, argv);
        EnsureParentDirectory(config.eventsPath);
        std::vector<UsbXhciProbeEventRecord> events;

        if (config.backend == HostEmitterBackend::Import) {
            events = ReadObservationEvents(*config.importEventsPath);
        } else {
            HANDLE device = OpenLowerControlDevice();
            if (device == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("failed to open \\\\.\\GaYmFilterCtl (run elevated and ensure the composite probe is active)");
            }

            UsbXhciObservationCaptureConfig captureConfig = {};
            captureConfig.SampleCount = config.sampleCount;
            captureConfig.DueTimeMs = config.dueTimeMs;

            events.resize(static_cast<size_t>(config.sampleCount) * GAYM_OBSERVATION_CAPTURE_EVENTS_PER_SAMPLE);

            DWORD bytesReturned = 0;
            const bool success = CaptureObservation(
                device,
                &captureConfig,
                reinterpret_cast<PGAYM_OBSERVATION_EVENT_RECORD>(events.data()),
                static_cast<DWORD>(events.size() * sizeof(events[0])),
                &bytesReturned);
            const DWORD error = success ? ERROR_SUCCESS : GetLastError();
            CloseHandle(device);

            if (!success) {
                throw std::runtime_error("host-emitter adapter capture failed with error " + std::to_string(error));
            }

            if ((bytesReturned % sizeof(events[0])) != 0) {
                throw std::runtime_error("driver returned a partial observation record stream");
            }

            events.resize(bytesReturned / sizeof(events[0]));
        }

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
            if (config.backend == HostEmitterBackend::Import &&
                config.importCadencePath.has_value() &&
                std::filesystem::exists(*config.importCadencePath)) {
                CopyTextArtifact(*config.importCadencePath, *config.cadencePath);
            } else {
                WriteCadenceWindows(
                    *config.cadencePath,
                    BuildCadenceWindows(events, static_cast<uint64_t>(qpcFrequency.QuadPart)));
            }
        }

        std::printf("Host emitter mode : %s\n", BackendName(config.backend));
        if (config.backend == HostEmitterBackend::Import) {
            std::printf("Import events path: %s\n", config.importEventsPath->string().c_str());
            if (config.importCadencePath.has_value()) {
                std::printf("Import cadence path: %s\n", config.importCadencePath->string().c_str());
            }
        } else {
            std::printf("Lower control path: %ws\n", GaYmSecondaryControlDevicePath());
        }
        std::printf("Wrote binary events: %s\n", config.eventsPath.string().c_str());
        if (config.cadencePath.has_value()) {
            std::printf("Wrote cadence file : %s\n", config.cadencePath->string().c_str());
        }
        std::printf("Samples requested  : %u\n", config.sampleCount);
        std::printf("Rows returned      : %zu\n", events.size());
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "ObservationCaptureUsbXhciHost failed: %s\n", exception.what());
        return 1;
    }
}
