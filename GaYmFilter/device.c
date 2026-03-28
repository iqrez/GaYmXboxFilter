/*
 * GaYmFilter - Device lifecycle, I/O dispatch, PnP, and power management.
 *
 * Architecture: KMDF lower-filter on HID collection PDOs (below xinputhid).
 *
 *   xinputhid   ← upper filter, reads HID reports and exposes XInput
 *       ↓
 *   GaYmFilter  ← intercepts IRP_MJ_READ when override is active
 *       ↓
 *   HID collection PDO (HIDClass)
 *       ↓
 *   HID minidriver (hidusb / xboxgip / etc.)
 *
 * In the hybrid design this lower filter is capture-first: it observes the
 * live HID-child traffic and decodes semantic state, while the upper filter
 * owns user-visible override/output.
 */

#include "device.h"
#include <hidport.h>
#include <usb.h>
#include <usbioctl.h>
#include <ntstrsafe.h>

#define GAYM_FILTER_WDM_POOL_TAG 'fwYG'
#define GAYM_FILTER_QUERY_LAYOUT_VERSION  5u
#define GAYM_FILTER_BUILD_STAMP           0x20260327u

typedef struct _GAYM_WDM_IRP_CONTEXT {
    PDEVICE_CONTEXT DeviceContext;
    ULONG RequestType;
    ULONG IoControlCode;
    ULONG InputLength;
    ULONG OutputLength;
    UCHAR InputSampleLength;
    UCHAR InputSample[GAYM_TRACE_SAMPLE_BYTES];
} GAYM_WDM_IRP_CONTEXT, *PGAYM_WDM_IRP_CONTEXT;

static VOID GaYmSetControlFilterContext(
    _In_ PCONTROL_DEVICE_CONTEXT ControlContext,
    _In_opt_ PDEVICE_CONTEXT FilterCtx)
{
    WdfWaitLockAcquire(ControlContext->RouteLock, NULL);
    ControlContext->FilterCtx = FilterCtx;
    WdfWaitLockRelease(ControlContext->RouteLock);
}

static VOID GaYmClearControlFilterContextIfMatch(
    _In_ PCONTROL_DEVICE_CONTEXT ControlContext,
    _In_ PDEVICE_CONTEXT FilterCtx)
{
    WdfWaitLockAcquire(ControlContext->RouteLock, NULL);
    if (ControlContext->FilterCtx == FilterCtx) {
        ControlContext->FilterCtx = NULL;
    }
    WdfWaitLockRelease(ControlContext->RouteLock);
}

static PDEVICE_CONTEXT GaYmAcquireActiveControlFilterContext(
    _In_ PCONTROL_DEVICE_CONTEXT ControlContext)
{
    PDEVICE_CONTEXT filterCtx = NULL;

    WdfWaitLockAcquire(ControlContext->RouteLock, NULL);
    if (ControlContext->FilterCtx != NULL && ControlContext->FilterCtx->IsInD0) {
        filterCtx = ControlContext->FilterCtx;
        WdfObjectReference(filterCtx->Device);
    }
    WdfWaitLockRelease(ControlContext->RouteLock);

    return filterCtx;
}

static PVOID GaYmMapIrpBuffer(_In_ PIRP Irp);
static NTSTATUS GaYmRetrieveUsbUrbTransferBuffer(
    _In_ PIRP Irp,
    _Outptr_result_bytebuffer_(*BufferSize) PVOID* Buffer,
    _Out_ size_t* BufferSize);
static VOID GaYmRecordTrace(
    _Inout_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG Phase,
    _In_ ULONG RequestType,
    _In_ ULONG IoControlCode,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength,
    _In_ ULONG TransferLength,
    _In_ NTSTATUS Status,
    _In_reads_bytes_opt_(SampleLength) const VOID* Sample,
    _In_ ULONG SampleLength);

static ULONG GaYmMinUlong(_In_ ULONG left, _In_ ULONG right)
{
    return left < right ? left : right;
}

static UCHAR GaYmCaptureIrpInputSample(
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION Stack,
    _In_ ULONG InputLength,
    _Out_writes_bytes_(GAYM_TRACE_SAMPLE_BYTES) UCHAR* Sample)
{
    PVOID buffer = NULL;
    UCHAR bytesToCopy;

    RtlZeroMemory(Sample, GAYM_TRACE_SAMPLE_BYTES);

    if (InputLength == 0) {
        return 0;
    }

    if (Irp->AssociatedIrp.SystemBuffer != NULL) {
        buffer = Irp->AssociatedIrp.SystemBuffer;
    } else if (Stack->Parameters.DeviceIoControl.Type3InputBuffer != NULL) {
        buffer = Stack->Parameters.DeviceIoControl.Type3InputBuffer;
    }

    if (buffer == NULL) {
        return 0;
    }

    bytesToCopy = (UCHAR)GaYmMinUlong(InputLength, GAYM_TRACE_SAMPLE_BYTES);

    __try {
        RtlCopyMemory(Sample, buffer, bytesToCopy);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RtlZeroMemory(Sample, GAYM_TRACE_SAMPLE_BYTES);
        return 0;
    }

    return bytesToCopy;
}

static UCHAR GaYmCopyIrpOutputSample(
    _In_ PIRP Irp,
    _In_ ULONG RequestedBytes,
    _Out_writes_bytes_(GAYM_TRACE_SAMPLE_BYTES) UCHAR* Sample)
{
    PVOID buffer;
    UCHAR bytesToCopy;

    RtlZeroMemory(Sample, GAYM_TRACE_SAMPLE_BYTES);

    if (RequestedBytes == 0) {
        return 0;
    }

    buffer = GaYmMapIrpBuffer(Irp);
    if (buffer == NULL) {
        return 0;
    }

    bytesToCopy = (UCHAR)GaYmMinUlong(RequestedBytes, GAYM_TRACE_SAMPLE_BYTES);

    __try {
        RtlCopyMemory(Sample, buffer, bytesToCopy);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RtlZeroMemory(Sample, GAYM_TRACE_SAMPLE_BYTES);
        return 0;
    }

    return bytesToCopy;
}

static VOID GaYmRecordIrpDispatchTrace(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ const GAYM_WDM_IRP_CONTEXT* IrpContext)
{
    GaYmRecordTrace(
        Ctx,
        GAYM_TRACE_PHASE_DISPATCH,
        IrpContext->RequestType,
        IrpContext->IoControlCode,
        IrpContext->InputLength,
        IrpContext->OutputLength,
        0,
        STATUS_SUCCESS,
        IrpContext->InputSampleLength != 0 ? IrpContext->InputSample : NULL,
        IrpContext->InputSampleLength);
}

static VOID GaYmRecordIrpCompletionTrace(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ const GAYM_WDM_IRP_CONTEXT* IrpContext,
    _In_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG TransferLength)
{
    UCHAR sample[GAYM_TRACE_SAMPLE_BYTES];
    UCHAR sampleLength = 0;

    if (IrpContext->RequestType != GAYM_TRACE_REQUEST_WRITE) {
        sampleLength = GaYmCopyIrpOutputSample(Irp, TransferLength, sample);
    }

    GaYmRecordTrace(
        Ctx,
        GAYM_TRACE_PHASE_COMPLETION,
        IrpContext->RequestType,
        IrpContext->IoControlCode,
        IrpContext->InputLength,
        IrpContext->OutputLength,
        TransferLength,
        Status,
        sampleLength != 0 ? sample : NULL,
        sampleLength);
}

static PVOID GaYmMapIrpBuffer(_In_ PIRP Irp)
{
    if (Irp == NULL) {
        return NULL;
    }

    if (Irp->MdlAddress != NULL) {
        return MmGetSystemAddressForMdlSafe(
            Irp->MdlAddress,
            NormalPagePriority | MdlMappingNoExecute);
    }

    if (Irp->UserBuffer != NULL) {
        return Irp->UserBuffer;
    }

    return Irp->AssociatedIrp.SystemBuffer;
}

static VOID GaYmCopySampleBytes(
    _Out_writes_bytes_(DestinationCapacity) UCHAR* Destination,
    _In_ ULONG DestinationCapacity,
    _Out_ PULONG DestinationLength,
    _In_reads_bytes_opt_(SourceLength) const VOID* Source,
    _In_ ULONG SourceLength)
{
    ULONG bytesToCopy = 0;

    if (DestinationLength != NULL) {
        *DestinationLength = 0;
    }

    if (Destination == NULL || DestinationCapacity == 0) {
        return;
    }

    RtlZeroMemory(Destination, DestinationCapacity);

    if (Source == NULL || SourceLength == 0) {
        return;
    }

    bytesToCopy = GaYmMinUlong(DestinationCapacity, SourceLength);
    RtlCopyMemory(Destination, Source, bytesToCopy);

    if (DestinationLength != NULL) {
        *DestinationLength = bytesToCopy;
    }
}

