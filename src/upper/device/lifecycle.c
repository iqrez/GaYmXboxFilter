#include "../include/upper_device.h"

NTSTATUS GaYmXInputFilterEvtD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
    PUPPER_DEVICE_CONTEXT context = UpperGetContext(Device);

    UpperDeviceRefreshAttachmentState(context);
    context->IsInD0 = TRUE;
    return UpperDeviceCreateControlDevice(Device);
}

NTSTATUS GaYmXInputFilterEvtD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);
    PUPPER_DEVICE_CONTEXT context = UpperGetContext(Device);

    context->IsInD0 = FALSE;
    context->HasObservedReport = FALSE;
    UpperDeviceResetWriterState(context, NULL);
    return STATUS_SUCCESS;
}

VOID GaYmXInputFilterEvtSurpriseRemoval(_In_ WDFDEVICE Device)
{
    PUPPER_DEVICE_CONTEXT context = UpperGetContext(Device);

    context->IsAttached = FALSE;
    context->IsInD0 = FALSE;
    context->VendorId = 0;
    context->ProductId = 0;
    context->HasObservedReport = FALSE;
    UpperDeviceResetWriterState(context, NULL);
    UpperDeviceShutdownControlDevice();
}
