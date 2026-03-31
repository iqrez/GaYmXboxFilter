#include "../include/upper_device.h"
#include "../include/driver.h"

typedef struct _UPPER_CONTROL_DEVICE_CONTEXT {
    PUPPER_DEVICE_CONTEXT FilterContext;
} UPPER_CONTROL_DEVICE_CONTEXT, *PUPPER_CONTROL_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(UPPER_CONTROL_DEVICE_CONTEXT, UpperControlGetContext)

DECLARE_CONST_UNICODE_STRING(g_UpperCtlDeviceName, L"\\Device\\GaYmXInputFilterCtl");
DECLARE_CONST_UNICODE_STRING(g_UpperCtlSymLink, L"\\DosDevices\\GaYmXInputFilterCtl");

static WDFDEVICE g_UpperControlDevice;

static BOOLEAN UpperTryParseHexWord(_In_reads_(4) PCWSTR Text, _Out_ PUSHORT Value)
{
    USHORT parsedValue;
    USHORT nibble;
    ULONG index;

    parsedValue = 0;
    for (index = 0; index < 4; ++index) {
        WCHAR ch;

        ch = Text[index];
        if (ch >= L'0' && ch <= L'9') {
            nibble = (USHORT)(ch - L'0');
        } else if (ch >= L'A' && ch <= L'F') {
            nibble = (USHORT)(10 + (ch - L'A'));
        } else if (ch >= L'a' && ch <= L'f') {
            nibble = (USHORT)(10 + (ch - L'a'));
        } else {
            return FALSE;
        }

        parsedValue = (USHORT)((parsedValue << 4) | nibble);
    }

    *Value = parsedValue;
    return TRUE;
}

static VOID UpperParseHardwareId(
    _In_opt_z_ PCWSTR HardwareId,
    _Out_ PUSHORT VendorId,
    _Out_ PUSHORT ProductId)
{
    PCWSTR cursor;

    *VendorId = 0;
    *ProductId = 0;

    if (HardwareId == NULL) {
        return;
    }

    for (cursor = HardwareId; *cursor != L'\0'; ++cursor) {
        if (cursor[0] == L'V' && cursor[1] == L'I' && cursor[2] == L'D' && cursor[3] == L'_' &&
            UpperTryParseHexWord(cursor + 4, VendorId)) {
            break;
        }
    }

    for (cursor = HardwareId; *cursor != L'\0'; ++cursor) {
        if (cursor[0] == L'P' && cursor[1] == L'I' && cursor[2] == L'D' && cursor[3] == L'_' &&
            UpperTryParseHexWord(cursor + 4, ProductId)) {
            break;
        }
    }
}

VOID UpperDeviceRefreshAttachmentState(_Inout_ PUPPER_DEVICE_CONTEXT Context)
{
    PDEVICE_OBJECT physicalDeviceObject;
    WCHAR hardwareIds[512];
    ULONG resultLength;
    NTSTATUS status;
    KIRQL oldIrql;
    USHORT vendorId;
    USHORT productId;
    BOOLEAN isSupportedTarget;

    if (Context == NULL || Context->Device == NULL) {
        return;
    }

    vendorId = 0;
    productId = 0;
    isSupportedTarget = FALSE;
    physicalDeviceObject = WdfDeviceWdmGetPhysicalDevice(Context->Device);
    if (physicalDeviceObject != NULL) {
        RtlZeroMemory(hardwareIds, sizeof(hardwareIds));
        resultLength = 0;
        status = IoGetDeviceProperty(
            physicalDeviceObject,
            DevicePropertyHardwareID,
            sizeof(hardwareIds),
            hardwareIds,
            &resultLength);
        if (NT_SUCCESS(status)) {
            UpperParseHardwareId(hardwareIds, &vendorId, &productId);
            isSupportedTarget = (vendorId == 0x045E && productId == 0x02FF);
        }
    }

    KeAcquireSpinLock(&Context->StateLock, &oldIrql);
    Context->VendorId = vendorId;
    Context->ProductId = productId;
    Context->IsAttached = isSupportedTarget;
    KeReleaseSpinLock(&Context->StateLock, oldIrql);
}

static VOID UpperEvtFileCleanup(_In_ WDFFILEOBJECT FileObject)
{
    PUPPER_CONTROL_DEVICE_CONTEXT controlContext;
    PUPPER_DEVICE_CONTEXT filterContext;

    controlContext = UpperControlGetContext(WdfFileObjectGetDevice(FileObject));
    filterContext = controlContext->FilterContext;
    if (filterContext == NULL) {
        return;
    }

    UpperDeviceResetWriterState(filterContext, FileObject);
}

static NTSTATUS UpperCreateQueue(_In_ WDFDEVICE Device, _Out_ WDFQUEUE* Queue)
{
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_OBJECT_ATTRIBUTES queueAttributes;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoRead = UpperEvtIoRead;
    queueConfig.EvtIoDeviceControl = UpperEvtIoDeviceControl;
    queueConfig.EvtIoInternalDeviceControl = UpperEvtIoInternalDeviceControl;

    WDF_OBJECT_ATTRIBUTES_INIT(&queueAttributes);
    queueAttributes.ParentObject = Device;

    return WdfIoQueueCreate(Device, &queueConfig, &queueAttributes, Queue);
}

