#pragma once

#ifndef GAYM_OBSERVATION_V1_H
#define GAYM_OBSERVATION_V1_H

#include "protocol.h"
#include "device_ids.h"
#include "capability_flags.h"

#pragma pack(push, 1)
typedef struct _GAYM_OBSERVATION_V1 {
    GAYM_PROTOCOL_HEADER Header;
    ULONG AdapterFamily;
    ULONG CapabilityFlags;
    ULONG StatusFlags;
    ULONGLONG LastObservedSequence;
    ULONGLONG LastInjectedSequence;
    ULONGLONG TimestampQpc;
    ULONG Buttons;
    SHORT LeftStickX;
    SHORT LeftStickY;
    SHORT RightStickX;
    SHORT RightStickY;
    USHORT LeftTrigger;
    USHORT RightTrigger;
    UCHAR BatteryPercent;
    UCHAR PowerFlags;
    USHORT Reserved;
} GAYM_OBSERVATION_V1, *PGAYM_OBSERVATION_V1;
#pragma pack(pop)

GAYM_STATIC_ASSERT(sizeof(GAYM_OBSERVATION_V1) == 76, gaym_observation_v1_must_be_76_bytes);

#endif
