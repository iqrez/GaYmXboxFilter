/*
 * GaYmXboxFilter - XboxComposite upper filter with sideband control and
 * bounded request tracing.
 */

#include "device.h"

static WDFDEVICE g_ControlDevice = NULL;

DECLARE_CONST_UNICODE_STRING(g_CtlDeviceName, L"\\Device\\GaYmXInputFilterCtl");
DECLARE_CONST_UNICODE_STRING(g_CtlSymLink, L"\\DosDevices\\GaYmXInputFilterCtl");

#define GAYM_XINPUTHID_IOCTL_GET_STATE 0x8000E00C
#define GAYM_XINPUTHID_IOCTL_SET_STATE 0x8000A010
#define GAYM_XINPUTHID_STATE_SELECTOR_LENGTH 3
#define GAYM_XINPUTHID_STATE_PACKET_LENGTH 29
#define GAYM_XINPUTHID_OUTPUT_PACKET_LENGTH 5
#define GAYM_WDM_POOL_TAG 'mwYG'
#define GAYM_XINPUT_FILTER_QUERY_LAYOUT_VERSION 5u
#define GAYM_XINPUT_FILTER_BUILD_STAMP 0x20260327u

typedef struct _GAYM_WDM_IRP_CONTEXT {
    PDEVICE_CONTEXT DeviceContext;
    ULONG RequestType;
    ULONG IoControlCode;
    ULONG InputLength;
    ULONG OutputLength;
    UCHAR InputSampleLength;
    UCHAR InputSample[GAYM_TRACE_SAMPLE_BYTES];
} GAYM_WDM_IRP_CONTEXT, *PGAYM_WDM_IRP_CONTEXT;

static VOID GaYmAppendTraceEntry(
    _In_ PDEVICE_CONTEXT ctx,
    _In_ ULONG phase,
    _In_ ULONG requestType,
    _In_ ULONG ioControlCode,
    _In_ ULONG inputLength,
    _In_ ULONG outputLength,
    _In_ ULONG transferLength,
    _In_ ULONG status,
    _In_reads_bytes_opt_(sampleLength) const UCHAR* sample,
    _In_ UCHAR sampleLength);
static BOOLEAN GaYmIsControlTarget(_In_ PDEVICE_CONTEXT ctx);
static VOID GaYmUpdateLastWriteSample(
    _In_ PDEVICE_CONTEXT ctx,
    _In_reads_bytes_opt_(sampleLength) const UCHAR* sample,
    _In_ ULONG sampleLength);
static VOID GaYmUpdateOutputCapture(
    _In_ PDEVICE_CONTEXT ctx,
    _In_reads_bytes_opt_(sampleLength) const UCHAR* sample,
    _In_ ULONG sampleLength,
    _In_ ULONG ioControlCode);

static VOID GaYmSetControlFilterContext(
    _In_ PCONTROL_DEVICE_CONTEXT controlContext,
    _In_opt_ PDEVICE_CONTEXT filterCtx)
{
    WdfWaitLockAcquire(controlContext->RouteLock, NULL);
    controlContext->FilterCtx = filterCtx;
    WdfWaitLockRelease(controlContext->RouteLock);
}

static VOID GaYmClearControlFilterContextIfMatch(
    _In_ PCONTROL_DEVICE_CONTEXT controlContext,
    _In_ PDEVICE_CONTEXT filterCtx)
{
    WdfWaitLockAcquire(controlContext->RouteLock, NULL);
    if (controlContext->FilterCtx == filterCtx) {
        controlContext->FilterCtx = NULL;
    }
    WdfWaitLockRelease(controlContext->RouteLock);
}

static PDEVICE_CONTEXT GaYmAcquireActiveControlFilterContext(
    _In_ PCONTROL_DEVICE_CONTEXT controlContext)
{
    PDEVICE_CONTEXT filterCtx;

    filterCtx = NULL;

    WdfWaitLockAcquire(controlContext->RouteLock, NULL);
    if (controlContext->FilterCtx != NULL &&
        controlContext->FilterCtx->IsInD0 &&
        GaYmIsControlTarget(controlContext->FilterCtx)) {
        filterCtx = controlContext->FilterCtx;
        WdfObjectReference(filterCtx->Device);
    }
    WdfWaitLockRelease(controlContext->RouteLock);

    return filterCtx;
}

static VOID GaYmResetTraceState(_In_ PDEVICE_CONTEXT ctx)
{
    KIRQL oldIrql;

    ctx->PendingInputRequests = 0;
    ctx->QueuedInputRequests = 0;
    ctx->CompletedInputRequests = 0;
    ctx->ForwardedInputRequests = 0;
    ctx->LastInterceptedIoctl = 0;
    ctx->ReadRequestsSeen = 0;
    ctx->DeviceControlRequestsSeen = 0;
    ctx->InternalDeviceControlRequestsSeen = 0;
    ctx->WriteRequestsSeen = 0;
    ctx->LastCompletedStatus = STATUS_SUCCESS;
    ctx->LastCompletionInformation = 0;
    ctx->LastReadLength = 0;
    ctx->LastWriteLength = 0;
    ctx->LastWriteSampleLength = 0;
    ctx->LastOutputCaptureIoctl = 0;
    ctx->LastOutputCaptureLength = 0;
    ctx->LastOutputCaptureSampleLength = 0;
    ctx->LastDeviceControlInputLength = 0;
    ctx->LastDeviceControlOutputLength = 0;
    ctx->LastInternalInputLength = 0;
    ctx->LastInternalOutputLength = 0;
    ctx->LastRawReadSampleLength = 0;
    ctx->LastPatchedReadSampleLength = 0;
    ctx->LastRawReadCompletionLength = 0;
    ctx->LastPatchedReadCompletionLength = 0;
    ctx->LastNativeOverrideApplied = FALSE;
    ctx->LastNativeOverrideBytesWritten = 0;
    ctx->TraceSequence = 0;
    ctx->TraceWriteIndex = 0;
    ctx->TraceCount = 0;

    KeAcquireSpinLock(&ctx->TraceLock, &oldIrql);
    RtlZeroMemory(ctx->Trace, sizeof(ctx->Trace));
    RtlZeroMemory(ctx->LastRawReadSample, sizeof(ctx->LastRawReadSample));
    RtlZeroMemory(ctx->LastPatchedReadSample, sizeof(ctx->LastPatchedReadSample));
    RtlZeroMemory(ctx->LastWriteSample, sizeof(ctx->LastWriteSample));
    RtlZeroMemory(ctx->LastOutputCaptureSample, sizeof(ctx->LastOutputCaptureSample));
    RtlZeroMemory(&ctx->LastOutputCaptureState, sizeof(ctx->LastOutputCaptureState));
    KeReleaseSpinLock(&ctx->TraceLock, oldIrql);
}

static VOID GaYmResetRuntimeState(_In_ PDEVICE_CONTEXT ctx)
{
    KIRQL oldIrql;

    ctx->OverrideEnabled = FALSE;
    ctx->HasReport = FALSE;
    ctx->ReportsSent = 0;
    ctx->SeqCounter = 0;
    RtlZeroMemory(&ctx->JitterConfig, sizeof(ctx->JitterConfig));

    KeAcquireSpinLock(&ctx->ReportLock, &oldIrql);
    RtlZeroMemory(&ctx->CurrentReport, sizeof(ctx->CurrentReport));
    KeReleaseSpinLock(&ctx->ReportLock, oldIrql);

    GaYmResetTraceState(ctx);
}

static BOOLEAN GaYmIsControlTarget(_In_ PDEVICE_CONTEXT ctx)
{
    return ctx->VendorId == 0x045E &&
        ctx->ProductId == 0x02FF &&
        !ctx->IsUsbStack;
}

static ULONG GaYmMinUlong(_In_ ULONG left, _In_ ULONG right)
{
    return left < right ? left : right;
}

static LONG GaYmSaturatingDecrementCounter(_Inout_ volatile LONG* value)
{
    LONG current;
    LONG observed;

    current = InterlockedCompareExchange(value, 0, 0);
    while (current > 0) {
        observed = InterlockedCompareExchange(value, current - 1, current);
        if (observed == current) {
            return current - 1;
        }

        current = observed;
    }

    return 0;
}

static ULONG GaYmReadNonNegativeCounter(_In_ volatile LONG* value)
{
    LONG current;

    current = InterlockedCompareExchange(value, 0, 0);
    return current > 0 ? (ULONG)current : 0;
}

static PUCHAR GaYmMapIrpBuffer(_In_ PIRP irp)
{
    if (irp->MdlAddress != NULL) {
        return (PUCHAR)MmGetSystemAddressForMdlSafe(
            irp->MdlAddress,
            NormalPagePriority | MdlMappingNoExecute);
    }

    if (irp->AssociatedIrp.SystemBuffer != NULL) {
        return (PUCHAR)irp->AssociatedIrp.SystemBuffer;
    }

    return NULL;
}

static UCHAR GaYmCaptureIrpInputSample(
    _In_ PIRP irp,
    _In_ PIO_STACK_LOCATION stack,
    _In_ ULONG inputLength,
    _Out_writes_bytes_(GAYM_TRACE_SAMPLE_BYTES) PUCHAR sample)
{
    PVOID buffer = NULL;
    UCHAR bytesToCopy;

    RtlZeroMemory(sample, GAYM_TRACE_SAMPLE_BYTES);

    if (inputLength == 0) {
        return 0;
    }

    if (irp->AssociatedIrp.SystemBuffer != NULL) {
        buffer = irp->AssociatedIrp.SystemBuffer;
    } else if (stack->Parameters.DeviceIoControl.Type3InputBuffer != NULL) {
        buffer = stack->Parameters.DeviceIoControl.Type3InputBuffer;
    }

    if (buffer == NULL) {
        return 0;
    }

    bytesToCopy = (UCHAR)GaYmMinUlong(inputLength, GAYM_TRACE_SAMPLE_BYTES);

    __try {
        RtlCopyMemory(sample, buffer, bytesToCopy);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RtlZeroMemory(sample, GAYM_TRACE_SAMPLE_BYTES);
        return 0;
    }

    return bytesToCopy;
}

