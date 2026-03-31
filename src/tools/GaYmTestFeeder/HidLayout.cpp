/*
 * HidLayout.cpp - Construct HID reports via API to discover exact byte layout
 * Uses HidP_SetUsageValue to place known values, then dumps raw bytes.
 *
 * Compile: cl /EHsc HidLayout.cpp /link hid.lib setupapi.lib
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

static void DumpBytes(const char* label, const BYTE* buf, DWORD len)
{
    printf("%s (%lu bytes):\n", label, len);
    for (DWORD i = 0; i < len; i++) {
        printf("  [%2lu] = 0x%02X  (%5u)", i, buf[i], buf[i]);
        if (i == 0) printf("  <-- Report ID");
        printf("\n");
    }
    printf("\n");
}

int main()
{
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(ifData);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData); i++) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &reqSize, NULL);

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(reqSize);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, reqSize, NULL, NULL);

        if (!wcsstr(detail->DevicePath, L"vid_045e") ||
            !wcsstr(detail->DevicePath, L"pid_02ff")) {
            free(detail);
            continue;
        }

        printf("Found: %ls\n\n", detail->DevicePath);

        /* Open with zero access (just need preparsed data) */
        HANDLE h = CreateFileW(detail->DevicePath, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);

        if (h == INVALID_HANDLE_VALUE) {
            printf("Open failed: %lu\n", GetLastError());
            free(detail);
            continue;
        }

        PHIDP_PREPARSED_DATA ppd = NULL;
        if (!HidD_GetPreparsedData(h, &ppd)) {
            printf("GetPreparsedData failed\n");
            CloseHandle(h);
            free(detail);
            continue;
        }

        HIDP_CAPS caps;
        HidP_GetCaps(ppd, &caps);
        ULONG reportLen = caps.InputReportByteLength;
        printf("InputReportByteLength = %lu\n\n", reportLen);

        BYTE* report = (BYTE*)calloc(1, reportLen);

        /* === Test 1: All zeros (baseline) === */
        memset(report, 0, reportLen);
        HidP_InitializeReportForID(HidP_Input, 0, ppd, (PCHAR)report, reportLen);
        DumpBytes("BASELINE (all zeros, initialized)", report, reportLen);

        /* === Test 2: Set Left Stick X = 0xAAAA === */
        memset(report, 0, reportLen);
        HidP_InitializeReportForID(HidP_Input, 0, ppd, (PCHAR)report, reportLen);
        NTSTATUS st = HidP_SetUsageValue(HidP_Input, 0x01, 0, 0x30, 0xAAAA, ppd, (PCHAR)report, reportLen);
        printf("SetUsageValue(X=0xAAAA) = 0x%08lX\n", st);
        DumpBytes("Left Stick X = 0xAAAA", report, reportLen);

        /* === Test 3: Set Left Stick Y = 0xBBBB === */
        memset(report, 0, reportLen);
        HidP_InitializeReportForID(HidP_Input, 0, ppd, (PCHAR)report, reportLen);
        st = HidP_SetUsageValue(HidP_Input, 0x01, 0, 0x31, 0xBBBB, ppd, (PCHAR)report, reportLen);
        printf("SetUsageValue(Y=0xBBBB) = 0x%08lX\n", st);
        DumpBytes("Left Stick Y = 0xBBBB", report, reportLen);

        /* === Test 4: Set Right Stick X (Rx) = 0xCCCC === */
        memset(report, 0, reportLen);
        HidP_InitializeReportForID(HidP_Input, 0, ppd, (PCHAR)report, reportLen);
        st = HidP_SetUsageValue(HidP_Input, 0x01, 0, 0x33, 0xCCCC, ppd, (PCHAR)report, reportLen);
        printf("SetUsageValue(Rx=0xCCCC) = 0x%08lX\n", st);
        DumpBytes("Right Stick X = 0xCCCC", report, reportLen);

        /* === Test 5: Set Right Stick Y (Ry) = 0xDDDD === */
        memset(report, 0, reportLen);
        HidP_InitializeReportForID(HidP_Input, 0, ppd, (PCHAR)report, reportLen);
        st = HidP_SetUsageValue(HidP_Input, 0x01, 0, 0x34, 0xDDDD, ppd, (PCHAR)report, reportLen);
        printf("SetUsageValue(Ry=0xDDDD) = 0x%08lX\n", st);
        DumpBytes("Right Stick Y = 0xDDDD", report, reportLen);

        /* === Test 6: Set Z axis (triggers) = 0xEEEE === */
        memset(report, 0, reportLen);
        HidP_InitializeReportForID(HidP_Input, 0, ppd, (PCHAR)report, reportLen);
        st = HidP_SetUsageValue(HidP_Input, 0x01, 0, 0x32, 0xEEEE, ppd, (PCHAR)report, reportLen);
        printf("SetUsageValue(Z=0xEEEE) = 0x%08lX\n", st);
        DumpBytes("Z axis (triggers) = 0xEEEE", report, reportLen);

        /* === Test 7: Set Hat switch = 3 === */
        memset(report, 0, reportLen);
        HidP_InitializeReportForID(HidP_Input, 0, ppd, (PCHAR)report, reportLen);
        st = HidP_SetUsageValue(HidP_Input, 0x01, 0, 0x39, 3, ppd, (PCHAR)report, reportLen);
        printf("SetUsageValue(Hat=3) = 0x%08lX\n", st);
        DumpBytes("Hat switch = 3", report, reportLen);

        /* === Test 8: Set buttons 1 + 5 + 16 === */
        memset(report, 0, reportLen);
        HidP_InitializeReportForID(HidP_Input, 0, ppd, (PCHAR)report, reportLen);
        USAGE usages[] = { 1, 5, 16 };
        ULONG numUsages = 3;
        st = HidP_SetUsages(HidP_Input, 0x09, 0, usages, &numUsages, ppd, (PCHAR)report, reportLen);
        printf("SetUsages(Btn 1,5,16) = 0x%08lX\n", st);
        DumpBytes("Buttons 1 + 5 + 16", report, reportLen);

        /* === Test 9: FULL REPORT - everything at once === */
        memset(report, 0, reportLen);
        HidP_InitializeReportForID(HidP_Input, 0, ppd, (PCHAR)report, reportLen);
        HidP_SetUsageValue(HidP_Input, 0x01, 0, 0x30, 0x1111, ppd, (PCHAR)report, reportLen); /* LX */
        HidP_SetUsageValue(HidP_Input, 0x01, 0, 0x31, 0x2222, ppd, (PCHAR)report, reportLen); /* LY */
        HidP_SetUsageValue(HidP_Input, 0x01, 0, 0x33, 0x3333, ppd, (PCHAR)report, reportLen); /* RX */
        HidP_SetUsageValue(HidP_Input, 0x01, 0, 0x34, 0x4444, ppd, (PCHAR)report, reportLen); /* RY */
        HidP_SetUsageValue(HidP_Input, 0x01, 0, 0x32, 0x5555, ppd, (PCHAR)report, reportLen); /* Z  */
        HidP_SetUsageValue(HidP_Input, 0x01, 0, 0x39, 5, ppd, (PCHAR)report, reportLen);       /* Hat */
        USAGE allBtns[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
        ULONG allBtnCount = 16;
        HidP_SetUsages(HidP_Input, 0x09, 0, allBtns, &allBtnCount, ppd, (PCHAR)report, reportLen);
        DumpBytes("FULL REPORT (all axes + all buttons)", report, reportLen);

        free(report);
        HidD_FreePreparsedData(ppd);
        CloseHandle(h);
        free(detail);
        break;
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return 0;
}
