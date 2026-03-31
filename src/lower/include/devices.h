#pragma once
/*
 * GaYmFilter - Per-device descriptor table
 * Maps VID/PID pairs to device types and native HID report translators.
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

/* Per-device descriptor - one entry for the supported MVP VID/PID */
typedef struct _GAYM_DEVICE_DESCRIPTOR {
    USHORT                      VendorId;
    USHORT                      ProductId;
    GAYM_DEVICE_TYPE            DeviceType;
    const char*                 FriendlyName;
    UCHAR                       NativeReportId;
    ULONG                       NativeReportSize;  /* Including report ID byte */
    PFN_GAYM_TRANSLATE_REPORT   TranslateReport;
} GAYM_DEVICE_DESCRIPTOR, *PGAYM_DEVICE_DESCRIPTOR;

/* ─── Public API ─── */

/* Look up a device descriptor by VID/PID. Returns NULL if not found. */
const GAYM_DEVICE_DESCRIPTOR* GaYmLookupDevice(_In_ USHORT VendorId, _In_ USHORT ProductId);

/* Get a human-readable name for a device type. */
const char* GaYmDeviceTypeName(_In_ GAYM_DEVICE_TYPE Type);

/* Parse VID/PID from a hardware ID string (e.g. "HID\\VID_045E&PID_02FF"). */
VOID GaYmParseHardwareId(
    _In_  PCWSTR HardwareId,
    _Out_ PUSHORT VendorId,
    _Out_ PUSHORT ProductId
);