static VOID GaYmResetTelemetry(_Inout_ PDEVICE_CONTEXT Ctx)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Ctx->TelemetryLock, &oldIrql);
    Ctx->LastCompletedStatus = 0;
    Ctx->LastCompletionInformation = 0;
    Ctx->LastReadLength = 0;
    Ctx->LastWriteLength = 0;
    Ctx->LastWriteSampleLength = 0;
    Ctx->LastDeviceControlInputLength = 0;
    Ctx->LastDeviceControlOutputLength = 0;
    Ctx->LastInternalInputLength = 0;
    Ctx->LastInternalOutputLength = 0;
    Ctx->LastRawReadSampleLength = 0;
    Ctx->LastPatchedReadSampleLength = 0;
    Ctx->LastRawReadCompletionLength = 0;
    Ctx->LastPatchedReadCompletionLength = 0;
    Ctx->LastNativeOverrideApplied = 0;
    Ctx->LastNativeOverrideBytesWritten = 0;
    Ctx->LastSemanticCaptureFlags = 0;
    Ctx->LastSemanticCaptureLength = 0;
    Ctx->LastSemanticCaptureIoctl = 0;
    Ctx->LastSemanticCaptureSampleLength = 0;
    Ctx->TraceSequence = 0;
    Ctx->TraceCount = 0;
    RtlZeroMemory(Ctx->LastRawReadSample, sizeof(Ctx->LastRawReadSample));
    RtlZeroMemory(Ctx->LastPatchedReadSample, sizeof(Ctx->LastPatchedReadSample));
    RtlZeroMemory(Ctx->LastWriteSample, sizeof(Ctx->LastWriteSample));
    RtlZeroMemory(Ctx->LastSemanticCaptureSample, sizeof(Ctx->LastSemanticCaptureSample));
    RtlZeroMemory(&Ctx->LastSemanticCaptureReport, sizeof(Ctx->LastSemanticCaptureReport));
    RtlZeroMemory(Ctx->Trace, sizeof(Ctx->Trace));
    KeReleaseSpinLock(&Ctx->TelemetryLock, oldIrql);
}

static VOID GaYmRecordTrace(
    _Inout_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG Phase,
    _In_ ULONG RequestType,
    _In_ ULONG IoControlCode,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength,
    _In_ ULONG TransferLength,
    _In_ NTSTATUS Status,
    _In_reads_bytes_opt_(SampleLength) const VOID* Sample,
    _In_ ULONG SampleLength)
{
    KIRQL oldIrql;
    ULONG slotIndex;
    GAYM_TRACE_ENTRY* entry;
    ULONG actualSampleLength = SampleLength;

    KeAcquireSpinLock(&Ctx->TelemetryLock, &oldIrql);

    if (Ctx->TraceCount < GAYM_TRACE_HISTORY_COUNT) {
        slotIndex = Ctx->TraceCount;
        Ctx->TraceCount += 1;
    } else {
        RtlMoveMemory(
            &Ctx->Trace[0],
            &Ctx->Trace[1],
            sizeof(Ctx->Trace) - sizeof(Ctx->Trace[0]));
        slotIndex = GAYM_TRACE_HISTORY_COUNT - 1;
    }

    entry = &Ctx->Trace[slotIndex];
    RtlZeroMemory(entry, sizeof(*entry));
    entry->Sequence = ++Ctx->TraceSequence;
    entry->Phase = Phase;
    entry->RequestType = RequestType;
    entry->IoControlCode = IoControlCode;
    entry->InputLength = InputLength;
    entry->OutputLength = OutputLength;
    entry->TransferLength = TransferLength;
    entry->Status = (ULONG)Status;
    GaYmCopySampleBytes(entry->Sample, sizeof(entry->Sample), &actualSampleLength, Sample, SampleLength);
    entry->SampleLength = (UCHAR)actualSampleLength;

    KeReleaseSpinLock(&Ctx->TelemetryLock, oldIrql);
}

/* ═══════════════════════════════════════════════════════════════════
 * Helper: query VID/PID from the physical device object
 * ═══════════════════════════════════════════════════════════════════ */
