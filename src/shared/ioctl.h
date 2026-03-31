#pragma once
/*
 * Shared transport ABI for both the current prototype runtime and the emerging
 * SPEC-2 controller-platform contract.
 *
 * Include <ntddk.h> or <wdf.h> in kernel code, or <windows.h> plus
 * <winioctl.h> in user-mode code, before including this header.
 */

#include "protocol.h"
#include "device_ids.h"
#include "capability_flags.h"
#include "gaym_report.h"
#include "gaym_observation.h"

#define GAYM_CONTROL_DEVICE_UPPER_W      L"\\\\.\\GaYmXInputFilterCtl"
#define GAYM_CONTROL_DEVICE_DIAGNOSTIC_W L"\\\\.\\GaYmFilterCtl"
#define GAYM_CONTROL_DEVICE_UPPER_A      "\\\\.\\GaYmXInputFilterCtl"
#define GAYM_CONTROL_DEVICE_DIAGNOSTIC_A "\\\\.\\GaYmFilterCtl"

DEFINE_GUID(
    GUID_DEVINTERFACE_GAYM_FILTER,
    0xa3f2b4c1, 0x7d8e, 0x4f5a,
    0x9b, 0x6c, 0x1e, 0x2d, 0x3f, 0x4a, 0x5b, 0x6c);

#define GAYM_IOCTL_TYPE 0x8000

#define IOCTL_GAYM_ACQUIRE_WRITER_SESSION CTL_CODE(GAYM_IOCTL_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GAYM_OVERRIDE_ON            CTL_CODE(GAYM_IOCTL_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GAYM_OVERRIDE_OFF           CTL_CODE(GAYM_IOCTL_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GAYM_INJECT_REPORT          CTL_CODE(GAYM_IOCTL_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GAYM_QUERY_DEVICE           CTL_CODE(GAYM_IOCTL_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GAYM_SET_JITTER             CTL_CODE(GAYM_IOCTL_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GAYM_QUERY_OBSERVATION      CTL_CODE(GAYM_IOCTL_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GAYM_QUERY_SNAPSHOT         CTL_CODE(GAYM_IOCTL_TYPE, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GAYM_CAPTURE_OBSERVATION    CTL_CODE(GAYM_IOCTL_TYPE, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GAYM_RELEASE_WRITER_SESSION CTL_CODE(GAYM_IOCTL_TYPE, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef enum _GAYM_DEVICE_TYPE {
    GAYM_DEVICE_UNKNOWN        = 0,
    GAYM_DEVICE_XBOX_ONE       = 1,
    GAYM_DEVICE_XBOX_SERIES    = 2,
    GAYM_DEVICE_DUALSENSE      = 3,
    GAYM_DEVICE_DUALSENSE_EDGE = 4,
} GAYM_DEVICE_TYPE;

/*
 * Transitional legacy report still used by the lower-driver prototype. New
 * producer-facing code should prefer GAYM_REPORT_V1 from gaym_report.h.
 */
#pragma pack(push, 1)
typedef struct _GAYM_REPORT {
    UCHAR ReportId;
    UCHAR Buttons[4];
    UCHAR DPad;
    UCHAR TriggerLeft;
    UCHAR TriggerRight;
    SHORT ThumbLeftX;
    SHORT ThumbLeftY;
    SHORT ThumbRightX;
    SHORT ThumbRightY;
    UCHAR Reserved[32];
} GAYM_REPORT, *PGAYM_REPORT;
#pragma pack(pop)

GAYM_STATIC_ASSERT(sizeof(GAYM_REPORT) == 48, gaym_legacy_report_must_be_48_bytes);

typedef struct _GAYM_DEVICE_INFO {
    GAYM_DEVICE_TYPE DeviceType;
    USHORT VendorId;
    USHORT ProductId;
    BOOLEAN OverrideActive;
    ULONG ReportsSent;
    ULONG PendingInputRequests;
    ULONG QueuedInputRequests;
    ULONG CompletedInputRequests;
    ULONG ForwardedInputRequests;
    ULONG LastInterceptedIoctl;
    ULONG ReadRequestsSeen;
    ULONG DeviceControlRequestsSeen;
    ULONG InternalDeviceControlRequestsSeen;
    ULONG WriteRequestsSeen;
    ULONG LastRequestType;
    ULONG LastRequestInputLength;
    ULONG LastRequestOutputLength;
} GAYM_DEVICE_INFO, *PGAYM_DEVICE_INFO;

typedef struct _GAYM_JITTER_CONFIG {
    BOOLEAN Enabled;
    ULONG MinDelayUs;
    ULONG MaxDelayUs;
} GAYM_JITTER_CONFIG, *PGAYM_JITTER_CONFIG;

#define GAYM_BTN_A      0x01
#define GAYM_BTN_B      0x02
#define GAYM_BTN_X      0x04
#define GAYM_BTN_Y      0x08
#define GAYM_BTN_LB     0x10
#define GAYM_BTN_RB     0x20
#define GAYM_BTN_BACK   0x40
#define GAYM_BTN_START  0x80

#define GAYM_BTN_LSTICK 0x01
#define GAYM_BTN_RSTICK 0x02
#define GAYM_BTN_GUIDE  0x04
#define GAYM_BTN_MISC   0x08

#define GAYM_DPAD_UP        0
#define GAYM_DPAD_UPRIGHT   1
#define GAYM_DPAD_RIGHT     2
#define GAYM_DPAD_DOWNRIGHT 3
#define GAYM_DPAD_DOWN      4
#define GAYM_DPAD_DOWNLEFT  5
#define GAYM_DPAD_LEFT      6
#define GAYM_DPAD_UPLEFT    7
#define GAYM_DPAD_NEUTRAL   0x0F
