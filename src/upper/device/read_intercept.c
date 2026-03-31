#include "../include/upper_device.h"
#include "../include/upper_trace.h"

#include <hidport.h>

static EVT_WDF_REQUEST_COMPLETION_ROUTINE UpperEvtInputRequestCompletion;

static BOOLEAN UpperShouldInterceptReadIoctl(_In_ ULONG IoControlCode)
{
    switch (IoControlCode) {
    case IOCTL_HID_READ_REPORT:
    case IOCTL_HID_GET_INPUT_REPORT:
        return TRUE;
    default:
        return FALSE;
    }
}

static NTSTATUS UpperResolveReadBuffer(
    _In_ WDFREQUEST Request,
    _Outptr_result_bytebuffer_(*BufferSize) PVOID* Buffer,
    _Out_ size_t* BufferSize)
{
    WDF_REQUEST_PARAMETERS params;

    *Buffer = NULL;
    *BufferSize = 0;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    if (params.Type == WdfRequestTypeRead) {
        return WdfRequestRetrieveOutputBuffer(Request, 1, Buffer, BufferSize);
    }

    if (params.Type != WdfRequestTypeDeviceControl &&
        params.Type != WdfRequestTypeDeviceControlInternal) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (params.Parameters.DeviceIoControl.IoControlCode == IOCTL_HID_READ_REPORT) {
        PIRP irp = WdfRequestWdmGetIrp(Request);
        if (irp == NULL || irp->UserBuffer == NULL || params.Parameters.DeviceIoControl.OutputBufferLength == 0) {
            return STATUS_INVALID_USER_BUFFER;
        }

        *Buffer = irp->UserBuffer;
        *BufferSize = params.Parameters.DeviceIoControl.OutputBufferLength;
        return STATUS_SUCCESS;
    }

    if (params.Parameters.DeviceIoControl.IoControlCode == IOCTL_HID_GET_INPUT_REPORT) {
        PIRP irp = WdfRequestWdmGetIrp(Request);
        PHID_XFER_PACKET packet;

        if (irp == NULL) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }

        packet = (PHID_XFER_PACKET)irp->UserBuffer;
        if (packet == NULL || packet->reportBuffer == NULL || packet->reportBufferLen == 0) {
            return STATUS_INVALID_USER_BUFFER;
        }

        *Buffer = packet->reportBuffer;
        *BufferSize = packet->reportBufferLen;
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

static VOID UpperCompleteReadWithInjectedReport(
    _In_ PUPPER_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request)
{
    GAYM_REPORT reportSnapshot;
    PVOID buffer;
    size_t bufferSize;
    ULONG bytesWritten;
    KIRQL oldIrql;
    NTSTATUS status;

    status = UpperResolveReadBuffer(Request, &buffer, &bufferSize);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    KeAcquireSpinLock(&Context->StateLock, &oldIrql);
    reportSnapshot = Context->LastInjectedReport;
    KeReleaseSpinLock(&Context->StateLock, oldIrql);

    bytesWritten = 0;
    status = UpperDeviceTranslateReport(
        Context,
        (ULONG)sizeof(reportSnapshot),
        &reportSnapshot,
        buffer,
        (ULONG)bufferSize,
        &bytesWritten);
    if (NT_SUCCESS(status)) {
        KeAcquireSpinLock(&Context->StateLock, &oldIrql);
        Context->LastObservedReport = reportSnapshot;
        Context->HasObservedReport = TRUE;
        Context->ReportsObserved++;
        KeReleaseSpinLock(&Context->StateLock, oldIrql);
        UpperTraceRecord((ULONG)IOCTL_GAYM_INJECT_REPORT, (ULONG)status);
    }

    WdfRequestCompleteWithInformation(Request, status, (ULONG_PTR)bytesWritten);
}

static VOID UpperRecordObservedReport(
    _In_ PUPPER_DEVICE_CONTEXT Context,
    _In_reads_bytes_(BufferSize) const UCHAR* Buffer,
    _In_ ULONG BufferSize)
{
    GAYM_REPORT parsedReport;
    KIRQL oldIrql;

    if (!NT_SUCCESS(UpperDeviceParseNativeReport(Context, Buffer, BufferSize, &parsedReport))) {
        return;
    }

    KeAcquireSpinLock(&Context->StateLock, &oldIrql);
    Context->LastObservedReport = parsedReport;
    Context->HasObservedReport = TRUE;
    Context->ReportsObserved++;
    KeReleaseSpinLock(&Context->StateLock, oldIrql);
}

static NTSTATUS UpperForwardRequestToLower(
    _In_ PUPPER_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request)
{
    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSetCompletionRoutine(Request, UpperEvtInputRequestCompletion, Context);

    if (!WdfRequestSend(Request, Context->LowerTarget, WDF_NO_SEND_OPTIONS)) {
        return WdfRequestGetStatus(Request);
    }

    return STATUS_PENDING;
}

static VOID UpperForwardRequest(
    _In_ PUPPER_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request)
{
    WDF_REQUEST_SEND_OPTIONS options;

    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    WdfRequestFormatRequestUsingCurrentType(Request);

    if (!WdfRequestSend(Request, Context->LowerTarget, &options)) {
        WdfRequestComplete(Request, WdfRequestGetStatus(Request));
    }
}

static VOID UpperEvtInputRequestCompletion(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context)
{
    PUPPER_DEVICE_CONTEXT upperContext;
    BOOLEAN overrideEnabled;
    BOOLEAN hasInjectedReport;
    PVOID buffer;
    size_t bufferSize;
    KIRQL oldIrql;

    UNREFERENCED_PARAMETER(Target);

    upperContext = (PUPPER_DEVICE_CONTEXT)Context;

    KeAcquireSpinLock(&upperContext->StateLock, &oldIrql);
    overrideEnabled = upperContext->OverrideEnabled;
    hasInjectedReport = upperContext->HasInjectedReport;
    KeReleaseSpinLock(&upperContext->StateLock, oldIrql);

    if (overrideEnabled && hasInjectedReport) {
        UpperCompleteReadWithInjectedReport(upperContext, Request);
        return;
    }

    if (NT_SUCCESS(Params->IoStatus.Status) &&
        NT_SUCCESS(UpperResolveReadBuffer(Request, &buffer, &bufferSize))) {
        UpperRecordObservedReport(upperContext, (const UCHAR*)buffer, (ULONG)min(bufferSize, Params->IoStatus.Information));
    }

    WdfRequestCompleteWithInformation(Request, Params->IoStatus.Status, Params->IoStatus.Information);
}

static VOID UpperHandleReadRequest(
    _In_ PUPPER_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode)
{
    BOOLEAN overrideEnabled;
    BOOLEAN hasInjectedReport;
    KIRQL oldIrql;
    NTSTATUS status;

    KeAcquireSpinLock(&Context->StateLock, &oldIrql);
    overrideEnabled = Context->OverrideEnabled;
    hasInjectedReport = Context->HasInjectedReport;
    Context->LastInterceptedIoctl = (LONG)IoControlCode;
    KeReleaseSpinLock(&Context->StateLock, oldIrql);

    if (overrideEnabled && hasInjectedReport) {
        UpperCompleteReadWithInjectedReport(Context, Request);
        return;
    }

    status = UpperForwardRequestToLower(Context, Request);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
    }
}

