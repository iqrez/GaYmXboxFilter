/*
 * GaYm Controller CLI - Standalone command-line control tool.
 *
 * Sends IOCTLs to the GaYmFilter driver without running the feeder loop.
 *
 * Usage:
 *   GaYmCLI.exe status                    List devices and override state
 *   GaYmCLI.exe on    [device_index]      Enable override
 *   GaYmCLI.exe off   [device_index]      Disable override
 *   GaYmCLI.exe jitter <min_us> <max_us>  Set timing jitter
 *   GaYmCLI.exe jitter off                Disable jitter
 *   GaYmCLI.exe test  [device_index]      Send a test report (A + left stick right)
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "DeviceHelper.h"

static void PrintUsage()
{
    printf("GaYm Controller CLI v2.0\n\n");
    printf("Usage:\n");
    printf("  GaYmCLI status                     Show all devices and state\n");
    printf("  GaYmCLI on    [device_index]       Enable input override\n");
    printf("  GaYmCLI off   [device_index]       Disable input override\n");
    printf("  GaYmCLI jitter <min_us> <max_us>   Set timing jitter range\n");
    printf("  GaYmCLI jitter off                 Disable jitter\n");
    printf("  GaYmCLI test  [device_index]       Inject a test report\n");
}

static void CmdStatus()
{
    auto devices = EnumerateGaYmDevices();
    if (devices.empty()) {
        printf("No GaYmFilter devices found.\n");
        printf("  The package may be staged but not attached to the live device stack.\n");
        printf("  Run attach_filter.ps1 as Administrator to attach the lower filter to the USB HID stack.\n");
        return;
    }

    printf("Found %zu device(s):\n\n", devices.size());

    for (auto& d : devices) {
        HANDLE h = CreateFileW(d.path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);

        if (h == INVALID_HANDLE_VALUE) {
            printf("  [%d] %ls  (FAILED TO OPEN - error %lu)\n", d.index, d.path.c_str(), GetLastError());
            continue;
        }

        GAYM_DEVICE_INFO info;
        if (QueryDeviceInfo(h, &info)) {
            printf("  [%d] %s  VID:%04X PID:%04X  Override:%s  Reports:%lu\n",
                d.index,
                DeviceTypeName(info.DeviceType),
                info.VendorId, info.ProductId,
                info.OverrideActive ? "ON" : "OFF",
                info.ReportsSent);
        } else {
            printf("  [%d] (query failed - error %lu)\n", d.index, GetLastError());
        }

        CloseHandle(h);
    }
}

static HANDLE OpenDevice(int argc, char* argv[], int argIdx)
{
    int devIdx = 0;
    if (argIdx < argc) devIdx = atoi(argv[argIdx]);

    HANDLE h = OpenGaYmDevice(devIdx);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ERROR: Cannot open device %d (error %lu). Run as Administrator.\n",
            devIdx, GetLastError());
    }
    return h;
}

static void CmdOverride(int argc, char* argv[], bool enable)
{
    HANDLE h = OpenDevice(argc, argv, 2);
    if (h == INVALID_HANDLE_VALUE) return;

    DWORD ioctl = enable ? IOCTL_GAYM_OVERRIDE_ON : IOCTL_GAYM_OVERRIDE_OFF;
    if (SendIoctl(h, ioctl)) {
        printf("Override %s.\n", enable ? "ENABLED" : "DISABLED");
    } else {
        fprintf(stderr, "Failed to %s override (error %lu).\n",
            enable ? "enable" : "disable", GetLastError());
    }
    CloseHandle(h);
}

static void CmdJitter(int argc, char* argv[])
{
    if (argc < 3) {
        PrintUsage();
        return;
    }

    HANDLE h = OpenGaYmDevice(0);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ERROR: Cannot open device (error %lu).\n", GetLastError());
        return;
    }

    GAYM_JITTER_CONFIG jcfg = {};

    if (_stricmp(argv[2], "off") == 0) {
        jcfg.Enabled = FALSE;
        printf("Jitter disabled.\n");
    } else if (argc >= 4) {
        jcfg.Enabled   = TRUE;
        jcfg.MinDelayUs = (ULONG)atoi(argv[2]);
        jcfg.MaxDelayUs = (ULONG)atoi(argv[3]);
        printf("Jitter enabled: %lu - %lu us\n", jcfg.MinDelayUs, jcfg.MaxDelayUs);
    } else {
        PrintUsage();
        CloseHandle(h);
        return;
    }

    if (!SetJitter(h, &jcfg)) {
        fprintf(stderr, "Failed to set jitter (error %lu).\n", GetLastError());
    }
    CloseHandle(h);
}

static void CmdTest(int argc, char* argv[])
{
    HANDLE h = OpenDevice(argc, argv, 2);
    if (h == INVALID_HANDLE_VALUE) return;

    /* Enable override first */
    SendIoctl(h, IOCTL_GAYM_OVERRIDE_ON);

    /* Send a test report: A button + left stick full right */
    GAYM_REPORT report;
    RtlZeroMemory(&report, sizeof(report));
    report.DPad       = GAYM_DPAD_NEUTRAL;
    report.Buttons[0] = GAYM_BTN_A;
    report.ThumbLeftX = 32767;

    if (InjectReport(h, &report)) {
        printf("Test report injected: A + LX=32767\n");
        printf("Holding for 2 seconds...\n");
        Sleep(2000);
    } else {
        fprintf(stderr, "InjectReport failed (error %lu).\n", GetLastError());
    }

    /* Release: send neutral report then disable override */
    RtlZeroMemory(&report, sizeof(report));
    report.DPad = GAYM_DPAD_NEUTRAL;
    InjectReport(h, &report);
    Sleep(50);

    SendIoctl(h, IOCTL_GAYM_OVERRIDE_OFF);
    printf("Override disabled. Test complete.\n");

    CloseHandle(h);
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        PrintUsage();
        return 0;
    }

    const char* cmd = argv[1];

    if      (_stricmp(cmd, "status") == 0) CmdStatus();
    else if (_stricmp(cmd, "on")     == 0) CmdOverride(argc, argv, true);
    else if (_stricmp(cmd, "off")    == 0) CmdOverride(argc, argv, false);
    else if (_stricmp(cmd, "jitter") == 0) CmdJitter(argc, argv);
    else if (_stricmp(cmd, "test")   == 0) CmdTest(argc, argv);
    else {
        fprintf(stderr, "Unknown command: %s\n\n", cmd);
        PrintUsage();
        return 1;
    }

    return 0;
}
