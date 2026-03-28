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

DEFINE_GUID(GUID_DEVINTERFACE_GAYM_XINPUT_FILTER,
    0x6c48a2b7, 0x7a2b, 0x49f1,
    0x9e, 0x2a, 0x7d, 0x51, 0x9a, 0xb8, 0xe6, 0xd7);

/* ─── IOCTL codes ─── */
#define GAYM_IOCTL_TYPE  0x8000

#define IOCTL_GAYM_OVERRIDE_ON     ((ULONG)CTL_CODE(GAYM_IOCTL_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS))
#define IOCTL_GAYM_OVERRIDE_OFF    ((ULONG)CTL_CODE(GAYM_IOCTL_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS))
#define IOCTL_GAYM_INJECT_REPORT   ((ULONG)CTL_CODE(GAYM_IOCTL_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS))
#define IOCTL_GAYM_QUERY_DEVICE    ((ULONG)CTL_CODE(GAYM_IOCTL_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS))
#define IOCTL_GAYM_SET_JITTER      ((ULONG)CTL_CODE(GAYM_IOCTL_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS))
#define IOCTL_GAYM_APPLY_OUTPUT    ((ULONG)CTL_CODE(GAYM_IOCTL_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS))

#define GAYM_TRACE_HISTORY_COUNT   8
#define GAYM_TRACE_SAMPLE_BYTES    32
#define GAYM_NATIVE_SAMPLE_BYTES   160

/* ─── Device type enum ─── */
typedef enum _GAYM_DEVICE_TYPE {
    GAYM_DEVICE_UNKNOWN        = 0,
    GAYM_DEVICE_XBOX_ONE       = 1,
    GAYM_DEVICE_XBOX_SERIES    = 2,
    GAYM_DEVICE_DUALSENSE      = 3,
    GAYM_DEVICE_DUALSENSE_EDGE = 4,
} GAYM_DEVICE_TYPE;

typedef enum _GAYM_TRACE_PHASE {
    GAYM_TRACE_PHASE_NONE         = 0,
    GAYM_TRACE_PHASE_DISPATCH     = 1,
    GAYM_TRACE_PHASE_COMPLETION   = 2,
    GAYM_TRACE_PHASE_SEND_FAILURE = 3,
} GAYM_TRACE_PHASE;

typedef enum _GAYM_TRACE_REQUEST_TYPE {
    GAYM_TRACE_REQUEST_NONE                    = 0,
    GAYM_TRACE_REQUEST_READ                    = 1,
    GAYM_TRACE_REQUEST_WRITE                   = 2,
    GAYM_TRACE_REQUEST_DEVICE_CONTROL          = 3,
    GAYM_TRACE_REQUEST_INTERNAL_DEVICE_CONTROL = 4,
} GAYM_TRACE_REQUEST_TYPE;

#define GAYM_CAPTURE_FLAG_VALID              0x00000001u
#define GAYM_CAPTURE_FLAG_PARTIAL            0x00000002u
#define GAYM_CAPTURE_FLAG_TRIGGERS_COMBINED  0x00000004u
#define GAYM_CAPTURE_FLAG_SOURCE_NATIVE_READ 0x00000008u

typedef ULONGLONG GAYM_CAPABILITY_FLAGS;

#define GAYM_OUTPUT_UPDATE_RUMBLE               0x00000001u
#define GAYM_OUTPUT_UPDATE_TRIGGER_RUMBLE       0x00000002u
#define GAYM_OUTPUT_UPDATE_PLAYER_LIGHT         0x00000004u
#define GAYM_OUTPUT_UPDATE_RGB_LIGHT            0x00000008u
#define GAYM_OUTPUT_UPDATE_MUTE_LIGHT           0x00000010u
#define GAYM_OUTPUT_UPDATE_LEFT_TRIGGER_EFFECT  0x00000020u
#define GAYM_OUTPUT_UPDATE_RIGHT_TRIGGER_EFFECT 0x00000040u
#define GAYM_OUTPUT_UPDATE_VENDOR_DATA          0x00000080u

typedef enum _GAYM_LIGHT_MODE {
    GAYM_LIGHT_MODE_OFF          = 0,
    GAYM_LIGHT_MODE_PLAYER_INDEX = 1,
    GAYM_LIGHT_MODE_RGB          = 2,
} GAYM_LIGHT_MODE;

typedef enum _GAYM_TRIGGER_EFFECT_MODE {
    GAYM_TRIGGER_EFFECT_NONE           = 0,
    GAYM_TRIGGER_EFFECT_CONTINUOUS     = 1,
    GAYM_TRIGGER_EFFECT_SECTION        = 2,
    GAYM_TRIGGER_EFFECT_PULSE          = 3,
    GAYM_TRIGGER_EFFECT_WEAPON         = 4,
    GAYM_TRIGGER_EFFECT_VENDOR_DEFINED = 255,
} GAYM_TRIGGER_EFFECT_MODE;

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

typedef struct _GAYM_TRIGGER_EFFECT {
    UCHAR Mode;
    UCHAR StartPosition;
    UCHAR KeepPosition;
    UCHAR Strength;
    UCHAR Parameters[8];
} GAYM_TRIGGER_EFFECT, *PGAYM_TRIGGER_EFFECT;

