#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gaym_client_compat.h"

using gaym::client::DeviceTypeName;
using gaym::client::DisableOverride;
using gaym::client::EnableOverride;
using gaym::client::InjectReport;
using gaym::client::OpenSupportedAdapter;
using gaym::client::QueryAdapterInfo;
using gaym::client::QueryObservation;
using gaym::client::AcquireWriterSession;
using gaym::client::ReleaseWriterSession;

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

struct HidObservation {
    bool  sawChange = false;
    DWORD reportsRead = 0;
    DWORD firstDiffOffset = 0;
    DWORD reportLength = 0;
    BYTE  baseline[64] = {};
    BYTE  changed[64] = {};
};

static void InitNeutralReport(GAYM_REPORT* report)
{
    std::memset(report, 0, sizeof(*report));
    report->DPad = GAYM_DPAD_NEUTRAL;
}

static void PrintObservation(const GAYM_OBSERVATION_V1& observation)
{
    std::printf(
        "Observation: family=%lu caps=0x%08lX status=0x%08lX obs=%llu inj=%llu%s\n",
        observation.AdapterFamily,
        observation.CapabilityFlags,
        observation.StatusFlags,
        observation.LastObservedSequence,
        observation.LastInjectedSequence,
        (observation.StatusFlags & GAYM_STATUS_OBSERVATION_SYNTHETIC) ? " synthetic" : "");
}

static void SendNeutralBurst(HANDLE device, DWORD durationMs)
{
    GAYM_REPORT neutral = {};
    InitNeutralReport(&neutral);

    DWORD start = GetTickCount();
    while (GetTickCount() - start < durationMs) {
        InjectReport(device, &neutral);
        Sleep(8);
    }
}

static HANDLE OpenXboxHidDevice(USHORT* inputReportLength)
{
    GUID hidGuid = {};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }

    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);

    HANDLE result = INVALID_HANDLE_VALUE;

    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, idx, &ifData); ++idx) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &needed, NULL);
        if (needed == 0) {
            continue;
        }

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
            reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(std::malloc(needed));
        if (!detail) {
            continue;
        }

        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, needed, NULL, NULL)) {
            std::free(detail);
            continue;
        }

        if (!wcsstr(detail->DevicePath, L"vid_045e") || !wcsstr(detail->DevicePath, L"pid_02ff")) {
            std::free(detail);
            continue;
        }

        HANDLE metadataHandle = CreateFileW(
            detail->DevicePath,
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL);

        HIDP_CAPS caps = {};
        NTSTATUS hidStatus = HIDP_STATUS_INVALID_PREPARSED_DATA;
        if (metadataHandle != INVALID_HANDLE_VALUE) {
            PHIDP_PREPARSED_DATA preparsed = NULL;
            if (HidD_GetPreparsedData(metadataHandle, &preparsed)) {
                hidStatus = HidP_GetCaps(preparsed, &caps);
                HidD_FreePreparsedData(preparsed);
            }
            CloseHandle(metadataHandle);
        }

        HANDLE hid = CreateFileW(
            detail->DevicePath,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            NULL);

        std::free(detail);

        if (hid == INVALID_HANDLE_VALUE) {
            continue;
        }

        if (hidStatus == HIDP_STATUS_SUCCESS &&
            caps.InputReportByteLength > 0 &&
            caps.InputReportByteLength <= 64) {
            *inputReportLength = caps.InputReportByteLength;
        } else {
            *inputReportLength = 64;
        }

        result = hid;
        break;
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

static bool ReadHidReport(HANDLE hid, BYTE* buffer, DWORD length, DWORD timeoutMs)
{
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent) {
        return false;
    }

    DWORD bytesRead = 0;
    BOOL readStarted = ReadFile(hid, buffer, length, NULL, &overlapped);
    DWORD lastError = GetLastError();

    bool success = false;
    if (readStarted || lastError == ERROR_IO_PENDING) {
        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, timeoutMs);
        if (waitResult == WAIT_OBJECT_0) {
            success = GetOverlappedResult(hid, &overlapped, &bytesRead, FALSE) != FALSE &&
                bytesRead > 0;
        } else {
            CancelIo(hid);
        }
    }

    CloseHandle(overlapped.hEvent);
    return success;
}

static bool CaptureBaselineReport(HANDLE device, HANDLE hid, DWORD reportLength, BYTE* baseline)
{
    BYTE discard[64] = {};

    SendNeutralBurst(device, 200);
    ReadHidReport(hid, discard, reportLength, 250);
    return ReadHidReport(hid, baseline, reportLength, 800);
}

