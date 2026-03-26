#pragma once
/*
 * GaYmFilter - Shared IOCTL and type definitions
 * Used by both kernel driver and user-mode applications.
 *
 * PREREQUISITES: Include <ntddk.h> or <wdf.h> (kernel) or
 *                <windows.h> + <winioctl.h> (user-mode) BEFORE this header.
 */

/* ─── Device interface GUID: {A3F2B4C1-7D8E-4F5A-9B6C-1E2D3F4A5B6C} ─── */
DEFINE_GUID(GUID_DEVINTERFACE_GAYM_FILTER,
    0xa3f2b4c1, 0x7d8e, 0x4f5a,
    0x9b, 0x6c, 0x1e, 0x2d, 0x3f, 0x4a, 0x5b, 0x6c);

/* ─── IOCTL codes ─── */
#define GAYM_IOCTL_TYPE  0x8000

#define IOCTL_GAYM_OVERRIDE_ON     CTL_CODE(GAYM_IOCTL_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GAYM_OVERRIDE_OFF    CTL_CODE(GAYM_IOCTL_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GAYM_INJECT_REPORT   CTL_CODE(GAYM_IOCTL_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GAYM_QUERY_DEVICE    CTL_CODE(GAYM_IOCTL_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GAYM_SET_JITTER      CTL_CODE(GAYM_IOCTL_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ─── Device type enum ─── */
typedef enum _GAYM_DEVICE_TYPE {
    GAYM_DEVICE_UNKNOWN        = 0,
    GAYM_DEVICE_XBOX_ONE       = 1,
    GAYM_DEVICE_XBOX_SERIES    = 2,
    GAYM_DEVICE_DUALSENSE      = 3,
    GAYM_DEVICE_DUALSENSE_EDGE = 4,
} GAYM_DEVICE_TYPE;

/* ─── Normalized gamepad report ───
 * All input providers fill this. The kernel translates it to native HID format.
 */
#pragma pack(push, 1)
typedef struct _GAYM_REPORT {
    UCHAR   ReportId;          /* Ignored by kernel (auto-set per device)    */
    UCHAR   Buttons[4];       /* See GAYM_BTN_* below                       */
    UCHAR   DPad;             /* Hat switch: 0-7 direction, 0x0F = center   */
    UCHAR   TriggerLeft;      /* 0-255                                      */
    UCHAR   TriggerRight;     /* 0-255                                      */
    SHORT   ThumbLeftX;       /* -32767 .. 32767                             */
    SHORT   ThumbLeftY;       /* -32767 .. 32767                             */
    SHORT   ThumbRightX;      /* -32767 .. 32767                             */
    SHORT   ThumbRightY;      /* -32767 .. 32767                             */
    UCHAR   Reserved[32];     /* Future: touchpad, gyro, etc.               */
} GAYM_REPORT, *PGAYM_REPORT;
#pragma pack(pop)

/* ─── Device info (IOCTL_GAYM_QUERY_DEVICE response) ─── */
typedef struct _GAYM_DEVICE_INFO {
    GAYM_DEVICE_TYPE DeviceType;
    USHORT           VendorId;
    USHORT           ProductId;
    BOOLEAN          OverrideActive;
    ULONG            ReportsSent;
    ULONG            PendingInputRequests;
    ULONG            QueuedInputRequests;
    ULONG            CompletedInputRequests;
    ULONG            ForwardedInputRequests;
    ULONG            LastInterceptedIoctl;
    ULONG            ReadRequestsSeen;
    ULONG            DeviceControlRequestsSeen;
    ULONG            InternalDeviceControlRequestsSeen;
    ULONG            WriteRequestsSeen;
} GAYM_DEVICE_INFO, *PGAYM_DEVICE_INFO;

/* ─── Jitter config (IOCTL_GAYM_SET_JITTER) ─── */
typedef struct _GAYM_JITTER_CONFIG {
    BOOLEAN Enabled;
    ULONG   MinDelayUs;        /* Minimum extra delay in microseconds       */
    ULONG   MaxDelayUs;        /* Maximum extra delay in microseconds       */
} GAYM_JITTER_CONFIG, *PGAYM_JITTER_CONFIG;

/* ─── Button bit definitions ─── */

/* Buttons[0] */
#define GAYM_BTN_A          0x01
#define GAYM_BTN_B          0x02
#define GAYM_BTN_X          0x04
#define GAYM_BTN_Y          0x08
#define GAYM_BTN_LB         0x10
#define GAYM_BTN_RB         0x20
#define GAYM_BTN_BACK       0x40   /* View (Xbox) / Share (PS)    */
#define GAYM_BTN_START      0x80   /* Menu (Xbox) / Options (PS)  */

/* Buttons[1] */
#define GAYM_BTN_LSTICK     0x01
#define GAYM_BTN_RSTICK     0x02
#define GAYM_BTN_GUIDE      0x04   /* Xbox button / PS button     */
#define GAYM_BTN_MISC       0x08   /* Share (Xbox) / Mute (PS)    */

/* ─── D-pad hat values ─── */
#define GAYM_DPAD_UP        0
#define GAYM_DPAD_UPRIGHT   1
#define GAYM_DPAD_RIGHT     2
#define GAYM_DPAD_DOWNRIGHT 3
#define GAYM_DPAD_DOWN      4
#define GAYM_DPAD_DOWNLEFT  5
#define GAYM_DPAD_LEFT      6
#define GAYM_DPAD_UPLEFT    7
#define GAYM_DPAD_NEUTRAL   0x0F
