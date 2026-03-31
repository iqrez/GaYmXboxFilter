#include "../include/upper_device.h"
#include "../include/upper_trace.h"

static NTSTATUS UpperCopyOutputBuffer(
    _In_ WDFREQUEST Request,
    _In_reads_bytes_(BufferSize) const VOID* Buffer,
    _In_ size_t BufferSize)
{
    PVOID outputBuffer;
    NTSTATUS status;

    status = WdfRequestRetrieveOutputBuffer(Request, BufferSize, &outputBuffer, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlCopyMemory(outputBuffer, Buffer, BufferSize);
    WdfRequestSetInformation(Request, BufferSize);
    return STATUS_SUCCESS;
}

static VOID UpperBuildDeviceInfo(
    _In_ PUPPER_DEVICE_CONTEXT Context,
    _Out_ PGAYM_DEVICE_INFO DeviceInfo)
{
    BOOLEAN hasSupportedTarget;
    KIRQL oldIrql;

    RtlZeroMemory(DeviceInfo, sizeof(*DeviceInfo));

    KeAcquireSpinLock(&Context->StateLock, &oldIrql);
    hasSupportedTarget =
        Context->IsAttached &&
        Context->IsInD0 &&
        Context->VendorId == 0x045E &&
        Context->ProductId == 0x02FF;
    DeviceInfo->DeviceType = hasSupportedTarget ? GAYM_DEVICE_XBOX_ONE : GAYM_DEVICE_UNKNOWN;
    DeviceInfo->VendorId = Context->IsInD0 ? Context->VendorId : 0;
    DeviceInfo->ProductId = Context->IsInD0 ? Context->ProductId : 0;
    DeviceInfo->OverrideActive = Context->OverrideEnabled;
    DeviceInfo->ReportsSent = Context->ReportsInjected;
    DeviceInfo->ReadRequestsSeen = (ULONG)Context->ReadRequestsSeen;
    DeviceInfo->DeviceControlRequestsSeen = (ULONG)Context->DeviceControlRequestsSeen;
    DeviceInfo->InternalDeviceControlRequestsSeen = (ULONG)Context->InternalDeviceControlRequestsSeen;
    DeviceInfo->WriteRequestsSeen = (ULONG)Context->WriteRequestsSeen;
    DeviceInfo->LastInterceptedIoctl = (ULONG)Context->LastInterceptedIoctl;
    KeReleaseSpinLock(&Context->StateLock, oldIrql);
}

static NTSTATUS UpperStoreInjectedReport(
    _In_ PUPPER_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request)
{
    const GAYM_REPORT* report;
    NTSTATUS status;
    KIRQL oldIrql;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*report), (PVOID*)&report, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    KeAcquireSpinLock(&Context->StateLock, &oldIrql);
    RtlCopyMemory(&Context->LastInjectedReport, report, sizeof(*report));
    Context->HasInjectedReport = TRUE;
    Context->ReportsInjected++;
    Context->WriteRequestsSeen++;
    KeReleaseSpinLock(&Context->StateLock, oldIrql);
    return STATUS_SUCCESS;
}

static BOOLEAN UpperIoctlRequiresFileObject(_In_ ULONG IoControlCode)
{
    switch (IoControlCode) {
    case IOCTL_GAYM_ACQUIRE_WRITER_SESSION:
    case IOCTL_GAYM_RELEASE_WRITER_SESSION:
    case IOCTL_GAYM_OVERRIDE_ON:
    case IOCTL_GAYM_OVERRIDE_OFF:
    case IOCTL_GAYM_INJECT_REPORT:
        return TRUE;
    default:
        return FALSE;
    }
}

VOID UpperDeviceResetWriterState(
    _Inout_ PUPPER_DEVICE_CONTEXT Context,
    _In_opt_ WDFFILEOBJECT ExpectedFileObject)
{
    KIRQL oldIrql;

    if (Context == NULL) {
        return;
    }

    KeAcquireSpinLock(&Context->StateLock, &oldIrql);
    if (ExpectedFileObject == NULL || Context->WriterFileObject == ExpectedFileObject) {
        Context->WriterFileObject = NULL;
        Context->WriterSessionHeld = FALSE;
        Context->OverrideEnabled = FALSE;
        Context->HasInjectedReport = FALSE;
    }
    KeReleaseSpinLock(&Context->StateLock, oldIrql);
}

static BOOLEAN UpperHasSupportedTarget(_In_ PUPPER_DEVICE_CONTEXT Context)
{
    BOOLEAN hasSupportedTarget;
    KIRQL oldIrql;

    KeAcquireSpinLock(&Context->StateLock, &oldIrql);
    hasSupportedTarget =
        Context->IsAttached &&
        Context->IsInD0 &&
        Context->VendorId == 0x045E &&
        Context->ProductId == 0x02FF;
    KeReleaseSpinLock(&Context->StateLock, oldIrql);
    return hasSupportedTarget;
}

