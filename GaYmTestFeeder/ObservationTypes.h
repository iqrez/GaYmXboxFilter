#pragma once

#include <cstddef>
#include <cstdint>

#include "../ioctl.h"

enum class UsbXhciProbePoint : uint16_t {
    Unknown = GAYM_OBSERVATION_PROBE_POINT_UNKNOWN,
    ExSetTimer = GAYM_OBSERVATION_PROBE_POINT_EX_SET_TIMER,
    KeWaitForSingleObjectEnter = GAYM_OBSERVATION_PROBE_POINT_WAIT_ENTER,
    KeWaitForSingleObjectExit = GAYM_OBSERVATION_PROBE_POINT_WAIT_EXIT,
    KeQueryUnbiasedInterruptTime = GAYM_OBSERVATION_PROBE_POINT_POST_WAKE_SAMPLE,
};

enum UsbXhciProbeNoteFlags : uint16_t {
    UsbXhciNoteFlagNone = GAYM_OBSERVATION_NOTE_FLAG_NONE,
    UsbXhciNoteFlagPeriodicTimer = GAYM_OBSERVATION_NOTE_FLAG_PERIODIC_TIMER,
    UsbXhciNoteFlagOneShotTimer = GAYM_OBSERVATION_NOTE_FLAG_ONE_SHOT_TIMER,
    UsbXhciNoteFlagUnmatchedWake = GAYM_OBSERVATION_NOTE_FLAG_UNMATCHED_WAKE,
    UsbXhciNoteFlagCrossCpu = GAYM_OBSERVATION_NOTE_FLAG_CROSS_CPU,
    UsbXhciNoteFlagTruncatedContext = GAYM_OBSERVATION_NOTE_FLAG_TRUNCATED_CONTEXT,
    UsbXhciNoteFlagBufferPressureObserved = GAYM_OBSERVATION_NOTE_FLAG_BUFFER_PRESSURE_OBSERVED,
    UsbXhciNoteFlagPostWakeSample = GAYM_OBSERVATION_NOTE_FLAG_POST_WAKE_SAMPLE,
};

using UsbXhciProbeEventRecord = GAYM_OBSERVATION_EVENT_RECORD;
using UsbXhciObservationCaptureConfig = GAYM_OBSERVATION_CAPTURE_CONFIG;

static_assert(sizeof(UsbXhciProbeEventRecord) == 64, "UsbXhciProbeEventRecord must stay fixed-size");
