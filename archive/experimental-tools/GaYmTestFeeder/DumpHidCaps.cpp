#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

static void PrintButtonCaps(const HIDP_BUTTON_CAPS& caps)
{
    const USAGE usageMin = caps.IsRange ? caps.Range.UsageMin : caps.NotRange.Usage;
    const USAGE usageMax = caps.IsRange ? caps.Range.UsageMax : caps.NotRange.Usage;
    const USHORT dataIndexMin = caps.IsRange ? caps.Range.DataIndexMin : caps.NotRange.DataIndex;
    const USHORT dataIndexMax = caps.IsRange ? caps.Range.DataIndexMax : caps.NotRange.DataIndex;

    std::printf(
        "  button reportId=%u usagePage=0x%04X bitField=%u count=%u dataIndex=%u..%u usage=%u..%u absolute=%u\n",
        caps.ReportID,
        caps.UsagePage,
        caps.BitField,
        caps.ReportCount,
        dataIndexMin,
        dataIndexMax,
        usageMin,
        usageMax,
        caps.IsAbsolute);
}

static void PrintValueCaps(const HIDP_VALUE_CAPS& caps)
{
    const USAGE usage = caps.IsRange ? caps.Range.UsageMin : caps.NotRange.Usage;

    std::printf(
        "  value  reportId=%u usagePage=0x%04X usage=%u bitField=%u bits=%u count=%u logical=[%ld,%ld] physical=[%ld,%ld] absolute=%u\n",
        caps.ReportID,
        caps.UsagePage,
        usage,
        caps.BitField,
        caps.BitSize,
        caps.ReportCount,
        caps.LogicalMin,
        caps.LogicalMax,
        caps.PhysicalMin,
        caps.PhysicalMax,
        caps.IsAbsolute);
}

int wmain()
{
    GUID hidGuid = {};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &hidGuid,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "SetupDiGetClassDevsW failed: %lu\n", GetLastError());
        return 1;
    }

    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);

    int exitCode = 1;

    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, index, &ifData); ++index) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &required, nullptr);
        if (required == 0) {
            continue;
        }

        auto detailBuffer = std::vector<BYTE>(required);
        auto detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, required, nullptr, nullptr)) {
            continue;
        }

        if (!wcsstr(detail->DevicePath, L"vid_045e") || !wcsstr(detail->DevicePath, L"pid_02ff")) {
            continue;
        }

        std::wprintf(L"Path: %ls\n", detail->DevicePath);

        HANDLE device = CreateFileW(
            detail->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (device == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "CreateFileW failed: %lu\n", GetLastError());
            continue;
        }

        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (!HidD_GetPreparsedData(device, &preparsed)) {
            std::fprintf(stderr, "HidD_GetPreparsedData failed: %lu\n", GetLastError());
            CloseHandle(device);
            continue;
        }

        HIDP_CAPS caps = {};
        NTSTATUS status = HidP_GetCaps(preparsed, &caps);
        if (status != HIDP_STATUS_SUCCESS) {
            std::fprintf(stderr, "HidP_GetCaps failed: 0x%08lX\n", status);
            HidD_FreePreparsedData(preparsed);
            CloseHandle(device);
            continue;
        }

        std::printf(
            "Caps: usagePage=0x%04X usage=0x%04X input=%u output=%u feature=%u buttonCaps=%u valueCaps=%u\n",
            caps.UsagePage,
            caps.Usage,
            caps.InputReportByteLength,
            caps.OutputReportByteLength,
            caps.FeatureReportByteLength,
            caps.NumberInputButtonCaps,
            caps.NumberInputValueCaps);

        USHORT buttonCapCount = caps.NumberInputButtonCaps;
        std::vector<HIDP_BUTTON_CAPS> buttonCaps(buttonCapCount);
        status = HidP_GetButtonCaps(HidP_Input, buttonCaps.data(), &buttonCapCount, preparsed);
        if (status == HIDP_STATUS_SUCCESS) {
            std::printf("Input button caps (%u):\n", buttonCapCount);
            for (USHORT i = 0; i < buttonCapCount; ++i) {
                PrintButtonCaps(buttonCaps[i]);
            }
        } else {
            std::fprintf(stderr, "HidP_GetButtonCaps failed: 0x%08lX\n", status);
        }

        USHORT valueCapCount = caps.NumberInputValueCaps;
        std::vector<HIDP_VALUE_CAPS> valueCaps(valueCapCount);
        status = HidP_GetValueCaps(HidP_Input, valueCaps.data(), &valueCapCount, preparsed);
        if (status == HIDP_STATUS_SUCCESS) {
            std::printf("Input value caps (%u):\n", valueCapCount);
            for (USHORT i = 0; i < valueCapCount; ++i) {
                PrintValueCaps(valueCaps[i]);
            }
        } else {
            std::fprintf(stderr, "HidP_GetValueCaps failed: 0x%08lX\n", status);
        }

        if (caps.InputReportByteLength > 0 && caps.InputReportByteLength <= 64) {
            BYTE report[64] = {};
            report[0] = 0;

            if (HidD_GetInputReport(device, report, caps.InputReportByteLength)) {
                std::printf("Input report:");
                for (USHORT i = 0; i < caps.InputReportByteLength; ++i) {
                    std::printf(" %02X", report[i]);
                }
                std::printf("\n");
            } else {
                std::fprintf(stderr, "HidD_GetInputReport failed: %lu\n", GetLastError());
            }

            HANDLE readHandle = CreateFileW(
                detail->DevicePath,
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                nullptr);
            if (readHandle != INVALID_HANDLE_VALUE) {
                OVERLAPPED overlapped = {};
                overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (overlapped.hEvent != nullptr) {
                    std::memset(report, 0, sizeof(report));
                    DWORD bytesRead = 0;
                    BOOL readStarted = ReadFile(readHandle, report, caps.InputReportByteLength, nullptr, &overlapped);
                    DWORD readError = GetLastError();

                    if (readStarted || readError == ERROR_IO_PENDING) {
                        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 500);
                        if (waitResult == WAIT_OBJECT_0 &&
                            GetOverlappedResult(readHandle, &overlapped, &bytesRead, FALSE) &&
                            bytesRead != 0) {
                            std::printf("ReadFile report:");
                            for (DWORD i = 0; i < bytesRead; ++i) {
                                std::printf(" %02X", report[i]);
                            }
                            std::printf("\n");
                        } else {
                            CancelIo(readHandle);
                            std::fprintf(stderr, "ReadFile report timed out or failed.\n");
                        }
                    } else {
                        std::fprintf(stderr, "ReadFile start failed: %lu\n", readError);
                    }

                    CloseHandle(overlapped.hEvent);
                }

                CloseHandle(readHandle);
            }
        }

        HidD_FreePreparsedData(preparsed);
        CloseHandle(device);
        exitCode = 0;
        break;
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return exitCode;
}
