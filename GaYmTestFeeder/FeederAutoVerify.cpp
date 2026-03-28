#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <xinput.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "DeviceHelper.h"

#pragma comment(lib, "xinput.lib")

namespace {

constexpr DWORD kLaunchTimeoutMs = 4000;
constexpr DWORD kConditionTimeoutMs = 2500;
constexpr DWORD kShutdownTimeoutMs = 4000;
constexpr DWORD kFeederDurationMs = 4500;
constexpr SHORT kAxisThreshold = 20000;
constexpr SHORT kAxisNeutralThreshold = 4000;
constexpr BYTE kTriggerThreshold = 200;
constexpr BYTE kTriggerNeutralThreshold = 20;

XINPUT_STATE g_BaselineState = {};

struct VerificationStep {
    const char* Label;
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

static std::string BuildTempPath(const char* suffix)
{
    char tempPath[MAX_PATH] = {};
    DWORD length = GetTempPathA(MAX_PATH, tempPath);
    if (length == 0 || length >= MAX_PATH) {
        return std::string(".\\") + suffix;
    }

    char fileName[MAX_PATH] = {};
    std::snprintf(
        fileName,
        sizeof(fileName),
        "%sGaYmFeederAutoVerify_%lu_%s",
        tempPath,
        GetTickCount(),
        suffix);
    return fileName;
}

static bool WriteTextFile(const std::string& path, const std::string& contents)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    file << contents;
    return file.good();
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
    const VerificationStep& step,
    XINPUT_STATE* matchedState)
{
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < step.TimeoutMs) {
        XINPUT_STATE state = {};
        if (XInputGetState(padIndex, &state) == ERROR_SUCCESS && step.Predicate(state)) {
            *matchedState = state;
            return true;
        }
        Sleep(8);
    }

    return false;
}

static bool LaunchFeeder(
    const std::string& feederPath,
    const std::string& configPath,
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
        QuoteArgument(feederPath) + " -p macro -c " + QuoteArgument(configPath) +
        " --duration-ms " + std::to_string(kFeederDurationMs);

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
    DWORD waitResult = WaitForSingleObject(processInfo->hProcess, 500);
    if (waitResult == WAIT_OBJECT_0) {
        CloseHandle(processInfo->hThread);
        CloseHandle(processInfo->hProcess);
        processInfo->hThread = NULL;
        processInfo->hProcess = NULL;
        return;
    }

    SetConsoleCtrlHandler(NULL, TRUE);
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, processInfo->dwProcessId);
    SetConsoleCtrlHandler(NULL, FALSE);

    waitResult = WaitForSingleObject(processInfo->hProcess, kShutdownTimeoutMs);
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

static std::string BuildMacroContents()
{
    return
        "0,0,0,15,0,0,0,0,0,0\n"
        "300,0,0,15,0,0,32767,0,0,0\n"
        "600,0,0,15,0,0,0,0,0,0\n"
        "900,0,0,15,0,0,-32767,0,0,0\n"
        "1200,0,0,15,0,0,0,0,0,0\n"
        "1500,0,0,15,0,0,0,0,0,-32767\n"
        "1800,0,0,15,0,0,0,0,0,0\n"
        "2100,0,0,15,255,0,0,0,0,0\n"
        "2400,0,0,15,0,0,0,0,0,0\n"
        "2700,1,0,15,0,0,0,0,0,0\n"
        "3000,0,0,15,0,0,0,0,0,0\n";
}

static std::string BuildConfigContents(const std::string& macroPath)
{
    return
        "; Generated by FeederAutoVerify\n"
        "[General]\n"
        "Provider = macro\n"
        "PollRateHz = 125\n"
        "DeviceIndex = 0\n"
        "\n"
        "[Jitter]\n"
        "Enabled = false\n"
        "MinUs = 0\n"
        "MaxUs = 0\n"
        "\n"
        "[Network]\n"
        "BindAddr = 127.0.0.1\n"
        "Port = 43210\n"
        "\n"
        "[Macros]\n"
        "File = " + macroPath + "\n" +
        "Loop = false\n";
}

} // namespace

int main()
{
    std::printf("GaYmFeeder AutoVerify\n\n");

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

    const std::string macroPath = BuildTempPath("macro.csv");
    const std::string configPath = BuildTempPath("config.ini");
    if (!WriteTextFile(macroPath, BuildMacroContents()) ||
        !WriteTextFile(configPath, BuildConfigContents(macroPath))) {
        std::fprintf(stderr, "ERROR: Failed to write temp macro/config files.\n");
        DeleteFileA(macroPath.c_str());
        DeleteFileA(configPath.c_str());
        CloseHandle(device);
        return 1;
    }

    const std::string feederPath = GetModuleDirectory() + "\\GaYmFeeder.exe";
    PROCESS_INFORMATION processInfo = {};
    if (!LaunchFeeder(feederPath, configPath, &processInfo)) {
        std::fprintf(stderr, "ERROR: Failed to launch GaYmFeeder.exe (error %lu)\n", GetLastError());
        DeleteFileA(macroPath.c_str());
        DeleteFileA(configPath.c_str());
        CloseHandle(device);
        return 1;
    }

    bool allPassed = true;
    if (!WaitForOverrideOn(device, kLaunchTimeoutMs)) {
        std::fprintf(stderr, "ERROR: Feeder did not enable override in time.\n");
        allPassed = false;
    }

    const VerificationStep steps[] = {
        { "Left stick right", kConditionTimeoutMs, IsLeftStickRight },
        { "Neutral",          kConditionTimeoutMs, IsNeutralState },
        { "Left stick left",  kConditionTimeoutMs, IsLeftStickLeft },
        { "Neutral",          kConditionTimeoutMs, IsNeutralState },
        { "Right stick up",   kConditionTimeoutMs, IsRightStickUp },
        { "Neutral",          kConditionTimeoutMs, IsNeutralState },
        { "Left trigger",     kConditionTimeoutMs, IsLeftTriggerFull },
        { "Neutral",          kConditionTimeoutMs, IsNeutralState },
        { "Button A",         kConditionTimeoutMs, IsButtonA },
    };

    if (allPassed) {
        std::printf("\n=== Verification ===\n");
        for (const VerificationStep& step : steps) {
            XINPUT_STATE matchedState = {};
            const bool matched = WaitForCondition(padIndex, step, &matchedState);
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
        }
    }

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

    DeleteFileA(macroPath.c_str());
    DeleteFileA(configPath.c_str());
    CloseHandle(device);

    std::printf("\nResult: %s\n", allPassed ? "PASS" : "FAIL");
    return allPassed ? 0 : 2;
}
