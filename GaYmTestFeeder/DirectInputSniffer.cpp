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
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

struct DeviceSelection {
    DIDEVICEINSTANCEW instance = {};
    bool found = false;
};

static volatile LONG g_Running = TRUE;

static BOOL WINAPI ConsoleHandler(DWORD eventType)
{
    if (eventType == CTRL_C_EVENT ||
        eventType == CTRL_BREAK_EVENT ||
        eventType == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_Running, FALSE);
        return TRUE;
    }

    return FALSE;
}

static DWORD ParsePositiveArg(int argc, char* argv[], const char* name, DWORD defaultValue)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (_stricmp(argv[index], name) == 0) {
            const long value = std::strtol(argv[index + 1], nullptr, 10);
            if (value > 0) {
                return static_cast<DWORD>(value);
            }
        }
    }

    return defaultValue;
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

static bool StatesEqual(const DIJOYSTATE2& left, const DIJOYSTATE2& right)
{
    return std::memcmp(&left, &right, sizeof(DIJOYSTATE2)) == 0;
}

static void PrintState(const DIJOYSTATE2& state)
{
    std::printf(
        "lX=%ld lY=%ld lZ=%ld lRx=%ld lRy=%ld lRz=%ld POV0=%lu Buttons0=%u Buttons1=%u\n",
        state.lX,
        state.lY,
        state.lZ,
        state.lRx,
        state.lRy,
        state.lRz,
        state.rgdwPOV[0],
        state.rgbButtons[0],
        state.rgbButtons[1]);
}

static void PrintStateDiff(const DIJOYSTATE2& previous, const DIJOYSTATE2& current)
{
    bool printed = false;

    auto printField = [&](const char* name, LONG oldValue, LONG newValue) {
        if (oldValue == newValue) {
            return;
        }

        if (!printed) {
            std::printf("  diff:");
            printed = true;
        }

        std::printf(" %s=%ld->%ld", name, oldValue, newValue);
    };

    auto printDwordField = [&](const char* name, DWORD oldValue, DWORD newValue) {
        if (oldValue == newValue) {
            return;
        }

        if (!printed) {
            std::printf("  diff:");
            printed = true;
        }

        std::printf(" %s=%lu->%lu", name, oldValue, newValue);
    };

    printField("lX", previous.lX, current.lX);
    printField("lY", previous.lY, current.lY);
    printField("lZ", previous.lZ, current.lZ);
    printField("lRx", previous.lRx, current.lRx);
    printField("lRy", previous.lRy, current.lRy);
    printField("lRz", previous.lRz, current.lRz);
    printDwordField("POV0", previous.rgdwPOV[0], current.rgdwPOV[0]);
    printDwordField("B0", previous.rgbButtons[0], current.rgbButtons[0]);
    printDwordField("B1", previous.rgbButtons[1], current.rgbButtons[1]);

    if (!printed) {
        std::printf("  diff: none");
    }

    std::printf("\n");
}

int main(int argc, char* argv[])
{
    const DWORD durationMs = ParsePositiveArg(argc, argv, "--duration-ms", 45000);
    const DWORD pollMs = ParsePositiveArg(argc, argv, "--poll-ms", 16);

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::fprintf(stderr, "ERROR: CoInitializeEx failed: 0x%08lX\n", hr);
        return 1;
    }

    IDirectInput8W* directInput = nullptr;
    hr = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&directInput), nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "ERROR: DirectInput8Create failed: 0x%08lX\n", hr);
        CoUninitialize();
        return 1;
    }

    DeviceSelection selection = {};
    selection.instance.dwSize = sizeof(selection.instance);
    hr = directInput->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumGameControllerCallback, &selection, DIEDFL_ATTACHEDONLY);
    if (FAILED(hr) || !selection.found) {
        std::fprintf(stderr, "ERROR: No DirectInput game controller found.\n");
        directInput->Release();
        CoUninitialize();
        return 1;
    }

    IDirectInputDevice8W* controller = nullptr;
    hr = directInput->CreateDevice(selection.instance.guidInstance, &controller, nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "ERROR: CreateDevice failed: 0x%08lX\n", hr);
        directInput->Release();
        CoUninitialize();
        return 1;
    }

    hr = controller->SetDataFormat(&c_dfDIJoystick2);
    if (FAILED(hr)) {
        std::fprintf(stderr, "ERROR: SetDataFormat failed: 0x%08lX\n", hr);
        controller->Release();
        directInput->Release();
        CoUninitialize();
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
        return 1;
    }

    hr = controller->Acquire();
    if (FAILED(hr) && hr != DIERR_OTHERAPPHASPRIO) {
        std::fprintf(stderr, "ERROR: Acquire failed: 0x%08lX\n", hr);
        controller->Release();
        directInput->Release();
        CoUninitialize();
        return 1;
    }

    DIJOYSTATE2 baseline = {};
    hr = PollDirectInputState(controller, &baseline);
    if (FAILED(hr)) {
        std::fprintf(stderr, "ERROR: Poll baseline failed: 0x%08lX\n", hr);
        controller->Unacquire();
        controller->Release();
        directInput->Release();
        CoUninitialize();
        return 1;
    }

    std::printf("DirectInput sniffer\n");
    std::printf("Device: %ls\n", selection.instance.tszProductName);
    std::printf("Listening for %lu ms. Spam buttons, triggers, or sticks now.\n\n", durationMs);
    std::printf("[baseline] ");
    PrintState(baseline);

    DIJOYSTATE2 previous = baseline;
    bool sawChange = false;
    const DWORD startTick = GetTickCount();

    while (InterlockedCompareExchange(&g_Running, TRUE, TRUE) != FALSE &&
           GetTickCount() - startTick < durationMs) {
        Sleep(pollMs);

        DIJOYSTATE2 current = {};
        hr = PollDirectInputState(controller, &current);
        if (FAILED(hr)) {
            continue;
        }

        if (StatesEqual(previous, current)) {
            continue;
        }

        sawChange = true;
        std::printf("[%6lu ms] ", GetTickCount() - startTick);
        PrintState(current);
        PrintStateDiff(previous, current);
        previous = current;
    }

    controller->Unacquire();
    controller->Release();
    directInput->Release();
    CoUninitialize();

    if (!sawChange) {
        std::printf("No DirectInput changes observed.\n");
        return 2;
    }

    std::printf("Capture complete.\n");
    return 0;
}
