/*
 * GaYm Controller CLI - Standalone command-line control tool.
 *
 * Sends IOCTLs through the gaym_client boundary for the supported Xbox 02FF
 * adapter without running the feeder loop.
 *
 * Usage:
 *   GaYmCLI.exe status                    Show supported adapter state
 *   GaYmCLI.exe on    [adapter_slot]      Enable override
 *   GaYmCLI.exe off   [adapter_slot]      Disable override
 *   GaYmCLI.exe jitter <min_us> <max_us>  Set timing jitter
 *   GaYmCLI.exe jitter off                Disable jitter
 *   GaYmCLI.exe test  [adapter_slot]      Send a test report (A + left stick right)
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

#include "gaym_client_compat.h"

using gaym::client::ConfigureJitter;
using gaym::client::DisableOverride;
using gaym::client::DeviceTypeName;
using gaym::client::EnableOverride;
using gaym::client::EnumerateSupportedAdapters;
using gaym::client::InjectReport;
using gaym::client::OpenSupportedAdapter;
using gaym::client::QueryAdapterInfo;
using gaym::client::QueryObservation;
using gaym::client::AcquireWriterSession;
using gaym::client::ReleaseWriterSession;

static void PrintUsage()
{
    printf("GaYm Controller CLI v2.0\n\n");
    printf("Usage:\n");
    printf("  GaYmCLI status                     Show supported adapter state\n");
    printf("  GaYmCLI on    [adapter_slot]       Enable input override\n");
    printf("  GaYmCLI off   [adapter_slot]       Disable input override\n");
    printf("  GaYmCLI jitter <min_us> <max_us>   Set timing jitter range\n");
    printf("  GaYmCLI jitter off                 Disable jitter\n");
    printf("  GaYmCLI test  [adapter_slot]       Inject a test report\n");
    printf("\nNote: \\\\.\\GaYmXInputFilterCtl is the sole control path.\n");
}

static void PrintObservation(const GAYM_OBSERVATION_V1& observation)
{
    printf("    Observation: family=%lu caps=0x%08lX status=0x%08lX seq(obs=%llu inj=%llu)%s\n",
        observation.AdapterFamily,
        observation.CapabilityFlags,
        observation.StatusFlags,
        observation.LastObservedSequence,
        observation.LastInjectedSequence,
        (observation.StatusFlags & GAYM_STATUS_OBSERVATION_SYNTHETIC) ? " synthetic" : "");
}

static bool WaitForOverrideState(HANDLE device, bool expectedEnabled, DWORD timeoutMs)
{
    DWORD start = GetTickCount();

    while (GetTickCount() - start < timeoutMs) {
        GAYM_DEVICE_INFO info = {};
        if (QueryAdapterInfo(device, &info) && info.OverrideActive == (expectedEnabled ? TRUE : FALSE)) {
            return true;
        }

        Sleep(25);
    }

    SetLastError(WAIT_TIMEOUT);
    return false;
}

static int CmdStatus()
{
    auto devices = EnumerateSupportedAdapters();
    if (devices.empty()) {
        printf("No supported Xbox 02FF adapter found.\n");
        printf("  The package may be staged but not attached to the live device stack.\n");
        printf("  Run scripts\\install-driver.ps1 -Configuration Release as Administrator to bind the staged upper/lower packages to the live stack.\n");
        printf("  The authoritative control path is \\\\.\\GaYmXInputFilterCtl.\n");
        return 0;
    }

    printf("Found %zu supported adapter%s:\n\n", devices.size(), (devices.size() == 1) ? "" : "s");

    int exitCode = 0;
    for (auto& d : devices) {
        HANDLE h = CreateFileW(d.path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);

        if (h == INVALID_HANDLE_VALUE) {
            printf("  [%d] %ls  (FAILED TO OPEN - error %lu)\n", d.index, d.path.c_str(), GetLastError());
            exitCode = 1;
            continue;
        }

        GAYM_DEVICE_INFO info;
        if (QueryAdapterInfo(h, &info)) {
            printf("  [%d] %s  VID:%04X PID:%04X  Override:%s  Reports:%lu\n",
                d.index,
                DeviceTypeName(info.DeviceType),
                info.VendorId, info.ProductId,
                info.OverrideActive ? "ON" : "OFF",
                info.ReportsSent);
            printf("    Counters: pending=%lu queued=%lu completed=%lu forwarded=%lu read=%lu devctl=%lu internal=%lu write=%lu lastIoctl=0x%08lX\n",
                info.PendingInputRequests,
                info.QueuedInputRequests,
                info.CompletedInputRequests,
                info.ForwardedInputRequests,
                info.ReadRequestsSeen,
                info.DeviceControlRequestsSeen,
                info.InternalDeviceControlRequestsSeen,
                info.WriteRequestsSeen,
                info.LastInterceptedIoctl);
            printf("    Last request: type=%lu in=%lu out=%lu\n",
                info.LastRequestType,
                info.LastRequestInputLength,
                info.LastRequestOutputLength);
            GAYM_OBSERVATION_V1 observation = {};
            if (QueryObservation(h, &observation)) {
                PrintObservation(observation);
            } else {
                printf("    Observation: unavailable (error %lu)\n", GetLastError());
            }
        } else {
            printf("  [%d] (query failed - error %lu)\n", d.index, GetLastError());
            exitCode = 1;
        }

        CloseHandle(h);
    }

    return exitCode;
}

static HANDLE OpenDevice(int argc, char* argv[], int argIdx)
{
    int devIdx = 0;
    if (argIdx < argc) devIdx = atoi(argv[argIdx]);

    HANDLE h = OpenSupportedAdapter(devIdx);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ERROR: Cannot open device %d (error %lu). Run as Administrator.\n",
            devIdx, GetLastError());
    }
    return h;
}

static int CmdOverride(int argc, char* argv[], bool enable)
{
    HANDLE h = OpenDevice(argc, argv, 2);
    if (h == INVALID_HANDLE_VALUE) return 1;

    printf("Control path: \\\\.\\GaYmXInputFilterCtl (sole control plane).\n");
    if (!AcquireWriterSession(h)) {
        fprintf(stderr, "Failed to acquire writer session (error %lu).\n", GetLastError());
        CloseHandle(h);
        return 1;
    }

    int exitCode = 0;
    if (enable ? EnableOverride(h) : DisableOverride(h)) {
        printf("Override %s.\n", enable ? "ENABLED" : "DISABLED");
    } else {
        fprintf(stderr, "Failed to %s override (error %lu).\n",
            enable ? "enable" : "disable", GetLastError());
        exitCode = 1;
    }

    if (!ReleaseWriterSession(h)) {
        fprintf(stderr, "Failed to release writer session (error %lu).\n", GetLastError());
        exitCode = 1;
    }
    CloseHandle(h);
    return exitCode;
}

static int CmdJitter(int argc, char* argv[])
{
    if (argc < 3) {
        PrintUsage();
        return 1;
    }

    HANDLE h = OpenSupportedAdapter(0);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ERROR: Cannot open device (error %lu).\n", GetLastError());
        return 1;
    }

    printf("Control path: \\\\.\\GaYmXInputFilterCtl (sole control plane).\n");
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
        return 1;
    }

    int exitCode = 0;
    if (!ConfigureJitter(h, &jcfg)) {
        fprintf(stderr, "Failed to set jitter (error %lu).\n", GetLastError());
        exitCode = 1;
    }
    CloseHandle(h);
    return exitCode;
}

static int CmdTest(int argc, char* argv[])
{
    HANDLE h = OpenDevice(argc, argv, 2);
    if (h == INVALID_HANDLE_VALUE) return 1;

    printf("Control path: \\\\.\\GaYmXInputFilterCtl (sole control plane).\n");
    if (!AcquireWriterSession(h)) {
        fprintf(stderr, "Failed to acquire writer session (error %lu).\n", GetLastError());
        CloseHandle(h);
        return 1;
    }

    int exitCode = 0;
    /* Enable override first */
    if (!EnableOverride(h)) {
        fprintf(stderr, "Failed to enable override (error %lu).\n", GetLastError());
        ReleaseWriterSession(h);
        CloseHandle(h);
        return 1;
    }

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
        exitCode = 1;
    }

    /* Release: send neutral report then disable override */
    RtlZeroMemory(&report, sizeof(report));
    report.DPad = GAYM_DPAD_NEUTRAL;
    if (!InjectReport(h, &report)) {
        fprintf(stderr, "Neutral report injection failed (error %lu).\n", GetLastError());
        exitCode = 1;
    }
    Sleep(50);

    if (!DisableOverride(h)) {
        fprintf(stderr, "Failed to disable override (error %lu).\n", GetLastError());
        exitCode = 1;
    } else if (!WaitForOverrideState(h, false, 2000)) {
        fprintf(stderr, "Override did not settle OFF before timeout (error %lu).\n", GetLastError());
        exitCode = 1;
    }
    if (!ReleaseWriterSession(h)) {
        fprintf(stderr, "Failed to release writer session (error %lu).\n", GetLastError());
        exitCode = 1;
    }
    printf("Override disabled. Test complete.\n");

    CloseHandle(h);
    return exitCode;
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        PrintUsage();
        return 0;
    }

    const char* cmd = argv[1];

    if      (_stricmp(cmd, "status") == 0) return CmdStatus();
    else if (_stricmp(cmd, "on")     == 0) return CmdOverride(argc, argv, true);
    else if (_stricmp(cmd, "off")    == 0) return CmdOverride(argc, argv, false);
    else if (_stricmp(cmd, "jitter") == 0) return CmdJitter(argc, argv);
    else if (_stricmp(cmd, "test")   == 0) return CmdTest(argc, argv);
    else {
        fprintf(stderr, "Unknown command: %s\n\n", cmd);
        PrintUsage();
        return 1;
    }
}