static UCHAR GaYmCopyIrpOutputSample(
    _In_ PIRP irp,
    _In_ ULONG requestedBytes,
    _Out_writes_bytes_(GAYM_TRACE_SAMPLE_BYTES) PUCHAR sample)
{
    PUCHAR buffer;
    UCHAR bytesToCopy;

    RtlZeroMemory(sample, GAYM_TRACE_SAMPLE_BYTES);

    if (requestedBytes == 0) {
        return 0;
    }

    buffer = GaYmMapIrpBuffer(irp);
    if (buffer == NULL) {
        return 0;
    }

    bytesToCopy = (UCHAR)GaYmMinUlong(requestedBytes, GAYM_TRACE_SAMPLE_BYTES);

    __try {
        RtlCopyMemory(sample, buffer, bytesToCopy);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RtlZeroMemory(sample, GAYM_TRACE_SAMPLE_BYTES);
        return 0;
    }

    return bytesToCopy;
}

static VOID GaYmRecordIrpDispatchTrace(
    _In_ PDEVICE_CONTEXT ctx,
    _In_ const GAYM_WDM_IRP_CONTEXT* irpContext)
{
    GaYmAppendTraceEntry(
        ctx,
        GAYM_TRACE_PHASE_DISPATCH,
        irpContext->RequestType,
        irpContext->IoControlCode,
        irpContext->InputLength,
        irpContext->OutputLength,
        0,
        STATUS_SUCCESS,
        irpContext->InputSampleLength != 0 ? irpContext->InputSample : NULL,
        irpContext->InputSampleLength);
}

static VOID GaYmRecordIrpCompletionTrace(
    _In_ PDEVICE_CONTEXT ctx,
    _In_ const GAYM_WDM_IRP_CONTEXT* irpContext,
    _In_ PIRP irp,
    _In_ NTSTATUS status,
    _In_ ULONG transferLength,
    _In_ ULONG phase)
{
    UCHAR sample[GAYM_TRACE_SAMPLE_BYTES];
    UCHAR sampleLength = 0;

    if (phase == GAYM_TRACE_PHASE_COMPLETION &&
        irpContext->RequestType != GAYM_TRACE_REQUEST_WRITE) {
        sampleLength = GaYmCopyIrpOutputSample(irp, transferLength, sample);
    }

    GaYmAppendTraceEntry(
        ctx,
        phase,
        irpContext->RequestType,
        irpContext->IoControlCode,
        irpContext->InputLength,
        irpContext->OutputLength,
        transferLength,
        (ULONG)status,
        sampleLength != 0 ? sample : NULL,
        sampleLength);
}

static VOID GaYmStoreNativeReadSample(
    _In_ PDEVICE_CONTEXT ctx,
    _In_reads_bytes_(sampleLength) const UCHAR* sample,
    _In_ ULONG sampleLength,
    _In_ ULONG completionLength,
    _In_ BOOLEAN patched,
    _In_ ULONG bytesWritten)
{
    KIRQL oldIrql;
    ULONG bytesToCopy;
    UCHAR* targetBuffer;

    bytesToCopy = GaYmMinUlong(sampleLength, GAYM_NATIVE_SAMPLE_BYTES);
    targetBuffer = patched ? ctx->LastPatchedReadSample : ctx->LastRawReadSample;

    KeAcquireSpinLock(&ctx->TraceLock, &oldIrql);
    RtlZeroMemory(targetBuffer, GAYM_NATIVE_SAMPLE_BYTES);
    if (bytesToCopy != 0) {
        RtlCopyMemory(targetBuffer, sample, bytesToCopy);
    }

    if (patched) {
        ctx->LastPatchedReadSampleLength = bytesToCopy;
        ctx->LastPatchedReadCompletionLength = completionLength;
        ctx->LastNativeOverrideApplied = bytesWritten != 0;
        ctx->LastNativeOverrideBytesWritten = bytesWritten;
    } else {
        ctx->LastRawReadSampleLength = bytesToCopy;
        ctx->LastRawReadCompletionLength = completionLength;
        ctx->LastNativeOverrideApplied = FALSE;
        ctx->LastNativeOverrideBytesWritten = 0;
        RtlZeroMemory(ctx->LastPatchedReadSample, sizeof(ctx->LastPatchedReadSample));
        ctx->LastPatchedReadSampleLength = 0;
        ctx->LastPatchedReadCompletionLength = 0;
    }
    KeReleaseSpinLock(&ctx->TraceLock, oldIrql);
}

static VOID GaYmWriteLeU16(_Out_writes_(2) PUCHAR buffer, _In_ USHORT value)
{
    buffer[0] = (UCHAR)(value & 0xFF);
    buffer[1] = (UCHAR)(value >> 8);
}

static VOID GaYmWriteLeU32(_Out_writes_(4) PUCHAR buffer, _In_ ULONG value)
{
    buffer[0] = (UCHAR)(value & 0xFF);
    buffer[1] = (UCHAR)((value >> 8) & 0xFF);
    buffer[2] = (UCHAR)((value >> 16) & 0xFF);
    buffer[3] = (UCHAR)((value >> 24) & 0xFF);
}

static USHORT GaYmBuildWrappedButtons(_In_ const GAYM_REPORT* report)
{
    USHORT buttons = 0;

    /* XINPUT_GAMEPAD::wButtons mapping inside the 0x8000E00C wrapper. */
    if (report->DPad != GAYM_DPAD_NEUTRAL) {
        switch (report->DPad) {
        case GAYM_DPAD_UP:
            buttons |= 0x0001;
            break;
        case GAYM_DPAD_UPRIGHT:
            buttons |= 0x0001 | 0x0002;
            break;
        case GAYM_DPAD_RIGHT:
            buttons |= 0x0002;
            break;
        case GAYM_DPAD_DOWNRIGHT:
            buttons |= 0x0002 | 0x0004;
            break;
        case GAYM_DPAD_DOWN:
            buttons |= 0x0004;
            break;
        case GAYM_DPAD_DOWNLEFT:
            buttons |= 0x0004 | 0x0008;
            break;
        case GAYM_DPAD_LEFT:
            buttons |= 0x0008;
            break;
        case GAYM_DPAD_UPLEFT:
            buttons |= 0x0008 | 0x0001;
            break;
        default:
            break;
        }
    }

    if (report->Buttons[0] & GAYM_BTN_A)     buttons |= 0x1000;
    if (report->Buttons[0] & GAYM_BTN_B)     buttons |= 0x2000;
    if (report->Buttons[0] & GAYM_BTN_X)     buttons |= 0x4000;
    if (report->Buttons[0] & GAYM_BTN_Y)     buttons |= 0x8000;
    if (report->Buttons[0] & GAYM_BTN_LB)    buttons |= 0x0100;
    if (report->Buttons[0] & GAYM_BTN_RB)    buttons |= 0x0200;
    if (report->Buttons[0] & GAYM_BTN_BACK)  buttons |= 0x0020;
    if (report->Buttons[0] & GAYM_BTN_START) buttons |= 0x0010;
    if (report->Buttons[1] & GAYM_BTN_LSTICK) buttons |= 0x0040;
    if (report->Buttons[1] & GAYM_BTN_RSTICK) buttons |= 0x0080;

    return buttons;
}

static VOID GaYmApplyWrappedStateReport(
    _Out_writes_(GAYM_XINPUTHID_STATE_PACKET_LENGTH) PUCHAR outputBuffer,
    _In_ const GAYM_REPORT* report,
    _In_ ULONG packetNumber)
{
    USHORT buttons;

    /* Bytes 5-8 are the XINPUT_STATE::dwPacketNumber inside the wrapper. */
    GaYmWriteLeU32(outputBuffer + 5, packetNumber);

    buttons = GaYmBuildWrappedButtons(report);
    GaYmWriteLeU16(outputBuffer + 11, buttons);
    outputBuffer[13] = report->TriggerLeft;
    outputBuffer[14] = report->TriggerRight;
    GaYmWriteLeU16(outputBuffer + 15, (USHORT)report->ThumbLeftX);
    GaYmWriteLeU16(outputBuffer + 17, (USHORT)report->ThumbLeftY);
    GaYmWriteLeU16(outputBuffer + 19, (USHORT)report->ThumbRightX);
    GaYmWriteLeU16(outputBuffer + 21, (USHORT)report->ThumbRightY);
}

static VOID GaYmApplyXInputStateOverride(
    _In_ PDEVICE_CONTEXT ctx,
    _In_ PREQUEST_CONTEXT requestContext,
    _In_ WDFREQUEST request,
    _In_ NTSTATUS completionStatus,
    _In_ ULONG completionInformation)
{
    NTSTATUS status;
    PUCHAR outputBuffer = NULL;
    size_t outputLength = 0;
    KIRQL oldIrql;
    GAYM_REPORT report;
    ULONG packetNumber;

    if (!NT_SUCCESS(completionStatus) ||
        !ctx->OverrideEnabled ||
        !ctx->HasReport ||
        requestContext->RequestType != GAYM_TRACE_REQUEST_DEVICE_CONTROL ||
        requestContext->IoControlCode != GAYM_XINPUTHID_IOCTL_GET_STATE ||
        completionInformation < GAYM_XINPUTHID_STATE_PACKET_LENGTH) {
        return;
    }

    if (requestContext->InputSampleLength < GAYM_XINPUTHID_STATE_SELECTOR_LENGTH ||
        requestContext->InputSample[0] != 0x01 ||
        requestContext->InputSample[1] != 0x01 ||
        requestContext->InputSample[2] != 0x00) {
        return;
    }

    status = WdfRequestRetrieveOutputBuffer(
        request,
        GAYM_XINPUTHID_STATE_PACKET_LENGTH,
        (PVOID*)&outputBuffer,
        &outputLength);
    if (!NT_SUCCESS(status) || outputLength < GAYM_XINPUTHID_STATE_PACKET_LENGTH) {
        return;
    }

    KeAcquireSpinLock(&ctx->ReportLock, &oldIrql);
    report = ctx->CurrentReport;
    packetNumber = ctx->ReportsSent;
    KeReleaseSpinLock(&ctx->ReportLock, oldIrql);

    if (packetNumber == 0) {
        packetNumber = 1;
    }

    GaYmApplyWrappedStateReport(outputBuffer, &report, packetNumber);
}

