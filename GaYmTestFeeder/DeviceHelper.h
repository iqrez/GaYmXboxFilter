#pragma once
/*
 * GaYmTestFeeder - Device enumeration and communication helper.
 * Opens GaYmFilter device interfaces via SetupDi and falls back to the
 * control device object when no per-device interface is published.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <initguid.h>
#include "../GaYmFilter/ioctl.h"

#pragma comment(lib, "setupapi.lib")

#include <string>
#include <vector>

/* Device path info for enumerated GaYmFilter devices */
struct GaYmDevicePath {
    std::wstring path;
    int          index;
};

inline bool HasGaYmControlDevice()
{
    HANDLE h = CreateFileW(
        L"\\\\.\\GaYmFilterCtl",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    CloseHandle(h);
    return true;
}

/* Enumerate all present GaYmFilter device interfaces */
inline std::vector<GaYmDevicePath> EnumerateGaYmDevices()
{
    std::vector<GaYmDevicePath> result;

    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_GAYM_FILTER,
        NULL, NULL,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);

    if (devInfo == INVALID_HANDLE_VALUE)
        return result;

    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(ifData);

    for (DWORD idx = 0;
         SetupDiEnumDeviceInterfaces(devInfo, NULL,
             &GUID_DEVINTERFACE_GAYM_FILTER, idx, &ifData);
         idx++)
    {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &needed, NULL);
        if (needed == 0) continue;

        std::vector<BYTE> buf(needed);
        auto detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, needed, NULL, NULL)) {
            GaYmDevicePath dp;
            dp.path  = detail->DevicePath;
            dp.index = (int)idx;
            result.push_back(dp);
        }
    }

    SetupDiDestroyDeviceInfoList(devInfo);

    if (result.empty() && HasGaYmControlDevice()) {
        GaYmDevicePath dp;
        dp.path = L"\\\\.\\GaYmFilterCtl";
        dp.index = 0;
        result.push_back(dp);
    }

    return result;
}

/* Open a GaYmFilter device by index (0 = first). Returns INVALID_HANDLE_VALUE on failure. */
inline HANDLE OpenGaYmDevice(int index = 0)
{
    auto devices = EnumerateGaYmDevices();
    if (index < 0 || index >= (int)devices.size())
        return INVALID_HANDLE_VALUE;

    return CreateFileW(
        devices[index].path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
}

/* Send a simple IOCTL with no input/output */
inline bool SendIoctl(HANDLE hDevice, DWORD ioctl)
{
    DWORD bytes = 0;
    return DeviceIoControl(hDevice, ioctl, NULL, 0, NULL, 0, &bytes, NULL) != FALSE;
}

/* Inject a gamepad report */
inline bool InjectReport(HANDLE hDevice, const GAYM_REPORT* report)
{
    DWORD bytes = 0;
    return DeviceIoControl(hDevice, IOCTL_GAYM_INJECT_REPORT,
        (LPVOID)report, sizeof(GAYM_REPORT),
        NULL, 0, &bytes, NULL) != FALSE;
}

/* Query device info */
inline bool QueryDeviceInfo(HANDLE hDevice, GAYM_DEVICE_INFO* info)
{
    DWORD bytes = 0;
    return DeviceIoControl(hDevice, IOCTL_GAYM_QUERY_DEVICE,
        NULL, 0,
        info, sizeof(GAYM_DEVICE_INFO),
        &bytes, NULL) != FALSE;
}

/* Set jitter configuration */
inline bool SetJitter(HANDLE hDevice, const GAYM_JITTER_CONFIG* config)
{
    DWORD bytes = 0;
    return DeviceIoControl(hDevice, IOCTL_GAYM_SET_JITTER,
        (LPVOID)config, sizeof(GAYM_JITTER_CONFIG),
        NULL, 0, &bytes, NULL) != FALSE;
}

/* Get device type name */
inline const char* DeviceTypeName(GAYM_DEVICE_TYPE type)
{
    switch (type) {
    case GAYM_DEVICE_XBOX_ONE:       return "Xbox One";
    case GAYM_DEVICE_XBOX_SERIES:    return "Xbox Series";
    case GAYM_DEVICE_DUALSENSE:      return "DualSense";
    case GAYM_DEVICE_DUALSENSE_EDGE: return "DualSense Edge";
    default:                         return "Unknown";
    }
}
