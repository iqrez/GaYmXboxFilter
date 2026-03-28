#include <windows.h>
#include <stdio.h>
#include "C:\Users\IQRez\Desktop\GaYmXboxFilter\GaYmTestFeeder\DeviceHelper.h"
int wmain() {
    SetEnvironmentVariableW(L"GAYM_CONTROL_TARGET", L"upper");
    auto devices = EnumerateGaYmDevices();
    wprintf(L"count=%zu\n", devices.size());
    for (const auto& d : devices) {
        wprintf(L"path=%ls\n", d.path.c_str());
        HANDLE h = CreateFileW(d.path.c_str(), GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        wprintf(L"open=%p err=%lu\n", h, h==INVALID_HANDLE_VALUE?GetLastError():0);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    return 0;
}
