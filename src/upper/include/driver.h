#pragma once

#include <ntddk.h>
#include <wdf.h>

NTSTATUS GaYmXInputFilterEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit);
EVT_WDF_DRIVER_DEVICE_ADD GaYmXInputFilterEvtDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY GaYmXInputFilterEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT GaYmXInputFilterEvtD0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL GaYmXInputFilterEvtSurpriseRemoval;
