#pragma once
/*
 * GaYmXboxFilter - Driver entry points.
 */

#include <ntddk.h>
#include <wdf.h>

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD GaYmEvtDriverUnload;
