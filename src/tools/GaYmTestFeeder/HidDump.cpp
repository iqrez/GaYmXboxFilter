/*
 * HidDump.cpp - Read HID caps + one raw report from Xbox controller
 * Shows the exact report format that xinputhid expects.
 *
 * Compile: cl /EHsc HidDump.cpp /link hid.lib setupapi.lib
 * Run:     HidDump.exe   (with override OFF! Move a stick when prompted)
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

int main()
{
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (devInfo == INVALID_HANDLE_VALUE) {
        printf("SetupDi failed\n");
        return 1;
    }

    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(ifData);

    BOOL found = FALSE;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData); i++) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &reqSize, NULL);

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(reqSize);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, reqSize, NULL, NULL)) {
            free(detail);
            continue;
        }

        /* Look for VID_045E & PID_02FF & IG_00 */
        if (!wcsstr(detail->DevicePath, L"vid_045e") ||
            !wcsstr(detail->DevicePath, L"pid_02ff")) {
            free(detail);
            continue;
        }

        printf("=== HID Device Found ===\n");
        printf("Path: %ls\n\n", detail->DevicePath);

        HANDLE h = CreateFileW(
            detail->DevicePath,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);

        if (h == INVALID_HANDLE_VALUE) {
            printf("Open failed: %lu (may be locked by xinputhid)\n", GetLastError());
            /* Try read-only without GENERIC_READ */
            h = CreateFileW(
                detail->DevicePath, 0,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                printf("Open (0-access) also failed: %lu\n", GetLastError());
                free(detail);
                continue;
            }
            printf("Opened with 0-access (can get caps but not read)\n");
        }

        /* Get attributes */
        HIDD_ATTRIBUTES attrs = { sizeof(attrs) };
        if (HidD_GetAttributes(h, &attrs)) {
            printf("VID: 0x%04X  PID: 0x%04X  Version: %d\n",
                attrs.VendorID, attrs.ProductID, attrs.VersionNumber);
        }

        /* Get preparsed data + caps */
        PHIDP_PREPARSED_DATA ppd = NULL;
        if (HidD_GetPreparsedData(h, &ppd)) {
            HIDP_CAPS caps;
            if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                printf("\n--- HID Capabilities ---\n");
                printf("UsagePage:              0x%04X\n", caps.UsagePage);
                printf("Usage:                  0x%04X\n", caps.Usage);
                printf("InputReportByteLength:  %u\n", caps.InputReportByteLength);
                printf("OutputReportByteLength: %u\n", caps.OutputReportByteLength);
                printf("FeatureReportByteLength:%u\n", caps.FeatureReportByteLength);
                printf("NumberInputButtonCaps:  %u\n", caps.NumberInputButtonCaps);
                printf("NumberInputValueCaps:   %u\n", caps.NumberInputValueCaps);
                printf("NumberOutputButtonCaps: %u\n", caps.NumberOutputButtonCaps);
                printf("NumberOutputValueCaps:  %u\n", caps.NumberOutputValueCaps);

                /* Button caps */
                if (caps.NumberInputButtonCaps > 0) {
                    USHORT numBtnCaps = caps.NumberInputButtonCaps;
                    HIDP_BUTTON_CAPS* btnCaps = (HIDP_BUTTON_CAPS*)malloc(
                        numBtnCaps * sizeof(HIDP_BUTTON_CAPS));
                    if (btnCaps && HidP_GetButtonCaps(HidP_Input, btnCaps, &numBtnCaps, ppd)
                        == HIDP_STATUS_SUCCESS) {
                        printf("\n--- Input Button Caps (%u) ---\n", numBtnCaps);
                        for (USHORT b = 0; b < numBtnCaps; b++) {
                            printf("  [%u] UsagePage=0x%04X  ReportID=0x%02X  "
                                   "Range=%u-%u  BitField=%u  IsRange=%d\n",
                                b,
                                btnCaps[b].UsagePage,
                                btnCaps[b].ReportID,
                                btnCaps[b].IsRange ? btnCaps[b].Range.UsageMin : btnCaps[b].NotRange.Usage,
                                btnCaps[b].IsRange ? btnCaps[b].Range.UsageMax : btnCaps[b].NotRange.Usage,
                                btnCaps[b].BitField,
                                btnCaps[b].IsRange);
                        }
                    }
                    free(btnCaps);
                }

                /* Value caps */
                if (caps.NumberInputValueCaps > 0) {
                    USHORT numValCaps = caps.NumberInputValueCaps;
                    HIDP_VALUE_CAPS* valCaps = (HIDP_VALUE_CAPS*)malloc(
                        numValCaps * sizeof(HIDP_VALUE_CAPS));
                    if (valCaps && HidP_GetValueCaps(HidP_Input, valCaps, &numValCaps, ppd)
                        == HIDP_STATUS_SUCCESS) {
                        printf("\n--- Input Value Caps (%u) ---\n", numValCaps);
                        for (USHORT v = 0; v < numValCaps; v++) {
                            printf("  [%u] UsagePage=0x%04X  Usage=0x%04X  ReportID=0x%02X\n"
                                   "       BitSize=%u  LogMin=%ld  LogMax=%ld  PhyMin=%ld  PhyMax=%ld\n",
                                v,
                                valCaps[v].UsagePage,
                                valCaps[v].IsRange ? valCaps[v].Range.UsageMin : valCaps[v].NotRange.Usage,
                                valCaps[v].ReportID,
                                valCaps[v].BitSize,
                                valCaps[v].LogicalMin, valCaps[v].LogicalMax,
                                valCaps[v].PhysicalMin, valCaps[v].PhysicalMax);
                        }
                    }
                    free(valCaps);
                }

                /* Try to read one raw report */
                printf("\n--- Reading Raw Input Report ---\n");
                printf("(Move a stick on the controller...)\n");

                BYTE buf[256] = {0};
                DWORD bytesRead = 0;

                /* Set a short timeout via overlapped I/O */
                HANDLE h2 = CreateFileW(
                    detail->DevicePath, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

                if (h2 != INVALID_HANDLE_VALUE) {
                    OVERLAPPED ov = {0};
                    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

                    if (ReadFile(h2, buf, caps.InputReportByteLength, NULL, &ov) ||
                        GetLastError() == ERROR_IO_PENDING)
                    {
                        DWORD waitResult = WaitForSingleObject(ov.hEvent, 5000);
                        if (waitResult == WAIT_OBJECT_0 &&
                            GetOverlappedResult(h2, &ov, &bytesRead, FALSE))
                        {
                            printf("Got %lu bytes:\n", bytesRead);
                            for (DWORD j = 0; j < bytesRead; j++) {
                                printf("  [%2lu] = 0x%02X  (%3u)", j, buf[j], buf[j]);
                                if (j == 0) printf("  <-- Report ID");
                                printf("\n");
                            }

                            /* Interpret as potential formats */
                            printf("\n--- Quick Interpretation ---\n");
                            if (bytesRead >= 14) {
                                /* Try XInput GIP-like format */
                                printf("If GIP-like: Buttons=0x%02X%02X  LT=%u  RT=%u\n",
                                    buf[3], buf[2], buf[4], buf[5]);
                                SHORT lx = *(SHORT*)&buf[6];
                                SHORT ly = *(SHORT*)&buf[8];
                                SHORT rx = *(SHORT*)&buf[10];
                                SHORT ry = *(SHORT*)&buf[12];
                                printf("  LStick(%6d,%6d)  RStick(%6d,%6d)\n", lx, ly, rx, ry);
                            }
                            if (bytesRead >= 16) {
                                /* Try BT HID format */
                                USHORT blx = *(USHORT*)&buf[1];
                                USHORT bly = *(USHORT*)&buf[3];
                                printf("If BT-HID: LX=%u  LY=%u  LT=%u  RT=%u\n",
                                    blx, bly, buf[9], buf[10]);
                            }
                        } else {
                            printf("Read timed out (5s). Is override ON? Turn it OFF first.\n");
                            CancelIo(h2);
                        }
                    } else {
                        printf("ReadFile failed: %lu\n", GetLastError());
                    }
                    CloseHandle(ov.hEvent);
                    CloseHandle(h2);
                } else {
                    printf("Could not open for reading: %lu\n", GetLastError());
                }
            }
            HidD_FreePreparsedData(ppd);
        } else {
            printf("GetPreparsedData failed\n");
        }

        CloseHandle(h);
        free(detail);
        found = TRUE;
        break;
    }

    SetupDiDestroyDeviceInfoList(devInfo);

    if (!found) {
        printf("No Xbox controller (VID_045E PID_02FF) found.\n");
        printf("Is the controller connected via USB?\n");
    }

    return 0;
}
