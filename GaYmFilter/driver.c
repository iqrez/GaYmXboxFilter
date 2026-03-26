/*
 * GaYmFilter - Driver entry and unload
 */

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>       /* Must precede ioctl.h to define the GUID */

#include "ioctl.h"
#include "driver.h"
#include "device.h"
#include "logging.h"

VOID GaYmEvtDriverUnload(_In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    GaYmDeleteControlDevice();
    GAYM_LOG_INFO("DriverUnload: GaYmFilter unloading");
}

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    GAYM_LOG_INFO("DriverEntry: GaYmFilter v2.0 loading");

    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, GaYmEvtDeviceAdd);
    config.EvtDriverUnload = GaYmEvtDriverUnload;
    config.DriverPoolTag   = GAYM_TAG;

    NTSTATUS status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE);

    if (!NT_SUCCESS(status)) {
        GAYM_LOG_ERROR("WdfDriverCreate failed: 0x%08X", status);
    }
    return status;
}
