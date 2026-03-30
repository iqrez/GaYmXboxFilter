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
#include <string>
#include <vector>

#include "ObservationTypes.h"

namespace {

struct CaptureStubConfig {
    std::filesystem::path eventsPath;
    std::optional<std::filesystem::path> cadencePath;
    uint32_t chainCount = 8;
    uint64_t armSpacing = 10000;
    uint64_t armToEnter = 120;
    uint64_t enterToExit = 360;
    uint64_t sampleToExit = 20;
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

static bool ParseUnsigned64(const char* text, uint64_t* value)
{
    uint64_t parsed = 0;
    const char* end = text + std::strlen(text);
    const auto result = std::from_chars(text, end, parsed, 10);
    if (result.ec != std::errc() || result.ptr != end) {
        return false;
    }

    *value = parsed;
    return true;
}

static void PrintUsage()
{
    std::printf(
        "ObservationCaptureStub.exe <events_path> "
        "[chain_count] [arm_spacing] [arm_to_enter] [enter_to_exit] [sample_to_exit] [cadence_path]\n");
    std::printf("  Emits a binary UsbXhciProbeEventRecord stream for parser testing.\n");
}

static CaptureStubConfig ParseArgs(int argc, char* argv[])
{
    if (argc < 2) {
        PrintUsage();
        throw std::runtime_error("missing events_path");
    }

    CaptureStubConfig config = {};
    config.eventsPath = argv[1];

    if (argc >= 3 && !ParseUnsigned32(argv[2], &config.chainCount)) {
        throw std::runtime_error("invalid chain_count");
    }
    if (argc >= 4 && !ParseUnsigned64(argv[3], &config.armSpacing)) {
        throw std::runtime_error("invalid arm_spacing");
    }
    if (argc >= 5 && !ParseUnsigned64(argv[4], &config.armToEnter)) {
        throw std::runtime_error("invalid arm_to_enter");
    }
    if (argc >= 6 && !ParseUnsigned64(argv[5], &config.enterToExit)) {
        throw std::runtime_error("invalid enter_to_exit");
    }
    if (argc >= 7 && !ParseUnsigned64(argv[6], &config.sampleToExit)) {
        throw std::runtime_error("invalid sample_to_exit");
    }
    if (argc >= 8) {
        config.cadencePath = std::filesystem::path(argv[7]);
    }

    if (config.chainCount == 0) {
        throw std::runtime_error("chain_count must be > 0");
    }

    return config;
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
    uint8_t irql,
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
    record.Irql = irql;
    record.Cpu = cpu;
    record.Period = period;
    record.WaitStatus = waitStatus;
    return record;
}

static void EnsureParentDirectory(const std::filesystem::path& path)
{
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
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

} // namespace

int main(int argc, char* argv[])
{
    try {
        const CaptureStubConfig config = ParseArgs(argc, argv);
        EnsureParentDirectory(config.eventsPath);

        std::ofstream output(config.eventsPath, std::ios::binary);
        if (!output.is_open()) {
            throw std::runtime_error("failed to open events output: " + config.eventsPath.string());
        }

        std::vector<CadenceWindowRow> cadenceWindows;
        cadenceWindows.reserve(config.chainCount);

        uint64_t nextSequence = 1;
        for (uint32_t chainIndex = 0; chainIndex < config.chainCount; ++chainIndex) {
            const uint64_t armTimestamp = 100000 + (static_cast<uint64_t>(chainIndex) * config.armSpacing);
            const uint64_t enterTimestamp = armTimestamp + config.armToEnter;
            const uint64_t exitTimestamp = enterTimestamp + config.enterToExit;
            const uint64_t sampleTimestamp = exitTimestamp + config.sampleToExit;
            const uint32_t timerId = chainIndex + 1;
            const uint32_t contextTag = 100 + (chainIndex % 3);
            const uint32_t threadTag = 1000 + chainIndex;
            const uint8_t cpu = static_cast<uint8_t>(chainIndex % 4);
            const uint32_t period = 8 + (chainIndex % 2);
            const double upperHz = 250.0 - (static_cast<double>(chainIndex % 5) * 10.0);

            const UsbXhciProbeEventRecord arm = MakeEvent(
                nextSequence++,
                armTimestamp,
                -static_cast<int64_t>(config.armSpacing / 4),
                timerId,
                0,
                contextTag,
                threadTag,
                UsbXhciProbePoint::ExSetTimer,
                UsbXhciNoteFlagPeriodicTimer,
                2,
                cpu,
                period,
                0);

            const UsbXhciProbeEventRecord enter = MakeEvent(
                nextSequence++,
                enterTimestamp,
                0,
                timerId,
                static_cast<uint32_t>(arm.Sequence),
                contextTag,
                threadTag,
                UsbXhciProbePoint::KeWaitForSingleObjectEnter,
                UsbXhciNoteFlagNone,
                2,
                cpu,
                0,
                0);

            const UsbXhciProbeEventRecord exit = MakeEvent(
                nextSequence++,
                exitTimestamp,
                0,
                timerId,
                static_cast<uint32_t>(arm.Sequence),
                contextTag,
                threadTag,
                UsbXhciProbePoint::KeWaitForSingleObjectExit,
                UsbXhciNoteFlagNone,
                2,
                cpu,
                0,
                0);

            const UsbXhciProbeEventRecord sample = MakeEvent(
                nextSequence++,
                sampleTimestamp,
                0,
                timerId,
                static_cast<uint32_t>(arm.Sequence),
                contextTag,
                threadTag,
                UsbXhciProbePoint::KeQueryUnbiasedInterruptTime,
                UsbXhciNoteFlagPostWakeSample,
                2,
                cpu,
                0,
                0);

            output.write(reinterpret_cast<const char*>(&arm), sizeof(arm));
            output.write(reinterpret_cast<const char*>(&enter), sizeof(enter));
            output.write(reinterpret_cast<const char*>(&exit), sizeof(exit));
            output.write(reinterpret_cast<const char*>(&sample), sizeof(sample));

            cadenceWindows.push_back(CadenceWindowRow{
                chainIndex + 1,
                armTimestamp - 100,
                sampleTimestamp + 100,
                upperHz
            });
        }

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
        std::printf("Chains emitted     : %u\n", config.chainCount);
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "ObservationCaptureStub failed: %s\n", exception.what());
        return 1;
    }
}
