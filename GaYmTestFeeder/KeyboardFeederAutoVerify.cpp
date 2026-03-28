#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <xinput.h>

#include <cstdio>
#include <string>

#include "DeviceHelper.h"

#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr DWORD kLaunchTimeoutMs = 4000;
constexpr DWORD kConditionTimeoutMs = 2500;
constexpr DWORD kShutdownTimeoutMs = 4000;
constexpr DWORD kFeederDurationMs = 7000;
constexpr DWORD kForegroundRetries = 20;
constexpr DWORD kForegroundRetryDelayMs = 50;
constexpr SHORT kAxisThreshold = 20000;
constexpr SHORT kAxisNeutralThreshold = 4000;
constexpr BYTE kTriggerThreshold = 200;
constexpr BYTE kTriggerNeutralThreshold = 20;

XINPUT_STATE g_BaselineState = {};

struct VerificationStep {
    const char* Label;
    int VirtualKey;
    DWORD TimeoutMs;
    bool (*Predicate)(const XINPUT_STATE&);
};

static std::string GetModuleDirectory()
{
    char modulePath[MAX_PATH] = {};
    DWORD length = GetModuleFileNameA(NULL, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return ".";
    }

    std::string path(modulePath, length);
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) {
        return ".";
    }

    return path.substr(0, slash);
}

static std::string QuoteArgument(const std::string& argument)
{
    std::string quoted = "\"";
    for (char character : argument) {
        if (character == '"') {
            quoted += "\\\"";
        } else {
            quoted += character;
        }
    }
    quoted += "\"";
    return quoted;
}

static bool TryGetPadState(DWORD* index, XINPUT_STATE* state)
{
    for (DWORD padIndex = 0; padIndex < XUSER_MAX_COUNT; ++padIndex) {
        XINPUT_STATE current = {};
        if (XInputGetState(padIndex, &current) == ERROR_SUCCESS) {
            *index = padIndex;
            *state = current;
            return true;
        }
    }

    return false;
}

static bool WaitForPad(DWORD timeoutMs, DWORD* index, XINPUT_STATE* state)
{
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        if (TryGetPadState(index, state)) {
            return true;
        }
        Sleep(25);
    }

    return false;
}

static bool IsNeutralState(const XINPUT_STATE& state)
{
    const XINPUT_GAMEPAD& gamepad = state.Gamepad;
    const XINPUT_GAMEPAD& baseline = g_BaselineState.Gamepad;
    return
        gamepad.sThumbLX >= baseline.sThumbLX - kAxisNeutralThreshold &&
        gamepad.sThumbLX <= baseline.sThumbLX + kAxisNeutralThreshold &&
        gamepad.sThumbLY >= baseline.sThumbLY - kAxisNeutralThreshold &&
        gamepad.sThumbLY <= baseline.sThumbLY + kAxisNeutralThreshold &&
        gamepad.sThumbRX >= baseline.sThumbRX - kAxisNeutralThreshold &&
        gamepad.sThumbRX <= baseline.sThumbRX + kAxisNeutralThreshold &&
        gamepad.sThumbRY >= baseline.sThumbRY - kAxisNeutralThreshold &&
        gamepad.sThumbRY <= baseline.sThumbRY + kAxisNeutralThreshold &&
        gamepad.bLeftTrigger <= (BYTE)(baseline.bLeftTrigger + kTriggerNeutralThreshold) &&
        gamepad.bRightTrigger <= (BYTE)(baseline.bRightTrigger + kTriggerNeutralThreshold) &&
        gamepad.wButtons == baseline.wButtons;
}

static bool IsLeftStickRight(const XINPUT_STATE& state)
{
    return (state.Gamepad.sThumbLX - g_BaselineState.Gamepad.sThumbLX) >= kAxisThreshold;
}

