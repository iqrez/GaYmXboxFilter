#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define DIRECTINPUT_VERSION 0x0800

#include <windows.h>
#include <objbase.h>
#include <dinput.h>

#include <cstdio>
#include <cstring>

#include "DeviceHelper.h"

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

struct DeviceSelection {
    DIDEVICEINSTANCEW instance = {};
    bool found = false;
};

static void InitNeutralReport(GAYM_REPORT* report)
{
    std::memset(report, 0, sizeof(*report));
    report->DPad = GAYM_DPAD_NEUTRAL;
}

static BOOL CALLBACK EnumGameControllerCallback(const DIDEVICEINSTANCEW* instance, VOID* context)
{
    auto* selection = static_cast<DeviceSelection*>(context);
    selection->instance = *instance;
    selection->found = true;
    return DIENUM_STOP;
}

static HRESULT PollDirectInputState(IDirectInputDevice8W* device, DIJOYSTATE2* state)
{
    HRESULT hr = device->Poll();
    if (FAILED(hr)) {
        hr = device->Acquire();
        while (hr == DIERR_INPUTLOST) {
            hr = device->Acquire();
        }
        if (FAILED(hr)) {
            return hr;
        }
        hr = device->Poll();
        if (FAILED(hr)) {
            return hr;
        }
    }

    std::memset(state, 0, sizeof(*state));
    hr = device->GetDeviceState(sizeof(*state), state);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        hr = device->Acquire();
        while (hr == DIERR_INPUTLOST) {
            hr = device->Acquire();
        }
        if (FAILED(hr)) {
            return hr;
        }
        hr = device->GetDeviceState(sizeof(*state), state);
    }
    return hr;
}

