#include "../include/upper_device.h"
#include "../include/upper_trace.h"

#include <hidport.h>

static EVT_WDF_REQUEST_COMPLETION_ROUTINE UpperEvtInputRequestCompletion;

#define UPPER_OBSERVATION_TAG 'ObUG'

#define UPPER_XINPUT_GAMEPAD_DPAD_UP        0x0001
#define UPPER_XINPUT_GAMEPAD_DPAD_DOWN      0x0002
#define UPPER_XINPUT_GAMEPAD_DPAD_LEFT      0x0004
#define UPPER_XINPUT_GAMEPAD_DPAD_RIGHT     0x0008
#define UPPER_XINPUT_GAMEPAD_START          0x0010
#define UPPER_XINPUT_GAMEPAD_BACK           0x0020
#define UPPER_XINPUT_GAMEPAD_LEFT_THUMB     0x0040
#define UPPER_XINPUT_GAMEPAD_RIGHT_THUMB    0x0080
#define UPPER_XINPUT_GAMEPAD_LEFT_SHOULDER  0x0100
#define UPPER_XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define UPPER_XINPUT_GAMEPAD_A              0x1000
#define UPPER_XINPUT_GAMEPAD_B              0x2000
#define UPPER_XINPUT_GAMEPAD_X              0x4000
#define UPPER_XINPUT_GAMEPAD_Y              0x8000

typedef struct _UPPER_XINPUT_GAMEPAD {
    USHORT Buttons;
    UCHAR LeftTrigger;
    UCHAR RightTrigger;
    SHORT LeftThumbX;
    SHORT LeftThumbY;
    SHORT RightThumbX;
    SHORT RightThumbY;
} UPPER_XINPUT_GAMEPAD, *PUPPER_XINPUT_GAMEPAD;

typedef struct _UPPER_XINPUT_STATE {
    ULONG PacketNumber;
    UPPER_XINPUT_GAMEPAD Gamepad;
} UPPER_XINPUT_STATE, *PUPPER_XINPUT_STATE;

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

static BOOLEAN UpperRequestIsExactXInputStateRequest(
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode)
{
    WDF_REQUEST_PARAMETERS params;

    if (UpperShouldInterceptReadIoctl(IoControlCode)) {
        return FALSE;
    }

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    if (params.Type != WdfRequestTypeDeviceControl &&
        params.Type != WdfRequestTypeDeviceControlInternal) {
        return FALSE;
    }

    // XInput state queries are output-only device-control requests with an exact
    // XINPUT_STATE-sized payload. Native HID traffic is excluded above.
    return params.Parameters.DeviceIoControl.InputBufferLength == 0 &&
        params.Parameters.DeviceIoControl.OutputBufferLength == sizeof(UPPER_XINPUT_STATE);
}

static BOOLEAN UpperRequestIsAppFacingXInputRequest(
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode)
{
    return UpperRequestIsExactXInputStateRequest(Request, IoControlCode);
}

static NTSTATUS UpperResolveOutputBufferFromIrp(
    _In_ WDFREQUEST Request,
    _In_ size_t MinimumLength,
    _In_ size_t OutputLength,
    _Outptr_result_bytebuffer_(*BufferSize) PVOID* Buffer,
    _Out_ size_t* BufferSize)
{
    PIRP irp;

    *Buffer = NULL;
    *BufferSize = 0;

    irp = WdfRequestWdmGetIrp(Request);
    if (irp == NULL || irp->UserBuffer == NULL || OutputLength < MinimumLength) {
        return STATUS_INVALID_USER_BUFFER;
    }

    *Buffer = irp->UserBuffer;
    *BufferSize = OutputLength;
    return STATUS_SUCCESS;
}

