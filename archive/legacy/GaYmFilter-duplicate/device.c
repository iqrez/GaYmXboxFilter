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
 * When override is OFF:  all I/O forwarded transparently.
 * When override is ON:   reads are queued; INJECT_REPORT completes them
 *                         with translated native HID data.
 */

#include "device.h"
#include <hidport.h>
#include <ntstrsafe.h>

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
    PDEVICE_CONTEXT ctx = ctlCtx->FilterCtx;

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
        GAYM_LOG_INFO("CDO: Override ON (VID:%04X PID:%04X)", ctx->VendorId, ctx->ProductId);
        status = STATUS_SUCCESS;
        break;

    case IOCTL_GAYM_OVERRIDE_OFF:
        ctx->OverrideEnabled = FALSE;
        ctx->HasReport       = FALSE;
        GaYmDrainPendingReads(ctx, STATUS_SUCCESS);
        GAYM_LOG_INFO("CDO: Override OFF");
        status = STATUS_SUCCESS;
        break;

    case IOCTL_GAYM_INJECT_REPORT:
    {
        PVOID  inBuf;
        size_t inLen;
        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(GAYM_REPORT), &inBuf, &inLen);
        if (!NT_SUCCESS(status)) break;

        KIRQL oldIrql;
        KeAcquireSpinLock(&ctx->ReportLock, &oldIrql);
        RtlCopyMemory(&ctx->CurrentReport, inBuf, sizeof(GAYM_REPORT));
        ctx->HasReport = TRUE;
        ctx->ReportsSent++;
        KeReleaseSpinLock(&ctx->ReportLock, oldIrql);

        WDFREQUEST readReq;
        while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(
                   ctx->PendingReadsQueue, &readReq))) {
            GaYmCompleteReadWithReport(ctx, readReq);
        }

        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_GAYM_QUERY_DEVICE:
    {
        PVOID outBuf;
        size_t outLen;
        status = WdfRequestRetrieveOutputBuffer(
            Request, sizeof(GAYM_DEVICE_INFO), &outBuf, &outLen);
        if (NT_SUCCESS(status)) {
            PGAYM_DEVICE_INFO di = (PGAYM_DEVICE_INFO)outBuf;
            di->DeviceType     = ctx->DeviceType;
            di->VendorId       = ctx->VendorId;
            di->ProductId      = ctx->ProductId;
            di->OverrideActive = ctx->OverrideEnabled;
            di->ReportsSent    = ctx->ReportsSent;
            di->PendingInputRequests   = (ULONG)ctx->PendingInputRequests;
            di->QueuedInputRequests    = (ULONG)ctx->QueuedInputRequests;
            di->CompletedInputRequests = (ULONG)ctx->CompletedInputRequests;
            di->ForwardedInputRequests = (ULONG)ctx->ForwardedInputRequests;
            di->LastInterceptedIoctl   = (ULONG)ctx->LastInterceptedIoctl;
            di->ReadRequestsSeen             = (ULONG)ctx->ReadRequestsSeen;
            di->DeviceControlRequestsSeen    = (ULONG)ctx->DeviceControlRequestsSeen;
            di->InternalDeviceControlRequestsSeen = (ULONG)ctx->InternalDeviceControlRequestsSeen;
            di->WriteRequestsSeen            = (ULONG)ctx->WriteRequestsSeen;
            info = sizeof(GAYM_DEVICE_INFO);
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

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, info);
}

NTSTATUS GaYmCreateControlDevice(
    _In_ WDFDEVICE       FilterDevice,
    _In_ PDEVICE_CONTEXT FilterCtx)
{
    UNREFERENCED_PARAMETER(FilterDevice);

    if (g_ControlDevice) {
        /* Already exists — just update the filter context pointer */
        PCONTROL_DEVICE_CONTEXT ctlCtx = ControlGetContext(g_ControlDevice);
        if (!ctlCtx->FilterCtx) {
            ctlCtx->FilterCtx = FilterCtx;
        }
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
    ctlCtx->FilterCtx = FilterCtx;

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
        ctlCtx->FilterCtx = NULL;
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
    UNREFERENCED_PARAMETER(Length);
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    InterlockedIncrement(&ctx->ReadRequestsSeen);

    if (!ctx->OverrideEnabled) {
        /* Pass-through: real device data flows normally */
        GaYmForwardRequest(ctx->Device, Request);
        return;
    }

    /*
     * Override is active. Queue the read request.
     * It will be completed when user-mode sends IOCTL_GAYM_INJECT_REPORT.
     */
    GaYmQueuePendingInputRequest(ctx, Request);
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
    UNREFERENCED_PARAMETER(Length);
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    InterlockedIncrement(&ctx->WriteRequestsSeen);
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
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    InterlockedIncrement(&ctx->DeviceControlRequestsSeen);

    if (ctx->OverrideEnabled && GaYmShouldInterceptInternalIoctl(IoControlCode)) {
        InterlockedExchange(&ctx->LastInterceptedIoctl, (LONG)IoControlCode);
        GaYmQueuePendingInputRequest(ctx, Request);
        return;
    }

    GaYmForwardRequest(ctx->Device, Request);
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
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    InterlockedIncrement(&ctx->InternalDeviceControlRequestsSeen);

    if (ctx->OverrideEnabled && GaYmShouldInterceptInternalIoctl(IoControlCode)) {
        InterlockedExchange(&ctx->LastInterceptedIoctl, (LONG)IoControlCode);
        GaYmQueuePendingInputRequest(ctx, Request);
        return;
    }

    GaYmForwardRequest(ctx->Device, Request);
}

/* ═══════════════════════════════════════════════════════════════════
 * PnP: Prepare Hardware
 * ═══════════════════════════════════════════════════════════════════ */
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
        if (ctlCtx->FilterCtx == ctx) {
            ctlCtx->FilterCtx = NULL;
            GAYM_LOG_INFO("CDO: unbound from released device");
        }
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
        ctlCtx->FilterCtx = ctx;
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
        if (ctlCtx->FilterCtx == ctx) {
            ctlCtx->FilterCtx = NULL;
            GAYM_LOG_INFO("CDO: unbound from D0-exiting device");
        }
    }

    if (ctx->OverrideEnabled) {
        ctx->OverrideEnabled = FALSE;
        ctx->HasReport       = FALSE;

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

    if (g_ControlDevice) {
        PCONTROL_DEVICE_CONTEXT ctlCtx = ControlGetContext(g_ControlDevice);
        if (ctlCtx->FilterCtx == ctx) {
            ctlCtx->FilterCtx = NULL;
            GAYM_LOG_INFO("CDO: unbound from surprise-removed device");
        }
    }

    /* Purge the manual queue (cancels all requests with STATUS_CANCELLED) */
    WdfIoQueuePurgeSynchronously(ctx->PendingReadsQueue);

    GAYM_LOG_INFO("SurpriseRemoval: %s unplugged (VID:%04X PID:%04X)",
        GaYmDeviceTypeName(ctx->DeviceType),
        ctx->VendorId, ctx->ProductId);
}
