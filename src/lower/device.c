/*
 * GaYmFilter - Lower-filter forwarding and native observation capture.
 *
 * The lower driver no longer exposes a producer-facing control device. It
 * remains in the HID stack to forward requests to the real controller and to
 * parse completed native reads for diagnostic observation.
 */

#include "device.h"

#include <hidport.h>

static EVT_WDF_REQUEST_COMPLETION_ROUTINE GaYmEvtInputRequestCompletion;

static SHORT GaYmAxisU16ToI16(_In_ USHORT value)
{
    LONG shifted = (LONG)value - 32768L;

    if (shifted < -32767L) {
        shifted = -32767L;
    } else if (shifted > 32767L) {
        shifted = 32767L;
    }

    return (SHORT)shifted;
}

static BOOLEAN GaYmTryParseXboxUsbObservedReport(
    _In_reads_bytes_(bufferLength) const UCHAR* buffer,
    _In_ ULONG bufferLength,
    _Out_ PGAYM_REPORT report)
{
    USHORT leftX;
    USHORT leftY;
    USHORT rightX;
    USHORT rightY;
    USHORT zAxis;

    if (buffer == NULL || report == NULL || bufferLength < 16) {
        return FALSE;
    }

    RtlZeroMemory(report, sizeof(*report));
    report->ReportId = buffer[0];

    leftX = (USHORT)(buffer[1] | (buffer[2] << 8));
    leftY = (USHORT)(buffer[3] | (buffer[4] << 8));
    rightX = (USHORT)(buffer[5] | (buffer[6] << 8));
    rightY = (USHORT)(buffer[7] | (buffer[8] << 8));
    zAxis = (USHORT)(buffer[9] | (buffer[10] << 8));

    report->ThumbLeftX = GaYmAxisU16ToI16(leftX);
    report->ThumbLeftY = GaYmAxisU16ToI16(leftY);
    report->ThumbRightX = GaYmAxisU16ToI16(rightX);
    report->ThumbRightY = GaYmAxisU16ToI16(rightY);
    report->Buttons[0] = buffer[11];
    report->Buttons[1] = (UCHAR)(buffer[12] & 0x0F);

    if (buffer[13] == 0) {
        report->DPad = GAYM_DPAD_NEUTRAL;
    } else if (buffer[13] <= 8) {
        report->DPad = (UCHAR)(buffer[13] - 1);
    } else {
        report->DPad = GAYM_DPAD_NEUTRAL;
    }

    if (zAxis > 32768U) {
        ULONG rightTrigger = (ULONG)(zAxis - 32768U) / 128UL;
        if (rightTrigger > 255UL) {
            rightTrigger = 255UL;
        }
        report->TriggerRight = (UCHAR)rightTrigger;
    } else if (zAxis < 32768U) {
        ULONG leftTrigger = (ULONG)(32768U - zAxis) / 128UL;
        if (leftTrigger > 255UL) {
            leftTrigger = 255UL;
        }
        report->TriggerLeft = (UCHAR)leftTrigger;
    }

    return TRUE;
}

static VOID GaYmCaptureObservedReport(
    _In_ PDEVICE_CONTEXT context,
    _In_reads_bytes_(bufferLength) const UCHAR* buffer,
    _In_ ULONG bufferLength)
{
    GAYM_REPORT parsedReport;
    KIRQL oldIrql;

    if (context == NULL || buffer == NULL) {
        return;
    }

    if (context->VendorId != 0x045E || context->ProductId != 0x02FF) {
        return;
    }

    if (!GaYmTryParseXboxUsbObservedReport(buffer, bufferLength, &parsedReport)) {
        return;
    }

    KeAcquireSpinLock(&context->ReportLock, &oldIrql);
    context->ObservedReport = parsedReport;
    context->HasObservedReport = TRUE;
    KeReleaseSpinLock(&context->ReportLock, oldIrql);
}