static NTSTATUS UpperResolveXInputBuffer(
    _In_ WDFREQUEST Request,
    _Outptr_result_bytebuffer_(*BufferSize) PVOID* Buffer,
    _Out_ size_t* BufferSize)
{
    WDF_REQUEST_PARAMETERS params;
    NTSTATUS status;

    *Buffer = NULL;
    *BufferSize = 0;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    if (params.Type == WdfRequestTypeRead) {
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(UPPER_XINPUT_STATE), Buffer, BufferSize);
        if (NT_SUCCESS(status)) {
            return status;
        }

        return UpperResolveOutputBufferFromIrp(
            Request,
            sizeof(UPPER_XINPUT_STATE),
            params.Parameters.Read.Length,
            Buffer,
            BufferSize);
    }

    if (params.Type != WdfRequestTypeDeviceControl &&
        params.Type != WdfRequestTypeDeviceControlInternal) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(UPPER_XINPUT_STATE), Buffer, BufferSize);
    if (NT_SUCCESS(status)) {
        return status;
    }

    return UpperResolveOutputBufferFromIrp(
        Request,
        sizeof(UPPER_XINPUT_STATE),
        params.Parameters.DeviceIoControl.OutputBufferLength,
        Buffer,
        BufferSize);
}

static NTSTATUS UpperResolveNativeReadBuffer(
    _In_ WDFREQUEST Request,
    _Outptr_result_bytebuffer_(*BufferSize) PVOID* Buffer,
    _Out_ size_t* BufferSize)
{
    WDF_REQUEST_PARAMETERS params;
    NTSTATUS status;
    PIRP irp;
    PHID_XFER_PACKET packet;

    *Buffer = NULL;
    *BufferSize = 0;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    if (params.Type == WdfRequestTypeRead) {
        status = WdfRequestRetrieveOutputBuffer(Request, 1, Buffer, BufferSize);
        if (NT_SUCCESS(status)) {
            return status;
        }

        return UpperResolveOutputBufferFromIrp(
            Request,
            1,
            params.Parameters.Read.Length,
            Buffer,
            BufferSize);
    }

    if (params.Type != WdfRequestTypeDeviceControl &&
        params.Type != WdfRequestTypeDeviceControlInternal) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (params.Parameters.DeviceIoControl.IoControlCode == IOCTL_HID_READ_REPORT) {
        irp = WdfRequestWdmGetIrp(Request);
        if (irp == NULL || irp->UserBuffer == NULL || params.Parameters.DeviceIoControl.OutputBufferLength == 0) {
            return STATUS_INVALID_USER_BUFFER;
        }

        *Buffer = irp->UserBuffer;
        *BufferSize = params.Parameters.DeviceIoControl.OutputBufferLength;
        return STATUS_SUCCESS;
    }

    if (params.Parameters.DeviceIoControl.IoControlCode != IOCTL_HID_GET_INPUT_REPORT) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    irp = WdfRequestWdmGetIrp(Request);
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

static VOID UpperQueuePendingReadRequest(
    _In_ PUPPER_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request)
{
    NTSTATUS status;

    if (Context == NULL || Context->PendingReadQueue == NULL) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
        return;
    }

    InterlockedIncrement(&Context->QueuedInputRequests);
    InterlockedIncrement(&Context->PendingInputRequests);

    status = WdfRequestForwardToIoQueue(Request, Context->PendingReadQueue);
    if (!NT_SUCCESS(status)) {
        InterlockedDecrement(&Context->PendingInputRequests);
        UpperTraceRecord(UPPER_TRACE_EVENT_BUFFER_RESOLVE_FAILED, status);
        WdfRequestComplete(Request, status);
    }
}

