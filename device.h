#pragma once
/*
 * GaYmFilter - Device context and WDF callback declarations
 */

#include <ntddk.h>
#include <wdf.h>
#include "ioctl.h"
#include "devices.h"
#include "logging.h"

/* ─── Per-device context (filter device in the HID stack) ─── */
typedef struct _DEVICE_CONTEXT {
    WDFDEVICE                       Device;
    WDFIOTARGET                     IoTarget;           /* Lower driver       */
    WDFQUEUE                        DefaultQueue;       /* Parallel dispatch   */
    WDFQUEUE                        PendingReadsQueue;  /* Manual: pended HID input requests */
    WDFCOLLECTION                   ActiveInputRequests; /* Requests currently sent to HidUsb */
    WDFSPINLOCK                     ActiveInputRequestsLock;

    /* Device identification */
    USHORT                          VendorId;
    USHORT                          ProductId;
    GAYM_DEVICE_TYPE                DeviceType;
    const GAYM_DEVICE_DESCRIPTOR*   DeviceDesc;

    /* Override state */
    BOOLEAN                         OverrideEnabled;
    KSPIN_LOCK                      ReportLock;
    GAYM_REPORT                     CurrentReport;
    BOOLEAN                         HasReport;          /* TRUE after first inject */
    ULONG                           ReportsSent;
    UCHAR                           SeqCounter;         /* For DualSense sequence  */
    volatile LONG                   PendingInputRequests;
    volatile LONG                   QueuedInputRequests;
    volatile LONG                   CompletedInputRequests;
    volatile LONG                   ForwardedInputRequests;
    volatile LONG                   LastInterceptedIoctl;
    volatile LONG                   ReadRequestsSeen;
    volatile LONG                   DeviceControlRequestsSeen;
    volatile LONG                   InternalDeviceControlRequestsSeen;
    volatile LONG                   WriteRequestsSeen;
    BOOLEAN                         IsInD0;

    /* Jitter (config only - actual delay is in user-mode) */
    GAYM_JITTER_CONFIG              JitterConfig;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

/* ─── Control device context (CDO — direct IOCTL path, bypasses xinputhid) ─── */
typedef struct _CONTROL_DEVICE_CONTEXT {
    PDEVICE_CONTEXT                 FilterCtx;          /* Points to the filter's context */
} CONTROL_DEVICE_CONTEXT, *PCONTROL_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CONTROL_DEVICE_CONTEXT, ControlGetContext)

/* ─── WDF callbacks ─── */
EVT_WDF_DRIVER_DEVICE_ADD                   GaYmEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_READ                    GaYmEvtIoRead;
EVT_WDF_IO_QUEUE_IO_WRITE                   GaYmEvtIoWrite;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL          GaYmEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL          GaYmEvtCtlIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL GaYmEvtIoInternalDeviceControl;
EVT_WDF_DEVICE_PREPARE_HARDWARE             GaYmEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE             GaYmEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY                     GaYmEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT                      GaYmEvtD0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL             GaYmEvtSurpriseRemoval;

/* ─── Helpers ─── */
VOID GaYmForwardRequest(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);
VOID GaYmCompleteReadWithReport(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST ReadRequest);
VOID GaYmDrainPendingReads(_In_ PDEVICE_CONTEXT Ctx, _In_ NTSTATUS CompletionStatus);
NTSTATUS GaYmCreateControlDevice(_In_ WDFDEVICE FilterDevice, _In_ PDEVICE_CONTEXT FilterCtx);
VOID GaYmDeleteControlDevice(VOID);