VOID UpperEvtIoRead(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t Length)
{
    PUPPER_DEVICE_CONTEXT context = UpperGetContext(WdfIoQueueGetDevice(Queue));

    UNREFERENCED_PARAMETER(Length);

    InterlockedIncrement(&context->ReadRequestsSeen);
    UpperHandleReadRequest(context, Request, IRP_MJ_READ);
}

VOID UpperEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PUPPER_DEVICE_CONTEXT context = UpperGetContext(WdfIoQueueGetDevice(Queue));

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    InterlockedIncrement(&context->DeviceControlRequestsSeen);
    if (UpperShouldInterceptReadIoctl(IoControlCode)) {
        UpperHandleReadRequest(context, Request, IoControlCode);
        return;
    }

    UpperForwardRequest(context, Request);
}

VOID UpperEvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PUPPER_DEVICE_CONTEXT context = UpperGetContext(WdfIoQueueGetDevice(Queue));

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    InterlockedIncrement(&context->InternalDeviceControlRequestsSeen);
    if (UpperShouldInterceptReadIoctl(IoControlCode)) {
        UpperHandleReadRequest(context, Request, IoControlCode);
        return;
    }

    UpperForwardRequest(context, Request);
}

NTSTATUS UpperDeviceHandleReadIntercept(_In_ PUPPER_DEVICE_CONTEXT Context, _In_ WDFREQUEST Request)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Request);
    return STATUS_NOT_SUPPORTED;
}
