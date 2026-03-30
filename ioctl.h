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

#define IOCTL_GAYM_OVERRIDE_ON     ((ULONG)CTL_CODE(GAYM_IOCTL_TYPE, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS))
#define IOCTL_GAYM_OVERRIDE_OFF    ((ULONG)CTL_CODE(GAYM_IOCTL_TYPE, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS))
#define IOCTL_GAYM_INJECT_REPORT   ((ULONG)CTL_CODE(GAYM_IOCTL_TYPE, 0x803, METHOD_BUFFERED, FILE_WRITE_ACCESS))
#define IOCTL_GAYM_QUERY_DEVICE    ((ULONG)CTL_CODE(GAYM_IOCTL_TYPE, 0x804, METHOD_BUFFERED, FILE_READ_ACCESS))
#define IOCTL_GAYM_SET_JITTER      ((ULONG)CTL_CODE(GAYM_IOCTL_TYPE, 0x805, METHOD_BUFFERED, FILE_WRITE_ACCESS))
#define IOCTL_GAYM_APPLY_OUTPUT    ((ULONG)CTL_CODE(GAYM_IOCTL_TYPE, 0x806, METHOD_BUFFERED, FILE_WRITE_ACCESS))
#define IOCTL_GAYM_QUERY_SNAPSHOT  ((ULONG)CTL_CODE(GAYM_IOCTL_TYPE, 0x807, METHOD_BUFFERED, FILE_READ_ACCESS))
#define IOCTL_GAYM_CAPTURE_OBSERVATION ((ULONG)CTL_CODE(GAYM_IOCTL_TYPE, 0x808, METHOD_BUFFERED, FILE_READ_ACCESS))

#ifndef GAYM_ENABLE_DEV_DIAGNOSTICS
#if defined(_DEBUG) || defined(DBG)
#define GAYM_ENABLE_DEV_DIAGNOSTICS 1
#else
#define GAYM_ENABLE_DEV_DIAGNOSTICS 0
#endif
#endif

#define GAYM_TRACE_HISTORY_COUNT   8
#define GAYM_TRACE_SAMPLE_BYTES    32
#define GAYM_NATIVE_SAMPLE_BYTES   160

#define GAYM_PROTOCOL_MAGIC              0x314D5947u /* 'GYM1' */
#define GAYM_PROTOCOL_ABI_MAJOR          1u
#define GAYM_PROTOCOL_ABI_MINOR          0u
#define GAYM_PROTOCOL_FLAG_RESPONSE      0x00000001u
#define GAYM_PROTOCOL_FLAG_LEGACY_BRIDGE 0x00000002u

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

typedef enum _GAYM_SNAPSHOT_KIND {
    GAYM_SNAPSHOT_DEVICE_SUMMARY   = 1,
    GAYM_SNAPSHOT_RUNTIME_COUNTERS = 2,
    GAYM_SNAPSHOT_LAST_IO          = 3,
    GAYM_SNAPSHOT_TRACE            = 4,
    GAYM_SNAPSHOT_OUTPUT           = 5,
} GAYM_SNAPSHOT_KIND;

/* ─── Versioned snapshot protocol ─── */
#pragma pack(push, 1)
typedef struct _GAYM_PROTOCOL_HEADER {
    ULONG  Magic;
    USHORT AbiMajor;
    USHORT AbiMinor;
    ULONG  HeaderSize;
    ULONG  PayloadSize;
    ULONG  Flags;
} GAYM_PROTOCOL_HEADER, *PGAYM_PROTOCOL_HEADER;

typedef struct _GAYM_QUERY_SNAPSHOT_REQUEST {
    GAYM_PROTOCOL_HEADER Header;
    ULONG                SnapshotKind;
    ULONG                Reserved;
} GAYM_QUERY_SNAPSHOT_REQUEST, *PGAYM_QUERY_SNAPSHOT_REQUEST;

/* ─── Normalized gamepad report ───
 * All input providers fill this. The kernel translates it to native HID format.
 */
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

typedef struct _GAYM_DEVICE_SUMMARY {
    GAYM_DEVICE_TYPE      DeviceType;
    USHORT                VendorId;
    USHORT                ProductId;
    BOOLEAN               OverrideActive;
    UCHAR                 Reserved0[3];
    ULONG                 ReportsSent;
    ULONG                 DriverBuildStamp;
    GAYM_CAPABILITY_FLAGS InputCapabilities;
    GAYM_CAPABILITY_FLAGS OutputCapabilities;
} GAYM_DEVICE_SUMMARY, *PGAYM_DEVICE_SUMMARY;

typedef struct _GAYM_RUNTIME_COUNTERS {
    ULONG PendingInputRequests;
    ULONG QueuedInputRequests;
    ULONG CompletedInputRequests;
    ULONG ForwardedInputRequests;
    ULONG ReadRequestsSeen;
    ULONG WriteRequestsSeen;
    ULONG DeviceControlRequestsSeen;
    ULONG InternalDeviceControlRequestsSeen;
    ULONG LastInterceptedIoctl;
} GAYM_RUNTIME_COUNTERS, *PGAYM_RUNTIME_COUNTERS;

