#pragma once
/*
 * GaYmTestFeeder - Device enumeration and communication helper.
 * Sideband IOCTLs are most reliable through the dedicated control device
 * objects; direct device-interface opens on the HID stack can route into
 * xinputhid/HidUsb and reject our private IOCTLs.
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
#include "../ioctl.h"

#pragma comment(lib, "setupapi.lib")

#include <string>
#include <vector>

/* Device path info for enumerated GaYmFilter devices */
struct GaYmDevicePath {
    std::wstring path;
    int          index;
};

enum class GaYmControlTarget {
    Auto,
    Upper,
    Lower,
    Legacy,
};

inline GaYmControlTarget GaYmGetControlTargetPreference()
{
    wchar_t value[32] = {};
    const DWORD length = GetEnvironmentVariableW(L"GAYM_CONTROL_TARGET", value, ARRAYSIZE(value));
    if (length == 0 || length >= ARRAYSIZE(value)) {
        return GaYmControlTarget::Auto;
    }

    if (_wcsicmp(value, L"upper") == 0) {
        return GaYmControlTarget::Upper;
    }

    if (_wcsicmp(value, L"lower") == 0) {
        return GaYmControlTarget::Lower;
    }

    if (_wcsicmp(value, L"legacy") == 0) {
        return GaYmControlTarget::Legacy;
    }

    return GaYmControlTarget::Auto;
}

inline const wchar_t* GaYmPrimaryControlDevicePath()
{
    return L"\\\\.\\GaYmXInputFilterCtl";
}

inline const wchar_t* GaYmSecondaryControlDevicePath()
{
    return L"\\\\.\\GaYmFilterCtl";
}

inline const wchar_t* GaYmLegacyControlDevicePath()
{
    return L"\\\\.\\GaYmXboxFilterCtl";
}

inline const wchar_t* GaYmPreferredControlDevicePathForTarget(GaYmControlTarget target)
{
    switch (target) {
    case GaYmControlTarget::Upper:
        return GaYmPrimaryControlDevicePath();
    case GaYmControlTarget::Lower:
        return GaYmSecondaryControlDevicePath();
    case GaYmControlTarget::Legacy:
        return GaYmLegacyControlDevicePath();
    case GaYmControlTarget::Auto:
    default:
        return GaYmPrimaryControlDevicePath();
    }
}

inline const wchar_t* GaYmFallbackControlDevicePathForTarget(GaYmControlTarget target)
{
    switch (target) {
    case GaYmControlTarget::Upper:
        return GaYmSecondaryControlDevicePath();
    case GaYmControlTarget::Lower:
        return GaYmPrimaryControlDevicePath();
    case GaYmControlTarget::Legacy:
        return GaYmPrimaryControlDevicePath();
    case GaYmControlTarget::Auto:
    default:
        return GaYmSecondaryControlDevicePath();
    }
}

inline std::vector<GaYmDevicePath> EnumerateGaYmInterfaces(const GUID& interfaceGuid)
{
    std::vector<GaYmDevicePath> result;

    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &interfaceGuid,
        NULL, NULL,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);

    if (devInfo == INVALID_HANDLE_VALUE)
        return result;

    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(ifData);

    for (DWORD idx = 0;
         SetupDiEnumDeviceInterfaces(devInfo, NULL, &interfaceGuid, idx, &ifData);
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
            dp.path = detail->DevicePath;
            dp.index = static_cast<int>(idx);
            result.push_back(dp);
        }
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

inline const wchar_t* GaYmPreferredControlDevicePath()
{
    return GaYmPreferredControlDevicePathForTarget(GaYmGetControlTargetPreference());
}

inline const wchar_t* GaYmFallbackControlDevicePath()
{
    return GaYmFallbackControlDevicePathForTarget(GaYmGetControlTargetPreference());
}

inline bool HasGaYmControlDeviceForTarget(GaYmControlTarget target)
{
    HANDLE h = CreateFileW(
        GaYmPreferredControlDevicePathForTarget(target),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);

    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(
            GaYmFallbackControlDevicePathForTarget(target),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);
    }

    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(
            GaYmLegacyControlDevicePath(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);
    }

    if (h == INVALID_HANDLE_VALUE)
        return false;

    CloseHandle(h);
    return true;
}

