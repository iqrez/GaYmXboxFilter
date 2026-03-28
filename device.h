#pragma once
/*
 * GaYmXboxFilter - Device context and callbacks for the XboxComposite upper filter.
 */

#include <ntddk.h>
#include <wdf.h>

#include "ioctl.h"
#include "devices.h"
#include "logging.h"

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE Device;
    WDFIOTARGET IoTarget;
    WDFQUEUE DefaultQueue;
    WDFQUEUE PendingReadsQueue;

    USHORT VendorId;
    USHORT ProductId;
    GAYM_DEVICE_TYPE DeviceType;
    const GAYM_DEVICE_DESCRIPTOR* DeviceDesc;
    BOOLEAN IsUsbStack;

    BOOLEAN OverrideEnabled;
    BOOLEAN HasReport;
    BOOLEAN IsInD0;
    KSPIN_LOCK ReportLock;
    KSPIN_LOCK TraceLock;
    GAYM_REPORT CurrentReport;
    GAYM_JITTER_CONFIG JitterConfig;
    ULONG ReportsSent;
    UCHAR SeqCounter;

    volatile LONG PendingInputRequests;
    volatile LONG QueuedInputRequests;
    volatile LONG CompletedInputRequests;
    volatile LONG ForwardedInputRequests;
    volatile LONG LastInterceptedIoctl;
    volatile LONG ReadRequestsSeen;
    volatile LONG DeviceControlRequestsSeen;
    volatile LONG InternalDeviceControlRequestsSeen;
    volatile LONG WriteRequestsSeen;
    volatile LONG LastCompletedStatus;
    volatile LONG LastCompletionInformation;
    volatile LONG LastReadLength;
    volatile LONG LastWriteLength;
    volatile LONG LastWriteSampleLength;
    volatile LONG LastOutputCaptureIoctl;
    volatile LONG LastOutputCaptureLength;
    volatile LONG LastOutputCaptureSampleLength;
    volatile LONG LastDeviceControlInputLength;
    volatile LONG LastDeviceControlOutputLength;
    volatile LONG LastInternalInputLength;
    volatile LONG LastInternalOutputLength;
    volatile LONG LastRawReadSampleLength;
    volatile LONG LastPatchedReadSampleLength;
    volatile LONG LastRawReadCompletionLength;
    volatile LONG LastPatchedReadCompletionLength;
    volatile LONG LastNativeOverrideApplied;
    volatile LONG LastNativeOverrideBytesWritten;
    volatile LONG TraceSequence;
    ULONG TraceWriteIndex;
    ULONG TraceCount;
    UCHAR LastRawReadSample[GAYM_NATIVE_SAMPLE_BYTES];
    UCHAR LastPatchedReadSample[GAYM_NATIVE_SAMPLE_BYTES];
    UCHAR LastWriteSample[GAYM_TRACE_SAMPLE_BYTES];
    UCHAR LastOutputCaptureSample[GAYM_TRACE_SAMPLE_BYTES];
    GAYM_OUTPUT_STATE LastOutputCaptureState;
    GAYM_TRACE_ENTRY Trace[GAYM_TRACE_HISTORY_COUNT];
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

typedef struct _CONTROL_DEVICE_CONTEXT {
    PDEVICE_CONTEXT FilterCtx;
    WDFWAITLOCK RouteLock;
} CONTROL_DEVICE_CONTEXT, *PCONTROL_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CONTROL_DEVICE_CONTEXT, ControlGetContext)

typedef struct _REQUEST_CONTEXT {
    ULONG RequestType;
    ULONG IoControlCode;
    ULONG InputLength;
    ULONG OutputLength;
    UCHAR InputSampleLength;
    UCHAR InputSample[GAYM_TRACE_SAMPLE_BYTES];
    UCHAR Reserved[3];
} REQUEST_CONTEXT, *PREQUEST_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(REQUEST_CONTEXT, RequestGetContext)

EVT_WDF_DRIVER_DEVICE_ADD GaYmEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_READ GaYmEvtIoRead;
EVT_WDF_IO_QUEUE_IO_WRITE GaYmEvtIoWrite;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL GaYmEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL GaYmEvtCtlIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL GaYmEvtIoInternalDeviceControl;
EVT_WDF_REQUEST_COMPLETION_ROUTINE GaYmEvtRequestCompletion;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS GaYmEvtWdmPreprocessRead;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS GaYmEvtWdmPreprocessDeviceControl;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS GaYmEvtWdmPreprocessInternalDeviceControl;
EVT_WDF_DEVICE_PREPARE_HARDWARE GaYmEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE GaYmEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY GaYmEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT GaYmEvtD0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL GaYmEvtSurpriseRemoval;

VOID GaYmForwardRequest(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request);
NTSTATUS GaYmCreateControlDevice(_In_ WDFDEVICE FilterDevice, _In_ PDEVICE_CONTEXT FilterCtx);
VOID GaYmDeleteControlDevice(VOID);