static VOID QueryDeviceIds(
    _In_  WDFDEVICE Device,
    _Out_ PUSHORT   VendorId,
    _Out_ PUSHORT   ProductId)
{
    *VendorId  = 0;
    *ProductId = 0;

    PDEVICE_OBJECT pdo = WdfDeviceWdmGetPhysicalDevice(Device);
    if (!pdo) return;

    WCHAR hwId[512];
    ULONG resultLen = 0;

    NTSTATUS status = IoGetDeviceProperty(
        pdo,
        DevicePropertyHardwareID,
        sizeof(hwId),
        hwId,
        &resultLen);

    if (NT_SUCCESS(status)) {
        GaYmParseHardwareId(hwId, VendorId, ProductId);
    } else {
        GAYM_LOG_WARN("IoGetDeviceProperty(HardwareID) failed: 0x%08X", status);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Helper: forward a request to the lower driver (send-and-forget)
 * ═══════════════════════════════════════════════════════════════════ */
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

static ULONG GaYmTraceRequestTypeFromWdfType(_In_ WDF_REQUEST_TYPE RequestType)
{
    switch (RequestType) {
    case WdfRequestTypeRead:
        return GAYM_TRACE_REQUEST_READ;
    case WdfRequestTypeWrite:
        return GAYM_TRACE_REQUEST_WRITE;
    case WdfRequestTypeDeviceControl:
        return GAYM_TRACE_REQUEST_DEVICE_CONTROL;
    case WdfRequestTypeDeviceControlInternal:
        return GAYM_TRACE_REQUEST_INTERNAL_DEVICE_CONTROL;
    default:
        return GAYM_TRACE_REQUEST_NONE;
    }
}

static NTSTATUS GaYmRetrieveInputBufferForRequest(
    _In_ WDFREQUEST Request,
    _In_ PWDF_REQUEST_PARAMETERS Params,
    _Outptr_result_bytebuffer_(*BufferSize) PVOID* Buffer,
    _Out_ size_t* BufferSize)
{
    PIRP irp;
    ULONG ioControlCode;

    *Buffer = NULL;
    *BufferSize = 0;

    switch (Params->Type) {
    case WdfRequestTypeRead:
    {
        NTSTATUS status = WdfRequestRetrieveOutputBuffer(Request, 1, Buffer, BufferSize);

        if (NT_SUCCESS(status) && *Buffer != NULL && *BufferSize != 0) {
            return STATUS_SUCCESS;
        }

        irp = WdfRequestWdmGetIrp(Request);
        if (irp == NULL) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }

        *Buffer = GaYmMapIrpBuffer(irp);
        *BufferSize = Params->Parameters.Read.Length;
        return (*Buffer != NULL && *BufferSize != 0) ? STATUS_SUCCESS : STATUS_INVALID_USER_BUFFER;
    }

    case WdfRequestTypeDeviceControl:
    case WdfRequestTypeDeviceControlInternal:
        irp = WdfRequestWdmGetIrp(Request);
        if (irp == NULL) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }

        ioControlCode = Params->Parameters.DeviceIoControl.IoControlCode;
        if (ioControlCode == IOCTL_HID_READ_REPORT) {
            *Buffer = irp->UserBuffer;
            *BufferSize = Params->Parameters.DeviceIoControl.OutputBufferLength;
            return (*Buffer != NULL && *BufferSize != 0) ? STATUS_SUCCESS : STATUS_INVALID_USER_BUFFER;
        }

        if (ioControlCode == IOCTL_HID_GET_INPUT_REPORT) {
            PHID_XFER_PACKET packet = (PHID_XFER_PACKET)irp->UserBuffer;
            if (packet == NULL || packet->reportBuffer == NULL || packet->reportBufferLen == 0) {
                return STATUS_INVALID_USER_BUFFER;
            }

            *Buffer = packet->reportBuffer;
            *BufferSize = packet->reportBufferLen;
            return STATUS_SUCCESS;
        }

        if (ioControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB) {
            return GaYmRetrieveUsbUrbTransferBuffer(irp, Buffer, BufferSize);
        }

        return STATUS_INVALID_DEVICE_REQUEST;

    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

static NTSTATUS GaYmRetrieveUsbUrbTransferBuffer(
    _In_ PIRP Irp,
    _Outptr_result_bytebuffer_(*BufferSize) PVOID* Buffer,
    _Out_ size_t* BufferSize)
{
    PIO_STACK_LOCATION currentStack;
    PIO_STACK_LOCATION nextStack;
    PURB urb = NULL;

    *Buffer = NULL;
    *BufferSize = 0;

    if (Irp == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    currentStack = IoGetCurrentIrpStackLocation(Irp);
    nextStack = IoGetNextIrpStackLocation(Irp);

    if (currentStack != NULL &&
        currentStack->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL &&
        currentStack->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB) {
        urb = (PURB)currentStack->Parameters.Others.Argument1;
    }

    if (urb == NULL &&
        nextStack != NULL &&
        nextStack->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL &&
        nextStack->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB) {
        urb = (PURB)nextStack->Parameters.Others.Argument1;
    }

    if (urb == NULL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (urb->UrbHeader.Function != URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if ((urb->UrbBulkOrInterruptTransfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN) == 0) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    *BufferSize = urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
    if (*BufferSize == 0) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (urb->UrbBulkOrInterruptTransfer.TransferBuffer != NULL) {
        *Buffer = urb->UrbBulkOrInterruptTransfer.TransferBuffer;
        return STATUS_SUCCESS;
    }

    if (urb->UrbBulkOrInterruptTransfer.TransferBufferMDL != NULL) {
        *Buffer = MmGetSystemAddressForMdlSafe(
            urb->UrbBulkOrInterruptTransfer.TransferBufferMDL,
            NormalPagePriority | MdlMappingNoExecute);
        return (*Buffer != NULL) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_INVALID_USER_BUFFER;
}

static VOID GaYmClearPatchedTelemetry(_Inout_ PDEVICE_CONTEXT Ctx)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Ctx->TelemetryLock, &oldIrql);
    Ctx->LastPatchedReadSampleLength = 0;
    Ctx->LastPatchedReadCompletionLength = 0;
    Ctx->LastNativeOverrideApplied = 0;
    Ctx->LastNativeOverrideBytesWritten = 0;
    RtlZeroMemory(Ctx->LastPatchedReadSample, sizeof(Ctx->LastPatchedReadSample));
    KeReleaseSpinLock(&Ctx->TelemetryLock, oldIrql);
}

static VOID GaYmUpdateCompletionTelemetry(
    _Inout_ PDEVICE_CONTEXT Ctx,
    _In_ NTSTATUS Status,
    _In_ ULONG CompletionInformation,
    _In_ BOOLEAN OverrideApplied,
    _In_ ULONG BytesWritten,
    _In_reads_bytes_opt_(RawLength) const UCHAR* RawSample,
    _In_ ULONG RawLength,
    _In_reads_bytes_opt_(PatchedLength) const UCHAR* PatchedSample,
    _In_ ULONG PatchedLength)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Ctx->TelemetryLock, &oldIrql);
    Ctx->LastCompletedStatus = (ULONG)Status;
    Ctx->LastCompletionInformation = CompletionInformation;
    Ctx->LastRawReadCompletionLength = CompletionInformation;
    Ctx->LastPatchedReadCompletionLength = CompletionInformation;
    Ctx->LastNativeOverrideApplied = OverrideApplied ? 1u : 0u;
    Ctx->LastNativeOverrideBytesWritten = BytesWritten;
    GaYmCopySampleBytes(Ctx->LastRawReadSample, sizeof(Ctx->LastRawReadSample), &Ctx->LastRawReadSampleLength, RawSample, RawLength);
    GaYmCopySampleBytes(Ctx->LastPatchedReadSample, sizeof(Ctx->LastPatchedReadSample), &Ctx->LastPatchedReadSampleLength, PatchedSample, PatchedLength);
    KeReleaseSpinLock(&Ctx->TelemetryLock, oldIrql);
}

static BOOLEAN GaYmShouldReplaceSemanticCaptureLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ NTSTATUS ParseStatus,
    _In_ ULONG SourceIoctl,
    _In_ ULONG SampleLength)
{
    BOOLEAN newIsUsbSource;
    BOOLEAN currentIsUsbSource;

    if (!NT_SUCCESS(ParseStatus)) {
        return (Ctx->LastSemanticCaptureFlags == 0);
    }

    if (Ctx->LastSemanticCaptureFlags == 0 || Ctx->LastSemanticCaptureLength == 0) {
        return TRUE;
    }

    newIsUsbSource = (SourceIoctl == IOCTL_INTERNAL_USB_SUBMIT_URB);
    currentIsUsbSource = (Ctx->LastSemanticCaptureIoctl == IOCTL_INTERNAL_USB_SUBMIT_URB);
    if (newIsUsbSource != currentIsUsbSource) {
        return newIsUsbSource;
    }

    if (SourceIoctl == Ctx->LastSemanticCaptureIoctl) {
        return TRUE;
    }

    return (SampleLength > Ctx->LastSemanticCaptureLength);
}

static VOID GaYmUpdateSemanticCapture(
    _Inout_ PDEVICE_CONTEXT Ctx,
    _In_reads_bytes_opt_(SampleLength) const UCHAR* Sample,
    _In_ ULONG SampleLength,
    _In_ ULONG SourceIoctl)
{
    GAYM_REPORT parsedReport;
    ULONG captureFlags = 0;
    NTSTATUS status;
    KIRQL oldIrql;
    ULONG copiedLength = 0;

    RtlZeroMemory(&parsedReport, sizeof(parsedReport));
    parsedReport.DPad = GAYM_DPAD_NEUTRAL;

    status = STATUS_INVALID_PARAMETER;
    if (Sample != NULL && SampleLength != 0) {
        status = GaYmParseNativeReport(
            Ctx->DeviceDesc,
            Sample,
            SampleLength,
            &parsedReport,
            &captureFlags);
    }

    KeAcquireSpinLock(&Ctx->TelemetryLock, &oldIrql);
    if (!GaYmShouldReplaceSemanticCaptureLocked(Ctx, status, SourceIoctl, SampleLength)) {
        KeReleaseSpinLock(&Ctx->TelemetryLock, oldIrql);
        return;
    }

    Ctx->LastSemanticCaptureLength = SampleLength;
    Ctx->LastSemanticCaptureIoctl = SourceIoctl;
    GaYmCopySampleBytes(
        Ctx->LastSemanticCaptureSample,
        sizeof(Ctx->LastSemanticCaptureSample),
        &copiedLength,
        Sample,
        SampleLength);
    Ctx->LastSemanticCaptureSampleLength = copiedLength;
    if (NT_SUCCESS(status)) {
        Ctx->LastSemanticCaptureFlags = captureFlags;
        RtlCopyMemory(&Ctx->LastSemanticCaptureReport, &parsedReport, sizeof(parsedReport));
    } else {
        Ctx->LastSemanticCaptureFlags = 0;
        RtlZeroMemory(&Ctx->LastSemanticCaptureReport, sizeof(Ctx->LastSemanticCaptureReport));
        Ctx->LastSemanticCaptureReport.DPad = GAYM_DPAD_NEUTRAL;
    }
    KeReleaseSpinLock(&Ctx->TelemetryLock, oldIrql);
}

static BOOLEAN GaYmShouldCaptureSemanticRequest(
    _In_ const GAYM_WDM_IRP_CONTEXT* IrpContext)
{
    if (IrpContext->RequestType == GAYM_TRACE_REQUEST_READ) {
        return TRUE;
    }

    if ((IrpContext->RequestType == GAYM_TRACE_REQUEST_DEVICE_CONTROL ||
         IrpContext->RequestType == GAYM_TRACE_REQUEST_INTERNAL_DEVICE_CONTROL) &&
        (IrpContext->IoControlCode == IOCTL_HID_READ_REPORT ||
         IrpContext->IoControlCode == IOCTL_HID_GET_INPUT_REPORT)) {
        return TRUE;
    }

    if (IrpContext->RequestType == GAYM_TRACE_REQUEST_INTERNAL_DEVICE_CONTROL &&
        IrpContext->IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB) {
        return TRUE;
    }

    return FALSE;
}

static NTSTATUS GaYmEvtWdmIrpCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PGAYM_WDM_IRP_CONTEXT irpContext = (PGAYM_WDM_IRP_CONTEXT)Context;
    PDEVICE_CONTEXT deviceContext;
    UCHAR rawSample[GAYM_NATIVE_SAMPLE_BYTES];
    ULONG rawSampleLength = 0;
    ULONG completionInformation;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (irpContext == NULL) {
        return STATUS_CONTINUE_COMPLETION;
    }

    deviceContext = irpContext->DeviceContext;
    completionInformation = (ULONG)Irp->IoStatus.Information;

    InterlockedIncrement(&deviceContext->CompletedInputRequests);
    if (deviceContext->PendingInputRequests > 0) {
        InterlockedDecrement(&deviceContext->PendingInputRequests);
    }

    RtlZeroMemory(rawSample, sizeof(rawSample));
    if (NT_SUCCESS(Irp->IoStatus.Status)) {
        __try {
            PVOID buffer = NULL;
            size_t bufferSize = 0;

            if (irpContext->IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB) {
                if (NT_SUCCESS(GaYmRetrieveUsbUrbTransferBuffer(Irp, &buffer, &bufferSize))) {
                    rawSampleLength = GaYmMinUlong((ULONG)bufferSize, GAYM_NATIVE_SAMPLE_BYTES);
                }
            } else if (completionInformation != 0) {
                buffer = GaYmMapIrpBuffer(Irp);
                rawSampleLength = GaYmMinUlong(completionInformation, GAYM_NATIVE_SAMPLE_BYTES);
            }

            if (buffer != NULL && rawSampleLength != 0) {
                RtlCopyMemory(rawSample, buffer, rawSampleLength);
            } else {
                rawSampleLength = 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            rawSampleLength = 0;
        }
    }

    if (rawSampleLength != 0 && GaYmShouldCaptureSemanticRequest(irpContext)) {
        GaYmUpdateSemanticCapture(deviceContext, rawSample, rawSampleLength, irpContext->IoControlCode);
    }

    GaYmUpdateCompletionTelemetry(
        deviceContext,
        Irp->IoStatus.Status,
        completionInformation,
        FALSE,
        0,
        rawSampleLength != 0 ? rawSample : NULL,
        rawSampleLength,
        NULL,
        0);
    GaYmRecordIrpCompletionTrace(
        deviceContext,
        irpContext,
        Irp,
        Irp->IoStatus.Status,
        completionInformation);

    ExFreePoolWithTag(irpContext, GAYM_FILTER_WDM_POOL_TAG);

    if (Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }

    return STATUS_CONTINUE_COMPLETION;
}

static NTSTATUS GaYmPreprocessIrp(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp,
    _In_ ULONG RequestType)
{
    PDEVICE_CONTEXT ctx;
    PIO_STACK_LOCATION stack;
    PGAYM_WDM_IRP_CONTEXT irpContext;
    ULONG ioControlCode = 0;
    ULONG inputLength = 0;
    ULONG outputLength = 0;

    ctx = DeviceGetContext(Device);
    stack = IoGetCurrentIrpStackLocation(Irp);

    switch (RequestType) {
    case GAYM_TRACE_REQUEST_READ:
        outputLength = stack->Parameters.Read.Length;
        InterlockedIncrement(&ctx->ReadRequestsSeen);
        InterlockedExchange(&ctx->LastReadLength, (LONG)outputLength);
        break;

    case GAYM_TRACE_REQUEST_DEVICE_CONTROL:
        ioControlCode = stack->Parameters.DeviceIoControl.IoControlCode;
        inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
        outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
        InterlockedIncrement(&ctx->DeviceControlRequestsSeen);
        InterlockedExchange(&ctx->LastInterceptedIoctl, (LONG)ioControlCode);
        InterlockedExchange(&ctx->LastDeviceControlInputLength, (LONG)inputLength);
        InterlockedExchange(&ctx->LastDeviceControlOutputLength, (LONG)outputLength);
        break;

    case GAYM_TRACE_REQUEST_INTERNAL_DEVICE_CONTROL:
        ioControlCode = stack->Parameters.DeviceIoControl.IoControlCode;
        inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
        outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
        InterlockedIncrement(&ctx->InternalDeviceControlRequestsSeen);
        InterlockedExchange(&ctx->LastInterceptedIoctl, (LONG)ioControlCode);
        InterlockedExchange(&ctx->LastInternalInputLength, (LONG)inputLength);
        InterlockedExchange(&ctx->LastInternalOutputLength, (LONG)outputLength);
        break;

    default:
        break;
    }

    irpContext = (PGAYM_WDM_IRP_CONTEXT)ExAllocatePoolZero(
        NonPagedPoolNx,
        sizeof(*irpContext),
        GAYM_FILTER_WDM_POOL_TAG);
    if (irpContext == NULL) {
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    irpContext->DeviceContext = ctx;
    irpContext->RequestType = RequestType;
    irpContext->IoControlCode = ioControlCode;
    irpContext->InputLength = inputLength;
    irpContext->OutputLength = outputLength;
    irpContext->InputSampleLength = GaYmCaptureIrpInputSample(
        Irp,
        stack,
        inputLength,
        irpContext->InputSample);

    InterlockedIncrement(&ctx->QueuedInputRequests);
    InterlockedIncrement(&ctx->PendingInputRequests);
    InterlockedIncrement(&ctx->ForwardedInputRequests);
    GaYmRecordIrpDispatchTrace(ctx, irpContext);

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(
        Irp,
        GaYmEvtWdmIrpCompletion,
        irpContext,
        TRUE,
        TRUE,
        TRUE);

    return IoCallDriver(WdfDeviceWdmGetAttachedDevice(Device), Irp);
}

NTSTATUS GaYmEvtWdmPreprocessRead(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp)
{
    return GaYmPreprocessIrp(Device, Irp, GAYM_TRACE_REQUEST_READ);
}

NTSTATUS GaYmEvtWdmPreprocessDeviceControl(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp)
{
    return GaYmPreprocessIrp(Device, Irp, GAYM_TRACE_REQUEST_DEVICE_CONTROL);
}

NTSTATUS GaYmEvtWdmPreprocessInternalDeviceControl(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp)
{
    return GaYmPreprocessIrp(Device, Irp, GAYM_TRACE_REQUEST_INTERNAL_DEVICE_CONTROL);
}

static VOID GaYmSendRequestWithCompletion(
    _Inout_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request)
{
    WDF_REQUEST_PARAMETERS params;
    ULONG requestType;
    ULONG ioControlCode = 0;
    ULONG inputLength = 0;
    ULONG outputLength = 0;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    requestType = GaYmTraceRequestTypeFromWdfType(params.Type);
    if (params.Type == WdfRequestTypeRead) {
        outputLength = (ULONG)params.Parameters.Read.Length;
    } else if (params.Type == WdfRequestTypeWrite) {
        outputLength = (ULONG)params.Parameters.Write.Length;
    } else if (params.Type == WdfRequestTypeDeviceControl || params.Type == WdfRequestTypeDeviceControlInternal) {
        ioControlCode = params.Parameters.DeviceIoControl.IoControlCode;
        inputLength = (ULONG)params.Parameters.DeviceIoControl.InputBufferLength;
        outputLength = (ULONG)params.Parameters.DeviceIoControl.OutputBufferLength;
    }

    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSetCompletionRoutine(Request, GaYmEvtRequestCompletion, Ctx);
    InterlockedIncrement(&Ctx->PendingInputRequests);
    InterlockedIncrement(&Ctx->ForwardedInputRequests);
    if (!WdfRequestSend(Request, Ctx->IoTarget, NULL)) {
        NTSTATUS status = WdfRequestGetStatus(Request);
        InterlockedDecrement(&Ctx->PendingInputRequests);
        InterlockedDecrement(&Ctx->ForwardedInputRequests);
        GaYmRecordTrace(
            Ctx,
            GAYM_TRACE_PHASE_SEND_FAILURE,
            requestType,
            ioControlCode,
            inputLength,
            outputLength,
            0,
            status,
            NULL,
            0);
        WdfRequestComplete(Request, status);
    }
}

static BOOLEAN GaYmShouldInterceptInternalIoctl(_In_ ULONG IoControlCode)
{
    switch (IoControlCode) {
    case IOCTL_HID_READ_REPORT:
    case IOCTL_HID_GET_INPUT_REPORT:
        return TRUE;
    default:
        return FALSE;
    }
}

static VOID GaYmQueuePendingInputRequest(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST Request)
{
    InterlockedIncrement(&Ctx->QueuedInputRequests);
    InterlockedIncrement(&Ctx->PendingInputRequests);

    NTSTATUS status = WdfRequestForwardToIoQueue(Request, Ctx->PendingReadsQueue);
    if (!NT_SUCCESS(status)) {
        InterlockedDecrement(&Ctx->PendingInputRequests);
        GAYM_LOG_WARN("ForwardToIoQueue failed: 0x%08X", status);
        WdfRequestComplete(Request, status);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Helper: complete a HID read request with the current injected report
 * Called with ReportLock NOT held (acquires internally).
 * ═══════════════════════════════════════════════════════════════════ */
VOID GaYmCompleteReadWithReport(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST ReadRequest)
{
    PVOID  buffer     = NULL;
    size_t bufferSize = 0;
    NTSTATUS status   = STATUS_SUCCESS;

    WDF_REQUEST_PARAMETERS params;
    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(ReadRequest, &params);

    if (params.Type == WdfRequestTypeRead) {
        status = WdfRequestRetrieveOutputBuffer(
            ReadRequest, 1, &buffer, &bufferSize);
    } else if (params.Type == WdfRequestTypeDeviceControl ||
               params.Type == WdfRequestTypeDeviceControlInternal) {
        PIRP irp = WdfRequestWdmGetIrp(ReadRequest);
        if (!irp) {
            status = STATUS_INVALID_DEVICE_REQUEST;
        } else {
            ULONG ioControlCode = params.Parameters.DeviceIoControl.IoControlCode;

            if (ioControlCode == IOCTL_HID_READ_REPORT) {
                buffer = irp->UserBuffer;
                bufferSize = params.Parameters.DeviceIoControl.OutputBufferLength;
                status = (buffer && bufferSize) ? STATUS_SUCCESS : STATUS_INVALID_USER_BUFFER;
            } else if (ioControlCode == IOCTL_HID_GET_INPUT_REPORT) {
                PHID_XFER_PACKET packet = (PHID_XFER_PACKET)irp->UserBuffer;
                if (packet && packet->reportBuffer && packet->reportBufferLen) {
                    buffer = packet->reportBuffer;
                    bufferSize = packet->reportBufferLen;
                    status = STATUS_SUCCESS;
                } else {
                    status = STATUS_INVALID_USER_BUFFER;
                }
            } else {
                status = STATUS_INVALID_DEVICE_REQUEST;
            }
        }
    } else {
        status = STATUS_INVALID_DEVICE_REQUEST;
    }

    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(ReadRequest, status);
        return;
    }

    ULONG bytesWritten = 0;
    KIRQL oldIrql;
    KeAcquireSpinLock(&Ctx->ReportLock, &oldIrql);

    if (Ctx->DeviceDesc && Ctx->DeviceDesc->TranslateReport) {
        /* Translate generic → native HID format */
        status = Ctx->DeviceDesc->TranslateReport(
            &Ctx->CurrentReport,
            (PUCHAR)buffer,
            (ULONG)bufferSize,
            &bytesWritten,
            &Ctx->SeqCounter);
    } else {
        /* Unknown device: send raw generic report (best-effort) */
        ULONG copySize = (ULONG)min(bufferSize, sizeof(GAYM_REPORT));
        RtlCopyMemory(buffer, &Ctx->CurrentReport, copySize);
        bytesWritten = copySize;
        status = STATUS_SUCCESS;
    }

    KeReleaseSpinLock(&Ctx->ReportLock, oldIrql);

    InterlockedIncrement(&Ctx->CompletedInputRequests);
    if (Ctx->PendingInputRequests > 0) {
        InterlockedDecrement(&Ctx->PendingInputRequests);
    }

    {
        ULONG copiedLength = 0;
        KIRQL telemetryIrql;
        KeAcquireSpinLock(&Ctx->TelemetryLock, &telemetryIrql);
        Ctx->LastCompletedStatus = (ULONG)status;
        Ctx->LastCompletionInformation = bytesWritten;
        Ctx->LastRawReadCompletionLength = 0;
        Ctx->LastPatchedReadCompletionLength = bytesWritten;
        Ctx->LastNativeOverrideApplied = NT_SUCCESS(status) ? 1u : 0u;
        Ctx->LastNativeOverrideBytesWritten = bytesWritten;
        RtlZeroMemory(Ctx->LastRawReadSample, sizeof(Ctx->LastRawReadSample));
        Ctx->LastRawReadSampleLength = 0;
        copiedLength = GaYmMinUlong((ULONG)sizeof(Ctx->LastPatchedReadSample), bytesWritten);
        RtlZeroMemory(Ctx->LastPatchedReadSample, sizeof(Ctx->LastPatchedReadSample));
        if (buffer != NULL && copiedLength != 0) {
            RtlCopyMemory(Ctx->LastPatchedReadSample, buffer, copiedLength);
        }
        Ctx->LastPatchedReadSampleLength = copiedLength;
        KeReleaseSpinLock(&Ctx->TelemetryLock, telemetryIrql);
    }
    GaYmRecordTrace(
        Ctx,
        GAYM_TRACE_PHASE_COMPLETION,
        params.Type == WdfRequestTypeRead ? GAYM_TRACE_REQUEST_READ :
            (params.Type == WdfRequestTypeDeviceControl ? GAYM_TRACE_REQUEST_DEVICE_CONTROL : GAYM_TRACE_REQUEST_INTERNAL_DEVICE_CONTROL),
        params.Type == WdfRequestTypeRead ? 0u : params.Parameters.DeviceIoControl.IoControlCode,
        0,
        (ULONG)bufferSize,
        bytesWritten,
        status,
        buffer,
        bytesWritten);

    WdfRequestCompleteWithInformation(ReadRequest, status, (ULONG_PTR)bytesWritten);
}

/* ═══════════════════════════════════════════════════════════════════
 * Helper: drain all pending reads (forward or cancel)
 * ═══════════════════════════════════════════════════════════════════ */
VOID GaYmDrainPendingReads(_In_ PDEVICE_CONTEXT Ctx, _In_ NTSTATUS CompletionStatus)
{
    WDFREQUEST req;
    while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Ctx->PendingReadsQueue, &req))) {
        if (CompletionStatus == STATUS_SUCCESS) {
            /* Forward to real device */
            if (Ctx->PendingInputRequests > 0) {
                InterlockedDecrement(&Ctx->PendingInputRequests);
            }
            InterlockedIncrement(&Ctx->ForwardedInputRequests);
            GaYmForwardRequest(Ctx->Device, req);
        } else {
            if (Ctx->PendingInputRequests > 0) {
                InterlockedDecrement(&Ctx->PendingInputRequests);
            }
            WdfRequestComplete(req, CompletionStatus);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Control Device Object (CDO) — direct IOCTL path from user-mode
 *
 * Because GaYmFilter is a lower filter (below xinputhid), IOCTLs sent
 * to the device stack enter at xinputhid which rejects unknown IOCTLs.
 * The CDO provides a sideband path: user-mode opens \\.\GaYmFilterCtl
 * and IOCTLs go directly to us.
 * ═══════════════════════════════════════════════════════════════════ */

static WDFDEVICE g_ControlDevice = NULL;

DECLARE_CONST_UNICODE_STRING(g_CtlDeviceName, L"\\Device\\GaYmFilterCtl");
DECLARE_CONST_UNICODE_STRING(g_CtlSymLink,    L"\\DosDevices\\GaYmFilterCtl");

/* CDO IOCTL handler — delegates to the filter's device context */
VOID GaYmEvtCtlIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    PCONTROL_DEVICE_CONTEXT ctlCtx = ControlGetContext(WdfIoQueueGetDevice(Queue));
    PDEVICE_CONTEXT ctx = GaYmAcquireActiveControlFilterContext(ctlCtx);

    if (!ctx) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    NTSTATUS  status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info   = 0;

    switch (IoControlCode) {

    case IOCTL_GAYM_OVERRIDE_ON:
        ctx->OverrideEnabled = TRUE;
        ctx->HasReport       = FALSE;
        ctx->SeqCounter      = 0;
        ctx->PendingInputRequests = 0;
        ctx->QueuedInputRequests = 0;
        ctx->CompletedInputRequests = 0;
        ctx->ForwardedInputRequests = 0;
        ctx->LastInterceptedIoctl = 0;
        ctx->ReadRequestsSeen = 0;
        ctx->DeviceControlRequestsSeen = 0;
        ctx->InternalDeviceControlRequestsSeen = 0;
        ctx->WriteRequestsSeen = 0;
        GaYmResetTelemetry(ctx);
        GAYM_LOG_INFO("CDO: Override ON (VID:%04X PID:%04X)", ctx->VendorId, ctx->ProductId);
        status = STATUS_SUCCESS;
        break;

    case IOCTL_GAYM_OVERRIDE_OFF:
        ctx->OverrideEnabled = FALSE;
        ctx->HasReport       = FALSE;
        GaYmClearPatchedTelemetry(ctx);
        GaYmDrainPendingReads(ctx, STATUS_SUCCESS);
        GAYM_LOG_INFO("CDO: Override OFF");
        status = STATUS_SUCCESS;
        break;

    case IOCTL_GAYM_INJECT_REPORT:
    {
        PVOID  inBuf;
        size_t inLen;
        WDFREQUEST readReq;
        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(GAYM_REPORT), &inBuf, &inLen);
        if (!NT_SUCCESS(status)) break;

        KIRQL oldIrql;
        KeAcquireSpinLock(&ctx->ReportLock, &oldIrql);
        RtlCopyMemory(&ctx->CurrentReport, inBuf, sizeof(GAYM_REPORT));
        ctx->HasReport = TRUE;
        ctx->ReportsSent++;
        KeReleaseSpinLock(&ctx->ReportLock, oldIrql);

        while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(ctx->PendingReadsQueue, &readReq))) {
            GaYmCompleteReadWithReport(ctx, readReq);
        }

        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_GAYM_QUERY_DEVICE:
    {
        PGAYM_DEVICE_INFO di;
        GAYM_DEVICE_INFO snapshot;
        size_t outLen;
        ULONG bytesToCopy;
        status = WdfRequestRetrieveOutputBuffer(
            Request, GAYM_DEVICE_INFO_MIN_SIZE, (PVOID*)&di, &outLen);
        if (NT_SUCCESS(status)) {
            KIRQL oldIrql;
            RtlZeroMemory(&snapshot, sizeof(snapshot));
            snapshot.DeviceType     = ctx->DeviceType;
            snapshot.VendorId       = ctx->VendorId;
            snapshot.ProductId      = ctx->ProductId;
            snapshot.OverrideActive = ctx->OverrideEnabled;
            snapshot.ReportsSent    = ctx->ReportsSent;
            snapshot.PendingInputRequests   = (ULONG)ctx->PendingInputRequests;
            snapshot.QueuedInputRequests    = (ULONG)ctx->QueuedInputRequests;
            snapshot.CompletedInputRequests = (ULONG)ctx->CompletedInputRequests;
            snapshot.ForwardedInputRequests = (ULONG)ctx->ForwardedInputRequests;
            snapshot.LastInterceptedIoctl   = (ULONG)ctx->LastInterceptedIoctl;
            snapshot.ReadRequestsSeen             = (ULONG)ctx->ReadRequestsSeen;
            snapshot.DeviceControlRequestsSeen    = (ULONG)ctx->DeviceControlRequestsSeen;
            snapshot.InternalDeviceControlRequestsSeen = (ULONG)ctx->InternalDeviceControlRequestsSeen;
            snapshot.WriteRequestsSeen            = (ULONG)ctx->WriteRequestsSeen;
            snapshot.InputCapabilities = GaYmGetInputCapabilities(ctx->DeviceDesc);
            snapshot.OutputCapabilities = GaYmGetOutputCapabilities(ctx->DeviceDesc);
            KeAcquireSpinLock(&ctx->TelemetryLock, &oldIrql);
            snapshot.LastCompletedStatus = ctx->LastCompletedStatus;
            snapshot.LastCompletionInformation = ctx->LastCompletionInformation;
            snapshot.LastReadLength = (ULONG)ctx->LastReadLength;
            snapshot.LastWriteLength = ctx->LastWriteLength;
            snapshot.LastWriteSampleLength = ctx->LastWriteSampleLength;
            snapshot.LastDeviceControlInputLength = (ULONG)ctx->LastDeviceControlInputLength;
            snapshot.LastDeviceControlOutputLength = (ULONG)ctx->LastDeviceControlOutputLength;
            snapshot.LastInternalInputLength = (ULONG)ctx->LastInternalInputLength;
            snapshot.LastInternalOutputLength = (ULONG)ctx->LastInternalOutputLength;
            snapshot.LastRawReadSampleLength = ctx->LastRawReadSampleLength;
            snapshot.LastPatchedReadSampleLength = ctx->LastPatchedReadSampleLength;
            snapshot.LastRawReadCompletionLength = ctx->LastRawReadCompletionLength;
            snapshot.LastPatchedReadCompletionLength = ctx->LastPatchedReadCompletionLength;
            snapshot.LastNativeOverrideApplied = ctx->LastNativeOverrideApplied;
            snapshot.LastNativeOverrideBytesWritten = ctx->LastNativeOverrideBytesWritten;
            snapshot.LastSemanticCaptureFlags = ctx->LastSemanticCaptureFlags;
            snapshot.LastSemanticCaptureLength = ctx->LastSemanticCaptureLength;
            snapshot.LastSemanticCaptureIoctl = ctx->LastSemanticCaptureIoctl;
            snapshot.LastSemanticCaptureSampleLength = ctx->LastSemanticCaptureSampleLength;
            snapshot.TraceSequence = ctx->TraceSequence;
            snapshot.TraceCount = ctx->TraceCount;
            RtlCopyMemory(snapshot.LastRawReadSample, ctx->LastRawReadSample, sizeof(snapshot.LastRawReadSample));
            RtlCopyMemory(snapshot.LastPatchedReadSample, ctx->LastPatchedReadSample, sizeof(snapshot.LastPatchedReadSample));
            RtlCopyMemory(snapshot.LastWriteSample, ctx->LastWriteSample, sizeof(snapshot.LastWriteSample));
            RtlCopyMemory(snapshot.LastSemanticCaptureSample, ctx->LastSemanticCaptureSample, sizeof(snapshot.LastSemanticCaptureSample));
            RtlCopyMemory(&snapshot.LastSemanticCaptureReport, &ctx->LastSemanticCaptureReport, sizeof(snapshot.LastSemanticCaptureReport));
            RtlCopyMemory(snapshot.Trace, ctx->Trace, sizeof(snapshot.Trace));
            KeReleaseSpinLock(&ctx->TelemetryLock, oldIrql);
            snapshot.QueryLayoutVersion = GAYM_FILTER_QUERY_LAYOUT_VERSION;
            snapshot.DriverBuildStamp = GAYM_FILTER_BUILD_STAMP;
            bytesToCopy = GaYmMinUlong((ULONG)outLen, (ULONG)sizeof(snapshot));
            RtlCopyMemory(di, &snapshot, bytesToCopy);
            info = bytesToCopy;
        }
        break;
    }

    case IOCTL_GAYM_SET_JITTER:
    {
        PVOID inBuf;
        size_t inLen;
        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(GAYM_JITTER_CONFIG), &inBuf, &inLen);
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(&ctx->JitterConfig, inBuf, sizeof(GAYM_JITTER_CONFIG));
            GAYM_LOG_INFO("CDO: Jitter config: enabled=%d min=%lu max=%lu us",
                ctx->JitterConfig.Enabled,
                ctx->JitterConfig.MinDelayUs,
                ctx->JitterConfig.MaxDelayUs);
        }
        break;
    }

    case IOCTL_GAYM_APPLY_OUTPUT:
        status = STATUS_NOT_SUPPORTED;
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, info);
    WdfObjectDereference(ctx->Device);
}