static VOID GaYmCaptureObservedReportFromRequest(
    _In_ PDEVICE_CONTEXT context,
    _In_ WDFREQUEST request,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS params)
{
    WDF_REQUEST_PARAMETERS requestParameters;
    PVOID buffer = NULL;
    size_t bufferSize = 0;
    ULONG observedBytes;

    if (context == NULL || request == NULL || params == NULL) {
        return;
    }

    if (!NT_SUCCESS(params->IoStatus.Status) || params->IoStatus.Information == 0) {
        return;
    }

    WDF_REQUEST_PARAMETERS_INIT(&requestParameters);
    WdfRequestGetParameters(request, &requestParameters);

    if (requestParameters.Type == WdfRequestTypeRead) {
        if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(request, 1, &buffer, &bufferSize))) {
            return;
        }
    } else if (requestParameters.Type == WdfRequestTypeDeviceControl ||
               requestParameters.Type == WdfRequestTypeDeviceControlInternal) {
        PIRP irp = WdfRequestWdmGetIrp(request);
        ULONG ioControlCode;

        if (irp == NULL) {
            return;
        }

        ioControlCode = requestParameters.Parameters.DeviceIoControl.IoControlCode;
        if (ioControlCode == IOCTL_HID_READ_REPORT) {
            buffer = irp->UserBuffer;
            bufferSize = requestParameters.Parameters.DeviceIoControl.OutputBufferLength;
        } else if (ioControlCode == IOCTL_HID_GET_INPUT_REPORT) {
            PHID_XFER_PACKET packet = (PHID_XFER_PACKET)irp->UserBuffer;
            if (packet == NULL || packet->reportBuffer == NULL || packet->reportBufferLen == 0) {
                return;
            }
            buffer = packet->reportBuffer;
            bufferSize = packet->reportBufferLen;
        } else {
            return;
        }
    } else {
        return;
    }

    if (buffer == NULL || bufferSize == 0) {
        return;
    }

    observedBytes = (ULONG)min(bufferSize, (size_t)params->IoStatus.Information);
    GaYmCaptureObservedReport(context, (const UCHAR*)buffer, observedBytes);
}

static NTSTATUS GaYmTrackActiveInputRequest(
    _In_ PDEVICE_CONTEXT context,
    _In_ WDFREQUEST request)
{
    NTSTATUS status;

    WdfSpinLockAcquire(context->ActiveInputRequestsLock);
    status = WdfCollectionAdd(context->ActiveInputRequests, request);
    WdfSpinLockRelease(context->ActiveInputRequestsLock);

    return status;
}

static VOID GaYmUntrackActiveInputRequest(
    _In_ PDEVICE_CONTEXT context,
    _In_ WDFREQUEST request)
{
    ULONG count;
    ULONG index;

    WdfSpinLockAcquire(context->ActiveInputRequestsLock);

    count = WdfCollectionGetCount(context->ActiveInputRequests);
    for (index = 0; index < count; index++) {
        if (WdfCollectionGetItem(context->ActiveInputRequests, index) == request) {
            WdfCollectionRemoveItem(context->ActiveInputRequests, index);
            break;
        }
    }

    WdfSpinLockRelease(context->ActiveInputRequestsLock);
}

static VOID GaYmCancelActiveInputRequests(_In_ PDEVICE_CONTEXT context)
{
    WDFREQUEST snapshot[32];
    ULONG count;
    ULONG index;

    RtlZeroMemory(snapshot, sizeof(snapshot));

    WdfSpinLockAcquire(context->ActiveInputRequestsLock);

    count = WdfCollectionGetCount(context->ActiveInputRequests);
    if (count > RTL_NUMBER_OF(snapshot)) {
        GAYM_LOG_WARN(
            "CancelActiveInputRequests truncating %lu requests to %Iu",
            count,
            RTL_NUMBER_OF(snapshot));
        count = RTL_NUMBER_OF(snapshot);
    }

    for (index = 0; index < count; index++) {
        snapshot[index] = (WDFREQUEST)WdfCollectionGetItem(context->ActiveInputRequests, index);
    }

    WdfSpinLockRelease(context->ActiveInputRequestsLock);

    for (index = 0; index < count; index++) {
        if (snapshot[index] != NULL) {
            (VOID)WdfRequestCancelSentRequest(snapshot[index]);
        }
    }
}

static NTSTATUS GaYmSendInputRequestToLower(
    _In_ PDEVICE_CONTEXT context,
    _In_ WDFREQUEST request)
{
    NTSTATUS status;

    WdfRequestFormatRequestUsingCurrentType(request);
    WdfRequestSetCompletionRoutine(request, GaYmEvtInputRequestCompletion, context);

    status = GaYmTrackActiveInputRequest(context, request);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (!WdfRequestSend(request, context->IoTarget, WDF_NO_SEND_OPTIONS)) {
        status = WdfRequestGetStatus(request);
        GaYmUntrackActiveInputRequest(context, request);
        return status;
    }

    return STATUS_PENDING;
}

