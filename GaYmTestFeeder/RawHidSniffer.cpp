#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

struct SnifferOptions {
    DWORD durationMs = 45000;
    DWORD timeoutMs = 250;
};

struct HidTarget {
    std::wstring path;
    USHORT inputReportBytes = 64;
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

static void PrintHex(const BYTE* bytes, DWORD length)
{
    for (DWORD index = 0; index < length; ++index) {
        std::printf("%02X", bytes[index]);
    }
}

static void PrintDiff(const BYTE* oldBytes, DWORD oldLength, const BYTE* newBytes, DWORD newLength)
{
    const DWORD maxLength = std::max(oldLength, newLength);
    bool printed = false;

    for (DWORD index = 0; index < maxLength; ++index) {
        const BYTE oldValue = index < oldLength ? oldBytes[index] : 0;
        const BYTE newValue = index < newLength ? newBytes[index] : 0;

        if (index < oldLength && index < newLength && oldValue == newValue) {
            continue;
        }

        if (!printed) {
            std::printf("  diff:");
            printed = true;
        }

        std::printf(" [%lu]=%02X->%02X", index, oldValue, newValue);
    }

    if (!printed) {
        std::printf("  diff: none");
    }

    std::printf("\n");
}

static bool TryGetInputReportLength(HANDLE device, USHORT* inputReportBytes)
{
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!HidD_GetPreparsedData(device, &preparsed)) {
        return false;
    }

    HIDP_CAPS caps = {};
    const NTSTATUS status = HidP_GetCaps(preparsed, &caps);
    HidD_FreePreparsedData(preparsed);
    if (status != HIDP_STATUS_SUCCESS || caps.InputReportByteLength == 0) {
        return false;
    }

    *inputReportBytes = caps.InputReportByteLength;
    return true;
}

static bool TryOpenHidTarget(HidTarget* target)
{
    GUID hidGuid = {};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &hidGuid,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return false;
    }

    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);

    bool found = false;

    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, index, &ifData); ++index) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &needed, nullptr);
        if (needed == 0) {
            continue;
        }

        std::vector<BYTE> detailBuffer(needed);
        auto detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, needed, nullptr, nullptr)) {
            continue;
        }

        if (wcsstr(detail->DevicePath, L"vid_045e") == nullptr ||
            wcsstr(detail->DevicePath, L"pid_02ff") == nullptr ||
            wcsstr(detail->DevicePath, L"ig_00") == nullptr) {
            continue;
        }

        HANDLE device = CreateFileW(
            detail->DevicePath,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            continue;
        }

        USHORT inputReportBytes = 64;
        if (!TryGetInputReportLength(device, &inputReportBytes)) {
            inputReportBytes = 64;
        }

        CloseHandle(device);

        target->path = detail->DevicePath;
        target->inputReportBytes = inputReportBytes;
        found = true;
        break;
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

static bool ReadReport(HANDLE hid, BYTE* buffer, DWORD capacity, DWORD* bytesRead, DWORD timeoutMs)
{
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        return false;
    }

    *bytesRead = 0;

    const BOOL readStarted = ReadFile(hid, buffer, capacity, nullptr, &overlapped);
    const DWORD lastError = GetLastError();
    bool success = false;

    if (readStarted || lastError == ERROR_IO_PENDING) {
        const DWORD waitResult = WaitForSingleObject(overlapped.hEvent, timeoutMs);
        if (waitResult == WAIT_OBJECT_0) {
            success = GetOverlappedResult(hid, &overlapped, bytesRead, FALSE) != FALSE && *bytesRead != 0;
        } else {
            CancelIo(hid);
        }
    }

    CloseHandle(overlapped.hEvent);
    return success;
}

static bool PollInputReport(HANDLE hid, BYTE* buffer, DWORD capacity, DWORD* bytesRead)
{
    if (capacity == 0) {
        return false;
    }

    std::memset(buffer, 0, capacity);
    buffer[0] = 0;
    if (!HidD_GetInputReport(hid, buffer, capacity)) {
        return false;
    }

    *bytesRead = capacity;
    return true;
}

int main(int argc, char* argv[])
{
    const SnifferOptions options = {
        ParsePositiveArg(argc, argv, "--duration-ms", 45000),
        ParsePositiveArg(argc, argv, "--timeout-ms", 250),
    };

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    HidTarget target = {};
    if (!TryOpenHidTarget(&target)) {
        std::fprintf(stderr, "ERROR: No readable Xbox HID target (VID_045E PID_02FF IG_00) found.\n");
        return 1;
    }

    HANDLE hid = CreateFileW(
        target.path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);
    if (hid == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "ERROR: Cannot open HID target (error %lu)\n", GetLastError());
        return 1;
    }

    HANDLE hidPolling = CreateFileW(
        target.path.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    std::vector<BYTE> previous(target.inputReportBytes);
    std::vector<BYTE> current(target.inputReportBytes);
    DWORD previousLength = 0;
    bool haveBaseline = false;
    bool sawChange = false;

    std::printf("Raw HID sniffer\n");
    std::printf("Path: %ls\n", target.path.c_str());
    std::printf("InputReportBytes: %u\n", target.inputReportBytes);
    std::printf("Listening for %lu ms. Spam buttons, triggers, or sticks now.\n\n", options.durationMs);

    const DWORD startTick = GetTickCount();
    while (InterlockedCompareExchange(&g_Running, TRUE, TRUE) != FALSE &&
           GetTickCount() - startTick < options.durationMs) {
        DWORD bytesRead = 0;
        bool haveReport = ReadReport(hid, current.data(), static_cast<DWORD>(current.size()), &bytesRead, options.timeoutMs);
        if (!haveReport && hidPolling != INVALID_HANDLE_VALUE) {
            haveReport = PollInputReport(hidPolling, current.data(), static_cast<DWORD>(current.size()), &bytesRead);
        }

        if (!haveReport) {
            continue;
        }

        if (!haveBaseline) {
            previousLength = bytesRead;
            std::memcpy(previous.data(), current.data(), bytesRead);
            haveBaseline = true;

            std::printf("[baseline] len=%lu report=", bytesRead);
            PrintHex(current.data(), bytesRead);
            std::printf("\n");
            continue;
        }

        if (previousLength == bytesRead &&
            std::memcmp(previous.data(), current.data(), bytesRead) == 0) {
            continue;
        }

        sawChange = true;
        std::printf("[%6lu ms] len=%lu report=", GetTickCount() - startTick, bytesRead);
        PrintHex(current.data(), bytesRead);
        std::printf("\n");
        PrintDiff(previous.data(), previousLength, current.data(), bytesRead);

        previousLength = bytesRead;
        std::memcpy(previous.data(), current.data(), bytesRead);
    }

    CloseHandle(hid);
    if (hidPolling != INVALID_HANDLE_VALUE) {
        CloseHandle(hidPolling);
    }

    if (!haveBaseline) {
        std::printf("No reports were captured.\n");
        return 2;
    }

    if (!sawChange) {
        std::printf("No report changes observed.\n");
        return 3;
    }

    std::printf("Capture complete.\n");
    return 0;
}