NTSTATUS GaYmCreateControlDevice(
    _In_ WDFDEVICE       FilterDevice,
    _In_ PDEVICE_CONTEXT FilterCtx)
{
    WDF_OBJECT_ATTRIBUTES lockAttrs;
    UNREFERENCED_PARAMETER(FilterDevice);

    if (g_ControlDevice) {
        /* Already exists — just update the filter context pointer */
        PCONTROL_DEVICE_CONTEXT ctlCtx = ControlGetContext(g_ControlDevice);
        GaYmSetControlFilterContext(ctlCtx, FilterCtx);
        GAYM_LOG_INFO("CDO: already exists");
        return STATUS_SUCCESS;
    }

    WDFDRIVER driver = WdfGetDriver();

    /* SDDL: System=ALL, Admin=RWX, World=RWX, Restricted=RWX */
    DECLARE_CONST_UNICODE_STRING(sddl,
        L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GRGWGX;;;WD)(A;;GRGWGX;;;RC)");

    PWDFDEVICE_INIT ctlInit = WdfControlDeviceInitAllocate(driver, &sddl);
    if (!ctlInit) {
        GAYM_LOG_ERROR("CDO: WdfControlDeviceInitAllocate failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WdfDeviceInitSetDeviceType(ctlInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(ctlInit, FILE_DEVICE_SECURE_OPEN, FALSE);
    WdfDeviceInitSetExclusive(ctlInit, FALSE);

    NTSTATUS status = WdfDeviceInitAssignName(ctlInit, &g_CtlDeviceName);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("CDO: AssignName failed: 0x%08X", status);
        WdfDeviceInitFree(ctlInit);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, CONTROL_DEVICE_CONTEXT);

    WDFDEVICE ctlDevice;
    status = WdfDeviceCreate(&ctlInit, &attrs, &ctlDevice);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("CDO: WdfDeviceCreate failed: 0x%08X", status);
        return status;
    }

    /* Store pointer to filter context */
    PCONTROL_DEVICE_CONTEXT ctlCtx = ControlGetContext(ctlDevice);
    ctlCtx->FilterCtx = NULL;

    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttrs);
    lockAttrs.ParentObject = ctlDevice;

    status = WdfWaitLockCreate(&lockAttrs, &ctlCtx->RouteLock);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("CDO: RouteLock create failed: 0x%08X", status);
        WdfObjectDelete(ctlDevice);
        return status;
    }

    GaYmSetControlFilterContext(ctlCtx, FilterCtx);

    /* Create symbolic link */
    status = WdfDeviceCreateSymbolicLink(ctlDevice, &g_CtlSymLink);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("CDO: CreateSymbolicLink failed: 0x%08X", status);
        WdfObjectDelete(ctlDevice);
        return status;
    }

    /* I/O queue for IOCTLs */
    WDF_IO_QUEUE_CONFIG qCfg;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qCfg, WdfIoQueueDispatchParallel);
    qCfg.EvtIoDeviceControl = GaYmEvtCtlIoDeviceControl;

    WDFQUEUE queue;
    status = WdfIoQueueCreate(ctlDevice, &qCfg, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("CDO: queue create failed: 0x%08X", status);
        WdfObjectDelete(ctlDevice);
        return status;
    }

    WdfControlFinishInitializing(ctlDevice);
    g_ControlDevice = ctlDevice;

    GAYM_LOG_INFO("CDO: \\\\.\\ GaYmFilterCtl created successfully");
    return STATUS_SUCCESS;
}