static USHORT UpperBuildXInputButtons(_In_ const GAYM_REPORT* Report)
{
    USHORT buttons;

    buttons = 0;

    if (Report->Buttons[0] & GAYM_BTN_A) {
        buttons |= UPPER_XINPUT_GAMEPAD_A;
    }
    if (Report->Buttons[0] & GAYM_BTN_B) {
        buttons |= UPPER_XINPUT_GAMEPAD_B;
    }
    if (Report->Buttons[0] & GAYM_BTN_X) {
        buttons |= UPPER_XINPUT_GAMEPAD_X;
    }
    if (Report->Buttons[0] & GAYM_BTN_Y) {
        buttons |= UPPER_XINPUT_GAMEPAD_Y;
    }
    if (Report->Buttons[0] & GAYM_BTN_LB) {
        buttons |= UPPER_XINPUT_GAMEPAD_LEFT_SHOULDER;
    }
    if (Report->Buttons[0] & GAYM_BTN_RB) {
        buttons |= UPPER_XINPUT_GAMEPAD_RIGHT_SHOULDER;
    }
    if (Report->Buttons[0] & GAYM_BTN_BACK) {
        buttons |= UPPER_XINPUT_GAMEPAD_BACK;
    }
    if (Report->Buttons[0] & GAYM_BTN_START) {
        buttons |= UPPER_XINPUT_GAMEPAD_START;
    }
    if (Report->Buttons[1] & GAYM_BTN_LSTICK) {
        buttons |= UPPER_XINPUT_GAMEPAD_LEFT_THUMB;
    }
    if (Report->Buttons[1] & GAYM_BTN_RSTICK) {
        buttons |= UPPER_XINPUT_GAMEPAD_RIGHT_THUMB;
    }

    switch (Report->DPad) {
    case GAYM_DPAD_UP:
        buttons |= UPPER_XINPUT_GAMEPAD_DPAD_UP;
        break;
    case GAYM_DPAD_UPRIGHT:
        buttons |= UPPER_XINPUT_GAMEPAD_DPAD_UP | UPPER_XINPUT_GAMEPAD_DPAD_RIGHT;
        break;
    case GAYM_DPAD_RIGHT:
        buttons |= UPPER_XINPUT_GAMEPAD_DPAD_RIGHT;
        break;
    case GAYM_DPAD_DOWNRIGHT:
        buttons |= UPPER_XINPUT_GAMEPAD_DPAD_DOWN | UPPER_XINPUT_GAMEPAD_DPAD_RIGHT;
        break;
    case GAYM_DPAD_DOWN:
        buttons |= UPPER_XINPUT_GAMEPAD_DPAD_DOWN;
        break;
    case GAYM_DPAD_DOWNLEFT:
        buttons |= UPPER_XINPUT_GAMEPAD_DPAD_DOWN | UPPER_XINPUT_GAMEPAD_DPAD_LEFT;
        break;
    case GAYM_DPAD_LEFT:
        buttons |= UPPER_XINPUT_GAMEPAD_DPAD_LEFT;
        break;
    case GAYM_DPAD_UPLEFT:
        buttons |= UPPER_XINPUT_GAMEPAD_DPAD_UP | UPPER_XINPUT_GAMEPAD_DPAD_LEFT;
        break;
    default:
        break;
    }

    return buttons;
}

static VOID UpperBuildXInputState(
    _In_ const GAYM_REPORT* Report,
    _In_ ULONG PacketNumber,
    _Out_ PUPPER_XINPUT_STATE State)
{
    RtlZeroMemory(State, sizeof(*State));
    State->PacketNumber = PacketNumber;
    State->Gamepad.Buttons = UpperBuildXInputButtons(Report);
    State->Gamepad.LeftTrigger = Report->TriggerLeft;
    State->Gamepad.RightTrigger = Report->TriggerRight;
    State->Gamepad.LeftThumbX = Report->ThumbLeftX;
    State->Gamepad.LeftThumbY = Report->ThumbLeftY;
    State->Gamepad.RightThumbX = Report->ThumbRightX;
    State->Gamepad.RightThumbY = Report->ThumbRightY;
}

static NTSTATUS UpperSnapshotXInputState(
    _In_ PUPPER_DEVICE_CONTEXT Context,
    _Out_ PUPPER_XINPUT_STATE State)
{
    GAYM_REPORT reportSnapshot;
    KIRQL oldIrql;
    SIZE_T compared;

    RtlZeroMemory(State, sizeof(*State));
    RtlZeroMemory(&reportSnapshot, sizeof(reportSnapshot));

    KeAcquireSpinLock(&Context->StateLock, &oldIrql);
    if (!Context->HasInjectedReport) {
        KeReleaseSpinLock(&Context->StateLock, oldIrql);
        return STATUS_INVALID_DEVICE_STATE;
    }

    reportSnapshot = Context->LastInjectedReport;
    compared = RtlCompareMemory(
        &Context->LastPresentedXInputReport,
        &reportSnapshot,
        sizeof(reportSnapshot));
    if (!Context->HasPresentedXInputReport || compared != sizeof(reportSnapshot)) {
        Context->XInputPacketNumber += 1;
        Context->LastPresentedXInputReport = reportSnapshot;
        Context->HasPresentedXInputReport = TRUE;
    }

    UpperBuildXInputState(&reportSnapshot, Context->XInputPacketNumber, State);
    KeReleaseSpinLock(&Context->StateLock, oldIrql);
    return STATUS_SUCCESS;
}

