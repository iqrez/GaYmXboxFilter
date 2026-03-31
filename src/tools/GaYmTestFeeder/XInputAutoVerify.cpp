#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <xinput.h>

#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "xinput.lib")

struct ObserverResult {
    bool anyPadAvailable = false;
    bool sawPacketChange = false;
    bool sawButtonA = false;
    bool sawTrigger = false;
    bool sawAxis = false;
    DWORD bestPadIndex = 0;
    DWORD baselinePacket = 0;
    DWORD finalPacket = 0;
    SHORT baselineLX = 0;
    SHORT minLX = 0;
    SHORT maxLX = 0;
    BYTE baselineLT = 0;
    BYTE maxLT = 0;
    WORD baselineButtons = 0;
    WORD observedButtons = 0;
};

static bool BuildSiblingToolPath(const wchar_t* selfPath, const wchar_t* toolName, wchar_t* siblingPath, size_t cchSiblingPath)
{
    wchar_t* slash = nullptr;

    if (wcscpy_s(siblingPath, cchSiblingPath, selfPath) != 0) {
        return false;
    }

    slash = wcsrchr(siblingPath, L'\\');
    if (slash == nullptr) {
        return false;
    }

    slash[1] = L'\0';
    return wcsncat_s(siblingPath, cchSiblingPath, toolName, _TRUNCATE) == 0;
}

static bool BuildSiblingDirectory(const wchar_t* selfPath, wchar_t* siblingDirectory, size_t cchSiblingDirectory)
{
    wchar_t* slash = nullptr;

    if (wcscpy_s(siblingDirectory, cchSiblingDirectory, selfPath) != 0) {
        return false;
    }

    slash = wcsrchr(siblingDirectory, L'\\');
    if (slash == nullptr) {
        return false;
    }

    *slash = L'\0';
    return true;
}

static bool LaunchSiblingCommand(const wchar_t* commandLineTemplate, DWORD deviceIndex, PROCESS_INFORMATION* processInfo)
{
    wchar_t selfPath[MAX_PATH] = {};
    wchar_t workingDirectory[MAX_PATH] = {};
    wchar_t cliPath[MAX_PATH] = {};
    wchar_t commandLine[1024] = {};
    STARTUPINFOW startupInfo = {};

    if (processInfo == nullptr) {
        return false;
    }

    if (GetModuleFileNameW(NULL, selfPath, ARRAYSIZE(selfPath)) == 0) {
        return false;
    }
    if (!BuildSiblingToolPath(selfPath, L"GaYmCLI.exe", cliPath, ARRAYSIZE(cliPath))) {
        return false;
    }
    if (!BuildSiblingDirectory(selfPath, workingDirectory, ARRAYSIZE(workingDirectory))) {
        return false;
    }

    if (_snwprintf_s(
            commandLine,
            ARRAYSIZE(commandLine),
            _TRUNCATE,
            commandLineTemplate,
            cliPath,
            deviceIndex) <= 0) {
        return false;
    }

    startupInfo.cb = sizeof(startupInfo);
    ZeroMemory(processInfo, sizeof(*processInfo));
    return CreateProcessW(
        NULL,
        commandLine,
        NULL,
        NULL,
        FALSE,
        0,
        NULL,
        workingDirectory,
        &startupInfo,
        processInfo) != FALSE;
}

static bool LaunchGaYmCliTest(DWORD deviceIndex, PROCESS_INFORMATION* processInfo)
{
    return LaunchSiblingCommand(L"\"%ls\" test %lu", deviceIndex, processInfo);
}

static void BestEffortRecoverOff(DWORD deviceIndex)
{
    PROCESS_INFORMATION processInfo = {};

    if (!LaunchSiblingCommand(L"\"%ls\" off %lu", deviceIndex, &processInfo)) {
        return;
    }

    WaitForSingleObject(processInfo.hProcess, 5000);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    Sleep(2000);
}

static bool CaptureAnyBaseline(XINPUT_STATE baselineStates[XUSER_MAX_COUNT], bool available[XUSER_MAX_COUNT])
{
    bool anyAvailable = false;

    std::memset(baselineStates, 0, sizeof(XINPUT_STATE) * XUSER_MAX_COUNT);
    std::memset(available, 0, sizeof(bool) * XUSER_MAX_COUNT);

    for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
        XINPUT_STATE state = {};
        if (XInputGetState(index, &state) == ERROR_SUCCESS) {
            baselineStates[index] = state;
            available[index] = true;
            anyAvailable = true;
        }
    }

    return anyAvailable;
}