VOID GaYmDeleteControlDevice(VOID)
{
    if (g_ControlDevice) {
        PCONTROL_DEVICE_CONTEXT ctlCtx = ControlGetContext(g_ControlDevice);
        GaYmSetControlFilterContext(ctlCtx, NULL);
        WdfObjectDelete(g_ControlDevice);
        g_ControlDevice = NULL;
        GAYM_LOG_INFO("CDO: deleted");
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * EvtDriverDeviceAdd - filter device creation
 * ═══════════════════════════════════════════════════════════════════ */
NTSTATUS GaYmEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);
    NTSTATUS status;

    GAYM_LOG_INFO("DeviceAdd: attaching filter");

    /* ── Mark as filter ── */
    WdfFdoInitSetFilter(DeviceInit);

    /* ── PnP / Power callbacks ── */
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCb;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCb);
    pnpCb.EvtDevicePrepareHardware = GaYmEvtPrepareHardware;
    pnpCb.EvtDeviceReleaseHardware = GaYmEvtReleaseHardware;
    pnpCb.EvtDeviceD0Entry         = GaYmEvtD0Entry;
    pnpCb.EvtDeviceD0Exit          = GaYmEvtD0Exit;
    pnpCb.EvtDeviceSurpriseRemoval = GaYmEvtSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCb);



    /* ── Create device with context ── */
    WDF_OBJECT_ATTRIBUTES devAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttrs, DEVICE_CONTEXT);

    WDFDEVICE device;
    status = WdfDeviceCreate(&DeviceInit, &devAttrs, &device);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("WdfDeviceCreate failed: 0x%08X", status);
        return status;
    }

    /* ── Initialize context ── */
    PDEVICE_CONTEXT ctx = DeviceGetContext(device);
    RtlZeroMemory(ctx, sizeof(DEVICE_CONTEXT));
    ctx->Device         = device;
    ctx->IoTarget       = WdfDeviceGetIoTarget(device);
    ctx->OverrideEnabled = FALSE;
    ctx->HasReport      = FALSE;
    ctx->ReportsSent    = 0;
    ctx->SeqCounter     = 0;
    ctx->IsInD0         = FALSE;
    KeInitializeSpinLock(&ctx->ReportLock);
    KeInitializeSpinLock(&ctx->TelemetryLock);
    GaYmResetTelemetry(ctx);

    /* ── Identify device (VID/PID → device table) ── */
    QueryDeviceIds(device, &ctx->VendorId, &ctx->ProductId);
    ctx->DeviceDesc = GaYmLookupDevice(ctx->VendorId, ctx->ProductId);
    ctx->DeviceType = ctx->DeviceDesc ? ctx->DeviceDesc->DeviceType : GAYM_DEVICE_UNKNOWN;

    GAYM_LOG_INFO("Device identified: %s (VID:%04X PID:%04X)",
        GaYmDeviceTypeName(ctx->DeviceType), ctx->VendorId, ctx->ProductId);

    /* ── Default queue: parallel dispatch for Read, IOCTL, Internal IOCTL, Write ── */
    WDF_IO_QUEUE_CONFIG qCfg;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qCfg, WdfIoQueueDispatchParallel);
    qCfg.EvtIoRead                    = GaYmEvtIoRead;
    qCfg.EvtIoWrite                   = GaYmEvtIoWrite;
    qCfg.EvtIoDeviceControl           = GaYmEvtIoDeviceControl;
    qCfg.EvtIoInternalDeviceControl   = GaYmEvtIoInternalDeviceControl;

    status = WdfIoQueueCreate(device, &qCfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->DefaultQueue);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("Default queue create failed: 0x%08X", status);
        return status;
    }

    /* ── Manual queue for pending HID reads during override ── */
    WDF_IO_QUEUE_CONFIG_INIT(&qCfg, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &qCfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->PendingReadsQueue);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("PendingReads queue create failed: 0x%08X", status);
        return status;
    }

    /* ── Create control device for sideband IOCTLs from user-mode ── */
    status = GaYmCreateControlDevice(device, ctx);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_WARN("CDO creation failed: 0x%08X (IOCTLs via stack may not work)", status);
        /* Non-fatal: filter still works, just no user-mode control */
    }

    GAYM_LOG_INFO("DeviceAdd: filter attached successfully");
    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════
 * I/O Dispatch: IRP_MJ_READ
 *
 * This is the main interception point. HID applications (DirectInput,
 * XInput, raw HID) read controller reports via ReadFile().
 * ═══════════════════════════════════════════════════════════════════ */