typedef struct _GAYM_OUTPUT_STATE {
    ULONG               ActiveMask;
    UCHAR               LowFrequencyMotor;
    UCHAR               HighFrequencyMotor;
    UCHAR               LeftTriggerMotor;
    UCHAR               RightTriggerMotor;
    UCHAR               PlayerIndex;
    UCHAR               LightMode;
    UCHAR               LightBrightness;
    UCHAR               MuteLightEnabled;
    UCHAR               LightRed;
    UCHAR               LightGreen;
    UCHAR               LightBlue;
    UCHAR               Reserved0[4];
    GAYM_TRIGGER_EFFECT LeftTriggerEffect;
    GAYM_TRIGGER_EFFECT RightTriggerEffect;
    UCHAR               VendorDefined[32];
} GAYM_OUTPUT_STATE, *PGAYM_OUTPUT_STATE;
#pragma pack(pop)

typedef struct _GAYM_TRACE_ENTRY {
    ULONG Sequence;
    ULONG Phase;
    ULONG RequestType;
    ULONG IoControlCode;
    ULONG InputLength;
    ULONG OutputLength;
    ULONG TransferLength;
    ULONG Status;
    UCHAR SampleLength;
    UCHAR Sample[GAYM_TRACE_SAMPLE_BYTES];
    UCHAR Reserved[3];
} GAYM_TRACE_ENTRY, *PGAYM_TRACE_ENTRY;

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
    ULONG            LastCompletedStatus;
    ULONG            LastCompletionInformation;
    ULONG            LastReadLength;
    ULONG            LastWriteLength;
    ULONG            LastDeviceControlInputLength;
    ULONG            LastDeviceControlOutputLength;
    ULONG            LastInternalInputLength;
    ULONG            LastInternalOutputLength;
    ULONG            LastRawReadSampleLength;
    ULONG            LastPatchedReadSampleLength;
    ULONG            LastRawReadCompletionLength;
    ULONG            LastPatchedReadCompletionLength;
    ULONG            LastNativeOverrideApplied;
    ULONG            LastNativeOverrideBytesWritten;
    ULONG            LastSemanticCaptureFlags;
    ULONG            LastSemanticCaptureLength;
    ULONG            LastSemanticCaptureIoctl;
    ULONG            LastSemanticCaptureSampleLength;
    ULONG            TraceSequence;
    ULONG            TraceCount;
    UCHAR            LastRawReadSample[GAYM_NATIVE_SAMPLE_BYTES];
    UCHAR            LastPatchedReadSample[GAYM_NATIVE_SAMPLE_BYTES];
    UCHAR            LastSemanticCaptureSample[GAYM_NATIVE_SAMPLE_BYTES];
    GAYM_REPORT      LastSemanticCaptureReport;
    GAYM_TRACE_ENTRY Trace[GAYM_TRACE_HISTORY_COUNT];
    ULONG            QueryLayoutVersion;
    ULONG            DriverBuildStamp;
    GAYM_CAPABILITY_FLAGS InputCapabilities;
    GAYM_CAPABILITY_FLAGS OutputCapabilities;
    ULONG            LastWriteSampleLength;
    UCHAR            LastWriteSample[GAYM_TRACE_SAMPLE_BYTES];
    ULONG            LastOutputCaptureIoctl;
    ULONG            LastOutputCaptureLength;
    ULONG            LastOutputCaptureSampleLength;
    GAYM_OUTPUT_STATE LastOutputCaptureState;
    UCHAR            LastOutputCaptureSample[GAYM_TRACE_SAMPLE_BYTES];
} GAYM_DEVICE_INFO, *PGAYM_DEVICE_INFO;

#define GAYM_DEVICE_INFO_MIN_SIZE FIELD_OFFSET(GAYM_DEVICE_INFO, OverrideActive)

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

/* Input capability flags */
#define GAYM_INPUT_CAP_FACE_BUTTONS       0x0000000000000001ull
#define GAYM_INPUT_CAP_SHOULDERS          0x0000000000000002ull
#define GAYM_INPUT_CAP_MENU_BUTTONS       0x0000000000000004ull
#define GAYM_INPUT_CAP_GUIDE_BUTTON       0x0000000000000008ull
#define GAYM_INPUT_CAP_MISC_BUTTON        0x0000000000000010ull
#define GAYM_INPUT_CAP_STICK_CLICKS       0x0000000000000020ull
#define GAYM_INPUT_CAP_DPAD_8WAY          0x0000000000000040ull
#define GAYM_INPUT_CAP_ANALOG_TRIGGERS    0x0000000000000080ull
#define GAYM_INPUT_CAP_LEFT_STICK         0x0000000000000100ull
#define GAYM_INPUT_CAP_RIGHT_STICK        0x0000000000000200ull
#define GAYM_INPUT_CAP_TOUCH_CLICK        0x0000000000000400ull
#define GAYM_INPUT_CAP_TOUCH_SURFACE      0x0000000000000800ull
#define GAYM_INPUT_CAP_MOTION_SENSORS     0x0000000000001000ull

/* Output capability flags */
#define GAYM_OUTPUT_CAP_DUAL_MOTOR_RUMBLE 0x0000000000000001ull
#define GAYM_OUTPUT_CAP_TRIGGER_RUMBLE    0x0000000000000002ull
#define GAYM_OUTPUT_CAP_PLAYER_LIGHT      0x0000000000000004ull
#define GAYM_OUTPUT_CAP_RGB_LIGHT         0x0000000000000008ull
#define GAYM_OUTPUT_CAP_MUTE_LIGHT        0x0000000000000010ull
#define GAYM_OUTPUT_CAP_TRIGGER_EFFECTS   0x0000000000000020ull
#define GAYM_OUTPUT_CAP_VENDOR_EFFECTS    0x0000000000000040ull
