/*
 * QuickVerify.cpp - bounded end-to-end override verification.
 *
 * Enables override, injects a known report for a bounded window, observes the
 * XInput state in-process, then always disables override before exit.
 *
 * Compile:
 *   cl /EHsc /W4 /DWIN32_LEAN_AND_MEAN /Fe:QuickVerify.exe QuickVerify.cpp /link xinput.lib
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <xinput.h>

#include <cstdio>
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

#pragma comment(lib, "xinput.lib")

struct ObservedState {
    DWORD baselinePacket;
    DWORD finalPacket;
    SHORT baselineLX;
    SHORT maxLX;
    SHORT minLX;
    BYTE baselineLT;
    BYTE maxLT;
    WORD baselineButtons;
    WORD observedButtons;
    DWORD samples;
    bool sawPacketChange;
    bool sawButtonA;
    bool sawAnyStateChange;
};

enum class XInputProbeMode {
    Unavailable,
    Static,
    Reflected
};

static void PrintDeviceCounters(const char* label, const GAYM_DEVICE_INFO* info)
{
    std::printf(
        "%s: Reports=%lu Pending=%lu Queued=%lu Completed=%lu Forwarded=%lu LastIoctl=0x%08X Read=%lu DevCtl=%lu Internal=%lu Write=%lu\n",
        label,
        info->ReportsSent,
        info->PendingInputRequests,
        info->QueuedInputRequests,
        info->CompletedInputRequests,
        info->ForwardedInputRequests,
        info->LastInterceptedIoctl,
        info->ReadRequestsSeen,
        info->DeviceControlRequestsSeen,
        info->InternalDeviceControlRequestsSeen,
        info->WriteRequestsSeen);
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

static void InitReport(GAYM_REPORT* report)
{
    std::memset(report, 0, sizeof(*report));
    report->DPad = GAYM_DPAD_NEUTRAL;
}

static bool WaitForPad(DWORD timeoutMs, DWORD* index, XINPUT_STATE* state)
{
    DWORD start = GetTickCount();

    while (GetTickCount() - start < timeoutMs) {
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE current = {};
            if (XInputGetState(i, &current) == ERROR_SUCCESS) {
                *index = i;
                *state = current;
                return true;
            }
        }

        Sleep(25);
    }

    return false;
}

static XInputProbeMode ProbeXInputAvailability(DWORD timeoutMs, DWORD* index, XINPUT_STATE* state)
{
    DWORD start = GetTickCount();
    XINPUT_STATE baseline = {};
    DWORD baselineIndex = 0;

    if (!WaitForPad(timeoutMs, &baselineIndex, &baseline)) {
        return XInputProbeMode::Unavailable;
    }

    *index = baselineIndex;
    *state = baseline;

    start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        XINPUT_STATE current = {};
        if (XInputGetState(baselineIndex, &current) != ERROR_SUCCESS) {
            Sleep(25);
            continue;
        }

        if (current.dwPacketNumber != baseline.dwPacketNumber ||
            std::memcmp(&current.Gamepad, &baseline.Gamepad, sizeof(current.Gamepad)) != 0) {
            *state = current;
            return XInputProbeMode::Reflected;
        }

        Sleep(25);
    }

    return XInputProbeMode::Static;
}

int main()
{
    std::printf("GaYm Xbox 02FF QuickVerify\n\n");

    HANDLE device = OpenSupportedAdapter(0);
    if (device == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "ERROR: Cannot open supported adapter (error %lu)\n", GetLastError());
        return 1;
    }

    bool overrideEnabled = false;
    bool writerHeld = false;
    GAYM_REPORT neutral = {};
    InitReport(&neutral);

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
    PrintDeviceCounters("Before", &before);

    GAYM_OBSERVATION_V1 observation = {};
    if (QueryObservation(device, &observation)) {
        PrintObservation(observation);
    } else {
        std::printf("Observation: unavailable (error %lu)\n", GetLastError());
    }
    std::printf("Control: sole control path is \\\\.\\GaYmXInputFilterCtl\n");

    if (!AcquireWriterSession(device)) {
        std::fprintf(stderr, "ERROR: Failed to acquire writer session (error %lu)\n", GetLastError());
        cleanup();
        return 1;
    }
    writerHeld = true;

    DWORD padIndex = 0;
    XINPUT_STATE baseline = {};
    XInputProbeMode probeMode = ProbeXInputAvailability(1500, &padIndex, &baseline);
    if (probeMode == XInputProbeMode::Unavailable) {
        std::fprintf(stderr, "ERROR: No XInput pad became available within 1500 ms.\n");
        cleanup();
        return 1;
    }

    std::printf(
        "Baseline: pad=%lu pkt=%lu LX=%d Buttons=0x%04X\n",
        padIndex,
        baseline.dwPacketNumber,
        baseline.Gamepad.sThumbLX,
        baseline.Gamepad.wButtons);
    if (probeMode == XInputProbeMode::Static) {
        std::printf("XInput probe: pad visible, but baseline remained static during the probe window.\n");
    } else {
        std::printf("XInput probe: pad visible and changing before injection.\n");
    }

    if (!EnableOverride(device)) {
        std::fprintf(stderr, "ERROR: Failed to enable override (error %lu)\n", GetLastError());
        cleanup();
        return 1;
    }
    overrideEnabled = true;

    GAYM_REPORT report = {};
    InitReport(&report);
    report.Buttons[0] = GAYM_BTN_A;
    report.ThumbLeftX = 32767;

    ObservedState observed = {};
    observed.baselinePacket = baseline.dwPacketNumber;
    observed.finalPacket = baseline.dwPacketNumber;
    observed.baselineLX = baseline.Gamepad.sThumbLX;
    observed.maxLX = baseline.Gamepad.sThumbLX;
    observed.minLX = baseline.Gamepad.sThumbLX;
    observed.baselineLT = baseline.Gamepad.bLeftTrigger;
    observed.maxLT = baseline.Gamepad.bLeftTrigger;
    observed.baselineButtons = baseline.Gamepad.wButtons;
    observed.observedButtons = baseline.Gamepad.wButtons;

    Sleep(250);

    DWORD start = GetTickCount();
    while (GetTickCount() - start < 3200) {
        if (!InjectReport(device, &report)) {
            std::fprintf(stderr, "ERROR: InjectReport failed (error %lu)\n", GetLastError());
            cleanup();
            return 1;
        }

        XINPUT_STATE current = {};
        if (XInputGetState(padIndex, &current) == ERROR_SUCCESS) {
            observed.samples++;
            observed.finalPacket = current.dwPacketNumber;
            observed.observedButtons |= current.Gamepad.wButtons;

            if (current.dwPacketNumber != observed.baselinePacket) {
                observed.sawPacketChange = true;
            }
            if ((current.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0) {
                observed.sawButtonA = true;
            }
            if (current.Gamepad.sThumbLX > observed.maxLX) {
                observed.maxLX = current.Gamepad.sThumbLX;
            }
            if (current.Gamepad.sThumbLX < observed.minLX) {
                observed.minLX = current.Gamepad.sThumbLX;
            }
            if (current.Gamepad.bLeftTrigger > observed.maxLT) {
                observed.maxLT = current.Gamepad.bLeftTrigger;
            }
            if (current.dwPacketNumber != observed.baselinePacket ||
                current.Gamepad.sThumbLX != observed.baselineLX ||
                current.Gamepad.wButtons != observed.baselineButtons ||
                current.Gamepad.bLeftTrigger != observed.baselineLT) {
                observed.sawAnyStateChange = true;
            }
        }

        Sleep(16);
    }

    GAYM_DEVICE_INFO after = {};
    if (!QueryAdapterInfo(device, &after)) {
        std::fprintf(stderr, "ERROR: Post-test IOCTL_GAYM_QUERY_DEVICE failed (error %lu)\n", GetLastError());
        cleanup();
        return 1;
    }

    cleanup();

    LONG reportDelta = (LONG)after.ReportsSent - (LONG)before.ReportsSent;
    LONG pendingDelta = (LONG)after.PendingInputRequests - (LONG)before.PendingInputRequests;
    LONG queuedDelta = (LONG)after.QueuedInputRequests - (LONG)before.QueuedInputRequests;
    LONG completedDelta = (LONG)after.CompletedInputRequests - (LONG)before.CompletedInputRequests;
    LONG forwardedDelta = (LONG)after.ForwardedInputRequests - (LONG)before.ForwardedInputRequests;
    LONG readDelta = (LONG)after.ReadRequestsSeen - (LONG)before.ReadRequestsSeen;
    LONG devCtlDelta = (LONG)after.DeviceControlRequestsSeen - (LONG)before.DeviceControlRequestsSeen;
    LONG internalDelta = (LONG)after.InternalDeviceControlRequestsSeen - (LONG)before.InternalDeviceControlRequestsSeen;
    LONG writeDelta = (LONG)after.WriteRequestsSeen - (LONG)before.WriteRequestsSeen;
    bool passPacket = observed.sawPacketChange;
    bool passLX = observed.maxLX >= 20000 || observed.minLX <= -20000;
    bool passA = observed.sawButtonA;
    bool passLT = observed.maxLT >= 200;
    bool passReports = reportDelta > 0;
    bool passSignal = passPacket || passLX || passA || passLT || observed.sawAnyStateChange;
    bool pass = passSignal && passReports;

    std::printf(
        "Observed: samples=%lu pkt=%lu->%lu LX=%d->[%d,%d] LT=%u->%u Buttons=0x%04X->0x%04X ReportsDelta=%ld\n",
        observed.samples,
        observed.baselinePacket,
        observed.finalPacket,
        observed.baselineLX,
        observed.minLX,
        observed.maxLX,
        observed.baselineLT,
        observed.maxLT,
        observed.baselineButtons,
        observed.observedButtons,
        reportDelta);
    PrintDeviceCounters("After ", &after);
    std::printf(
        "Deltas: Pending=%ld Queued=%ld Completed=%ld Forwarded=%ld Read=%ld DevCtl=%ld Internal=%ld Write=%ld\n",
        pendingDelta,
        queuedDelta,
        completedDelta,
        forwardedDelta,
        readDelta,
        devCtlDelta,
        internalDelta,
        writeDelta);

    std::printf(
        "Checks: packet=%s LX=%s A=%s LT=%s reports=%s any=%s\n",
        passPacket ? "PASS" : "FAIL",
        passLX ? "PASS" : "FAIL",
        passA ? "PASS" : "FAIL",
        passLT ? "PASS" : "FAIL",
        passReports ? "PASS" : "FAIL",
        observed.sawAnyStateChange ? "PASS" : "FAIL");

    std::printf(
        "XInput verdict: %s\n",
        probeMode == XInputProbeMode::Unavailable ? "unavailable" :
        passSignal ? "visible and reflecting injects" :
        "visible but static during inject");

    std::printf("\nResult: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 2;
}