VOID GaYmEvtIoRead(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     Length)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    InterlockedIncrement(&ctx->ReadRequestsSeen);
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&ctx->TelemetryLock, &oldIrql);
        ctx->LastReadLength = (ULONG)Length;
        KeReleaseSpinLock(&ctx->TelemetryLock, oldIrql);
    }
    GaYmRecordTrace(
        ctx,
        GAYM_TRACE_PHASE_DISPATCH,
        GAYM_TRACE_REQUEST_READ,
        0,
        0,
        (ULONG)Length,
        (ULONG)Length,
        STATUS_SUCCESS,
        NULL,
        0);
    if (ctx->OverrideEnabled) {
        GaYmQueuePendingInputRequest(ctx, Request);
        return;
    }

    GaYmSendRequestWithCompletion(ctx, Request);
}

/* ═══════════════════════════════════════════════════════════════════
 * I/O Dispatch: IRP_MJ_WRITE  (output reports, e.g. rumble)
 * Always forwarded to real device.
 * ═══════════════════════════════════════════════════════════════════ */
VOID GaYmEvtIoWrite(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     Length)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    PVOID writeBuffer = NULL;
    size_t writeBufferSize = 0;
    NTSTATUS sampleStatus;
    UCHAR writeSample[GAYM_TRACE_SAMPLE_BYTES];
    ULONG writeSampleLength = 0;

    RtlZeroMemory(writeSample, sizeof(writeSample));
    sampleStatus = WdfRequestRetrieveInputBuffer(Request, 1, &writeBuffer, &writeBufferSize);
    if (NT_SUCCESS(sampleStatus) && writeBuffer != NULL && writeBufferSize != 0) {
        GaYmCopySampleBytes(
            writeSample,
            sizeof(writeSample),
            &writeSampleLength,
            writeBuffer,
            (ULONG)Length);
    }

    InterlockedIncrement(&ctx->WriteRequestsSeen);
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&ctx->TelemetryLock, &oldIrql);
        ctx->LastWriteLength = (ULONG)Length;
        ctx->LastWriteSampleLength = writeSampleLength;
        RtlZeroMemory(ctx->LastWriteSample, sizeof(ctx->LastWriteSample));
        if (writeSampleLength != 0) {
            RtlCopyMemory(ctx->LastWriteSample, writeSample, writeSampleLength);
        }
        KeReleaseSpinLock(&ctx->TelemetryLock, oldIrql);
    }
    GaYmRecordTrace(
        ctx,
        GAYM_TRACE_PHASE_DISPATCH,
        GAYM_TRACE_REQUEST_WRITE,
        0,
        0,
        (ULONG)Length,
        (ULONG)Length,
        STATUS_SUCCESS,
        writeSampleLength != 0 ? writeSample : NULL,
        (UCHAR)writeSampleLength);
    GaYmForwardRequest(ctx->Device, Request);
}