static VOID UpperCompleteRequestWithXInputState(
    _In_ PUPPER_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode)
{
    UPPER_XINPUT_STATE state;
    PVOID buffer;
    size_t bufferSize;
    ULONG eventCode;
    NTSTATUS status;

    buffer = NULL;
    bufferSize = 0;
    RtlZeroMemory(&state, sizeof(state));

    status = UpperResolveXInputBuffer(Request, &buffer, &bufferSize);
    if (!NT_SUCCESS(status)) {
        UpperTraceRecord(UPPER_TRACE_EVENT_BUFFER_RESOLVE_FAILED, status);
        WdfRequestComplete(Request, status);
        return;
    }

    status = UpperSnapshotXInputState(Context, &state);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    RtlZeroMemory(buffer, bufferSize);
    RtlCopyMemory(buffer, &state, sizeof(state));

    InterlockedIncrement(&Context->CompletedInputRequests);

    eventCode = (IoControlCode == IRP_MJ_READ) ?
        UPPER_TRACE_EVENT_OVERRIDE_XINPUT_READ :
        UPPER_TRACE_EVENT_OVERRIDE_XINPUT_IOCTL;
    UpperTraceRecord(eventCode, STATUS_SUCCESS);
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(state));
}

VOID UpperDeviceCompletePendingReads(_In_ PUPPER_DEVICE_CONTEXT Context)
{
    WDFREQUEST request;
    WDF_REQUEST_PARAMETERS params;
    ULONG ioControlCode;

    if (Context == NULL || Context->PendingReadQueue == NULL) {
        return;
    }

    request = NULL;
    while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->PendingReadQueue, &request))) {
        if (InterlockedCompareExchange(&Context->PendingInputRequests, 0, 0) > 0) {
            InterlockedDecrement(&Context->PendingInputRequests);
        }

        WDF_REQUEST_PARAMETERS_INIT(&params);
        WdfRequestGetParameters(request, &params);

        ioControlCode = 0;
        if (params.Type == WdfRequestTypeDeviceControl ||
            params.Type == WdfRequestTypeDeviceControlInternal) {
            ioControlCode = params.Parameters.DeviceIoControl.IoControlCode;
        }

        InterlockedIncrement(&Context->CompletedInputRequests);
        UpperCompleteRequestWithXInputState(
            Context,
            request,
            (params.Type == WdfRequestTypeRead) ? IRP_MJ_READ : ioControlCode);
    }
}

VOID UpperDevicePurgePendingReads(_In_ PUPPER_DEVICE_CONTEXT Context)
{
    if (Context == NULL || Context->PendingReadQueue == NULL) {
        return;
    }

    InterlockedExchange(&Context->PendingInputRequests, 0);
    WdfIoQueuePurgeSynchronously(Context->PendingReadQueue);
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
    UpperTraceRecord(UPPER_TRACE_EVENT_OBSERVATION_CAPTURED, STATUS_SUCCESS);
}

