#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "../../shared/ioctl.h"

typedef struct _UPPER_DEVICE_CONTEXT {
    WDFDEVICE Device;
    WDFIOTARGET LowerTarget;
    WDFQUEUE DefaultQueue;
    WDFQUEUE PendingReadQueue;
    KSPIN_LOCK StateLock;
    USHORT VendorId;
    USHORT ProductId;
    WDFFILEOBJECT WriterFileObject;
    BOOLEAN WriterSessionHeld;
    BOOLEAN OverrideEnabled;
    BOOLEAN IsAttached;
    BOOLEAN IsInD0;
    BOOLEAN HasInjectedReport;
    BOOLEAN HasObservedReport;
    GAYM_JITTER_CONFIG JitterConfig;
    ULONG ReportsInjected;
    ULONG ReportsObserved;
    volatile LONG PendingInputRequests;
    volatile LONG QueuedInputRequests;
    volatile LONG CompletedInputRequests;
    volatile LONG ForwardedInputRequests;
    volatile LONG ReadRequestsSeen;
    volatile LONG DeviceControlRequestsSeen;
    volatile LONG InternalDeviceControlRequestsSeen;
    volatile LONG WriteRequestsSeen;
    volatile LONG LastInterceptedIoctl;
    volatile LONG LastRequestType;
    volatile LONG LastRequestInputLength;
    volatile LONG LastRequestOutputLength;
    GAYM_DEVICE_INFO LastDeviceInfo;
    GAYM_OBSERVATION_V1 LastObservation;
    GAYM_REPORT LastInjectedReport;
    GAYM_REPORT LastObservedReport;
    GAYM_REPORT LastPresentedXInputReport;
    ULONG XInputPacketNumber;
    BOOLEAN HasPresentedXInputReport;
} UPPER_DEVICE_CONTEXT, *PUPPER_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(UPPER_DEVICE_CONTEXT, UpperGetContext)

NTSTATUS UpperDeviceInitialize(_In_ WDFDEVICE Device);
NTSTATUS UpperDeviceCreateControlDevice(_In_ WDFDEVICE FilterDevice);
VOID UpperDeviceShutdownControlDevice(VOID);
VOID UpperDeviceResetWriterState(
    _Inout_ PUPPER_DEVICE_CONTEXT Context,
    _In_opt_ WDFFILEOBJECT ExpectedFileObject);
VOID UpperDeviceRefreshAttachmentState(_Inout_ PUPPER_DEVICE_CONTEXT Context);
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL UpperEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL UpperEvtIoInternalDeviceControl;
EVT_WDF_IO_QUEUE_IO_READ UpperEvtIoRead;
NTSTATUS UpperDeviceHandleIoctl(_In_ PUPPER_DEVICE_CONTEXT Context, _In_ WDFREQUEST Request, _In_ ULONG IoControlCode);
NTSTATUS UpperDeviceHandleReadIntercept(_In_ PUPPER_DEVICE_CONTEXT Context, _In_ WDFREQUEST Request);
NTSTATUS UpperDeviceTranslateReport(
    _In_ PUPPER_DEVICE_CONTEXT Context,
    _In_ ULONG InputSize,
    _In_reads_bytes_(InputSize) const GAYM_REPORT* Report,
    _Out_writes_bytes_(OutputSize) PVOID OutputBuffer,
    _In_ ULONG OutputSize,
    _Out_ PULONG BytesWritten);
NTSTATUS UpperDeviceParseNativeReport(
    _In_ PUPPER_DEVICE_CONTEXT Context,
    _In_reads_bytes_(InputSize) const UCHAR* InputBuffer,
    _In_ ULONG InputSize,
    _Out_ GAYM_REPORT* Report);
NTSTATUS UpperDeviceEnsureObservedReport(_In_ PUPPER_DEVICE_CONTEXT Context);
VOID UpperDeviceUpdateObservation(_In_ PUPPER_DEVICE_CONTEXT Context);
VOID UpperDeviceCompletePendingReads(_In_ PUPPER_DEVICE_CONTEXT Context);
VOID UpperDevicePurgePendingReads(_In_ PUPPER_DEVICE_CONTEXT Context);