int main()
{
    std::printf("GaYm DirectInput verifier\n\n");

    const bool activateFirst = (__argc >= 2) && (_stricmp(__argv[1], "--activate-first") == 0);

    HANDLE device = OpenGaYmDevice(0);
    if (device == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "ERROR: Cannot open GaYm device (error %lu)\n", GetLastError());
        return 1;
    }

    GAYM_REPORT neutral = {};
    InitNeutralReport(&neutral);

    bool overrideEnabled = false;
    if (activateFirst) {
        if (!SendIoctl(device, IOCTL_GAYM_OVERRIDE_ON)) {
            std::fprintf(stderr, "ERROR: Failed to enable override (error %lu)\n", GetLastError());
            CloseHandle(device);
            return 1;
        }
        overrideEnabled = true;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::fprintf(stderr, "ERROR: CoInitializeEx failed: 0x%08lX\n", hr);
        if (overrideEnabled) {
            InjectReport(device, &neutral);
            Sleep(50);
            SendIoctl(device, IOCTL_GAYM_OVERRIDE_OFF);
        }
        CloseHandle(device);
        return 1;
    }

    IDirectInput8W* directInput = nullptr;
    hr = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&directInput), nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "ERROR: DirectInput8Create failed: 0x%08lX\n", hr);
        CoUninitialize();
        if (overrideEnabled) {
            InjectReport(device, &neutral);
            Sleep(50);
            SendIoctl(device, IOCTL_GAYM_OVERRIDE_OFF);
        }
        CloseHandle(device);
        return 1;
    }

    DeviceSelection selection = {};
    selection.instance.dwSize = sizeof(selection.instance);
    hr = directInput->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumGameControllerCallback, &selection, DIEDFL_ATTACHEDONLY);
    if (FAILED(hr) || !selection.found) {
        std::fprintf(stderr, "ERROR: No DirectInput game controller found.\n");
        directInput->Release();
        CoUninitialize();
        if (overrideEnabled) {
            InjectReport(device, &neutral);
            Sleep(50);
            SendIoctl(device, IOCTL_GAYM_OVERRIDE_OFF);
        }
        CloseHandle(device);
        return 1;
    }

    IDirectInputDevice8W* controller = nullptr;
    hr = directInput->CreateDevice(selection.instance.guidInstance, &controller, nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "ERROR: CreateDevice failed: 0x%08lX\n", hr);
        directInput->Release();
        CoUninitialize();
        if (overrideEnabled) {
            InjectReport(device, &neutral);
            Sleep(50);
            SendIoctl(device, IOCTL_GAYM_OVERRIDE_OFF);
        }
        CloseHandle(device);
        return 1;
    }

    hr = controller->SetDataFormat(&c_dfDIJoystick2);
    if (FAILED(hr)) {
        std::fprintf(stderr, "ERROR: SetDataFormat failed: 0x%08lX\n", hr);
        controller->Release();
        directInput->Release();
        CoUninitialize();
        if (overrideEnabled) {
            InjectReport(device, &neutral);
            Sleep(50);
            SendIoctl(device, IOCTL_GAYM_OVERRIDE_OFF);
        }
        CloseHandle(device);
        return 1;
    }

    HWND hwnd = GetConsoleWindow();
    if (hwnd == nullptr) {
        hwnd = GetDesktopWindow();
    }

    hr = controller->SetCooperativeLevel(hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    if (FAILED(hr)) {
        std::fprintf(stderr, "ERROR: SetCooperativeLevel failed: 0x%08lX\n", hr);
        controller->Release();
        directInput->Release();
        CoUninitialize();
        if (overrideEnabled) {
            InjectReport(device, &neutral);
            Sleep(50);
            SendIoctl(device, IOCTL_GAYM_OVERRIDE_OFF);
        }
        CloseHandle(device);
        return 1;
    }

    hr = controller->Acquire();
    if (FAILED(hr) && hr != DIERR_OTHERAPPHASPRIO) {
        std::fprintf(stderr, "ERROR: Acquire failed: 0x%08lX\n", hr);
        controller->Release();
        directInput->Release();
        CoUninitialize();
        if (overrideEnabled) {
            InjectReport(device, &neutral);
            Sleep(50);
            SendIoctl(device, IOCTL_GAYM_OVERRIDE_OFF);
        }
        CloseHandle(device);
        return 1;
    }

    DIJOYSTATE2 baseline = {};
    if (!activateFirst) {
        hr = PollDirectInputState(controller, &baseline);
        if (FAILED(hr)) {
            std::fprintf(stderr, "ERROR: Poll baseline failed: 0x%08lX\n", hr);
            controller->Release();
            directInput->Release();
            CoUninitialize();
            if (overrideEnabled) {
                InjectReport(device, &neutral);
                Sleep(50);
                SendIoctl(device, IOCTL_GAYM_OVERRIDE_OFF);
            }
            CloseHandle(device);
            return 1;
        }
    }

    std::printf("Device: %ls\n", selection.instance.tszProductName);
    std::printf("Baseline: lX=%ld lY=%ld buttons[0]=%u\n",
        baseline.lX,
        baseline.lY,
        baseline.rgbButtons[0]);

    auto cleanup = [&]() {
        if (overrideEnabled) {
            InjectReport(device, &neutral);
            Sleep(50);
            SendIoctl(device, IOCTL_GAYM_OVERRIDE_OFF);
            overrideEnabled = false;
        }
        CloseHandle(device);
        controller->Unacquire();
        controller->Release();
        directInput->Release();
        CoUninitialize();
    };

    if (!overrideEnabled) {
        if (!SendIoctl(device, IOCTL_GAYM_OVERRIDE_ON)) {
            std::fprintf(stderr, "ERROR: Failed to enable override (error %lu)\n", GetLastError());
            cleanup();
            return 1;
        }
        overrideEnabled = true;
    }

    GAYM_REPORT report = {};
    InitNeutralReport(&report);
    report.Buttons[0] = GAYM_BTN_A;
    report.ThumbLeftX = 32767;

    DIJOYSTATE2 observed = {};
    bool pass = false;
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 1500) {
        if (!InjectReport(device, &report)) {
            std::fprintf(stderr, "ERROR: InjectReport failed (error %lu)\n", GetLastError());
            cleanup();
            return 1;
        }

        hr = PollDirectInputState(controller, &observed);
        if (SUCCEEDED(hr)) {
            const LONG deltaX = activateFirst ? observed.lX - 32767 : observed.lX - baseline.lX;
            const bool movedRight = deltaX > 10000;
            const bool buttonSet = observed.rgbButtons[0] != 0;
            if (movedRight || buttonSet) {
                pass = true;
                break;
            }
        }

        Sleep(8);
    }

    cleanup();

    std::printf("Observed: lX=%ld lY=%ld buttons[0]=%u\n",
        observed.lX,
        observed.lY,
        observed.rgbButtons[0]);
    std::printf("\nResult: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 2;
}