static VOID GaYmApplyNativeHidReadOverride(
    _In_ PDEVICE_CONTEXT ctx,
    _In_ PREQUEST_CONTEXT requestContext,
    _In_ WDFREQUEST request,
    _In_ NTSTATUS completionStatus,
    _Inout_ PULONG completionInformation)
{
    NTSTATUS status;
    PUCHAR outputBuffer = NULL;
    size_t outputLength = 0;
    KIRQL oldIrql;
    GAYM_REPORT report;
    ULONG bytesWritten = 0;
    ULONG nativeReportLength;
    ULONG sampleLength;

    if (!NT_SUCCESS(completionStatus) ||
        requestContext->RequestType != GAYM_TRACE_REQUEST_READ ||
        *completionInformation == 0) {
        return;
    }

    status = WdfRequestRetrieveOutputBuffer(
        request,
        1,
        (PVOID*)&outputBuffer,
        &outputLength);
    if (!NT_SUCCESS(status) || outputBuffer == NULL || outputLength == 0) {
        return;
    }

    sampleLength = GaYmMinUlong((ULONG)outputLength, *completionInformation);
    GaYmStoreNativeReadSample(ctx, outputBuffer, sampleLength, *completionInformation, FALSE, 0);

    if (!ctx->OverrideEnabled ||
        !ctx->HasReport ||
        ctx->DeviceDesc == NULL ||
        ctx->DeviceDesc->TranslateReport == NULL) {
        return;
    }

    nativeReportLength = ctx->DeviceDesc->NativeReportSize;
    if (nativeReportLength == 0 || *completionInformation < nativeReportLength || outputLength < nativeReportLength) {
        return;
    }

    KeAcquireSpinLock(&ctx->ReportLock, &oldIrql);
    report = ctx->CurrentReport;
    KeReleaseSpinLock(&ctx->ReportLock, oldIrql);

    status = ctx->DeviceDesc->TranslateReport(
        &report,
        outputBuffer,
        (ULONG)outputLength,
        &bytesWritten,
        &ctx->SeqCounter);
    if (!NT_SUCCESS(status) || bytesWritten == 0) {
        return;
    }

    if (*completionInformation < bytesWritten) {
        *completionInformation = bytesWritten;
    }

    sampleLength = GaYmMinUlong((ULONG)outputLength, *completionInformation);
    GaYmStoreNativeReadSample(ctx, outputBuffer, sampleLength, *completionInformation, TRUE, bytesWritten);
}

static ULONG GaYmTryCopyBufferSample(
    _In_ WDFREQUEST request,
    _In_ BOOLEAN outputBuffer,
    _In_ ULONG requestedBytes,
    _Out_writes_bytes_(GAYM_TRACE_SAMPLE_BYTES) PUCHAR sample)
{
    NTSTATUS status;
    PVOID buffer = NULL;
    size_t bufferLength = 0;
    ULONG bytesToCopy;

    RtlZeroMemory(sample, GAYM_TRACE_SAMPLE_BYTES);

    if (requestedBytes == 0) {
        return 0;
    }

    if (outputBuffer) {
        status = WdfRequestRetrieveOutputBuffer(request, 1, &buffer, &bufferLength);
    } else {
        status = WdfRequestRetrieveInputBuffer(request, 1, &buffer, &bufferLength);
    }

    if (!NT_SUCCESS(status) || buffer == NULL || bufferLength == 0) {
        return 0;
    }

    bytesToCopy = GaYmMinUlong((ULONG)bufferLength, requestedBytes);
    bytesToCopy = GaYmMinUlong(bytesToCopy, GAYM_TRACE_SAMPLE_BYTES);
    RtlCopyMemory(sample, buffer, bytesToCopy);
    return bytesToCopy;
}

static VOID GaYmAppendTraceEntry(
    _In_ PDEVICE_CONTEXT ctx,
    _In_ ULONG phase,
    _In_ ULONG requestType,
    _In_ ULONG ioControlCode,
    _In_ ULONG inputLength,
    _In_ ULONG outputLength,
    _In_ ULONG transferLength,
    _In_ ULONG status,
    _In_reads_bytes_opt_(sampleLength) const UCHAR* sample,
    _In_ UCHAR sampleLength)
{
    GAYM_TRACE_ENTRY entry;
    KIRQL oldIrql;
    ULONG sequence;

    RtlZeroMemory(&entry, sizeof(entry));
    sequence = (ULONG)InterlockedIncrement(&ctx->TraceSequence);

    entry.Sequence = sequence;
    entry.Phase = phase;
    entry.RequestType = requestType;
    entry.IoControlCode = ioControlCode;
    entry.InputLength = inputLength;
    entry.OutputLength = outputLength;
    entry.TransferLength = transferLength;
    entry.Status = status;
    entry.SampleLength = sampleLength;

    if (sample != NULL && sampleLength != 0) {
        RtlCopyMemory(entry.Sample, sample, sampleLength);
    }

    KeAcquireSpinLock(&ctx->TraceLock, &oldIrql);
    ctx->Trace[ctx->TraceWriteIndex] = entry;
    ctx->TraceWriteIndex = (ctx->TraceWriteIndex + 1) % GAYM_TRACE_HISTORY_COUNT;
    if (ctx->TraceCount < GAYM_TRACE_HISTORY_COUNT) {
        ctx->TraceCount++;
    }
    KeReleaseSpinLock(&ctx->TraceLock, oldIrql);
}

static VOID GaYmRecordDispatchTrace(
    _In_ PDEVICE_CONTEXT ctx,
    _In_ PREQUEST_CONTEXT requestContext,
    _In_ WDFREQUEST request)
{
    UCHAR sample[GAYM_TRACE_SAMPLE_BYTES];
    UCHAR sampleLength = 0;

    if (requestContext->RequestType == GAYM_TRACE_REQUEST_WRITE ||
        requestContext->RequestType == GAYM_TRACE_REQUEST_DEVICE_CONTROL ||
        requestContext->RequestType == GAYM_TRACE_REQUEST_INTERNAL_DEVICE_CONTROL) {
        sampleLength = (UCHAR)GaYmTryCopyBufferSample(
            request,
            FALSE,
            requestContext->InputLength,
            sample);
    }

    GaYmAppendTraceEntry(
        ctx,
        GAYM_TRACE_PHASE_DISPATCH,
        requestContext->RequestType,
        requestContext->IoControlCode,
        requestContext->InputLength,
        requestContext->OutputLength,
        0,
        STATUS_SUCCESS,
        sampleLength != 0 ? sample : NULL,
        sampleLength);
}

static VOID GaYmRecordCompletionTrace(
    _In_ PDEVICE_CONTEXT ctx,
    _In_ PREQUEST_CONTEXT requestContext,
    _In_ WDFREQUEST request,
    _In_ NTSTATUS status,
    _In_ ULONG transferLength,
    _In_ ULONG phase)
{
    UCHAR sample[GAYM_TRACE_SAMPLE_BYTES];
    UCHAR sampleLength = 0;

    if (phase == GAYM_TRACE_PHASE_COMPLETION &&
        requestContext->RequestType != GAYM_TRACE_REQUEST_WRITE) {
        sampleLength = (UCHAR)GaYmTryCopyBufferSample(
            request,
            TRUE,
            transferLength,
            sample);
    }

    GaYmAppendTraceEntry(
        ctx,
        phase,
        requestContext->RequestType,
        requestContext->IoControlCode,
        requestContext->InputLength,
        requestContext->OutputLength,
        transferLength,
        (ULONG)status,
        sampleLength != 0 ? sample : NULL,
        sampleLength);
}

static VOID GaYmCopyTraceSnapshot(
    _In_ PDEVICE_CONTEXT ctx,
    _Out_ PGAYM_DEVICE_INFO deviceInfo)
{
    KIRQL oldIrql;
    ULONG count;
    ULONG startIndex;
    ULONG i;

    KeAcquireSpinLock(&ctx->TraceLock, &oldIrql);
    count = ctx->TraceCount;
    startIndex = (ctx->TraceWriteIndex + GAYM_TRACE_HISTORY_COUNT - count) % GAYM_TRACE_HISTORY_COUNT;
    deviceInfo->TraceSequence = (ULONG)ctx->TraceSequence;
    deviceInfo->TraceCount = count;

    for (i = 0; i < count; ++i) {
        ULONG traceIndex = (startIndex + i) % GAYM_TRACE_HISTORY_COUNT;
        deviceInfo->Trace[i] = ctx->Trace[traceIndex];
    }

    for (; i < GAYM_TRACE_HISTORY_COUNT; ++i) {
        RtlZeroMemory(&deviceInfo->Trace[i], sizeof(deviceInfo->Trace[i]));
    }
    KeReleaseSpinLock(&ctx->TraceLock, oldIrql);
}

static VOID GaYmCopyNativeReadSnapshot(
    _In_ PDEVICE_CONTEXT ctx,
    _Out_ PGAYM_DEVICE_INFO deviceInfo)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&ctx->TraceLock, &oldIrql);
    deviceInfo->LastRawReadSampleLength = (ULONG)ctx->LastRawReadSampleLength;
    deviceInfo->LastPatchedReadSampleLength = (ULONG)ctx->LastPatchedReadSampleLength;
    deviceInfo->LastRawReadCompletionLength = (ULONG)ctx->LastRawReadCompletionLength;
    deviceInfo->LastPatchedReadCompletionLength = (ULONG)ctx->LastPatchedReadCompletionLength;
    deviceInfo->LastNativeOverrideApplied = (ULONG)ctx->LastNativeOverrideApplied;
    deviceInfo->LastNativeOverrideBytesWritten = (ULONG)ctx->LastNativeOverrideBytesWritten;
    RtlCopyMemory(deviceInfo->LastRawReadSample, ctx->LastRawReadSample, sizeof(deviceInfo->LastRawReadSample));
    RtlCopyMemory(deviceInfo->LastPatchedReadSample, ctx->LastPatchedReadSample, sizeof(deviceInfo->LastPatchedReadSample));
    KeReleaseSpinLock(&ctx->TraceLock, oldIrql);
}

static VOID GaYmCopyWriteSnapshot(
    _In_ PDEVICE_CONTEXT ctx,
    _Out_ PGAYM_DEVICE_INFO deviceInfo)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&ctx->TraceLock, &oldIrql);
    deviceInfo->LastWriteSampleLength = (ULONG)ctx->LastWriteSampleLength;
    RtlCopyMemory(deviceInfo->LastWriteSample, ctx->LastWriteSample, sizeof(deviceInfo->LastWriteSample));
    KeReleaseSpinLock(&ctx->TraceLock, oldIrql);
}