/* ═══════════════════════════════════════════════════════════════════
 * I/O Dispatch: IRP_MJ_DEVICE_CONTROL
 * Some HID stacks issue input-report requests here instead of via
 * IRP_MJ_INTERNAL_DEVICE_CONTROL, so intercept the same HID read path.
 * Our custom IOCTLs still go through the CDO (\\.\GaYmFilterCtl).
 * ═══════════════════════════════════════════════════════════════════ */
VOID GaYmEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    InterlockedIncrement(&ctx->DeviceControlRequestsSeen);
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&ctx->TelemetryLock, &oldIrql);
        ctx->LastDeviceControlInputLength = (ULONG)InputBufferLength;
        ctx->LastDeviceControlOutputLength = (ULONG)OutputBufferLength;
        KeReleaseSpinLock(&ctx->TelemetryLock, oldIrql);
    }
    GaYmRecordTrace(
        ctx,
        GAYM_TRACE_PHASE_DISPATCH,
        GAYM_TRACE_REQUEST_DEVICE_CONTROL,
        IoControlCode,
        (ULONG)InputBufferLength,
        (ULONG)OutputBufferLength,
        0,
        STATUS_SUCCESS,
        NULL,
        0);
    if (GaYmShouldInterceptInternalIoctl(IoControlCode)) {
        InterlockedExchange(&ctx->LastInterceptedIoctl, (LONG)IoControlCode);
    }

    if (ctx->OverrideEnabled && GaYmShouldInterceptInternalIoctl(IoControlCode)) {
        GaYmQueuePendingInputRequest(ctx, Request);
        return;
    }

    GaYmSendRequestWithCompletion(ctx, Request);
}