typedef struct _GAYM_LAST_IO_SNAPSHOT {
    ULONG       LastCompletedStatus;
    ULONG       LastCompletionInformation;
    ULONG       LastReadLength;
    ULONG       LastWriteLength;
    ULONG       LastDeviceControlInputLength;
    ULONG       LastDeviceControlOutputLength;
    ULONG       LastInternalInputLength;
    ULONG       LastInternalOutputLength;
    ULONG       LastRawReadSampleLength;
    ULONG       LastPatchedReadSampleLength;
    ULONG       LastRawReadCompletionLength;
    ULONG       LastPatchedReadCompletionLength;
    ULONG       LastNativeOverrideApplied;
    ULONG       LastNativeOverrideBytesWritten;
    ULONG       LastSemanticCaptureFlags;
    ULONG       LastSemanticCaptureLength;
    ULONG       LastSemanticCaptureIoctl;
    ULONG       LastSemanticCaptureSampleLength;
    UCHAR       LastRawReadSample[GAYM_NATIVE_SAMPLE_BYTES];
    UCHAR       LastPatchedReadSample[GAYM_NATIVE_SAMPLE_BYTES];
    UCHAR       LastSemanticCaptureSample[GAYM_NATIVE_SAMPLE_BYTES];
    GAYM_REPORT LastSemanticCaptureReport;
} GAYM_LAST_IO_SNAPSHOT, *PGAYM_LAST_IO_SNAPSHOT;

typedef struct _GAYM_TRACE_SNAPSHOT {
    ULONG            TraceSequence;
    ULONG            TraceCount;
    GAYM_TRACE_ENTRY Trace[GAYM_TRACE_HISTORY_COUNT];
} GAYM_TRACE_SNAPSHOT, *PGAYM_TRACE_SNAPSHOT;

typedef struct _GAYM_OUTPUT_SNAPSHOT {
    ULONG             LastWriteSampleLength;
    UCHAR             LastWriteSample[GAYM_TRACE_SAMPLE_BYTES];
    ULONG             LastOutputCaptureIoctl;
    ULONG             LastOutputCaptureLength;
    ULONG             LastOutputCaptureSampleLength;
    GAYM_OUTPUT_STATE LastOutputCaptureState;
    UCHAR             LastOutputCaptureSample[GAYM_TRACE_SAMPLE_BYTES];
} GAYM_OUTPUT_SNAPSHOT, *PGAYM_OUTPUT_SNAPSHOT;

typedef struct _GAYM_QUERY_DEVICE_SUMMARY_RESPONSE {
    GAYM_PROTOCOL_HEADER Header;
    GAYM_DEVICE_SUMMARY  Payload;
} GAYM_QUERY_DEVICE_SUMMARY_RESPONSE, *PGAYM_QUERY_DEVICE_SUMMARY_RESPONSE;

typedef struct _GAYM_QUERY_RUNTIME_COUNTERS_RESPONSE {
    GAYM_PROTOCOL_HEADER Header;
    GAYM_RUNTIME_COUNTERS Payload;
} GAYM_QUERY_RUNTIME_COUNTERS_RESPONSE, *PGAYM_QUERY_RUNTIME_COUNTERS_RESPONSE;

typedef struct _GAYM_QUERY_LAST_IO_RESPONSE {
    GAYM_PROTOCOL_HEADER Header;
    GAYM_LAST_IO_SNAPSHOT Payload;
} GAYM_QUERY_LAST_IO_RESPONSE, *PGAYM_QUERY_LAST_IO_RESPONSE;

typedef struct _GAYM_QUERY_TRACE_RESPONSE {
    GAYM_PROTOCOL_HEADER Header;
    GAYM_TRACE_SNAPSHOT  Payload;
} GAYM_QUERY_TRACE_RESPONSE, *PGAYM_QUERY_TRACE_RESPONSE;

typedef struct _GAYM_QUERY_OUTPUT_RESPONSE {
    GAYM_PROTOCOL_HEADER Header;
    GAYM_OUTPUT_SNAPSHOT Payload;
} GAYM_QUERY_OUTPUT_RESPONSE, *PGAYM_QUERY_OUTPUT_RESPONSE;
#pragma pack(pop)

#ifdef __cplusplus
#define GAYM_STATIC_ASSERT(name, expr) static_assert((expr), #name)
#else
#define GAYM_STATIC_ASSERT(name, expr) typedef char gaym_static_assert_##name[(expr) ? 1 : -1]
#endif