static VOID GaYmCopyOutputCaptureSnapshot(
    _In_ PDEVICE_CONTEXT ctx,
    _Out_ PGAYM_DEVICE_INFO deviceInfo)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&ctx->TraceLock, &oldIrql);
    deviceInfo->LastOutputCaptureIoctl = (ULONG)ctx->LastOutputCaptureIoctl;
    deviceInfo->LastOutputCaptureLength = (ULONG)ctx->LastOutputCaptureLength;
    deviceInfo->LastOutputCaptureSampleLength = (ULONG)ctx->LastOutputCaptureSampleLength;
    RtlCopyMemory(
        deviceInfo->LastOutputCaptureSample,
        ctx->LastOutputCaptureSample,
        sizeof(deviceInfo->LastOutputCaptureSample));
    RtlCopyMemory(
        &deviceInfo->LastOutputCaptureState,
        &ctx->LastOutputCaptureState,
        sizeof(deviceInfo->LastOutputCaptureState));
    KeReleaseSpinLock(&ctx->TraceLock, oldIrql);
}

static VOID GaYmUpdateLastWriteSample(
    _In_ PDEVICE_CONTEXT ctx,
    _In_reads_bytes_opt_(sampleLength) const UCHAR* sample,
    _In_ ULONG sampleLength)
{
    KIRQL oldIrql;
    ULONG bytesToCopy;

    bytesToCopy = GaYmMinUlong(sampleLength, GAYM_TRACE_SAMPLE_BYTES);

    KeAcquireSpinLock(&ctx->TraceLock, &oldIrql);
    RtlZeroMemory(ctx->LastWriteSample, sizeof(ctx->LastWriteSample));
    if (sample != NULL && bytesToCopy != 0) {
        RtlCopyMemory(ctx->LastWriteSample, sample, bytesToCopy);
    }
    ctx->LastWriteSampleLength = bytesToCopy;
    KeReleaseSpinLock(&ctx->TraceLock, oldIrql);
}

static VOID GaYmUpdateOutputCapture(
    _In_ PDEVICE_CONTEXT ctx,
    _In_reads_bytes_opt_(sampleLength) const UCHAR* sample,
    _In_ ULONG sampleLength,
    _In_ ULONG ioControlCode)
{
    KIRQL oldIrql;
    ULONG bytesToCopy;
    GAYM_OUTPUT_STATE outputState;
    NTSTATUS parseStatus;

    RtlZeroMemory(&outputState, sizeof(outputState));
    parseStatus = STATUS_INVALID_PARAMETER;
    if (sample != NULL &&
        sampleLength != 0 &&
        ctx->DeviceDesc != NULL &&
        ctx->DeviceDesc->ParseOutputReport != NULL) {
        parseStatus = GaYmParseOutputReport(
            ctx->DeviceDesc,
            sample,
            sampleLength,
            &outputState);
    }

    bytesToCopy = GaYmMinUlong(sampleLength, GAYM_TRACE_SAMPLE_BYTES);

    KeAcquireSpinLock(&ctx->TraceLock, &oldIrql);
    ctx->LastOutputCaptureIoctl = ioControlCode;
    ctx->LastOutputCaptureLength = sampleLength;
    ctx->LastOutputCaptureSampleLength = bytesToCopy;
    RtlZeroMemory(ctx->LastOutputCaptureSample, sizeof(ctx->LastOutputCaptureSample));
    if (sample != NULL && bytesToCopy != 0) {
        RtlCopyMemory(ctx->LastOutputCaptureSample, sample, bytesToCopy);
    }

    if (NT_SUCCESS(parseStatus)) {
        RtlCopyMemory(&ctx->LastOutputCaptureState, &outputState, sizeof(outputState));
    } else {
        RtlZeroMemory(&ctx->LastOutputCaptureState, sizeof(ctx->LastOutputCaptureState));
    }
    KeReleaseSpinLock(&ctx->TraceLock, oldIrql);
}

static VOID GaYmInitializeRequestContext(
    _In_ PREQUEST_CONTEXT requestContext,
    _In_ ULONG requestType,
    _In_ ULONG ioControlCode,
    _In_ ULONG inputLength,
    _In_ ULONG outputLength)
{
    requestContext->RequestType = requestType;
    requestContext->IoControlCode = ioControlCode;
    requestContext->InputLength = inputLength;
    requestContext->OutputLength = outputLength;
    requestContext->InputSampleLength = 0;
    RtlZeroMemory(requestContext->InputSample, sizeof(requestContext->InputSample));
}

static VOID GaYmCaptureRequestInputSample(
    _In_ WDFREQUEST request,
    _Inout_ PREQUEST_CONTEXT requestContext)
{
    NTSTATUS status;
    PUCHAR inputBuffer = NULL;
    size_t inputLength = 0;
    UCHAR bytesToCopy;

    if (requestContext->InputLength == 0) {
        return;
    }

    status = WdfRequestRetrieveInputBuffer(
        request,
        1,
        (PVOID*)&inputBuffer,
        &inputLength);
    if (!NT_SUCCESS(status) || inputBuffer == NULL || inputLength == 0) {
        return;
    }

    bytesToCopy = (UCHAR)GaYmMinUlong((ULONG)inputLength, sizeof(requestContext->InputSample));
    RtlCopyMemory(requestContext->InputSample, inputBuffer, bytesToCopy);
    requestContext->InputSampleLength = bytesToCopy;
}

static VOID QueryDeviceIds(
    _In_ WDFDEVICE device,
    _Out_ PUSHORT vendorId,
    _Out_ PUSHORT productId,
    _Out_ PBOOLEAN isUsbStack)
{
    PDEVICE_OBJECT physicalDevice;
    WCHAR hardwareId[512];
    ULONG resultLength = 0;
    NTSTATUS status;

    *vendorId = 0;
    *productId = 0;
    *isUsbStack = FALSE;

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
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_WARN("IoGetDeviceProperty(HardwareID) failed: 0x%08X", status);
        return;
    }

    GaYmParseHardwareId(hardwareId, vendorId, productId);
    *isUsbStack =
        hardwareId[0] == L'U' &&
        hardwareId[1] == L'S' &&
        hardwareId[2] == L'B' &&
        hardwareId[3] == L'\\';
}

static VOID GaYmSendFormattedRequest(
    _In_ PDEVICE_CONTEXT ctx,
    _In_ WDFREQUEST request,
    _In_ BOOLEAN recordDispatchTrace,
    _In_ BOOLEAN pendingAlreadyCounted)
{
    PREQUEST_CONTEXT requestContext;

    requestContext = RequestGetContext(request);
    if (recordDispatchTrace) {
        GaYmRecordDispatchTrace(ctx, requestContext, request);
    }

    WdfRequestFormatRequestUsingCurrentType(request);
    WdfRequestSetCompletionRoutine(request, GaYmEvtRequestCompletion, ctx);
    if (!pendingAlreadyCounted) {
        InterlockedIncrement(&ctx->PendingInputRequests);
    }
    InterlockedIncrement(&ctx->ForwardedInputRequests);

    if (!WdfRequestSend(request, ctx->IoTarget, WDF_NO_SEND_OPTIONS)) {
        NTSTATUS status = WdfRequestGetStatus(request);

        InterlockedDecrement(&ctx->PendingInputRequests);
        InterlockedDecrement(&ctx->ForwardedInputRequests);
        InterlockedExchange(&ctx->LastCompletedStatus, status);
        InterlockedExchange(&ctx->LastCompletionInformation, 0);
        GaYmRecordCompletionTrace(
            ctx,
            requestContext,
            request,
            status,
            0,
            GAYM_TRACE_PHASE_SEND_FAILURE);
        WdfRequestComplete(request, status);
    }
}

static VOID GaYmQueuePendingRead(
    _In_ PDEVICE_CONTEXT ctx,
    _In_ WDFREQUEST request)
{
    NTSTATUS status;

    GaYmRecordDispatchTrace(ctx, RequestGetContext(request), request);
    InterlockedIncrement(&ctx->PendingInputRequests);

    status = WdfRequestForwardToIoQueue(request, ctx->PendingReadsQueue);
    if (!NT_SUCCESS(status)) {
        InterlockedDecrement(&ctx->PendingInputRequests);
        InterlockedExchange(&ctx->LastCompletedStatus, status);
        InterlockedExchange(&ctx->LastCompletionInformation, 0);
        GaYmRecordCompletionTrace(
            ctx,
            RequestGetContext(request),
            request,
            status,
            0,
            GAYM_TRACE_PHASE_SEND_FAILURE);
        WdfRequestComplete(request, status);
    }
}

static VOID GaYmCompleteReadWithReport(
    _In_ PDEVICE_CONTEXT ctx,
    _In_ WDFREQUEST request)
{
    NTSTATUS status;
    PVOID buffer = NULL;
    size_t bufferLength = 0;
    KIRQL oldIrql;
    GAYM_REPORT report;
    ULONG bytesWritten = 0;
    ULONG sampleLength;
    PREQUEST_CONTEXT requestContext;

    requestContext = RequestGetContext(request);

    status = WdfRequestRetrieveOutputBuffer(request, 1, &buffer, &bufferLength);
    if (NT_SUCCESS(status)) {
        KeAcquireSpinLock(&ctx->ReportLock, &oldIrql);
        report = ctx->CurrentReport;
        if (ctx->DeviceDesc != NULL && ctx->DeviceDesc->TranslateReport != NULL) {
            status = ctx->DeviceDesc->TranslateReport(
                &report,
                (PUCHAR)buffer,
                (ULONG)bufferLength,
                &bytesWritten,
                &ctx->SeqCounter);
        } else {
            bytesWritten = (ULONG)GaYmMinUlong((ULONG)bufferLength, sizeof(report));
            if (bytesWritten != 0) {
                RtlCopyMemory(buffer, &report, bytesWritten);
            }
            status = STATUS_SUCCESS;
        }
        KeReleaseSpinLock(&ctx->ReportLock, oldIrql);
    }

    InterlockedDecrement(&ctx->PendingInputRequests);
    InterlockedIncrement(&ctx->CompletedInputRequests);
    InterlockedExchange(&ctx->LastCompletedStatus, status);
    InterlockedExchange(
        &ctx->LastCompletionInformation,
        NT_SUCCESS(status) ? (LONG)bytesWritten : 0);

    sampleLength = NT_SUCCESS(status) ? GaYmMinUlong((ULONG)bufferLength, bytesWritten) : 0;
    KeAcquireSpinLock(&ctx->TraceLock, &oldIrql);
    RtlZeroMemory(ctx->LastRawReadSample, sizeof(ctx->LastRawReadSample));
    ctx->LastRawReadSampleLength = 0;
    ctx->LastRawReadCompletionLength = 0;
    RtlZeroMemory(ctx->LastPatchedReadSample, sizeof(ctx->LastPatchedReadSample));
    if (buffer != NULL && sampleLength != 0) {
        RtlCopyMemory(ctx->LastPatchedReadSample, buffer, sampleLength);
    }
    ctx->LastPatchedReadSampleLength = sampleLength;
    ctx->LastPatchedReadCompletionLength = NT_SUCCESS(status) ? bytesWritten : 0;
    ctx->LastNativeOverrideApplied = NT_SUCCESS(status) && bytesWritten != 0 ? 1 : 0;
    ctx->LastNativeOverrideBytesWritten = NT_SUCCESS(status) ? bytesWritten : 0;
    KeReleaseSpinLock(&ctx->TraceLock, oldIrql);

    GaYmRecordCompletionTrace(
        ctx,
        requestContext,
        request,
        status,
        NT_SUCCESS(status) ? bytesWritten : 0,
        GAYM_TRACE_PHASE_COMPLETION);

    WdfRequestCompleteWithInformation(
        request,
        status,
        NT_SUCCESS(status) ? bytesWritten : 0);
}