int main()
{
    std::printf("GaYm Xbox 02FF QuickVerifyHid\n\n");

    HANDLE device = OpenSupportedAdapter(0);
    if (device == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "ERROR: Cannot open supported adapter (error %lu)\n", GetLastError());
        return 1;
    }

    bool overrideEnabled = false;
    bool writerHeld = false;
    GAYM_REPORT neutral = {};
    InitNeutralReport(&neutral);

    auto cleanup = [&]() {
        if (device != INVALID_HANDLE_VALUE) {
            if (overrideEnabled) {
                InjectReport(device, &neutral);
                Sleep(50);
                DisableOverride(device);
                overrideEnabled = false;
            }

            if (writerHeld) {
                ReleaseWriterSession(device);
                writerHeld = false;
            }

            CloseHandle(device);
            device = INVALID_HANDLE_VALUE;
        }
    };

    GAYM_DEVICE_INFO before = {};
    if (!QueryAdapterInfo(device, &before)) {
        std::fprintf(stderr, "ERROR: IOCTL_GAYM_QUERY_DEVICE failed (error %lu)\n", GetLastError());
        cleanup();
        return 1;
    }

    std::printf(
        "Device: %s VID:%04X PID:%04X Override:%s Reports:%lu\n",
        DeviceTypeName(before.DeviceType),
        before.VendorId,
        before.ProductId,
        before.OverrideActive ? "ON" : "OFF",
        before.ReportsSent);

    GAYM_OBSERVATION_V1 observation = {};
    if (QueryObservation(device, &observation)) {
        PrintObservation(observation);
    } else {
        std::printf("Observation: unavailable (error %lu)\n", GetLastError());
    }
    std::printf("Control: producer path prefers \\\\.\\GaYmXInputFilterCtl; diagnostics may fall back to \\\\.\\GaYmFilterCtl\n");

    if (!AcquireWriterSession(device)) {
        std::fprintf(stderr, "ERROR: Failed to acquire writer session (error %lu)\n", GetLastError());
        cleanup();
        return 1;
    }
    writerHeld = true;

    USHORT reportLength = 0;
    HANDLE hid = OpenXboxHidDevice(&reportLength);
    if (hid == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "ERROR: No readable HID path for VID_045E PID_02FF.\n");
        cleanup();
        return 1;
    }

    BYTE baseline[64] = {};
    bool haveBaseline = CaptureBaselineReport(device, hid, reportLength, baseline);

    if (!EnableOverride(device)) {
        std::fprintf(stderr, "ERROR: Failed to enable override (error %lu)\n", GetLastError());
        CloseHandle(hid);
        cleanup();
        return 1;
    }
    overrideEnabled = true;

    GAYM_REPORT report = {};
    InitNeutralReport(&report);
    report.Buttons[0] = GAYM_BTN_A;
    report.ThumbLeftX = 32767;

    HidObservation observed = {};
    observed.reportLength = reportLength;
    if (haveBaseline) {
        std::memcpy(observed.baseline, baseline, reportLength);
    }

    DWORD start = GetTickCount();
    BYTE current[64] = {};

    while (GetTickCount() - start < 1200) {
        if (!InjectReport(device, &report)) {
            std::fprintf(stderr, "ERROR: InjectReport failed (error %lu)\n", GetLastError());
            CloseHandle(hid);
            cleanup();
            return 1;
        }

        if (ReadHidReport(hid, current, reportLength, 40)) {
            observed.reportsRead++;
            if (!haveBaseline || std::memcmp(current, baseline, reportLength) != 0) {
                observed.sawChange = true;
                std::memcpy(observed.changed, current, reportLength);

                if (haveBaseline) {
                    for (DWORD i = 0; i < reportLength; ++i) {
                        if (current[i] != baseline[i]) {
                            observed.firstDiffOffset = i;
                            break;
                        }
                    }
                }
                break;
            }
        }

        Sleep(8);
    }

    GAYM_DEVICE_INFO after = {};
    bool haveAfter = QueryAdapterInfo(device, &after);

    CloseHandle(hid);
    cleanup();

    if (!haveAfter) {
        std::fprintf(stderr, "ERROR: Post-test IOCTL_GAYM_QUERY_DEVICE failed (error %lu)\n", GetLastError());
        return 1;
    }

    LONG reportDelta = (LONG)after.ReportsSent - (LONG)before.ReportsSent;

    std::printf(
        "Observed: reportsRead=%lu changed=%s firstDiff=%lu ReportsDelta=%ld\n",
        observed.reportsRead,
        observed.sawChange ? "YES" : "NO",
        observed.firstDiffOffset,
        reportDelta);

    if (haveBaseline) {
        std::printf("Baseline:");
        for (DWORD i = 0; i < reportLength; ++i) {
            std::printf(" %02X", observed.baseline[i]);
        }
        std::printf("\n");
    } else {
        std::printf("Baseline: unavailable\n");
    }

    if (observed.sawChange) {
        std::printf("Changed :");
        for (DWORD i = 0; i < reportLength; ++i) {
            std::printf(" %02X", observed.changed[i]);
        }
        std::printf("\n");
    }

    bool pass = observed.sawChange && reportDelta > 0;
    std::printf("\nResult: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 2;
}
