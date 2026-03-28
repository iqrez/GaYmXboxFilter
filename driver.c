/*
 * GaYmXboxFilter - Driver entry and unload.
 */

#include <ntddk.h>
#include <wdf.h>

#include "driver.h"
#include "device.h"
#include "logging.h"

VOID GaYmEvtDriverUnload(_In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);

    GaYmDeleteControlDevice();
    GAYM_LOG_INFO("DriverUnload: GaYmXboxFilter unloading");
}

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    GAYM_LOG_INFO("DriverEntry: GaYmXboxFilter loading");

    WDF_DRIVER_CONFIG_INIT(&config, GaYmEvtDeviceAdd);
    config.EvtDriverUnload = GaYmEvtDriverUnload;
    config.DriverPoolTag = GAYM_TAG;

    return WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE);
}