static VOID GaYmDrainPendingReads(
    _In_ PDEVICE_CONTEXT ctx,
    _In_ NTSTATUS completionStatus)
{
    WDFREQUEST request;

    while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(ctx->PendingReadsQueue, &request))) {
        if (completionStatus == STATUS_SUCCESS) {
            GaYmSendFormattedRequest(ctx, request, FALSE, TRUE);
        } else {
            InterlockedDecrement(&ctx->PendingInputRequests);
            InterlockedExchange(&ctx->LastCompletedStatus, completionStatus);
            InterlockedExchange(&ctx->LastCompletionInformation, 0);
            GaYmRecordCompletionTrace(
                ctx,
                RequestGetContext(request),
                request,
                completionStatus,
                0,
                GAYM_TRACE_PHASE_SEND_FAILURE);
            WdfRequestComplete(request, completionStatus);
        }
    }
}

static BOOLEAN GaYmIsSidebandIoctl(_In_ ULONG ioControlCode)
{
    switch (ioControlCode) {
    case IOCTL_GAYM_OVERRIDE_ON:
    case IOCTL_GAYM_OVERRIDE_OFF:
    case IOCTL_GAYM_INJECT_REPORT:
    case IOCTL_GAYM_QUERY_DEVICE:
    case IOCTL_GAYM_SET_JITTER:
    case IOCTL_GAYM_APPLY_OUTPUT:
        return TRUE;
    default:
        return FALSE;
    }
}

static NTSTATUS GaYmGetIoctlBufferLengths(
    _In_ WDFREQUEST request,
    _Out_ size_t* inputLength,
    _Out_ size_t* outputLength)
{
    WDF_REQUEST_PARAMETERS parameters;

    *inputLength = 0;
    *outputLength = 0;

    WDF_REQUEST_PARAMETERS_INIT(&parameters);
    WdfRequestGetParameters(request, &parameters);

    if (parameters.Type != WdfRequestTypeDeviceControl &&
        parameters.Type != WdfRequestTypeDeviceControlInternal) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    *inputLength = parameters.Parameters.DeviceIoControl.InputBufferLength;
    *outputLength = parameters.Parameters.DeviceIoControl.OutputBufferLength;
    return STATUS_SUCCESS;
}