static NTSTATUS UpperRefreshObservedReportFromLower(_In_ PUPPER_DEVICE_CONTEXT Context)
{
    WDF_MEMORY_DESCRIPTOR outputDescriptor;
    WDF_MEMORY_DESCRIPTOR inputDescriptor;
    HID_XFER_PACKET packet;
    PUCHAR buffer;
    ULONG bufferLength;
    ULONG_PTR bytesRead;
    BOOLEAN isAttached;
    BOOLEAN isInD0;
    USHORT vendorId;
    USHORT productId;
    KIRQL oldIrql;
    NTSTATUS status;

    if (Context == NULL || Context->LowerTarget == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    KeAcquireSpinLock(&Context->StateLock, &oldIrql);
    isAttached = Context->IsAttached;
    isInD0 = Context->IsInD0;
    vendorId = Context->VendorId;
    productId = Context->ProductId;
    KeReleaseSpinLock(&Context->StateLock, oldIrql);

    if (!isAttached || !isInD0 || vendorId != 0x045E || productId != 0x02FF) {
        return STATUS_DEVICE_NOT_READY;
    }

    bufferLength = 16;
    buffer = (PUCHAR)ExAllocatePoolZero(NonPagedPoolNx, bufferLength, UPPER_OBSERVATION_TAG);
    if (buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outputDescriptor, buffer, bufferLength);
    bytesRead = 0;
    status = WdfIoTargetSendReadSynchronously(
        Context->LowerTarget,
        NULL,
        &outputDescriptor,
        NULL,
        NULL,
        &bytesRead);
    if (NT_SUCCESS(status) && bytesRead != 0) {
        UpperRecordObservedReport(Context, buffer, (ULONG)min((ULONG_PTR)bufferLength, bytesRead));
        ExFreePoolWithTag(buffer, UPPER_OBSERVATION_TAG);
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&packet, sizeof(packet));
    packet.reportBuffer = buffer;
    packet.reportBufferLen = bufferLength;
    packet.reportId = 0;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputDescriptor, &packet, sizeof(packet));
    bytesRead = 0;
    status = WdfIoTargetSendInternalIoctlSynchronously(
        Context->LowerTarget,
        NULL,
        IOCTL_HID_GET_INPUT_REPORT,
        &inputDescriptor,
        NULL,
        NULL,
        &bytesRead);
    if (NT_SUCCESS(status) && packet.reportBufferLen != 0) {
        UpperRecordObservedReport(
            Context,
            buffer,
            (ULONG)min((ULONG)packet.reportBufferLen, bufferLength));
        ExFreePoolWithTag(buffer, UPPER_OBSERVATION_TAG);
        return STATUS_SUCCESS;
    }

    ExFreePoolWithTag(buffer, UPPER_OBSERVATION_TAG);
    return status;
}

NTSTATUS UpperDeviceEnsureObservedReport(_In_ PUPPER_DEVICE_CONTEXT Context)
{
    BOOLEAN hasObservedReport;
    KIRQL oldIrql;

    if (Context == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&Context->StateLock, &oldIrql);
    hasObservedReport = Context->HasObservedReport;
    KeReleaseSpinLock(&Context->StateLock, oldIrql);

    if (hasObservedReport) {
        return STATUS_SUCCESS;
    }

    return UpperRefreshObservedReportFromLower(Context);
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
    WDF_REQUEST_PARAMETERS requestParams;
    ULONG ioControlCode;
    BOOLEAN overrideEnabled;
    BOOLEAN hasInjectedReport;
    BOOLEAN isNativeHidIoctl;
    PVOID buffer;
    size_t bufferSize;
    KIRQL oldIrql;

    UNREFERENCED_PARAMETER(Target);

    upperContext = (PUPPER_DEVICE_CONTEXT)Context;
    buffer = NULL;
    bufferSize = 0;

    WDF_REQUEST_PARAMETERS_INIT(&requestParams);
    WdfRequestGetParameters(Request, &requestParams);

    ioControlCode = 0;
    if (requestParams.Type == WdfRequestTypeDeviceControl ||
        requestParams.Type == WdfRequestTypeDeviceControlInternal) {
        ioControlCode = requestParams.Parameters.DeviceIoControl.IoControlCode;
    }

    isNativeHidIoctl =
        (requestParams.Type == WdfRequestTypeDeviceControl ||
         requestParams.Type == WdfRequestTypeDeviceControlInternal) &&
        UpperShouldInterceptReadIoctl(ioControlCode);

    KeAcquireSpinLock(&upperContext->StateLock, &oldIrql);
    overrideEnabled = upperContext->OverrideEnabled;
    hasInjectedReport = upperContext->HasInjectedReport;
    KeReleaseSpinLock(&upperContext->StateLock, oldIrql);

    if (overrideEnabled && hasInjectedReport) {
        if (UpperRequestIsAppFacingXInputRequest(Request, ioControlCode)) {
            UpperCompleteRequestWithXInputState(
                upperContext,
                Request,
                ioControlCode);
            return;
        }
    }

    if (NT_SUCCESS(Params->IoStatus.Status) && isNativeHidIoctl &&
        NT_SUCCESS(UpperResolveNativeReadBuffer(Request, &buffer, &bufferSize))) {
        UpperRecordObservedReport(
            upperContext,
            (const UCHAR*)buffer,
            (ULONG)min(bufferSize, Params->IoStatus.Information));
    }

    UpperTraceRecord(UPPER_TRACE_EVENT_FORWARD_COMPLETION, Params->IoStatus.Status);
    WdfRequestCompleteWithInformation(Request, Params->IoStatus.Status, Params->IoStatus.Information);
}