static bool IsLeftStickLeft(const XINPUT_STATE& state)
{
    return (g_BaselineState.Gamepad.sThumbLX - state.Gamepad.sThumbLX) >= kAxisThreshold;
}

static bool IsRightStickUp(const XINPUT_STATE& state)
{
    return (g_BaselineState.Gamepad.sThumbRY - state.Gamepad.sThumbRY) >= kAxisThreshold;
}

static bool IsLeftTriggerFull(const XINPUT_STATE& state)
{
    const int delta = (int)state.Gamepad.bLeftTrigger - (int)g_BaselineState.Gamepad.bLeftTrigger;
    return state.Gamepad.bLeftTrigger >= kTriggerThreshold || delta >= kTriggerThreshold;
}

static bool IsButtonA(const XINPUT_STATE& state)
{
    const WORD buttonMask = XINPUT_GAMEPAD_A;
    return ((state.Gamepad.wButtons & buttonMask) != 0) &&
        ((g_BaselineState.Gamepad.wButtons & buttonMask) == 0 || state.dwPacketNumber != g_BaselineState.dwPacketNumber);
}

static void PrintPadState(const XINPUT_STATE& state)
{
    const XINPUT_GAMEPAD& gamepad = state.Gamepad;
    std::printf(
        "pkt=%lu LX=%6d LY=%6d RX=%6d RY=%6d LT=%3u RT=%3u Btn=0x%04X\n",
        state.dwPacketNumber,
        gamepad.sThumbLX,
        gamepad.sThumbLY,
        gamepad.sThumbRX,
        gamepad.sThumbRY,
        static_cast<unsigned>(gamepad.bLeftTrigger),
        static_cast<unsigned>(gamepad.bRightTrigger),
        gamepad.wButtons);
}

static bool WaitForOverrideOn(HANDLE device, DWORD timeoutMs)
{
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        GAYM_DEVICE_INFO info = {};
        if (QueryDeviceInfo(device, &info) && info.OverrideActive) {
            return true;
        }
        Sleep(25);
    }

    return false;
}

static bool WaitForCondition(
    DWORD padIndex,
    DWORD timeoutMs,
    bool (*predicate)(const XINPUT_STATE&),
    XINPUT_STATE* matchedState)
{
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        XINPUT_STATE state = {};
        if (XInputGetState(padIndex, &state) == ERROR_SUCCESS && predicate(state)) {
            *matchedState = state;
            return true;
        }
        Sleep(8);
    }

    return false;
}

static bool LaunchFeeder(
    const std::string& feederPath,
    PROCESS_INFORMATION* processInfo)
{
    SECURITY_ATTRIBUTES securityAttributes = {};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    HANDLE nulHandle = CreateFileA(
        "NUL",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &securityAttributes,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (nulHandle == INVALID_HANDLE_VALUE) {
        return false;
    }

    STARTUPINFOA startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = nulHandle;
    startupInfo.hStdOutput = nulHandle;
    startupInfo.hStdError = nulHandle;

    std::string commandLine =
        QuoteArgument(feederPath) + " -p keyboard --duration-ms " + std::to_string(kFeederDurationMs);

    const std::string workingDirectory = GetModuleDirectory();
    const BOOL created = CreateProcessA(
        NULL,
        commandLine.data(),
        NULL,
        NULL,
        TRUE,
        CREATE_NEW_PROCESS_GROUP,
        NULL,
        workingDirectory.c_str(),
        &startupInfo,
        processInfo);

    CloseHandle(nulHandle);
    return created != FALSE;
}

static void StopFeeder(PROCESS_INFORMATION* processInfo, HANDLE device)
{
    const DWORD earlyWait = WaitForSingleObject(processInfo->hProcess, 500);
    if (earlyWait == WAIT_OBJECT_0) {
        CloseHandle(processInfo->hThread);
        CloseHandle(processInfo->hProcess);
        processInfo->hThread = NULL;
        processInfo->hProcess = NULL;
        return;
    }

    SetConsoleCtrlHandler(NULL, TRUE);
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, processInfo->dwProcessId);
    SetConsoleCtrlHandler(NULL, FALSE);

    DWORD waitResult = WaitForSingleObject(processInfo->hProcess, kShutdownTimeoutMs);
    if (waitResult != WAIT_OBJECT_0) {
        SendIoctl(device, IOCTL_GAYM_OVERRIDE_OFF);
        TerminateProcess(processInfo->hProcess, 1);
        WaitForSingleObject(processInfo->hProcess, 1000);
    }

    CloseHandle(processInfo->hThread);
    CloseHandle(processInfo->hProcess);
    processInfo->hThread = NULL;
    processInfo->hProcess = NULL;
}