static void SeedBestResult(ObserverResult* result, DWORD padIndex, const XINPUT_STATE& baseline)
{
    result->anyPadAvailable = true;
    result->bestPadIndex = padIndex;
    result->baselinePacket = baseline.dwPacketNumber;
    result->finalPacket = baseline.dwPacketNumber;
    result->baselineLX = baseline.Gamepad.sThumbLX;
    result->minLX = baseline.Gamepad.sThumbLX;
    result->maxLX = baseline.Gamepad.sThumbLX;
    result->baselineLT = baseline.Gamepad.bLeftTrigger;
    result->maxLT = baseline.Gamepad.bLeftTrigger;
    result->baselineButtons = baseline.Gamepad.wButtons;
    result->observedButtons = baseline.Gamepad.wButtons;
}

static void ObserveAllPads(DWORD timeoutMs, ObserverResult* result)
{
    XINPUT_STATE baselineStates[XUSER_MAX_COUNT];
    bool available[XUSER_MAX_COUNT];
    DWORD start = GetTickCount();

    if (!CaptureAnyBaseline(baselineStates, available)) {
        return;
    }

    for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
        if (available[index]) {
            SeedBestResult(result, index, baselineStates[index]);
            break;
        }
    }

    while (GetTickCount() - start < timeoutMs) {
        for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
            XINPUT_STATE current = {};
            if (XInputGetState(index, &current) != ERROR_SUCCESS) {
                continue;
            }

            if (!result->anyPadAvailable) {
                SeedBestResult(result, index, current);
            }

            result->anyPadAvailable = true;

            if (current.dwPacketNumber != baselineStates[index].dwPacketNumber ||
                std::memcmp(&current.Gamepad, &baselineStates[index].Gamepad, sizeof(current.Gamepad)) != 0) {
                result->bestPadIndex = index;
                result->baselinePacket = baselineStates[index].dwPacketNumber;
                result->finalPacket = current.dwPacketNumber;
                result->baselineLX = baselineStates[index].Gamepad.sThumbLX;
                result->baselineLT = baselineStates[index].Gamepad.bLeftTrigger;
                result->baselineButtons = baselineStates[index].Gamepad.wButtons;
                result->observedButtons |= current.Gamepad.wButtons;
                result->sawPacketChange = (current.dwPacketNumber != baselineStates[index].dwPacketNumber);

                if (current.Gamepad.sThumbLX < result->minLX) {
                    result->minLX = current.Gamepad.sThumbLX;
                }
                if (current.Gamepad.sThumbLX > result->maxLX) {
                    result->maxLX = current.Gamepad.sThumbLX;
                }
                if (current.Gamepad.bLeftTrigger > result->maxLT) {
                    result->maxLT = current.Gamepad.bLeftTrigger;
                }
                if ((current.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0) {
                    result->sawButtonA = true;
                }
                if ((result->maxLX - result->baselineLX) >= 12000 ||
                    (result->baselineLX - result->minLX) >= 12000) {
                    result->sawAxis = true;
                }
                if (result->maxLT >= result->baselineLT + 64) {
                    result->sawTrigger = true;
                }
            }
        }

        Sleep(16);
    }
}

int wmain(int argc, wchar_t* argv[])
{
    DWORD deviceIndex = 0;
    DWORD childExitCode = STILL_ACTIVE;
    PROCESS_INFORMATION processInfo = {};
    ObserverResult result = {};

    if (argc >= 2) {
        deviceIndex = (DWORD)_wtoi(argv[1]);
    }

    std::printf("GaYm Xbox 02FF XInputAutoVerify\n\n");

    if (!LaunchGaYmCliTest(deviceIndex, &processInfo)) {
        std::fprintf(stderr, "ERROR: Failed to launch GaYmCLI child (error %lu)\n", GetLastError());
        return 1;
    }

    ObserveAllPads(7000, &result);

    WaitForSingleObject(processInfo.hProcess, 10000);
    if (!GetExitCodeProcess(processInfo.hProcess, &childExitCode)) {
        childExitCode = 1;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    if (childExitCode != 0) {
        BestEffortRecoverOff(deviceIndex);
    } else {
        Sleep(2000);
    }

    std::printf(
        "Observer: available=%s pad=%lu pkt=%lu->%lu LX=%d->[%d,%d] LT=%u->%u Btn=0x%04X->0x%04X child=%lu\n",
        result.anyPadAvailable ? "yes" : "no",
        result.bestPadIndex,
        result.baselinePacket,
        result.finalPacket,
        result.baselineLX,
        result.minLX,
        result.maxLX,
        (unsigned)result.baselineLT,
        (unsigned)result.maxLT,
        result.baselineButtons,
        result.observedButtons,
        childExitCode);

    if (childExitCode != 0) {
        std::printf("Result: CHILD_FAILED\n");
        return 4;
    }
    if (!result.anyPadAvailable) {
        std::printf("Result: SESSION_UNAVAILABLE\n");
        return 2;
    }
    if (result.sawAxis || result.sawTrigger || result.sawButtonA || result.sawPacketChange) {
        std::printf("Result: XINPUT_PASS\n");
        return 0;
    }

    std::printf("Result: XINPUT_STATIC\n");
    return 3;
}