static VOID UpperHandleReadRequest(
    _In_ PUPPER_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode)
{
    BOOLEAN overrideEnabled;
    BOOLEAN hasInjectedReport;
    WDF_REQUEST_PARAMETERS requestParams;
    KIRQL oldIrql;
    NTSTATUS status;

    WDF_REQUEST_PARAMETERS_INIT(&requestParams);
    WdfRequestGetParameters(Request, &requestParams);

    KeAcquireSpinLock(&Context->StateLock, &oldIrql);
    overrideEnabled = Context->OverrideEnabled;
    hasInjectedReport = Context->HasInjectedReport;
    Context->LastInterceptedIoctl = (LONG)IoControlCode;
    Context->LastRequestType = (LONG)requestParams.Type;
    if (requestParams.Type == WdfRequestTypeRead) {
        Context->LastRequestInputLength = (LONG)requestParams.Parameters.Read.Length;
        Context->LastRequestOutputLength = (LONG)requestParams.Parameters.Read.Length;
    } else if (requestParams.Type == WdfRequestTypeDeviceControl ||
        requestParams.Type == WdfRequestTypeDeviceControlInternal) {
        Context->LastRequestInputLength = (LONG)requestParams.Parameters.DeviceIoControl.InputBufferLength;
        Context->LastRequestOutputLength = (LONG)requestParams.Parameters.DeviceIoControl.OutputBufferLength;
    } else {
        Context->LastRequestInputLength = 0;
        Context->LastRequestOutputLength = 0;
    }
    KeReleaseSpinLock(&Context->StateLock, oldIrql);

    if (UpperRequestIsExactXInputStateRequest(Request, IoControlCode)) {
        if (overrideEnabled) {
            if (hasInjectedReport) {
                UpperCompleteRequestWithXInputState(Context, Request, IoControlCode);
            } else {
                UpperTraceRecord(UPPER_TRACE_EVENT_OVERRIDE_XINPUT_IOCTL, STATUS_PENDING);
                UpperQueuePendingReadRequest(Context, Request);
            }
        } else {
            status = UpperForwardRequestToLower(Context, Request);
            UpperTraceRecord(UPPER_TRACE_EVENT_FORWARD_READ_REQUEST, status);
            if (NT_SUCCESS(status)) {
                InterlockedIncrement(&Context->ForwardedInputRequests);
            }
            if (!NT_SUCCESS(status)) {
                WdfRequestComplete(Request, status);
            }
        }
        return;
    }

    if (IoControlCode == IRP_MJ_READ || UpperShouldInterceptReadIoctl(IoControlCode)) {
        status = UpperForwardRequestToLower(Context, Request);
        UpperTraceRecord(UPPER_TRACE_EVENT_FORWARD_READ_REQUEST, status);
        if (NT_SUCCESS(status)) {
            InterlockedIncrement(&Context->ForwardedInputRequests);
        }
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    status = UpperForwardRequestToLower(Context, Request);
    UpperTraceRecord(UPPER_TRACE_EVENT_FORWARD_READ_REQUEST, status);
    if (NT_SUCCESS(status)) {
        InterlockedIncrement(&Context->ForwardedInputRequests);
    }
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
    if (UpperShouldInterceptReadIoctl(IoControlCode) ||
        UpperRequestIsExactXInputStateRequest(Request, IoControlCode)) {
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
    if (UpperShouldInterceptReadIoctl(IoControlCode) ||
        UpperRequestIsExactXInputStateRequest(Request, IoControlCode)) {
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