static NTSTATUS GaYmValidateNoInputBuffer(_In_ WDFREQUEST request)
{
    NTSTATUS status;
    size_t inputLength;
    size_t outputLength;

    status = GaYmGetIoctlBufferLengths(request, &inputLength, &outputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    UNREFERENCED_PARAMETER(outputLength);
    return inputLength == 0 ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

static VOID GaYmHandleSidebandIoctl(
    _In_ PDEVICE_CONTEXT ctx,
    _In_ WDFREQUEST request,
    _In_ ULONG ioControlCode)
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    switch (ioControlCode) {
    case IOCTL_GAYM_OVERRIDE_ON:
        status = GaYmValidateNoInputBuffer(request);
        if (!NT_SUCCESS(status)) {
            break;
        }
        ctx->OverrideEnabled = TRUE;
        ctx->HasReport = FALSE;
        ctx->SeqCounter = 0;
        GaYmResetTraceState(ctx);
        GAYM_LOG_INFO("Override ON (VID:%04X PID:%04X)", ctx->VendorId, ctx->ProductId);
        status = STATUS_SUCCESS;
        break;

    case IOCTL_GAYM_OVERRIDE_OFF:
        status = GaYmValidateNoInputBuffer(request);
        if (!NT_SUCCESS(status)) {
            break;
        }
        ctx->OverrideEnabled = FALSE;
        ctx->HasReport = FALSE;
        GaYmDrainPendingReads(ctx, STATUS_SUCCESS);
        GAYM_LOG_INFO("Override OFF");
        status = STATUS_SUCCESS;
        break;

    case IOCTL_GAYM_INJECT_REPORT:
    {
        PGAYM_REPORT report;
        size_t reportLength;
        KIRQL oldIrql;
        WDFREQUEST pendingRequest;

        status = WdfRequestRetrieveInputBuffer(
            request,
            sizeof(GAYM_REPORT),
            (PVOID*)&report,
            &reportLength);
        if (!NT_SUCCESS(status) || reportLength != sizeof(GAYM_REPORT)) {
            status = NT_SUCCESS(status) ? STATUS_INVALID_BUFFER_SIZE : status;
            break;
        }

        KeAcquireSpinLock(&ctx->ReportLock, &oldIrql);
        RtlCopyMemory(&ctx->CurrentReport, report, sizeof(*report));
        ctx->HasReport = TRUE;
        ctx->ReportsSent++;
        KeReleaseSpinLock(&ctx->ReportLock, oldIrql);

        while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(ctx->PendingReadsQueue, &pendingRequest))) {
            GaYmCompleteReadWithReport(ctx, pendingRequest);
        }

        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_GAYM_APPLY_OUTPUT:
    {
        PGAYM_OUTPUT_STATE outputState;
        size_t outputStateLength;
        UCHAR nativeReport[GAYM_MAX_NATIVE_REPORT];
        ULONG nativeReportLength;
        UCHAR traceSampleLength;
        WDF_MEMORY_DESCRIPTOR inputDescriptor;
        ULONG_PTR bytesReturned;

        if (ctx->DeviceDesc == NULL || ctx->DeviceDesc->TranslateOutputState == NULL) {
            status = STATUS_NOT_SUPPORTED;
            break;
        }

        status = WdfRequestRetrieveInputBuffer(
            request,
            sizeof(GAYM_OUTPUT_STATE),
            (PVOID*)&outputState,
            &outputStateLength);
        if (!NT_SUCCESS(status) || outputStateLength != sizeof(GAYM_OUTPUT_STATE)) {
            status = NT_SUCCESS(status) ? STATUS_INVALID_BUFFER_SIZE : status;
            break;
        }

        nativeReportLength = 0;
        status = GaYmTranslateOutputState(
            ctx->DeviceDesc,
            outputState,
            nativeReport,
            sizeof(nativeReport),
            &nativeReportLength);
        if (!NT_SUCCESS(status)) {
            break;
        }

        if (nativeReportLength == 0 || nativeReportLength > GAYM_MAX_NATIVE_REPORT) {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }

        traceSampleLength = (UCHAR)GaYmMinUlong(nativeReportLength, GAYM_TRACE_SAMPLE_BYTES);
        GaYmUpdateOutputCapture(ctx, nativeReport, nativeReportLength, GAYM_XINPUTHID_IOCTL_SET_STATE);
        InterlockedExchange(&ctx->LastInterceptedIoctl, (LONG)GAYM_XINPUTHID_IOCTL_SET_STATE);
        InterlockedExchange(&ctx->LastDeviceControlInputLength, (LONG)nativeReportLength);
        InterlockedExchange(&ctx->LastDeviceControlOutputLength, 0);

        GaYmAppendTraceEntry(
            ctx,
            GAYM_TRACE_PHASE_DISPATCH,
            GAYM_TRACE_REQUEST_DEVICE_CONTROL,
            GAYM_XINPUTHID_IOCTL_SET_STATE,
            nativeReportLength,
            0,
            0,
            STATUS_SUCCESS,
            nativeReport,
            traceSampleLength);

        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputDescriptor, nativeReport, nativeReportLength);
        bytesReturned = 0;
        status = WdfIoTargetSendIoctlSynchronously(
            ctx->IoTarget,
            NULL,
            GAYM_XINPUTHID_IOCTL_SET_STATE,
            &inputDescriptor,
            NULL,
            NULL,
            &bytesReturned);

        InterlockedExchange(&ctx->LastCompletedStatus, status);
        InterlockedExchange(
            &ctx->LastCompletionInformation,
            NT_SUCCESS(status) ? (LONG)bytesReturned : 0);

        GaYmAppendTraceEntry(
            ctx,
            NT_SUCCESS(status) ? GAYM_TRACE_PHASE_COMPLETION : GAYM_TRACE_PHASE_SEND_FAILURE,
            GAYM_TRACE_REQUEST_DEVICE_CONTROL,
            GAYM_XINPUTHID_IOCTL_SET_STATE,
            nativeReportLength,
            0,
            (ULONG)bytesReturned,
            (ULONG)status,
            nativeReport,
            traceSampleLength);

        info = NT_SUCCESS(status) ? bytesReturned : 0;
        break;
    }

    case IOCTL_GAYM_QUERY_DEVICE:
    {
        PGAYM_DEVICE_INFO deviceInfo;
        GAYM_DEVICE_INFO snapshot;
        size_t outLength;
        ULONG bytesToCopy;

        status = GaYmValidateNoInputBuffer(request);
        if (!NT_SUCCESS(status)) {
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(
            request,
            GAYM_DEVICE_INFO_MIN_SIZE,
            (PVOID*)&deviceInfo,
            &outLength);
        if (!NT_SUCCESS(status)) {
            break;
        }

        RtlZeroMemory(&snapshot, sizeof(snapshot));
        snapshot.DeviceType = ctx->DeviceType;
        snapshot.VendorId = ctx->VendorId;
        snapshot.ProductId = ctx->ProductId;
        snapshot.OverrideActive = ctx->OverrideEnabled;
        snapshot.ReportsSent = ctx->ReportsSent;
        snapshot.PendingInputRequests = GaYmReadNonNegativeCounter(&ctx->PendingInputRequests);
        snapshot.QueuedInputRequests = (ULONG)ctx->QueuedInputRequests;
        snapshot.CompletedInputRequests = (ULONG)ctx->CompletedInputRequests;
        snapshot.ForwardedInputRequests = (ULONG)ctx->ForwardedInputRequests;
        snapshot.LastInterceptedIoctl = (ULONG)ctx->LastInterceptedIoctl;
        snapshot.ReadRequestsSeen = (ULONG)ctx->ReadRequestsSeen;
        snapshot.DeviceControlRequestsSeen = (ULONG)ctx->DeviceControlRequestsSeen;
        snapshot.InternalDeviceControlRequestsSeen = (ULONG)ctx->InternalDeviceControlRequestsSeen;
        snapshot.WriteRequestsSeen = (ULONG)ctx->WriteRequestsSeen;
        snapshot.LastCompletedStatus = (ULONG)ctx->LastCompletedStatus;
        snapshot.LastCompletionInformation = (ULONG)ctx->LastCompletionInformation;
        snapshot.LastReadLength = (ULONG)ctx->LastReadLength;
        snapshot.LastWriteLength = (ULONG)ctx->LastWriteLength;
        snapshot.LastDeviceControlInputLength = (ULONG)ctx->LastDeviceControlInputLength;
        snapshot.LastDeviceControlOutputLength = (ULONG)ctx->LastDeviceControlOutputLength;
        snapshot.LastInternalInputLength = (ULONG)ctx->LastInternalInputLength;
        snapshot.LastInternalOutputLength = (ULONG)ctx->LastInternalOutputLength;
        snapshot.InputCapabilities = GaYmGetInputCapabilities(ctx->DeviceDesc);
        snapshot.OutputCapabilities = GaYmGetOutputCapabilities(ctx->DeviceDesc);
        GaYmCopyNativeReadSnapshot(ctx, &snapshot);
        GaYmCopyWriteSnapshot(ctx, &snapshot);
        GaYmCopyOutputCaptureSnapshot(ctx, &snapshot);
        GaYmCopyTraceSnapshot(ctx, &snapshot);
        snapshot.QueryLayoutVersion = GAYM_XINPUT_FILTER_QUERY_LAYOUT_VERSION;
        snapshot.DriverBuildStamp = GAYM_XINPUT_FILTER_BUILD_STAMP;

        bytesToCopy = GaYmMinUlong((ULONG)outLength, (ULONG)sizeof(snapshot));
        RtlCopyMemory(deviceInfo, &snapshot, bytesToCopy);
        info = bytesToCopy;
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_GAYM_SET_JITTER:
    {
        PGAYM_JITTER_CONFIG jitterConfig;
        size_t jitterLength;

        status = WdfRequestRetrieveInputBuffer(
            request,
            sizeof(GAYM_JITTER_CONFIG),
            (PVOID*)&jitterConfig,
            &jitterLength);
        if (!NT_SUCCESS(status) || jitterLength != sizeof(GAYM_JITTER_CONFIG)) {
            status = NT_SUCCESS(status) ? STATUS_INVALID_BUFFER_SIZE : status;
            break;
        }

        if (jitterConfig->Enabled != FALSE &&
            jitterConfig->MinDelayUs > jitterConfig->MaxDelayUs) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        RtlCopyMemory(&ctx->JitterConfig, jitterConfig, sizeof(*jitterConfig));
        GAYM_LOG_INFO(
            "Jitter config updated: enabled=%u min=%lu max=%lu",
            ctx->JitterConfig.Enabled,
            ctx->JitterConfig.MinDelayUs,
            ctx->JitterConfig.MaxDelayUs);
        status = STATUS_SUCCESS;
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(request, status, info);
}

VOID GaYmForwardRequest(_In_ PDEVICE_CONTEXT ctx, _In_ WDFREQUEST request)
{
    GaYmSendFormattedRequest(ctx, request, TRUE, FALSE);
}

VOID GaYmEvtRequestCompletion(
    _In_ WDFREQUEST request,
    _In_ WDFIOTARGET target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS completionParams,
    _In_ WDFCONTEXT context)
{
    PDEVICE_CONTEXT deviceContext;
    PREQUEST_CONTEXT requestContext;
    ULONG completionInformation;

    UNREFERENCED_PARAMETER(target);

    deviceContext = (PDEVICE_CONTEXT)context;
    requestContext = RequestGetContext(request);
    completionInformation = (ULONG)completionParams->IoStatus.Information;

    GaYmSaturatingDecrementCounter(&deviceContext->PendingInputRequests);
    InterlockedIncrement(&deviceContext->CompletedInputRequests);
    InterlockedExchange(&deviceContext->LastCompletedStatus, completionParams->IoStatus.Status);

    GaYmApplyNativeHidReadOverride(
        deviceContext,
        requestContext,
        request,
        completionParams->IoStatus.Status,
        &completionInformation);

    GaYmApplyXInputStateOverride(
        deviceContext,
        requestContext,
        request,
        completionParams->IoStatus.Status,
        completionInformation);

    InterlockedExchange(&deviceContext->LastCompletionInformation, completionInformation);

    GaYmRecordCompletionTrace(
        deviceContext,
        requestContext,
        request,
        completionParams->IoStatus.Status,
        completionInformation,
        GAYM_TRACE_PHASE_COMPLETION);

    WdfRequestCompleteWithInformation(
        request,
        completionParams->IoStatus.Status,
        completionInformation);
}

static NTSTATUS GaYmEvtWdmIrpCompletion(
    _In_ PDEVICE_OBJECT deviceObject,
    _In_ PIRP irp,
    _In_opt_ PVOID context)
{
    PGAYM_WDM_IRP_CONTEXT irpContext;
    PDEVICE_CONTEXT deviceContext;
    PUCHAR outputBuffer;
    ULONG completionInformation;

    UNREFERENCED_PARAMETER(deviceObject);

    irpContext = (PGAYM_WDM_IRP_CONTEXT)context;
    if (irpContext == NULL || irpContext->DeviceContext == NULL) {
        return STATUS_CONTINUE_COMPLETION;
    }

    deviceContext = irpContext->DeviceContext;
    completionInformation = (ULONG)irp->IoStatus.Information;
    outputBuffer = GaYmMapIrpBuffer(irp);

    GaYmSaturatingDecrementCounter(&deviceContext->PendingInputRequests);
    InterlockedIncrement(&deviceContext->CompletedInputRequests);
    InterlockedExchange(&deviceContext->LastCompletedStatus, irp->IoStatus.Status);

    if (irpContext->RequestType == GAYM_TRACE_REQUEST_READ &&
        outputBuffer != NULL &&
        NT_SUCCESS(irp->IoStatus.Status) &&
        completionInformation != 0) {
        KIRQL oldIrql;
        GAYM_REPORT report;
        ULONG bytesWritten = 0;
        ULONG sampleLength;
        ULONG nativeReportLength;
        NTSTATUS status;

        sampleLength = GaYmMinUlong(irpContext->OutputLength, completionInformation);
        GaYmStoreNativeReadSample(
            deviceContext,
            outputBuffer,
            sampleLength,
            completionInformation,
            FALSE,
            0);

        if (deviceContext->OverrideEnabled &&
            deviceContext->HasReport &&
            deviceContext->DeviceDesc != NULL &&
            deviceContext->DeviceDesc->TranslateReport != NULL) {
            nativeReportLength = deviceContext->DeviceDesc->NativeReportSize;
            if (nativeReportLength != 0 &&
                completionInformation >= nativeReportLength &&
                irpContext->OutputLength >= nativeReportLength) {
                KeAcquireSpinLock(&deviceContext->ReportLock, &oldIrql);
                report = deviceContext->CurrentReport;
                KeReleaseSpinLock(&deviceContext->ReportLock, oldIrql);

                status = deviceContext->DeviceDesc->TranslateReport(
                    &report,
                    outputBuffer,
                    irpContext->OutputLength,
                    &bytesWritten,
                    &deviceContext->SeqCounter);
                if (NT_SUCCESS(status) && bytesWritten != 0) {
                    if (completionInformation < bytesWritten) {
                        completionInformation = bytesWritten;
                    }

                    sampleLength = GaYmMinUlong(irpContext->OutputLength, completionInformation);
                    GaYmStoreNativeReadSample(
                        deviceContext,
                        outputBuffer,
                        sampleLength,
                        completionInformation,
                        TRUE,
                        bytesWritten);
                }
            }
        }
    }

    if (irpContext->RequestType == GAYM_TRACE_REQUEST_DEVICE_CONTROL &&
        irpContext->IoControlCode == GAYM_XINPUTHID_IOCTL_GET_STATE &&
        irpContext->InputSampleLength >= GAYM_XINPUTHID_STATE_SELECTOR_LENGTH &&
        irpContext->InputSample[0] == 0x01 &&
        irpContext->InputSample[1] == 0x01 &&
        irpContext->InputSample[2] == 0x00 &&
        outputBuffer != NULL &&
        NT_SUCCESS(irp->IoStatus.Status) &&
        deviceContext->OverrideEnabled &&
        deviceContext->HasReport &&
        completionInformation >= GAYM_XINPUTHID_STATE_PACKET_LENGTH &&
        irpContext->OutputLength >= GAYM_XINPUTHID_STATE_PACKET_LENGTH) {
        KIRQL oldIrql;
        GAYM_REPORT report;
        ULONG packetNumber;

        KeAcquireSpinLock(&deviceContext->ReportLock, &oldIrql);
        report = deviceContext->CurrentReport;
        packetNumber = deviceContext->ReportsSent;
        KeReleaseSpinLock(&deviceContext->ReportLock, oldIrql);

        if (packetNumber == 0) {
            packetNumber = 1;
        }

        GaYmApplyWrappedStateReport(outputBuffer, &report, packetNumber);
    }

    InterlockedExchange(&deviceContext->LastCompletionInformation, completionInformation);
    irp->IoStatus.Information = completionInformation;

    GaYmRecordIrpCompletionTrace(
        deviceContext,
        irpContext,
        irp,
        irp->IoStatus.Status,
        completionInformation,
        GAYM_TRACE_PHASE_COMPLETION);

    ExFreePoolWithTag(irpContext, GAYM_WDM_POOL_TAG);

    if (irp->PendingReturned) {
        IoMarkIrpPending(irp);
    }

    return STATUS_CONTINUE_COMPLETION;
}

static NTSTATUS GaYmPreprocessIrp(
    _In_ WDFDEVICE device,
    _In_ PIRP irp,
    _In_ ULONG requestType)
{
    PDEVICE_CONTEXT context;
    PIO_STACK_LOCATION stack;
    PGAYM_WDM_IRP_CONTEXT irpContext;
    ULONG ioControlCode = 0;
    ULONG inputLength = 0;
    ULONG outputLength = 0;

    context = DeviceGetContext(device);
    stack = IoGetCurrentIrpStackLocation(irp);

    switch (requestType) {
    case GAYM_TRACE_REQUEST_READ:
        outputLength = stack->Parameters.Read.Length;
        InterlockedIncrement(&context->ReadRequestsSeen);
        InterlockedExchange(&context->LastReadLength, (LONG)outputLength);
        break;

    case GAYM_TRACE_REQUEST_DEVICE_CONTROL:
        ioControlCode = stack->Parameters.DeviceIoControl.IoControlCode;
        inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
        outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
        InterlockedIncrement(&context->DeviceControlRequestsSeen);
        InterlockedExchange(&context->LastInterceptedIoctl, (LONG)ioControlCode);
        InterlockedExchange(&context->LastDeviceControlInputLength, (LONG)inputLength);
        InterlockedExchange(&context->LastDeviceControlOutputLength, (LONG)outputLength);
        break;

    case GAYM_TRACE_REQUEST_INTERNAL_DEVICE_CONTROL:
        ioControlCode = stack->Parameters.DeviceIoControl.IoControlCode;
        inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
        outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
        InterlockedIncrement(&context->InternalDeviceControlRequestsSeen);
        InterlockedExchange(&context->LastInterceptedIoctl, (LONG)ioControlCode);
        InterlockedExchange(&context->LastInternalInputLength, (LONG)inputLength);
        InterlockedExchange(&context->LastInternalOutputLength, (LONG)outputLength);
        break;

    default:
        break;
    }

    irpContext = (PGAYM_WDM_IRP_CONTEXT)ExAllocatePoolZero(
        NonPagedPoolNx,
        sizeof(*irpContext),
        GAYM_WDM_POOL_TAG);
    if (irpContext == NULL) {
        irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    irpContext->DeviceContext = context;
    irpContext->RequestType = requestType;
    irpContext->IoControlCode = ioControlCode;
    irpContext->InputLength = inputLength;
    irpContext->OutputLength = outputLength;
    irpContext->InputSampleLength = GaYmCaptureIrpInputSample(
        irp,
        stack,
        inputLength,
        irpContext->InputSample);
    if (requestType == GAYM_TRACE_REQUEST_DEVICE_CONTROL &&
        ioControlCode == GAYM_XINPUTHID_IOCTL_SET_STATE &&
        irpContext->InputSampleLength != 0) {
        GaYmUpdateOutputCapture(
            context,
            irpContext->InputSample,
            irpContext->InputSampleLength,
            ioControlCode);
    }

    InterlockedIncrement(&context->QueuedInputRequests);
    InterlockedIncrement(&context->PendingInputRequests);
    InterlockedIncrement(&context->ForwardedInputRequests);
    GaYmRecordIrpDispatchTrace(context, irpContext);

    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(
        irp,
        GaYmEvtWdmIrpCompletion,
        irpContext,
        TRUE,
        TRUE,
        TRUE);

    return IoCallDriver(WdfDeviceWdmGetAttachedDevice(device), irp);
}

NTSTATUS GaYmEvtWdmPreprocessRead(
    _In_ WDFDEVICE device,
    _Inout_ PIRP irp)
{
    return GaYmPreprocessIrp(device, irp, GAYM_TRACE_REQUEST_READ);
}

NTSTATUS GaYmEvtWdmPreprocessDeviceControl(
    _In_ WDFDEVICE device,
    _Inout_ PIRP irp)
{
    return GaYmPreprocessIrp(device, irp, GAYM_TRACE_REQUEST_DEVICE_CONTROL);
}

NTSTATUS GaYmEvtWdmPreprocessInternalDeviceControl(
    _In_ WDFDEVICE device,
    _Inout_ PIRP irp)
{
    return GaYmPreprocessIrp(device, irp, GAYM_TRACE_REQUEST_INTERNAL_DEVICE_CONTROL);
}

VOID GaYmEvtCtlIoDeviceControl(
    _In_ WDFQUEUE queue,
    _In_ WDFREQUEST request,
    _In_ size_t outputBufferLength,
    _In_ size_t inputBufferLength,
    _In_ ULONG ioControlCode)
{
    PCONTROL_DEVICE_CONTEXT controlContext;
    PDEVICE_CONTEXT filterCtx;

    UNREFERENCED_PARAMETER(outputBufferLength);
    UNREFERENCED_PARAMETER(inputBufferLength);

    controlContext = ControlGetContext(WdfIoQueueGetDevice(queue));
    filterCtx = GaYmAcquireActiveControlFilterContext(controlContext);
    if (filterCtx == NULL) {
        WdfRequestComplete(request, STATUS_DEVICE_NOT_READY);
        return;
    }

    GaYmHandleSidebandIoctl(filterCtx, request, ioControlCode);
    WdfObjectDereference(filterCtx->Device);
}

NTSTATUS GaYmCreateControlDevice(
    _In_ WDFDEVICE filterDevice,
    _In_ PDEVICE_CONTEXT filterCtx)
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES lockAttributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    WDFDRIVER driver;
    PWDFDEVICE_INIT controlInit;
    WDFDEVICE controlDevice;
    PCONTROL_DEVICE_CONTEXT controlContext;
    DECLARE_CONST_UNICODE_STRING(
        sddl,
        L"D:P(A;;GA;;;SY)(A;;GRGW;;;BA)");

    UNREFERENCED_PARAMETER(filterDevice);

    if (g_ControlDevice != NULL) {
        controlContext = ControlGetContext(g_ControlDevice);
        GaYmSetControlFilterContext(controlContext, filterCtx);
        GAYM_LOG_INFO("Control device already exists; rebound active context");
        return STATUS_SUCCESS;
    }

    driver = WdfGetDriver();
    controlInit = WdfControlDeviceInitAllocate(driver, &sddl);
    if (controlInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WdfDeviceInitSetDeviceType(controlInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(controlInit, FILE_DEVICE_SECURE_OPEN, FALSE);
    WdfDeviceInitSetExclusive(controlInit, FALSE);

    status = WdfDeviceInitAssignName(controlInit, &g_CtlDeviceName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(controlInit);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, CONTROL_DEVICE_CONTEXT);
    status = WdfDeviceCreate(&controlInit, &attributes, &controlDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    controlContext = ControlGetContext(controlDevice);
    controlContext->FilterCtx = NULL;

    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
    lockAttributes.ParentObject = controlDevice;

    status = WdfWaitLockCreate(&lockAttributes, &controlContext->RouteLock);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(controlDevice);
        return status;
    }

    GaYmSetControlFilterContext(controlContext, filterCtx);

    status = WdfDeviceCreateSymbolicLink(controlDevice, &g_CtlSymLink);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(controlDevice);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = GaYmEvtCtlIoDeviceControl;

    status = WdfIoQueueCreate(controlDevice, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(controlDevice);
        return status;
    }

    WdfControlFinishInitializing(controlDevice);
    g_ControlDevice = controlDevice;
    GAYM_LOG_INFO("Created control device \\\\.\\GaYmXInputFilterCtl");
    return STATUS_SUCCESS;
}

VOID GaYmDeleteControlDevice(VOID)
{
    if (g_ControlDevice != NULL) {
        GaYmSetControlFilterContext(ControlGetContext(g_ControlDevice), NULL);
        WdfObjectDelete(g_ControlDevice);
        g_ControlDevice = NULL;
        GAYM_LOG_INFO("Deleted control device");
    }
}

NTSTATUS GaYmEvtDeviceAdd(
    _In_ WDFDRIVER driver,
    _Inout_ PWDFDEVICE_INIT deviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES requestAttributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    PDEVICE_CONTEXT context;

    UNREFERENCED_PARAMETER(driver);

    GAYM_LOG_INFO("DeviceAdd: attaching GaYm upper filter");

    WdfFdoInitSetFilter(deviceInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&requestAttributes, REQUEST_CONTEXT);
    WdfDeviceInitSetRequestAttributes(deviceInit, &requestAttributes);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = GaYmEvtPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = GaYmEvtReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry = GaYmEvtD0Entry;
    pnpCallbacks.EvtDeviceD0Exit = GaYmEvtD0Exit;
    pnpCallbacks.EvtDeviceSurpriseRemoval = GaYmEvtSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpCallbacks);

    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
        deviceInit,
        GaYmEvtWdmPreprocessDeviceControl,
        IRP_MJ_DEVICE_CONTROL,
        NULL,
        0);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("Assign preprocess DEVICE_CONTROL failed: 0x%08X", status);
        return status;
    }

    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
        deviceInit,
        GaYmEvtWdmPreprocessInternalDeviceControl,
        IRP_MJ_INTERNAL_DEVICE_CONTROL,
        NULL,
        0);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("Assign preprocess INTERNAL_DEVICE_CONTROL failed: 0x%08X", status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("WdfDeviceCreate failed: 0x%08X", status);
        return status;
    }

    context = DeviceGetContext(device);
    RtlZeroMemory(context, sizeof(*context));
    context->Device = device;
    context->IoTarget = WdfDeviceGetIoTarget(device);
    KeInitializeSpinLock(&context->ReportLock);
    KeInitializeSpinLock(&context->TraceLock);
    QueryDeviceIds(device, &context->VendorId, &context->ProductId, &context->IsUsbStack);
    context->DeviceDesc = GaYmLookupDevice(context->VendorId, context->ProductId);
    context->DeviceType = context->DeviceDesc != NULL ? context->DeviceDesc->DeviceType : GAYM_DEVICE_UNKNOWN;
    GaYmResetRuntimeState(context);

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoRead = GaYmEvtIoRead;
    queueConfig.EvtIoWrite = GaYmEvtIoWrite;
    queueConfig.EvtIoDeviceControl = GaYmEvtIoDeviceControl;
    queueConfig.EvtIoInternalDeviceControl = GaYmEvtIoInternalDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &context->DefaultQueue);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("WdfIoQueueCreate failed: 0x%08X", status);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &context->PendingReadsQueue);
    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("PendingReads queue create failed: 0x%08X", status);
        return status;
    }

    if (GaYmIsControlTarget(context)) {
        status = WdfDeviceCreateDeviceInterface(
            device,
            &GUID_DEVINTERFACE_GAYM_XINPUT_FILTER,
            NULL);
        if (!NT_SUCCESS(status)) {
            GAYM_LOG_WARN("Upper interface creation failed: 0x%08X", status);
        }

        status = GaYmCreateControlDevice(device, context);
        if (!NT_SUCCESS(status)) {
            GAYM_LOG_WARN("Control device creation failed: 0x%08X", status);
        }
    }

    GAYM_LOG_INFO(
        "Attached to device VID:%04X PID:%04X type=%s usb=%u control=%u",
        context->VendorId,
        context->ProductId,
        GaYmDeviceTypeName(context->DeviceType),
        context->IsUsbStack,
        GaYmIsControlTarget(context));
    return STATUS_SUCCESS;
}

