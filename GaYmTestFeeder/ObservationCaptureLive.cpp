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

#include "ObservationTypes.h"

namespace {

struct CaptureLiveConfig {
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

static uint64_t QueryQpc()
{
    LARGE_INTEGER value = {};
    QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart);
}

static uint8_t QueryCpu()
{
    return static_cast<uint8_t>(GetCurrentProcessorNumber());
}

static UsbXhciProbeEventRecord MakeEvent(
    uint64_t sequence,
    uint64_t timestamp,
    int64_t dueTime,
    uint32_t timerId,
    uint32_t matchedArmSequenceHint,
    uint32_t contextTag,
    uint32_t threadTag,
    UsbXhciProbePoint probePoint,
    uint16_t noteFlags,
    uint8_t cpu,
    uint32_t period,
    uint32_t waitStatus)
{
    UsbXhciProbeEventRecord record = {};
    record.Sequence = sequence;
    record.TimestampQpcLike = timestamp;
    record.DueTime = dueTime;
    record.TimerId = timerId;
    record.MatchedArmSequenceHint = matchedArmSequenceHint;
    record.ContextTag = contextTag;
    record.ThreadTag = threadTag;
    record.ProbePoint = static_cast<uint16_t>(probePoint);
    record.NoteFlags = noteFlags;
    record.Irql = 0;
    record.Cpu = cpu;
    record.Period = period;
    record.WaitStatus = waitStatus;
    return record;
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

static void PrintUsage()
{
    std::printf(
        "ObservationCaptureLive.exe <events_path> [sample_count] [due_time_ms] [cadence_path]\n");
    std::printf("  Emits buffered binary UsbXhciProbeEventRecord rows using a real waitable timer.\n");
}

static CaptureLiveConfig ParseArgs(int argc, char* argv[])
{
    if (argc < 2) {
        PrintUsage();
        throw std::runtime_error("missing events_path");
    }

    CaptureLiveConfig config = {};
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

    if (config.sampleCount == 0) {
        throw std::runtime_error("sample_count must be > 0");
    }
    if (config.dueTimeMs == 0) {
        throw std::runtime_error("due_time_ms must be > 0");
    }

    return config;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const CaptureLiveConfig config = ParseArgs(argc, argv);
        EnsureParentDirectory(config.eventsPath);

        HANDLE timerHandle = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (timerHandle == nullptr) {
            throw std::runtime_error("CreateWaitableTimerW failed");
        }

        LARGE_INTEGER qpcFrequency = {};
        QueryPerformanceFrequency(&qpcFrequency);

        std::vector<UsbXhciProbeEventRecord> events;
        events.reserve(static_cast<size_t>(config.sampleCount) * 4u);
        std::vector<CadenceWindowRow> cadenceWindows;
        cadenceWindows.reserve(config.sampleCount > 0 ? config.sampleCount - 1 : 0);

        const uint32_t contextTag = static_cast<uint32_t>(GetCurrentProcessId());
        const uint32_t threadTag = static_cast<uint32_t>(GetCurrentThreadId());
        const int64_t dueTime100ns = -static_cast<int64_t>(config.dueTimeMs) * 10000ll;
        uint64_t nextSequence = 1;
        uint64_t previousExitTimestamp = 0;

        for (uint32_t sampleIndex = 0; sampleIndex < config.sampleCount; ++sampleIndex) {
            const uint32_t timerId = sampleIndex + 1;
            const uint8_t cpu = QueryCpu();
            const uint64_t armTimestamp = QueryQpc();
            const uint64_t armSequence = nextSequence;

            events.push_back(MakeEvent(
                nextSequence++,
                armTimestamp,
                dueTime100ns,
                timerId,
                0,
                contextTag,
                threadTag,
                UsbXhciProbePoint::ExSetTimer,
                UsbXhciNoteFlagOneShotTimer,
                cpu,
                0,
                0));

            LARGE_INTEGER dueTime = {};
            dueTime.QuadPart = dueTime100ns;
            if (!SetWaitableTimer(timerHandle, &dueTime, 0, nullptr, nullptr, FALSE)) {
                CloseHandle(timerHandle);
                throw std::runtime_error("SetWaitableTimer failed");
            }

            const uint64_t waitEnterTimestamp = QueryQpc();
            events.push_back(MakeEvent(
                nextSequence++,
                waitEnterTimestamp,
                0,
                timerId,
                static_cast<uint32_t>(armSequence),
                contextTag,
                threadTag,
                UsbXhciProbePoint::KeWaitForSingleObjectEnter,
                UsbXhciNoteFlagNone,
                cpu,
                0,
                0));

            const DWORD waitResult = WaitForSingleObject(timerHandle, INFINITE);
            if (waitResult != WAIT_OBJECT_0) {
                CloseHandle(timerHandle);
                throw std::runtime_error("WaitForSingleObject failed");
            }

            const uint64_t waitExitTimestamp = QueryQpc();
            events.push_back(MakeEvent(
                nextSequence++,
                waitExitTimestamp,
                0,
                timerId,
                static_cast<uint32_t>(armSequence),
                contextTag,
                threadTag,
                UsbXhciProbePoint::KeWaitForSingleObjectExit,
                UsbXhciNoteFlagNone,
                QueryCpu(),
                0,
                WAIT_OBJECT_0));

            const uint64_t sampleTimestamp = QueryQpc();
            events.push_back(MakeEvent(
                nextSequence++,
                sampleTimestamp,
                0,
                timerId,
                static_cast<uint32_t>(armSequence),
                contextTag,
                threadTag,
                UsbXhciProbePoint::KeQueryUnbiasedInterruptTime,
                UsbXhciNoteFlagPostWakeSample,
                QueryCpu(),
                0,
                0));

            if (previousExitTimestamp != 0 && config.cadencePath.has_value()) {
                const uint64_t delta = waitExitTimestamp - previousExitTimestamp;
                const double upperHz = delta != 0
                    ? static_cast<double>(qpcFrequency.QuadPart) / static_cast<double>(delta)
                    : 0.0;
                cadenceWindows.push_back(CadenceWindowRow{
                    timerId,
                    previousExitTimestamp,
                    waitExitTimestamp,
                    upperHz
                });
            }

            previousExitTimestamp = waitExitTimestamp;
        }

        CloseHandle(timerHandle);

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

        if (config.cadencePath.has_value()) {
            WriteCadenceWindows(*config.cadencePath, cadenceWindows);
        }

        std::printf("Wrote binary events: %s\n", config.eventsPath.string().c_str());
        if (config.cadencePath.has_value()) {
            std::printf("Wrote cadence file : %s\n", config.cadencePath->string().c_str());
        }
        std::printf("Samples emitted    : %u\n", config.sampleCount);
        std::printf("QPC frequency      : %lld\n", static_cast<long long>(qpcFrequency.QuadPart));
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "ObservationCaptureLive failed: %s\n", exception.what());
        return 1;
    }
}