static VOID QueryDeviceIds(
    _In_ WDFDEVICE device,
    _Out_ PUSHORT vendorId,
    _Out_ PUSHORT productId)
{
    PDEVICE_OBJECT physicalDevice;
    WCHAR hardwareId[512];
    ULONG resultLength = 0;
    NTSTATUS status;

    *vendorId = 0;
    *productId = 0;

    physicalDevice = WdfDeviceWdmGetPhysicalDevice(device);
    if (physicalDevice == NULL) {
        return;
    }

    status = IoGetDeviceProperty(
        physicalDevice,
        DevicePropertyHardwareID,
        sizeof(hardwareId),
        hardwareId,
        &resultLength);
    if (NT_SUCCESS(status)) {
        GaYmParseHardwareId(hardwareId, vendorId, productId);
    } else {
        GAYM_LOG_WARN("IoGetDeviceProperty(HardwareID) failed: 0x%08X", status);
    }
}

VOID GaYmForwardRequest(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    WDF_REQUEST_SEND_OPTIONS options;

    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    WdfRequestFormatRequestUsingCurrentType(Request);

    if (!WdfRequestSend(Request, WdfDeviceGetIoTarget(Device), &options)) {
        NTSTATUS status = WdfRequestGetStatus(Request);

        GAYM_LOG_WARN("Forward failed: 0x%08X", status);
        WdfRequestComplete(Request, status);
    }
}

static BOOLEAN GaYmShouldInterceptInputIoctl(_In_ ULONG ioControlCode)
{
    switch (ioControlCode) {
    case IOCTL_HID_READ_REPORT:
    case IOCTL_HID_GET_INPUT_REPORT:
        return TRUE;
    default:
        return FALSE;
    }
}

static VOID GaYmEvtInputRequestCompletion(
    _In_ WDFREQUEST request,
    _In_ WDFIOTARGET target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS params,
    _In_ WDFCONTEXT contextValue)
{
    PDEVICE_CONTEXT context = (PDEVICE_CONTEXT)contextValue;

    UNREFERENCED_PARAMETER(target);

    GaYmUntrackActiveInputRequest(context, request);
    GaYmCaptureObservedReportFromRequest(context, request, params);
    InterlockedIncrement(&context->ForwardedInputRequests);
    WdfRequestCompleteWithInformation(
        request,
        params->IoStatus.Status,
        params->IoStatus.Information);
}

NTSTATUS GaYmEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_OBJECT_ATTRIBUTES objectAttributes;
    WDFDEVICE device;
    PDEVICE_CONTEXT context;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    GAYM_LOG_INFO("DeviceAdd: attaching lower observation filter");

    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = GaYmEvtPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = GaYmEvtReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry = GaYmEvtD0Entry;
    pnpCallbacks.EvtDeviceD0Exit = GaYmEvtD0Exit;
    pnpCallbacks.EvtDeviceSurpriseRemoval = GaYmEvtSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("WdfDeviceCreate failed: 0x%08X", status);
        return status;
    }

    context = DeviceGetContext(device);
    RtlZeroMemory(context, sizeof(*context));
    context->Device = device;
    context->IoTarget = WdfDeviceGetIoTarget(device);
    context->IsInD0 = FALSE;
    KeInitializeSpinLock(&context->ReportLock);

    QueryDeviceIds(device, &context->VendorId, &context->ProductId);
    context->DeviceDesc = GaYmLookupDevice(context->VendorId, context->ProductId);
    context->DeviceType = context->DeviceDesc != NULL
        ? context->DeviceDesc->DeviceType
        : GAYM_DEVICE_UNKNOWN;

    GAYM_LOG_INFO(
        "Device identified: %s (VID:%04X PID:%04X)",
        GaYmDeviceTypeName(context->DeviceType),
        context->VendorId,
        context->ProductId);

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoRead = GaYmEvtIoRead;
    queueConfig.EvtIoWrite = GaYmEvtIoWrite;
    queueConfig.EvtIoDeviceControl = GaYmEvtIoDeviceControl;
    queueConfig.EvtIoInternalDeviceControl = GaYmEvtIoInternalDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &context->DefaultQueue);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("Default queue create failed: 0x%08X", status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttributes);
    objectAttributes.ParentObject = device;
    status = WdfCollectionCreate(&objectAttributes, &context->ActiveInputRequests);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("ActiveInputRequests collection create failed: 0x%08X", status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttributes);
    objectAttributes.ParentObject = device;
    status = WdfSpinLockCreate(&objectAttributes, &context->ActiveInputRequestsLock);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("ActiveInputRequests lock create failed: 0x%08X", status);
        return status;
    }

    GAYM_LOG_INFO("DeviceAdd: lower observation filter attached successfully");
    return STATUS_SUCCESS;
}

