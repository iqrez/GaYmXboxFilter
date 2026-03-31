#pragma once
/*
 * GaYmFilter - Device context and WDF callback declarations
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
    WDFCOLLECTION ActiveInputRequests;
    WDFSPINLOCK ActiveInputRequestsLock;

    USHORT VendorId;
    USHORT ProductId;
    GAYM_DEVICE_TYPE DeviceType;
    const GAYM_DEVICE_DESCRIPTOR* DeviceDesc;

    KSPIN_LOCK ReportLock;
    GAYM_REPORT ObservedReport;
    BOOLEAN HasObservedReport;
    volatile LONG ForwardedInputRequests;
    volatile LONG LastInterceptedIoctl;
    volatile LONG ReadRequestsSeen;
    volatile LONG DeviceControlRequestsSeen;
    volatile LONG InternalDeviceControlRequestsSeen;
    volatile LONG WriteRequestsSeen;
    BOOLEAN IsInD0;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

EVT_WDF_DRIVER_DEVICE_ADD GaYmEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_READ GaYmEvtIoRead;
EVT_WDF_IO_QUEUE_IO_WRITE GaYmEvtIoWrite;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL GaYmEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL GaYmEvtIoInternalDeviceControl;
EVT_WDF_DEVICE_PREPARE_HARDWARE GaYmEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE GaYmEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY GaYmEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT GaYmEvtD0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL GaYmEvtSurpriseRemoval;

VOID GaYmForwardRequest(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);