NTSTATUS UpperDeviceHandleIoctl(_In_ PUPPER_DEVICE_CONTEXT Context, _In_ WDFREQUEST Request, _In_ ULONG IoControlCode)
{
    NTSTATUS status;
    KIRQL oldIrql;
    WDFFILEOBJECT requestFileObject;

    requestFileObject = WdfRequestGetFileObject(Request);
    if (requestFileObject == NULL && UpperIoctlRequiresFileObject(IoControlCode)) {
        return STATUS_INVALID_HANDLE;
    }

    switch (IoControlCode) {
    case IOCTL_GAYM_ACQUIRE_WRITER_SESSION:
        if (!UpperHasSupportedTarget(Context)) {
            return STATUS_DEVICE_NOT_READY;
        }
        KeAcquireSpinLock(&Context->StateLock, &oldIrql);
        if (Context->WriterSessionHeld && Context->WriterFileObject != requestFileObject) {
            KeReleaseSpinLock(&Context->StateLock, oldIrql);
            return STATUS_DEVICE_BUSY;
        }
        Context->WriterFileObject = requestFileObject;
        Context->WriterSessionHeld = TRUE;
        KeReleaseSpinLock(&Context->StateLock, oldIrql);
        return STATUS_SUCCESS;
    case IOCTL_GAYM_RELEASE_WRITER_SESSION:
        KeAcquireSpinLock(&Context->StateLock, &oldIrql);
        if (!Context->WriterSessionHeld || Context->WriterFileObject != requestFileObject) {
            KeReleaseSpinLock(&Context->StateLock, oldIrql);
            return STATUS_INVALID_DEVICE_STATE;
        }
        Context->WriterFileObject = NULL;
        Context->WriterSessionHeld = FALSE;
        Context->OverrideEnabled = FALSE;
        Context->HasInjectedReport = FALSE;
        KeReleaseSpinLock(&Context->StateLock, oldIrql);
        return STATUS_SUCCESS;
    case IOCTL_GAYM_OVERRIDE_ON:
        if (!UpperHasSupportedTarget(Context)) {
            return STATUS_DEVICE_NOT_READY;
        }
        KeAcquireSpinLock(&Context->StateLock, &oldIrql);
        if (!Context->WriterSessionHeld || Context->WriterFileObject != requestFileObject) {
            KeReleaseSpinLock(&Context->StateLock, oldIrql);
            return STATUS_ACCESS_DENIED;
        }
        Context->OverrideEnabled = TRUE;
        KeReleaseSpinLock(&Context->StateLock, oldIrql);
        return STATUS_SUCCESS;
    case IOCTL_GAYM_OVERRIDE_OFF:
        KeAcquireSpinLock(&Context->StateLock, &oldIrql);
        if (Context->WriterSessionHeld && Context->WriterFileObject != requestFileObject) {
            KeReleaseSpinLock(&Context->StateLock, oldIrql);
            return STATUS_ACCESS_DENIED;
        }
        Context->OverrideEnabled = FALSE;
        KeReleaseSpinLock(&Context->StateLock, oldIrql);
        return STATUS_SUCCESS;
    case IOCTL_GAYM_INJECT_REPORT:
        if (!UpperHasSupportedTarget(Context)) {
            return STATUS_DEVICE_NOT_READY;
        }
        KeAcquireSpinLock(&Context->StateLock, &oldIrql);
        if (!Context->WriterSessionHeld || Context->WriterFileObject != requestFileObject) {
            KeReleaseSpinLock(&Context->StateLock, oldIrql);
            return STATUS_ACCESS_DENIED;
        }
        if (!Context->OverrideEnabled) {
            KeReleaseSpinLock(&Context->StateLock, oldIrql);
            return STATUS_INVALID_DEVICE_STATE;
        }
        KeReleaseSpinLock(&Context->StateLock, oldIrql);
        status = UpperStoreInjectedReport(Context, Request);
        UpperTraceRecord(IoControlCode, status);
        return status;
    case IOCTL_GAYM_QUERY_DEVICE:
        UpperBuildDeviceInfo(Context, &Context->LastDeviceInfo);
        return UpperCopyOutputBuffer(Request, &Context->LastDeviceInfo, sizeof(Context->LastDeviceInfo));
    case IOCTL_GAYM_QUERY_OBSERVATION:
        UpperDeviceUpdateObservation(Context);
        return UpperCopyOutputBuffer(Request, &Context->LastObservation, sizeof(Context->LastObservation));
    case IOCTL_GAYM_SET_JITTER:
    case IOCTL_GAYM_QUERY_SNAPSHOT:
    case IOCTL_GAYM_CAPTURE_OBSERVATION:
        return STATUS_NOT_IMPLEMENTED;
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}