VOID GaYmEvtIoRead(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t Length)
{
    PDEVICE_CONTEXT context;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Length);

    context = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    InterlockedIncrement(&context->ReadRequestsSeen);

    status = GaYmSendInputRequestToLower(context, Request);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_WARN("Read forward failed: 0x%08X", status);
        WdfRequestComplete(Request, status);
    }
}

VOID GaYmEvtIoWrite(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t Length)
{
    PDEVICE_CONTEXT context;

    UNREFERENCED_PARAMETER(Length);

    context = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    InterlockedIncrement(&context->WriteRequestsSeen);
    GaYmForwardRequest(context->Device, Request);
}

VOID GaYmEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PDEVICE_CONTEXT context;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    context = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    InterlockedIncrement(&context->DeviceControlRequestsSeen);

    if (!GaYmShouldInterceptInputIoctl(IoControlCode)) {
        GaYmForwardRequest(context->Device, Request);
        return;
    }

    InterlockedExchange(&context->LastInterceptedIoctl, (LONG)IoControlCode);
    status = GaYmSendInputRequestToLower(context, Request);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_WARN("DeviceControl forward failed: 0x%08X", status);
        WdfRequestComplete(Request, status);
    }
}

VOID GaYmEvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PDEVICE_CONTEXT context;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    context = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    InterlockedIncrement(&context->InternalDeviceControlRequestsSeen);

    if (!GaYmShouldInterceptInputIoctl(IoControlCode)) {
        GaYmForwardRequest(context->Device, Request);
        return;
    }

    InterlockedExchange(&context->LastInterceptedIoctl, (LONG)IoControlCode);
    status = GaYmSendInputRequestToLower(context, Request);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_WARN("InternalDeviceControl forward failed: 0x%08X", status);
        WdfRequestComplete(Request, status);
    }
}

NTSTATUS GaYmEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT context;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    context = DeviceGetContext(Device);
    GAYM_LOG_INFO(
        "PrepareHardware: %s (VID:%04X PID:%04X)",
        GaYmDeviceTypeName(context->DeviceType),
        context->VendorId,
        context->ProductId);
    return STATUS_SUCCESS;
}

NTSTATUS GaYmEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT context;
    KIRQL oldIrql;

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    context = DeviceGetContext(Device);
    GaYmCancelActiveInputRequests(context);

    KeAcquireSpinLock(&context->ReportLock, &oldIrql);
    context->HasObservedReport = FALSE;
    RtlZeroMemory(&context->ObservedReport, sizeof(context->ObservedReport));
    KeReleaseSpinLock(&context->ReportLock, oldIrql);

    context->IsInD0 = FALSE;
    GAYM_LOG_INFO("ReleaseHardware: lower observation filter detached");
    return STATUS_SUCCESS;
}

NTSTATUS GaYmEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT context;

    context = DeviceGetContext(Device);
    context->IsInD0 = TRUE;

    GAYM_LOG_INFO(
        "D0Entry: %s resumed from power state %d",
        GaYmDeviceTypeName(context->DeviceType),
        (int)PreviousState);
    return STATUS_SUCCESS;
}

NTSTATUS GaYmEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT context;

    context = DeviceGetContext(Device);
    GaYmCancelActiveInputRequests(context);
    context->IsInD0 = FALSE;

    GAYM_LOG_INFO(
        "D0Exit: lower observation filter leaving D0 for state %d",
        (int)TargetState);
    return STATUS_SUCCESS;
}

VOID GaYmEvtSurpriseRemoval(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT context;

    context = DeviceGetContext(Device);
    GaYmCancelActiveInputRequests(context);
    context->IsInD0 = FALSE;

    GAYM_LOG_INFO(
        "SurpriseRemoval: %s unplugged (VID:%04X PID:%04X)",
        GaYmDeviceTypeName(context->DeviceType),
        context->VendorId,
        context->ProductId);
}