GAYM_STATIC_ASSERT(protocol_header_size, sizeof(GAYM_PROTOCOL_HEADER) == 20);
GAYM_STATIC_ASSERT(snapshot_request_size, sizeof(GAYM_QUERY_SNAPSHOT_REQUEST) == 28);
GAYM_STATIC_ASSERT(device_summary_size, sizeof(GAYM_DEVICE_SUMMARY) == 36);
GAYM_STATIC_ASSERT(device_summary_payload_offset, FIELD_OFFSET(GAYM_QUERY_DEVICE_SUMMARY_RESPONSE, Payload) == sizeof(GAYM_PROTOCOL_HEADER));
GAYM_STATIC_ASSERT(runtime_counters_payload_offset, FIELD_OFFSET(GAYM_QUERY_RUNTIME_COUNTERS_RESPONSE, Payload) == sizeof(GAYM_PROTOCOL_HEADER));
GAYM_STATIC_ASSERT(last_io_payload_offset, FIELD_OFFSET(GAYM_QUERY_LAST_IO_RESPONSE, Payload) == sizeof(GAYM_PROTOCOL_HEADER));
GAYM_STATIC_ASSERT(trace_payload_offset, FIELD_OFFSET(GAYM_QUERY_TRACE_RESPONSE, Payload) == sizeof(GAYM_PROTOCOL_HEADER));
GAYM_STATIC_ASSERT(output_payload_offset, FIELD_OFFSET(GAYM_QUERY_OUTPUT_RESPONSE, Payload) == sizeof(GAYM_PROTOCOL_HEADER));

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
#define GAYM_QUERY_SNAPSHOT_REQUEST_SIZE sizeof(GAYM_QUERY_SNAPSHOT_REQUEST)

/* ─── Jitter config (IOCTL_GAYM_SET_JITTER) ─── */
typedef struct _GAYM_JITTER_CONFIG {
    BOOLEAN Enabled;
    ULONG   MinDelayUs;        /* Minimum extra delay in microseconds       */
    ULONG   MaxDelayUs;        /* Maximum extra delay in microseconds       */
} GAYM_JITTER_CONFIG, *PGAYM_JITTER_CONFIG;

#define GAYM_OBSERVATION_CAPTURE_MAX_SAMPLES           64u
#define GAYM_OBSERVATION_CAPTURE_MAX_TOTAL_DURATION_MS 5000u
#define GAYM_OBSERVATION_CAPTURE_EVENTS_PER_SAMPLE     4u

typedef enum _GAYM_OBSERVATION_PROBE_POINT {
    GAYM_OBSERVATION_PROBE_POINT_UNKNOWN = 0,
    GAYM_OBSERVATION_PROBE_POINT_EX_SET_TIMER = 1,
    GAYM_OBSERVATION_PROBE_POINT_WAIT_ENTER = 2,
    GAYM_OBSERVATION_PROBE_POINT_WAIT_EXIT = 3,
    GAYM_OBSERVATION_PROBE_POINT_POST_WAKE_SAMPLE = 4,
} GAYM_OBSERVATION_PROBE_POINT;

#define GAYM_OBSERVATION_NOTE_FLAG_NONE                     0x0000u
#define GAYM_OBSERVATION_NOTE_FLAG_PERIODIC_TIMER           0x0001u
#define GAYM_OBSERVATION_NOTE_FLAG_ONE_SHOT_TIMER           0x0002u
#define GAYM_OBSERVATION_NOTE_FLAG_UNMATCHED_WAKE           0x0004u
#define GAYM_OBSERVATION_NOTE_FLAG_CROSS_CPU                0x0008u
#define GAYM_OBSERVATION_NOTE_FLAG_TRUNCATED_CONTEXT        0x0010u
#define GAYM_OBSERVATION_NOTE_FLAG_BUFFER_PRESSURE_OBSERVED 0x0020u
#define GAYM_OBSERVATION_NOTE_FLAG_POST_WAKE_SAMPLE         0x0040u

#pragma pack(push, 1)
typedef struct _GAYM_OBSERVATION_CAPTURE_CONFIG {
    ULONG SampleCount;
    ULONG DueTimeMs;
    ULONG Flags;
    ULONG Reserved;
} GAYM_OBSERVATION_CAPTURE_CONFIG, *PGAYM_OBSERVATION_CAPTURE_CONFIG;

typedef struct _GAYM_OBSERVATION_EVENT_RECORD {
    ULONGLONG Sequence;
    ULONGLONG TimestampQpcLike;
    LONGLONG  DueTime;
    ULONG     TimerId;
    ULONG     MatchedArmSequenceHint;
    ULONG     ContextTag;
    ULONG     ThreadTag;
    USHORT    ProbePoint;
    USHORT    NoteFlags;
    UCHAR     Irql;
    UCHAR     Cpu;
    USHORT    Reserved0;
    ULONG     Period;
    ULONG     WaitStatus;
    ULONGLONG Aux0;
} GAYM_OBSERVATION_EVENT_RECORD, *PGAYM_OBSERVATION_EVENT_RECORD;
#pragma pack(pop)

GAYM_STATIC_ASSERT(observation_capture_config_size, sizeof(GAYM_OBSERVATION_CAPTURE_CONFIG) == 16);
GAYM_STATIC_ASSERT(observation_event_record_size, sizeof(GAYM_OBSERVATION_EVENT_RECORD) == 64);

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