VOID GaYmEvtIoRead(
    _In_ WDFQUEUE queue,
    _In_ WDFREQUEST request,
    _In_ size_t length)
{
    PDEVICE_CONTEXT context;
    PREQUEST_CONTEXT requestContext;

    context = DeviceGetContext(WdfIoQueueGetDevice(queue));
    requestContext = RequestGetContext(request);

    InterlockedIncrement(&context->ReadRequestsSeen);
    InterlockedIncrement(&context->QueuedInputRequests);
    InterlockedExchange(&context->LastReadLength, (LONG)length);
    GaYmInitializeRequestContext(requestContext, GAYM_TRACE_REQUEST_READ, 0, 0, (ULONG)length);

    if (context->OverrideEnabled) {
        BOOLEAN hasReport;
        KIRQL oldIrql;

        KeAcquireSpinLock(&context->ReportLock, &oldIrql);
        hasReport = context->HasReport;
        KeReleaseSpinLock(&context->ReportLock, oldIrql);

        if (hasReport) {
            GaYmRecordDispatchTrace(context, requestContext, request);
            InterlockedIncrement(&context->PendingInputRequests);
            GaYmCompleteReadWithReport(context, request);
        } else {
            GaYmQueuePendingRead(context, request);
        }
        return;
    }

    GaYmForwardRequest(context, request);
}