inline bool HasGaYmControlDevice()
{
    return HasGaYmControlDeviceForTarget(GaYmGetControlTargetPreference());
}

inline HANDLE OpenGaYmPathWithFallback(const std::wstring& path)
{
    HANDLE h = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);

    if (h != INVALID_HANDLE_VALUE) {
        return h;
    }

    if (_wcsicmp(path.c_str(), GaYmPrimaryControlDevicePath()) == 0 ||
        _wcsicmp(path.c_str(), GaYmSecondaryControlDevicePath()) == 0 ||
        _wcsicmp(path.c_str(), GaYmLegacyControlDevicePath()) == 0) {
        return INVALID_HANDLE_VALUE;
    }

    h = CreateFileW(
        GaYmPreferredControlDevicePath(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);

    if (h != INVALID_HANDLE_VALUE) {
        return h;
    }

    h = CreateFileW(
        GaYmFallbackControlDevicePath(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);

    if (h != INVALID_HANDLE_VALUE) {
        return h;
    }

    return CreateFileW(
        GaYmLegacyControlDevicePath(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
}

/* Enumerate all present GaYmFilter device interfaces */
inline std::vector<GaYmDevicePath> EnumerateGaYmDevicesForTarget(GaYmControlTarget targetPreference)
{
    std::vector<GaYmDevicePath> result;

    if (targetPreference == GaYmControlTarget::Upper) {
        if (HasGaYmControlDeviceForTarget(targetPreference)) {
            GaYmDevicePath dp;
            dp.path = GaYmPreferredControlDevicePathForTarget(targetPreference);
            dp.index = 0;
            result.push_back(dp);
            return result;
        }

        result = EnumerateGaYmInterfaces(GUID_DEVINTERFACE_GAYM_XINPUT_FILTER);
        if (!result.empty()) {
            return result;
        }
        return result;
    }

    if (targetPreference == GaYmControlTarget::Lower) {
        if (HasGaYmControlDeviceForTarget(targetPreference)) {
            GaYmDevicePath dp;
            dp.path = GaYmPreferredControlDevicePathForTarget(targetPreference);
            dp.index = 0;
            result.push_back(dp);
            return result;
        }

        result = EnumerateGaYmInterfaces(GUID_DEVINTERFACE_GAYM_FILTER);
        if (!result.empty()) {
            return result;
        }
        return result;
    }

    if (targetPreference == GaYmControlTarget::Legacy) {
        if (HasGaYmControlDeviceForTarget(targetPreference)) {
            GaYmDevicePath dp;
            dp.path = GaYmPreferredControlDevicePathForTarget(targetPreference);
            dp.index = 0;
            result.push_back(dp);
        }
        return result;
    }

    if (HasGaYmControlDeviceForTarget(targetPreference)) {
        GaYmDevicePath dp;
        dp.path = GaYmPreferredControlDevicePathForTarget(targetPreference);
        dp.index = 0;
        result.push_back(dp);
        return result;
    }

    result = EnumerateGaYmInterfaces(GUID_DEVINTERFACE_GAYM_XINPUT_FILTER);
    if (!result.empty()) {
        return result;
    }

    result = EnumerateGaYmInterfaces(GUID_DEVINTERFACE_GAYM_FILTER);
    if (!result.empty()) {
        return result;
    }

    return result;
}

inline std::vector<GaYmDevicePath> EnumerateGaYmDevices()
{
    return EnumerateGaYmDevicesForTarget(GaYmGetControlTargetPreference());
}

inline HANDLE OpenGaYmDeviceForTarget(GaYmControlTarget target, int index = 0)
{
    auto devices = EnumerateGaYmDevicesForTarget(target);
    if (index < 0 || index >= (int)devices.size()) {
        return INVALID_HANDLE_VALUE;
    }

    return OpenGaYmPathWithFallback(devices[index].path);
}

/* Open a GaYmFilter device by index (0 = first). Returns INVALID_HANDLE_VALUE on failure. */
inline HANDLE OpenGaYmDevice(int index = 0)
{
    return OpenGaYmDeviceForTarget(GaYmGetControlTargetPreference(), index);
}

inline HANDLE OpenGaYmControlDevicePathStrict(const wchar_t* path)
{
    return CreateFileW(
        path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
}

inline bool SendIoctlRaw(
    HANDLE hDevice,
    DWORD ioctl,
    LPVOID inputBuffer,
    DWORD inputBytes,
    LPVOID outputBuffer,
    DWORD outputBytes,
    DWORD* bytesReturned = nullptr)
{
    DWORD bytes = 0;
    if (bytesReturned == nullptr) {
        bytesReturned = &bytes;
    }

    return DeviceIoControl(
        hDevice,
        ioctl,
        inputBuffer,
        inputBytes,
        outputBuffer,
        outputBytes,
        bytesReturned,
        NULL) != FALSE;
}

inline bool GaYmShouldMirrorToLower()
{
    const GaYmControlTarget targetPreference = GaYmGetControlTargetPreference();
    if (targetPreference == GaYmControlTarget::Lower ||
        targetPreference == GaYmControlTarget::Legacy) {
        return false;
    }

    HANDLE upper = OpenGaYmControlDevicePathStrict(GaYmPrimaryControlDevicePath());
    if (upper == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(upper);

    HANDLE lower = OpenGaYmControlDevicePathStrict(GaYmSecondaryControlDevicePath());
    if (lower == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(lower);

    return true;
}

inline void GaYmMirrorIoctlToLower(
    DWORD ioctl,
    LPVOID inputBuffer = nullptr,
    DWORD inputBytes = 0)
{
    HANDLE lower;
    DWORD bytes = 0;

    if (!GaYmShouldMirrorToLower()) {
        return;
    }

    lower = OpenGaYmControlDevicePathStrict(GaYmSecondaryControlDevicePath());
    if (lower == INVALID_HANDLE_VALUE) {
        return;
    }

    SendIoctlRaw(lower, ioctl, inputBuffer, inputBytes, NULL, 0, &bytes);
    CloseHandle(lower);
}

/* Send a simple IOCTL with no input/output */
inline bool SendIoctl(HANDLE hDevice, DWORD ioctl)
{
    bool success = SendIoctlRaw(hDevice, ioctl, NULL, 0, NULL, 0);

    if (success &&
        (ioctl == IOCTL_GAYM_OVERRIDE_ON || ioctl == IOCTL_GAYM_OVERRIDE_OFF)) {
        GaYmMirrorIoctlToLower(ioctl);
    }

    return success;
}

/* Inject a gamepad report */
inline bool InjectReport(HANDLE hDevice, const GAYM_REPORT* report)
{
    bool success = SendIoctlRaw(
        hDevice,
        IOCTL_GAYM_INJECT_REPORT,
        (LPVOID)report,
        sizeof(GAYM_REPORT),
        NULL,
        0);

    if (success) {
        GaYmMirrorIoctlToLower(
            IOCTL_GAYM_INJECT_REPORT,
            (LPVOID)report,
            sizeof(GAYM_REPORT));
    }

    return success;
}

/* Query device info */
inline bool QueryDeviceInfo(HANDLE hDevice, GAYM_DEVICE_INFO* info, DWORD* bytesReturned = nullptr)
{
    DWORD bytes = 0;
    if (info == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    ZeroMemory(info, sizeof(*info));
    if (bytesReturned != nullptr) {
        *bytesReturned = 0;
    }

    BOOL success = SendIoctlRaw(
        hDevice,
        IOCTL_GAYM_QUERY_DEVICE,
        NULL,
        0,
        info,
        sizeof(GAYM_DEVICE_INFO),
        &bytes);
    if (bytesReturned != nullptr) {
        *bytesReturned = bytes;
    }
    return success != FALSE;
}

/* Set jitter configuration */
inline bool SetJitter(HANDLE hDevice, const GAYM_JITTER_CONFIG* config)
{
    return SendIoctlRaw(
        hDevice,
        IOCTL_GAYM_SET_JITTER,
        (LPVOID)config,
        sizeof(GAYM_JITTER_CONFIG),
        NULL,
        0);
}

inline bool ApplyOutputState(HANDLE hDevice, const GAYM_OUTPUT_STATE* outputState)
{
    return SendIoctlRaw(
        hDevice,
        IOCTL_GAYM_APPLY_OUTPUT,
        (LPVOID)outputState,
        sizeof(GAYM_OUTPUT_STATE),
        NULL,
        0);
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