static NTSTATUS UpperCreatePendingReadQueue(_In_ WDFDEVICE Device, _Out_ WDFQUEUE* Queue)
{
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_OBJECT_ATTRIBUTES queueAttributes;

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

    WDF_OBJECT_ATTRIBUTES_INIT(&queueAttributes);
    queueAttributes.ParentObject = Device;

    return WdfIoQueueCreate(Device, &queueConfig, &queueAttributes, Queue);
}

static VOID UpperEvtCtlIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PUPPER_CONTROL_DEVICE_CONTEXT controlContext = UpperControlGetContext(WdfIoQueueGetDevice(Queue));
    NTSTATUS status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (controlContext->FilterContext == NULL) {
        status = STATUS_DEVICE_NOT_READY;
    } else {
        status = UpperDeviceHandleIoctl(controlContext->FilterContext, Request, IoControlCode);
    }

    WdfRequestComplete(Request, status);
}

NTSTATUS GaYmXInputFilterEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    WdfFdoInitSetFilter(DeviceInit);
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDeviceD0Entry = GaYmXInputFilterEvtD0Entry;
    pnpCallbacks.EvtDeviceD0Exit = GaYmXInputFilterEvtD0Exit;
    pnpCallbacks.EvtDeviceSurpriseRemoval = GaYmXInputFilterEvtSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, UPPER_DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = UpperDeviceInitialize(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = UpperCreateQueue(device, &UpperGetContext(device)->DefaultQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = UpperCreatePendingReadQueue(device, &UpperGetContext(device)->PendingReadQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = UpperDeviceCreateControlDevice(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS UpperDeviceInitialize(_In_ WDFDEVICE Device)
{
    PUPPER_DEVICE_CONTEXT context = UpperGetContext(Device);
    RtlZeroMemory(context, sizeof(*context));
    context->Device = Device;
    context->LowerTarget = WdfDeviceGetIoTarget(Device);
    context->WriterFileObject = NULL;
    context->PendingReadQueue = NULL;
    context->VendorId = 0;
    context->ProductId = 0;
    context->IsAttached = FALSE;
    context->IsInD0 = FALSE;
    context->OverrideEnabled = FALSE;
    context->WriterSessionHeld = FALSE;
    context->HasInjectedReport = FALSE;
    context->HasObservedReport = FALSE;
    context->PendingInputRequests = 0;
    context->QueuedInputRequests = 0;
    context->CompletedInputRequests = 0;
    context->ForwardedInputRequests = 0;
    KeInitializeSpinLock(&context->StateLock);
    RtlZeroMemory(&context->JitterConfig, sizeof(context->JitterConfig));
    RtlZeroMemory(&context->LastInjectedReport, sizeof(context->LastInjectedReport));
    RtlZeroMemory(&context->LastObservedReport, sizeof(context->LastObservedReport));
    RtlZeroMemory(&context->LastPresentedXInputReport, sizeof(context->LastPresentedXInputReport));
    RtlZeroMemory(&context->LastDeviceInfo, sizeof(context->LastDeviceInfo));
    RtlZeroMemory(&context->LastObservation, sizeof(context->LastObservation));
    context->XInputPacketNumber = 0;
    context->HasPresentedXInputReport = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS UpperDeviceCreateControlDevice(_In_ WDFDEVICE FilterDevice)
{
    PUPPER_DEVICE_CONTEXT filterContext = UpperGetContext(FilterDevice);
    WDFDRIVER driver;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDFDEVICE controlDevice;
    PWDFDEVICE_INIT controlInit;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(
        sddl,
        L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GRGWGX;;;WD)(A;;GRGWGX;;;RC)");

    if (g_UpperControlDevice != NULL) {
        UpperControlGetContext(g_UpperControlDevice)->FilterContext = filterContext;
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
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, UpperEvtFileCleanup);
    WdfDeviceInitSetFileObjectConfig(controlInit, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);

    status = WdfDeviceInitAssignName(controlInit, &g_UpperCtlDeviceName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(controlInit);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, UPPER_CONTROL_DEVICE_CONTEXT);
    status = WdfDeviceCreate(&controlInit, &attributes, &controlDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    UpperControlGetContext(controlDevice)->FilterContext = filterContext;

    status = WdfDeviceCreateSymbolicLink(controlDevice, &g_UpperCtlSymLink);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(controlDevice);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = UpperEvtCtlIoDeviceControl;

    status = WdfIoQueueCreate(controlDevice, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(controlDevice);
        return status;
    }

    WdfControlFinishInitializing(controlDevice);
    g_UpperControlDevice = controlDevice;
    return STATUS_SUCCESS;
}

VOID UpperDeviceShutdownControlDevice(VOID)
{
    if (g_UpperControlDevice != NULL) {
        UpperControlGetContext(g_UpperControlDevice)->FilterContext = NULL;
    }
}
