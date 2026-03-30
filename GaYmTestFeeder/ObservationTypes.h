#pragma once

#include <cstddef>
#include <cstdint>

enum class UsbXhciProbePoint : uint16_t {
    Unknown = 0,
    ExSetTimer = 1,
    KeWaitForSingleObjectEnter = 2,
    KeWaitForSingleObjectExit = 3,
    KeQueryUnbiasedInterruptTime = 4,
};

enum UsbXhciProbeNoteFlags : uint16_t {
    UsbXhciNoteFlagNone = 0x0000,
    UsbXhciNoteFlagPeriodicTimer = 0x0001,
    UsbXhciNoteFlagOneShotTimer = 0x0002,
    UsbXhciNoteFlagUnmatchedWake = 0x0004,
    UsbXhciNoteFlagCrossCpu = 0x0008,
    UsbXhciNoteFlagTruncatedContext = 0x0010,
    UsbXhciNoteFlagBufferPressureObserved = 0x0020,
    UsbXhciNoteFlagPostWakeSample = 0x0040,
};

#pragma pack(push, 1)
struct UsbXhciProbeEventRecord {
    uint64_t Sequence;
    uint64_t TimestampQpcLike;
    int64_t DueTime;
    uint32_t TimerId;
    uint32_t MatchedArmSequenceHint;
    uint32_t ContextTag;
    uint32_t ThreadTag;
    uint16_t ProbePoint;
    uint16_t NoteFlags;
    uint8_t Irql;
    uint8_t Cpu;
    uint16_t Reserved0;
    uint32_t Period;
    uint32_t WaitStatus;
    uint64_t Aux0;
};
#pragma pack(pop)

static_assert(sizeof(UsbXhciProbeEventRecord) == 64, "UsbXhciProbeEventRecord must stay fixed-size");
