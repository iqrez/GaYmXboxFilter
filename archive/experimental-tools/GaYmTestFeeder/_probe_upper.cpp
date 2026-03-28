#include <windows.h>
#include <cstdio>
#include <vector>
#include "DeviceHelper.h"

static void Probe(const wchar_t* label, const std::wstring& path)
{
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        std::wprintf(L"%ls open failed err=%lu path=%ls\n", label, GetLastError(), path.c_str());
        return;
    }

    GAYM_DEVICE_INFO info = {};
    DWORD bytes = 0;
    BOOL ok = QueryDeviceInfo(handle, &info, &bytes);
    std::wprintf(L"%ls query=%d err=%lu bytes=%lu path=%ls\n", label, ok ? 1 : 0, ok ? 0 : GetLastError(), bytes, path.c_str());
    if (ok) {
        std::printf("  type=%u vid=%04X pid=%04X layout=%lu build=0x%08lX\n", info.DeviceType, info.VendorId, info.ProductId, info.QueryLayoutVersion, info.DriverBuildStamp);
    }

    CloseHandle(handle);
}

int wmain()
{
    SetEnvironmentVariableW(L"GAYM_CONTROL_TARGET", L"upper");
    auto upper = EnumerateGaYmInterfaces(GUID_DEVINTERFACE_GAYM_XINPUT_FILTER);
    std::wprintf(L"upper_interfaces=%zu\n", upper.size());
    for (const auto& device : upper) {
        Probe(L"upper-iface", device.path);
    }
    Probe(L"upper-cdo", GaYmPrimaryControlDevicePath());
    Probe(L"lower-cdo", GaYmSecondaryControlDevicePath());
    return 0;
}
