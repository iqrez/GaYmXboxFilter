#pragma once
/*
 * GaYmFilter - Per-device descriptor table
 * Maps VID/PID pairs to device types and native input/output translators.
 */

#include <ntddk.h>
#include "ioctl.h"

/* Maximum native HID report size we'll produce */
#define GAYM_MAX_NATIVE_REPORT  64

/*
 * Report translation function type.
 * Converts a generic GAYM_REPORT into the device's native HID input report.
 *
 * Called at DISPATCH_LEVEL (under spinlock) - must not block or page-fault.
 */
typedef NTSTATUS (*PFN_GAYM_TRANSLATE_REPORT)(
    _In_                                    const GAYM_REPORT* GenericReport,
    _Out_writes_bytes_(NativeBufferSize)     PUCHAR             NativeReport,
    _In_                                    ULONG              NativeBufferSize,
    _Out_                                   PULONG             BytesWritten,
    _Inout_                                 PUCHAR             SequenceCounter
);

typedef NTSTATUS (*PFN_GAYM_PARSE_REPORT)(
    _In_reads_bytes_(NativeBufferSize) const UCHAR* NativeReport,
    _In_ ULONG NativeBufferSize,
    _Out_ PGAYM_REPORT GenericReport,
    _Out_ PULONG CaptureFlags
);

typedef NTSTATUS (*PFN_GAYM_TRANSLATE_OUTPUT_STATE)(
    _In_ const GAYM_OUTPUT_STATE* OutputState,
    _Out_writes_bytes_(NativeBufferSize) PUCHAR NativeReport,
    _In_ ULONG NativeBufferSize,
    _Out_ PULONG BytesWritten
);

typedef NTSTATUS (*PFN_GAYM_PARSE_OUTPUT_REPORT)(
    _In_reads_bytes_(NativeBufferSize) const UCHAR* NativeReport,
    _In_ ULONG NativeBufferSize,
    _Out_ PGAYM_OUTPUT_STATE OutputState
);

/* Per-device descriptor - one entry per supported VID/PID */
typedef struct _GAYM_DEVICE_DESCRIPTOR {
    USHORT                          VendorId;
    USHORT                          ProductId;
    GAYM_DEVICE_TYPE                DeviceType;
    const char*                     FriendlyName;
    UCHAR                           NativeReportId;         /* Input report ID */
    ULONG                           NativeReportSize;       /* Input report bytes */
    PFN_GAYM_TRANSLATE_REPORT       TranslateReport;
    PFN_GAYM_PARSE_REPORT           ParseReport;
    UCHAR                           NativeOutputReportId;
    ULONG                           NativeOutputReportSize; /* Output report bytes */
    GAYM_CAPABILITY_FLAGS           InputCapabilities;
    GAYM_CAPABILITY_FLAGS           OutputCapabilities;
    PFN_GAYM_TRANSLATE_OUTPUT_STATE TranslateOutputState;
    PFN_GAYM_PARSE_OUTPUT_REPORT    ParseOutputReport;
} GAYM_DEVICE_DESCRIPTOR, *PGAYM_DEVICE_DESCRIPTOR;

/* ─── Public API ─── */

/* Look up a device descriptor by VID/PID. Returns NULL if not found. */
const GAYM_DEVICE_DESCRIPTOR* GaYmLookupDevice(_In_ USHORT VendorId, _In_ USHORT ProductId);

/* Get a human-readable name for a device type. */
const char* GaYmDeviceTypeName(_In_ GAYM_DEVICE_TYPE Type);

NTSTATUS GaYmParseNativeReport(
    _In_ const GAYM_DEVICE_DESCRIPTOR* DeviceDesc,
    _In_reads_bytes_(NativeBufferSize) const UCHAR* NativeReport,
    _In_ ULONG NativeBufferSize,
    _Out_ PGAYM_REPORT GenericReport,
    _Out_ PULONG CaptureFlags
);

NTSTATUS GaYmTranslateOutputState(
    _In_ const GAYM_DEVICE_DESCRIPTOR* DeviceDesc,
    _In_ const GAYM_OUTPUT_STATE* OutputState,
    _Out_writes_bytes_(NativeBufferSize) PUCHAR NativeReport,
    _In_ ULONG NativeBufferSize,
    _Out_ PULONG BytesWritten
);

NTSTATUS GaYmParseOutputReport(
    _In_ const GAYM_DEVICE_DESCRIPTOR* DeviceDesc,
    _In_reads_bytes_(NativeBufferSize) const UCHAR* NativeReport,
    _In_ ULONG NativeBufferSize,
    _Out_ PGAYM_OUTPUT_STATE OutputState
);

GAYM_CAPABILITY_FLAGS GaYmGetInputCapabilities(_In_opt_ const GAYM_DEVICE_DESCRIPTOR* DeviceDesc);
GAYM_CAPABILITY_FLAGS GaYmGetOutputCapabilities(_In_opt_ const GAYM_DEVICE_DESCRIPTOR* DeviceDesc);

/* Parse VID/PID from a hardware ID string (e.g. "HID\\VID_045E&PID_02D1"). */
VOID GaYmParseHardwareId(
    _In_  PCWSTR HardwareId,
    _Out_ PUSHORT VendorId,
    _Out_ PUSHORT ProductId
);