static void SendVirtualKey(int virtualKey, bool keyUp)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(virtualKey);
    if (keyUp) {
        input.ki.dwFlags = KEYEVENTF_KEYUP;
    }

    SendInput(1, &input, sizeof(input));
}

static void ReleaseAllTestKeys()
{
    const int keys[] = { 'A', 'D', 'Z', VK_UP, VK_SPACE };
    for (int key : keys) {
        SendVirtualKey(key, true);
    }
}

static LRESULT CALLBACK FocusWindowProc(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(windowHandle, message, wParam, lParam);
}

static HWND CreateFocusWindow(HINSTANCE instanceHandle)
{
    const wchar_t* className = L"GaYmKeyboardAutoVerifyWindow";

    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = FocusWindowProc;
    windowClass.hInstance = instanceHandle;
    windowClass.lpszClassName = className;

    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return NULL;
    }

    HWND windowHandle = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        className,
        L"GaYm Keyboard AutoVerify",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        100,
        100,
        360,
        120,
        NULL,
        NULL,
        instanceHandle,
        NULL);

    if (!windowHandle) {
        return NULL;
    }

    ShowWindow(windowHandle, SW_SHOWNORMAL);
    UpdateWindow(windowHandle);
    return windowHandle;
}

static bool FocusWindow(HWND windowHandle)
{
    for (DWORD attempt = 0; attempt < kForegroundRetries; ++attempt) {
        ShowWindow(windowHandle, SW_SHOWNORMAL);
        BringWindowToTop(windowHandle);
        SetForegroundWindow(windowHandle);
        SetFocus(windowHandle);

        if (GetForegroundWindow() == windowHandle) {
            return true;
        }

        Sleep(kForegroundRetryDelayMs);
    }

    return GetForegroundWindow() == windowHandle;
}

static bool RunStep(
    DWORD padIndex,
    HWND focusWindow,
    const VerificationStep& step,
    XINPUT_STATE* matchedState)
{
    if (!FocusWindow(focusWindow)) {
        std::fprintf(stderr, "WARNING: Could not move focus to the keyboard test window.\n");
    }

    SendVirtualKey(step.VirtualKey, false);
    const bool matched = WaitForCondition(padIndex, step.TimeoutMs, step.Predicate, matchedState);
    SendVirtualKey(step.VirtualKey, true);
    return matched;
}

} // namespace