VOID GaYmEvtIoWrite(
    _In_ WDFQUEUE queue,
    _In_ WDFREQUEST request,
    _In_ size_t length)
{
    PDEVICE_CONTEXT context;
    PREQUEST_CONTEXT requestContext;
    UCHAR writeSample[GAYM_TRACE_SAMPLE_BYTES];
    ULONG writeSampleLength;

    context = DeviceGetContext(WdfIoQueueGetDevice(queue));
    requestContext = RequestGetContext(request);
    writeSampleLength = GaYmTryCopyBufferSample(request, FALSE, (ULONG)length, writeSample);

    InterlockedIncrement(&context->WriteRequestsSeen);
    InterlockedIncrement(&context->QueuedInputRequests);
    InterlockedExchange(&context->LastWriteLength, (LONG)length);
    GaYmUpdateLastWriteSample(context, writeSampleLength != 0 ? writeSample : NULL, writeSampleLength);
    GaYmInitializeRequestContext(requestContext, GAYM_TRACE_REQUEST_WRITE, 0, (ULONG)length, 0);
    GaYmForwardRequest(context, request);
}

VOID GaYmEvtIoDeviceControl(
    _In_ WDFQUEUE queue,
    _In_ WDFREQUEST request,
    _In_ size_t outputBufferLength,
    _In_ size_t inputBufferLength,
    _In_ ULONG ioControlCode)
{
    PDEVICE_CONTEXT context;
    PREQUEST_CONTEXT requestContext;

    context = DeviceGetContext(WdfIoQueueGetDevice(queue));
    requestContext = RequestGetContext(request);

    InterlockedIncrement(&context->DeviceControlRequestsSeen);
    InterlockedIncrement(&context->QueuedInputRequests);
    InterlockedExchange(&context->LastInterceptedIoctl, (LONG)ioControlCode);
    InterlockedExchange(&context->LastDeviceControlInputLength, (LONG)inputBufferLength);
    InterlockedExchange(&context->LastDeviceControlOutputLength, (LONG)outputBufferLength);
    GaYmInitializeRequestContext(
        requestContext,
        GAYM_TRACE_REQUEST_DEVICE_CONTROL,
        ioControlCode,
        (ULONG)inputBufferLength,
        (ULONG)outputBufferLength);
    GaYmCaptureRequestInputSample(request, requestContext);
    if (ioControlCode == GAYM_XINPUTHID_IOCTL_SET_STATE &&
        requestContext->InputSampleLength != 0) {
        GaYmUpdateOutputCapture(
            context,
            requestContext->InputSample,
            requestContext->InputSampleLength,
            ioControlCode);
    }

    if (GaYmIsSidebandIoctl(ioControlCode)) {
        GaYmHandleSidebandIoctl(context, request, ioControlCode);
        return;
    }

    GaYmForwardRequest(context, request);
}

VOID GaYmEvtIoInternalDeviceControl(
    _In_ WDFQUEUE queue,
    _In_ WDFREQUEST request,
    _In_ size_t outputBufferLength,
    _In_ size_t inputBufferLength,
    _In_ ULONG ioControlCode)
{
    PDEVICE_CONTEXT context;
    PREQUEST_CONTEXT requestContext;

    context = DeviceGetContext(WdfIoQueueGetDevice(queue));
    requestContext = RequestGetContext(request);

    InterlockedIncrement(&context->InternalDeviceControlRequestsSeen);
    InterlockedIncrement(&context->QueuedInputRequests);
    InterlockedExchange(&context->LastInterceptedIoctl, (LONG)ioControlCode);
    InterlockedExchange(&context->LastInternalInputLength, (LONG)inputBufferLength);
    InterlockedExchange(&context->LastInternalOutputLength, (LONG)outputBufferLength);
    GaYmInitializeRequestContext(
        requestContext,
        GAYM_TRACE_REQUEST_INTERNAL_DEVICE_CONTROL,
        ioControlCode,
        (ULONG)inputBufferLength,
        (ULONG)outputBufferLength);
    GaYmCaptureRequestInputSample(request, requestContext);
    GaYmForwardRequest(context, request);
}

NTSTATUS GaYmEvtPrepareHardware(
    _In_ WDFDEVICE device,
    _In_ WDFCMRESLIST resourcesRaw,
    _In_ WDFCMRESLIST resourcesTranslated)
{
    PDEVICE_CONTEXT context;

    UNREFERENCED_PARAMETER(resourcesRaw);
    UNREFERENCED_PARAMETER(resourcesTranslated);

    context = DeviceGetContext(device);
    GAYM_LOG_INFO(
        "PrepareHardware: VID:%04X PID:%04X type=%s",
        context->VendorId,
        context->ProductId,
        GaYmDeviceTypeName(context->DeviceType));
    return STATUS_SUCCESS;
}

NTSTATUS GaYmEvtReleaseHardware(
    _In_ WDFDEVICE device,
    _In_ WDFCMRESLIST resourcesTranslated)
{
    PDEVICE_CONTEXT context;
    PCONTROL_DEVICE_CONTEXT controlContext;

    UNREFERENCED_PARAMETER(resourcesTranslated);

    context = DeviceGetContext(device);
    context->IsInD0 = FALSE;
    context->OverrideEnabled = FALSE;
    context->HasReport = FALSE;
    GaYmDrainPendingReads(context, STATUS_POWER_STATE_INVALID);

    if (g_ControlDevice != NULL && GaYmIsControlTarget(context)) {
        controlContext = ControlGetContext(g_ControlDevice);
        GaYmClearControlFilterContextIfMatch(controlContext, context);
    }

    GAYM_LOG_INFO("ReleaseHardware: VID:%04X PID:%04X", context->VendorId, context->ProductId);
    return STATUS_SUCCESS;
}

NTSTATUS GaYmEvtD0Entry(
    _In_ WDFDEVICE device,
    _In_ WDF_POWER_DEVICE_STATE previousState)
{
    PDEVICE_CONTEXT context;
    PCONTROL_DEVICE_CONTEXT controlContext;

    context = DeviceGetContext(device);
    context->IsInD0 = TRUE;

    if (g_ControlDevice != NULL && GaYmIsControlTarget(context)) {
        controlContext = ControlGetContext(g_ControlDevice);
        GaYmSetControlFilterContext(controlContext, context);
    }

    GAYM_LOG_INFO(
        "D0Entry: VID:%04X PID:%04X previousState=%u",
        context->VendorId,
        context->ProductId,
        (ULONG)previousState);
    return STATUS_SUCCESS;
}

NTSTATUS GaYmEvtD0Exit(
    _In_ WDFDEVICE device,
    _In_ WDF_POWER_DEVICE_STATE targetState)
{
    PDEVICE_CONTEXT context;
    PCONTROL_DEVICE_CONTEXT controlContext;

    context = DeviceGetContext(device);
    context->IsInD0 = FALSE;
    context->OverrideEnabled = FALSE;
    context->HasReport = FALSE;
    GaYmDrainPendingReads(context, STATUS_POWER_STATE_INVALID);

    if (g_ControlDevice != NULL && GaYmIsControlTarget(context)) {
        controlContext = ControlGetContext(g_ControlDevice);
        GaYmClearControlFilterContextIfMatch(controlContext, context);
    }

    GAYM_LOG_INFO(
        "D0Exit: VID:%04X PID:%04X targetState=%u",
        context->VendorId,
        context->ProductId,
        (ULONG)targetState);
    return STATUS_SUCCESS;
}

VOID GaYmEvtSurpriseRemoval(_In_ WDFDEVICE device)
{
    PDEVICE_CONTEXT context;
    PCONTROL_DEVICE_CONTEXT controlContext;

    context = DeviceGetContext(device);
    context->IsInD0 = FALSE;
    context->OverrideEnabled = FALSE;
    context->HasReport = FALSE;
    GaYmDrainPendingReads(context, STATUS_POWER_STATE_INVALID);
    WdfIoQueuePurgeSynchronously(context->PendingReadsQueue);

    if (g_ControlDevice != NULL && GaYmIsControlTarget(context)) {
        controlContext = ControlGetContext(g_ControlDevice);
        GaYmClearControlFilterContextIfMatch(controlContext, context);
    }

    GAYM_LOG_WARN("SurpriseRemoval: VID:%04X PID:%04X", context->VendorId, context->ProductId);
}