/* ═══════════════════════════════════════════════════════════════════
 * I/O Dispatch: IRP_MJ_INTERNAL_DEVICE_CONTROL
 * Intercepts HID input requests used by xinputhid while override is active.
 * ═══════════════════════════════════════════════════════════════════ */
VOID GaYmEvtIoInternalDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    InterlockedIncrement(&ctx->InternalDeviceControlRequestsSeen);
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&ctx->TelemetryLock, &oldIrql);
        ctx->LastInternalInputLength = (ULONG)InputBufferLength;
        ctx->LastInternalOutputLength = (ULONG)OutputBufferLength;
        KeReleaseSpinLock(&ctx->TelemetryLock, oldIrql);
    }
    GaYmRecordTrace(
        ctx,
        GAYM_TRACE_PHASE_DISPATCH,
        GAYM_TRACE_REQUEST_INTERNAL_DEVICE_CONTROL,
        IoControlCode,
        (ULONG)InputBufferLength,
        (ULONG)OutputBufferLength,
        0,
        STATUS_SUCCESS,
        NULL,
        0);
    if (GaYmShouldInterceptInternalIoctl(IoControlCode)) {
        InterlockedExchange(&ctx->LastInterceptedIoctl, (LONG)IoControlCode);
    }

    if (ctx->OverrideEnabled && GaYmShouldInterceptInternalIoctl(IoControlCode)) {
        GaYmQueuePendingInputRequest(ctx, Request);
        return;
    }

    GaYmSendRequestWithCompletion(ctx, Request);
}

/* ═══════════════════════════════════════════════════════════════════
 * PnP: Prepare Hardware
 * ═══════════════════════════════════════════════════════════════════ */
VOID GaYmEvtRequestCompletion(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS CompletionParams,
    _In_ WDFCONTEXT Context)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    WDF_REQUEST_PARAMETERS params;
    PVOID buffer = NULL;
    size_t bufferSize = 0;
    ULONG requestType;
    ULONG ioControlCode = 0;
    ULONG inputLength = 0;
    ULONG outputLength = 0;
    ULONG completionInformation = (ULONG)CompletionParams->IoStatus.Information;
    NTSTATUS status = CompletionParams->IoStatus.Status;
    UCHAR rawSample[GAYM_NATIVE_SAMPLE_BYTES];
    UCHAR patchedSample[GAYM_NATIVE_SAMPLE_BYTES];
    ULONG rawSampleLength = 0;
    ULONG patchedSampleLength = 0;

    UNREFERENCED_PARAMETER(Target);

    InterlockedIncrement(&ctx->CompletedInputRequests);
    if (ctx->PendingInputRequests > 0) {
        InterlockedDecrement(&ctx->PendingInputRequests);
    }

    RtlZeroMemory(rawSample, sizeof(rawSample));
    RtlZeroMemory(patchedSample, sizeof(patchedSample));

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    requestType = GaYmTraceRequestTypeFromWdfType(params.Type);
    if (params.Type == WdfRequestTypeRead) {
        outputLength = (ULONG)params.Parameters.Read.Length;
    } else if (params.Type == WdfRequestTypeWrite) {
        outputLength = (ULONG)params.Parameters.Write.Length;
    } else if (params.Type == WdfRequestTypeDeviceControl || params.Type == WdfRequestTypeDeviceControlInternal) {
        ioControlCode = params.Parameters.DeviceIoControl.IoControlCode;
        inputLength = (ULONG)params.Parameters.DeviceIoControl.InputBufferLength;
        outputLength = (ULONG)params.Parameters.DeviceIoControl.OutputBufferLength;
    }

    if (NT_SUCCESS(status)) {
        NTSTATUS bufferStatus = GaYmRetrieveInputBufferForRequest(Request, &params, &buffer, &bufferSize);
        if (NT_SUCCESS(bufferStatus) && buffer != NULL && bufferSize != 0) {
            ULONG sampleLength = completionInformation != 0 ? completionInformation : (ULONG)bufferSize;
            GaYmCopySampleBytes(rawSample, sizeof(rawSample), &rawSampleLength, buffer, sampleLength);
            GaYmUpdateSemanticCapture(ctx, rawSample, rawSampleLength, ioControlCode);

            if (NT_SUCCESS(status)) {
                ULONG patchedLength = completionInformation != 0 ? completionInformation : (ULONG)bufferSize;
                GaYmCopySampleBytes(patchedSample, sizeof(patchedSample), &patchedSampleLength, buffer, patchedLength);
            }
        }
    }

    GaYmUpdateCompletionTelemetry(
        ctx,
        status,
        completionInformation,
        FALSE,
        0,
        rawSampleLength != 0 ? rawSample : NULL,
        rawSampleLength,
        patchedSampleLength != 0 ? patchedSample : NULL,
        patchedSampleLength);
    GaYmRecordTrace(
        ctx,
        GAYM_TRACE_PHASE_COMPLETION,
        requestType,
        ioControlCode,
        inputLength,
        outputLength,
        completionInformation,
        status,
        patchedSampleLength != 0 ? patchedSample : NULL,
        patchedSampleLength);

    WdfRequestCompleteWithInformation(Request, status, completionInformation);
}

NTSTATUS GaYmEvtPrepareHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    GAYM_LOG_INFO("PrepareHardware: %s (VID:%04X PID:%04X)",
        GaYmDeviceTypeName(ctx->DeviceType),
        ctx->VendorId, ctx->ProductId);

    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════
 * PnP: Release Hardware
 * ═══════════════════════════════════════════════════════════════════ */
NTSTATUS GaYmEvtReleaseHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    ctx->IsInD0         = FALSE;
    ctx->OverrideEnabled = FALSE;
    ctx->HasReport       = FALSE;

    if (g_ControlDevice) {
        PCONTROL_DEVICE_CONTEXT ctlCtx = ControlGetContext(g_ControlDevice);
        GaYmClearControlFilterContextIfMatch(ctlCtx, ctx);
        GAYM_LOG_INFO("CDO: unbound from released device");
    }

    GAYM_LOG_INFO("ReleaseHardware: device removed");
    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════
 * Power: D0 Entry (device powered on / resumed)
 * ═══════════════════════════════════════════════════════════════════ */
NTSTATUS GaYmEvtD0Entry(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    ctx->IsInD0 = TRUE;

    if (g_ControlDevice) {
        PCONTROL_DEVICE_CONTEXT ctlCtx = ControlGetContext(g_ControlDevice);
        GaYmSetControlFilterContext(ctlCtx, ctx);
        GAYM_LOG_INFO("CDO: bound to active D0 device (VID:%04X PID:%04X)",
            ctx->VendorId, ctx->ProductId);
    }

    GAYM_LOG_INFO("D0Entry: %s resumed from power state %d",
        GaYmDeviceTypeName(ctx->DeviceType), (int)PreviousState);

    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════
 * Power: D0 Exit (device suspending / powering down)
 * Disables override and drains pending reads to prevent hangs.
 * ═══════════════════════════════════════════════════════════════════ */
NTSTATUS GaYmEvtD0Exit(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    ctx->IsInD0 = FALSE;

    if (g_ControlDevice) {
        PCONTROL_DEVICE_CONTEXT ctlCtx = ControlGetContext(g_ControlDevice);
        GaYmClearControlFilterContextIfMatch(ctlCtx, ctx);
        GAYM_LOG_INFO("CDO: unbound from D0-exiting device");
    }

    if (ctx->OverrideEnabled) {
        ctx->OverrideEnabled = FALSE;
        ctx->HasReport       = FALSE;
        GaYmClearPatchedTelemetry(ctx);

        /* Cancel pending reads - device is going away temporarily */
        GaYmDrainPendingReads(ctx, STATUS_POWER_STATE_INVALID);

        GAYM_LOG_INFO("D0Exit: override disabled (target state %d)", (int)TargetState);
    }

    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════
 * PnP: Surprise Removal (hot-unplug)
 * ═══════════════════════════════════════════════════════════════════ */
VOID GaYmEvtSurpriseRemoval(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    ctx->IsInD0 = FALSE;

    ctx->OverrideEnabled = FALSE;
    ctx->HasReport       = FALSE;
    GaYmClearPatchedTelemetry(ctx);

    if (g_ControlDevice) {
        PCONTROL_DEVICE_CONTEXT ctlCtx = ControlGetContext(g_ControlDevice);
        GaYmClearControlFilterContextIfMatch(ctlCtx, ctx);
        GAYM_LOG_INFO("CDO: unbound from surprise-removed device");
    }

    /* Purge the manual queue (cancels all requests with STATUS_CANCELLED) */
    WdfIoQueuePurgeSynchronously(ctx->PendingReadsQueue);

    GAYM_LOG_INFO("SurpriseRemoval: %s unplugged (VID:%04X PID:%04X)",
        GaYmDeviceTypeName(ctx->DeviceType),
        ctx->VendorId, ctx->ProductId);
}