int main()
{
    std::printf("GaYm Keyboard Feeder AutoVerify\n\n");

    HANDLE device = OpenGaYmDevice(0);
    if (device == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "ERROR: Cannot open GaYm filter device (error %lu)\n", GetLastError());
        return 1;
    }

    DWORD padIndex = 0;
    XINPUT_STATE baseline = {};
    if (!WaitForPad(2000, &padIndex, &baseline)) {
        std::fprintf(stderr, "ERROR: No XInput controller is visible.\n");
        CloseHandle(device);
        return 1;
    }

    std::printf("Monitoring XInput pad %lu\n", padIndex);
    std::printf("Baseline: ");
    PrintPadState(baseline);
    g_BaselineState = baseline;

    const std::string feederPath = GetModuleDirectory() + "\\GaYmFeeder.exe";
    PROCESS_INFORMATION processInfo = {};
    if (!LaunchFeeder(feederPath, &processInfo)) {
        std::fprintf(stderr, "ERROR: Failed to launch GaYmFeeder.exe (error %lu)\n", GetLastError());
        CloseHandle(device);
        return 1;
    }

    bool allPassed = true;
    if (!WaitForOverrideOn(device, kLaunchTimeoutMs)) {
        std::fprintf(stderr, "ERROR: Feeder did not enable override in time.\n");
        StopFeeder(&processInfo, device);
        CloseHandle(device);
        return 1;
    }

    HWND focusWindow = CreateFocusWindow(GetModuleHandleW(NULL));
    if (!focusWindow) {
        std::fprintf(stderr, "ERROR: Failed to create keyboard focus window (error %lu)\n", GetLastError());
        StopFeeder(&processInfo, device);
        CloseHandle(device);
        return 1;
    }

    const VerificationStep steps[] = {
        { "Left stick right", 'D',      kConditionTimeoutMs, IsLeftStickRight },
        { "Left stick left",  'A',      kConditionTimeoutMs, IsLeftStickLeft },
        { "Right stick up",   VK_UP,    kConditionTimeoutMs, IsRightStickUp },
        { "Left trigger",     'Z',      kConditionTimeoutMs, IsLeftTriggerFull },
        { "Button A",         VK_SPACE, kConditionTimeoutMs, IsButtonA },
    };

    std::printf("\n=== Verification ===\n");
    for (const VerificationStep& step : steps) {
        XINPUT_STATE matchedState = {};
        const bool matched = RunStep(padIndex, focusWindow, step, &matchedState);
        std::printf("  %-16s : %s | ", step.Label, matched ? "PASS" : "FAIL");
        if (matched) {
            PrintPadState(matchedState);
        } else {
            XINPUT_STATE currentState = {};
            if (XInputGetState(padIndex, &currentState) == ERROR_SUCCESS) {
                PrintPadState(currentState);
            } else {
                std::printf("pad unavailable\n");
            }
            allPassed = false;
            break;
        }

        XINPUT_STATE neutralState = {};
        const bool neutral = WaitForCondition(
            padIndex,
            kConditionTimeoutMs,
            IsNeutralState,
            &neutralState);
        std::printf("  %-16s : %s | ", "Neutral", neutral ? "PASS" : "FAIL");
        if (neutral) {
            PrintPadState(neutralState);
        } else {
            XINPUT_STATE currentState = {};
            if (XInputGetState(padIndex, &currentState) == ERROR_SUCCESS) {
                PrintPadState(currentState);
            } else {
                std::printf("pad unavailable\n");
            }
            allPassed = false;
            break;
        }
    }

    ReleaseAllTestKeys();
    DestroyWindow(focusWindow);

    WaitForSingleObject(processInfo.hProcess, kFeederDurationMs + 1000);
    StopFeeder(&processInfo, device);

    GAYM_DEVICE_INFO finalInfo = {};
    if (QueryDeviceInfo(device, &finalInfo)) {
        std::printf(
            "\nFinal driver state: Override:%s Reports:%lu DevCtl:%lu Done:%lu\n",
            finalInfo.OverrideActive ? "ON" : "OFF",
            finalInfo.ReportsSent,
            finalInfo.DeviceControlRequestsSeen,
            finalInfo.CompletedInputRequests);
        if (finalInfo.OverrideActive) {
            std::fprintf(stderr, "ERROR: Override remained enabled after feeder shutdown.\n");
            allPassed = false;
        }
    } else {
        std::fprintf(stderr, "WARNING: Could not query final device state (error %lu)\n", GetLastError());
        allPassed = false;
    }

    CloseHandle(device);

    std::printf("\nResult: %s\n", allPassed ? "PASS" : "FAIL");
    return allPassed ? 0 : 2;
}
