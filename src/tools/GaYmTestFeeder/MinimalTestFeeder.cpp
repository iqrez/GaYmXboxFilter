/*
 * MinimalTestFeeder.cpp - GaYm Xbox 02FF user-mode test tool.
 *
 * Exercises the supported Xbox 02FF adapter through the gaym_client boundary,
 * enables override, and injects GAYM_REPORT structs.
 *
 * Modes:
 *   (default)   Keyboard-driven: WASD/Arrows/keys mapped to gamepad inputs
 *   --scripted  Automated 30-second test sequence, then clean exit
 *
 * Build via the repo scripts; this tool depends on the client boundary and the
 * shared ABI headers rather than a legacy GaYmFilter include tree.
 *
 * Run (elevated):
 *   MinimalTestFeeder.exe              (keyboard mode)
 *   MinimalTestFeeder.exe --scripted   (auto test sequence)
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "user32.lib")
#include "gaym_client_compat.h"

/* ── Helpers ─────────────────────────────────────────────────── */

static void PrintObservation(const GAYM_OBSERVATION_V1& observation)
{
    printf("[Observation] family=%lu caps=0x%08lX status=0x%08lX obs=%llu inj=%llu%s\n",
        observation.AdapterFamily,
        observation.CapabilityFlags,
        observation.StatusFlags,
        observation.LastObservedSequence,
        observation.LastInjectedSequence,
        (observation.StatusFlags & GAYM_STATUS_OBSERVATION_SYNTHETIC) ? " synthetic" : "");
}

/* ── Scripted test sequence ──────────────────────────────────── */

struct TestStep {
    const char* label;
    int         durationMs;
    GAYM_REPORT report;     /* pre-filled; ReportId/DPad set in Init */
};

static void InitReport(GAYM_REPORT* r) {
    memset(r, 0, sizeof(*r));
    r->ReportId = 0;       /* kernel ignores this, auto-sets per device */
    r->DPad     = GAYM_DPAD_NEUTRAL;
}

#define AXIS_MAX   32767
#define AXIS_MIN (-32767)

static void BuildSteps(TestStep* steps, int* count) {
    int n = 0;
    auto add = [&](const char* label, int ms,
                   SHORT lx = 0, SHORT ly = 0, SHORT rx = 0, SHORT ry = 0,
                   UCHAR b0 = 0, UCHAR b1 = 0,
                   UCHAR lt = 0, UCHAR rt = 0, UCHAR dpad = (UCHAR)GAYM_DPAD_NEUTRAL) {
        TestStep& s = steps[n];
        s.label      = label;
        s.durationMs = ms;
        InitReport(&s.report);
        s.report.ThumbLeftX   = lx;
        s.report.ThumbLeftY   = ly;
        s.report.ThumbRightX  = rx;
        s.report.ThumbRightY  = ry;
        s.report.Buttons[0]   = b0;
        s.report.Buttons[1]   = b1;
        s.report.TriggerLeft  = lt;
        s.report.TriggerRight = rt;
        s.report.DPad         = dpad;
        n++;
    };

    /* Neutral baseline */
    add("Neutral (center)",       1500);

    /* Left stick cardinal directions */
    add("Left stick RIGHT",       1200, AXIS_MAX, 0);
    add("Left stick LEFT",        1200, AXIS_MIN, 0);
    add("Left stick UP",          1200, 0, AXIS_MIN);
    add("Left stick DOWN",        1200, 0, AXIS_MAX);
    add("Left stick UP-RIGHT",    1000, AXIS_MAX, AXIS_MIN);

    /* Right stick */
    add("Right stick RIGHT",      1000, 0, 0, AXIS_MAX, 0);
    add("Right stick UP",         1000, 0, 0, 0, AXIS_MIN);
    add("Right stick DOWN-LEFT",  1000, 0, 0, AXIS_MIN, AXIS_MAX);

    /* Triggers */
    add("Left trigger FULL",      1000, 0, 0, 0, 0, 0, 0, 255, 0);
    add("Right trigger FULL",     1000, 0, 0, 0, 0, 0, 0, 0, 255);
    add("Both triggers FULL",     1000, 0, 0, 0, 0, 0, 0, 255, 255);

    /* Face buttons one at a time */
    add("Button A",               800, 0, 0, 0, 0, GAYM_BTN_A);
    add("Button B",               800, 0, 0, 0, 0, GAYM_BTN_B);
    add("Button X",               800, 0, 0, 0, 0, GAYM_BTN_X);
    add("Button Y",               800, 0, 0, 0, 0, GAYM_BTN_Y);
    add("Button LB",              800, 0, 0, 0, 0, GAYM_BTN_LB);
    add("Button RB",              800, 0, 0, 0, 0, GAYM_BTN_RB);
    add("Button Back/View",       800, 0, 0, 0, 0, GAYM_BTN_BACK);
    add("Button Start/Menu",      800, 0, 0, 0, 0, GAYM_BTN_START);

    /* Shoulder buttons (Buttons[1]) */
    add("Left stick click",       800, 0, 0, 0, 0, 0, GAYM_BTN_LSTICK);
    add("Right stick click",      800, 0, 0, 0, 0, 0, GAYM_BTN_RSTICK);

    /* D-pad */
    add("D-pad UP",               600, 0, 0, 0, 0, 0, 0, 0, 0, (UCHAR)GAYM_DPAD_UP);
    add("D-pad RIGHT",            600, 0, 0, 0, 0, 0, 0, 0, 0, (UCHAR)GAYM_DPAD_RIGHT);
    add("D-pad DOWN",             600, 0, 0, 0, 0, 0, 0, 0, 0, (UCHAR)GAYM_DPAD_DOWN);
    add("D-pad LEFT",             600, 0, 0, 0, 0, 0, 0, 0, 0, (UCHAR)GAYM_DPAD_LEFT);

    /* Combo: stick + button + trigger */
    add("COMBO: LStick RIGHT + A + LT",  1500, AXIS_MAX, 0, 0, 0, GAYM_BTN_A, 0, 255, 0);

    /* Smooth left-stick circle (8 steps) */
    for (int i = 0; i < 8; i++) {
        double angle = i * 3.14159265 / 4.0;
        SHORT lx = (SHORT)(AXIS_MAX * cos(angle));
        SHORT ly = (SHORT)(AXIS_MAX * sin(angle));
        static char labels[8][64];
        sprintf(labels[i], "LStick circle %d/8", i + 1);
        add(labels[i], 400, lx, ly);
    }

    /* Return to neutral */
    add("Return to neutral",      1000);

    *count = n;
}

static void RunScripted(HANDLE hDevice) {
    TestStep steps[64];
    int stepCount = 0;
    BuildSteps(steps, &stepCount);

    printf("\n=== SCRIPTED TEST: %d steps ===\n\n", stepCount);

    for (int i = 0; i < stepCount; i++) {
        TestStep& s = steps[i];
        printf("[%2d/%2d] %-35s  ", i + 1, stepCount, s.label);

        /* Print non-zero values */
        if (s.report.ThumbLeftX || s.report.ThumbLeftY)
            printf("LStick(%6d,%6d) ", s.report.ThumbLeftX, s.report.ThumbLeftY);
        if (s.report.ThumbRightX || s.report.ThumbRightY)
            printf("RStick(%6d,%6d) ", s.report.ThumbRightX, s.report.ThumbRightY);
        if (s.report.TriggerLeft)  printf("LT(%3d) ", s.report.TriggerLeft);
        if (s.report.TriggerRight) printf("RT(%3d) ", s.report.TriggerRight);
        if (s.report.Buttons[0])   printf("Btn0(0x%02X) ", s.report.Buttons[0]);
        if (s.report.Buttons[1])   printf("Btn1(0x%02X) ", s.report.Buttons[1]);
        if (s.report.DPad != GAYM_DPAD_NEUTRAL) printf("DPad(%d) ", s.report.DPad);
        printf("\n");

        /* Inject at ~125 Hz for the step duration */
        DWORD start = GetTickCount();
        DWORD elapsed = 0;
        int injected = 0;
        while ((elapsed = GetTickCount() - start) < (DWORD)s.durationMs) {
            if (!gaym::client::InjectReport(hDevice, &s.report)) {
                printf("  WARNING: Inject failed (err %lu)\n", GetLastError());
                break;
            }
            injected++;
            Sleep(8); /* ~125 Hz */
        }
    }

    printf("\n=== SCRIPTED TEST COMPLETE ===\n");
}

/* ── Keyboard mode ───────────────────────────────────────────── */

static void RunKeyboard(HANDLE hDevice) {
    printf("\n=== KEYBOARD MODE (Ctrl+C to stop) ===\n");
    printf("  WASD        = Left Stick\n");
    printf("  Arrows      = Right Stick\n");
    printf("  Space       = A    |  E = B    |  Q = X    |  R = Y\n");
    printf("  1 = LB      |  2 = RB\n");
    printf("  Z = LT full |  C = RT full\n");
    printf("  F = Back     |  G = Start\n");
    printf("  Tab = D-pad cycle\n\n");

    DWORD reportCount = 0;
    DWORD lastPrint   = GetTickCount();

    while (true) {
        GAYM_REPORT report;
        InitReport(&report);

        /* Left stick */
        if (GetAsyncKeyState('A') & 0x8000) report.ThumbLeftX = AXIS_MIN;
        if (GetAsyncKeyState('D') & 0x8000) report.ThumbLeftX = AXIS_MAX;
        if (GetAsyncKeyState('W') & 0x8000) report.ThumbLeftY = AXIS_MIN;
        if (GetAsyncKeyState('S') & 0x8000) report.ThumbLeftY = AXIS_MAX;

        /* Right stick */
        if (GetAsyncKeyState(VK_LEFT)  & 0x8000) report.ThumbRightX = AXIS_MIN;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) report.ThumbRightX = AXIS_MAX;
        if (GetAsyncKeyState(VK_UP)    & 0x8000) report.ThumbRightY = AXIS_MIN;
        if (GetAsyncKeyState(VK_DOWN)  & 0x8000) report.ThumbRightY = AXIS_MAX;

        /* Face buttons → Buttons[0] */
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) report.Buttons[0] |= GAYM_BTN_A;
        if (GetAsyncKeyState('E')      & 0x8000) report.Buttons[0] |= GAYM_BTN_B;
        if (GetAsyncKeyState('Q')      & 0x8000) report.Buttons[0] |= GAYM_BTN_X;
        if (GetAsyncKeyState('R')      & 0x8000) report.Buttons[0] |= GAYM_BTN_Y;
        if (GetAsyncKeyState('1')      & 0x8000) report.Buttons[0] |= GAYM_BTN_LB;
        if (GetAsyncKeyState('2')      & 0x8000) report.Buttons[0] |= GAYM_BTN_RB;
        if (GetAsyncKeyState('F')      & 0x8000) report.Buttons[0] |= GAYM_BTN_BACK;
        if (GetAsyncKeyState('G')      & 0x8000) report.Buttons[0] |= GAYM_BTN_START;

        /* Stick clicks → Buttons[1] */
        if (GetAsyncKeyState('3')      & 0x8000) report.Buttons[1] |= GAYM_BTN_LSTICK;
        if (GetAsyncKeyState('4')      & 0x8000) report.Buttons[1] |= GAYM_BTN_RSTICK;

        /* Triggers */
        if (GetAsyncKeyState('Z') & 0x8000) report.TriggerLeft  = 255;
        if (GetAsyncKeyState('C') & 0x8000) report.TriggerRight = 255;

        /* D-pad via Tab (cycles through directions while held) */
        if (GetAsyncKeyState(VK_TAB) & 0x8000) {
            report.DPad = (UCHAR)((GetTickCount() / 300) % 8);
        }

        if (!gaym::client::InjectReport(hDevice, &report)) {
            printf("WARNING: Inject failed (err %lu)\n", GetLastError());
            Sleep(100);
            continue;
        }
        reportCount++;

        /* Status line every 2 seconds */
        DWORD now = GetTickCount();
        if (now - lastPrint >= 2000) {
            printf("[Input] Reports: %lu  LStick(%6d,%6d) RStick(%6d,%6d) Btn0=0x%02X LT=%d RT=%d\n",
                   reportCount,
                   report.ThumbLeftX, report.ThumbLeftY,
                   report.ThumbRightX, report.ThumbRightY,
                   report.Buttons[0],
                   report.TriggerLeft, report.TriggerRight);
            lastPrint = now;
        }

        Sleep(8); /* ~125 Hz */
    }
}

/* ── Console Ctrl+C handler ──────────────────────────────────── */

static HANDLE g_hDevice = INVALID_HANDLE_VALUE;

static BOOL WINAPI CtrlHandler(DWORD dwType) {
    if (dwType == CTRL_C_EVENT || dwType == CTRL_CLOSE_EVENT) {
        printf("\nDisabling override...\n");
        if (g_hDevice != INVALID_HANDLE_VALUE) {
            gaym::client::DisableOverride(g_hDevice);
            gaym::client::ReleaseWriterSession(g_hDevice);
            CloseHandle(g_hDevice);
            g_hDevice = INVALID_HANDLE_VALUE;
        }
        printf("Override DISABLED. Controller returned to hardware.\n");
        ExitProcess(0);
    }
    return TRUE;
}

/* ── main ────────────────────────────────────────────────────── */

int main(int argc, char* argv[]) {
    bool scripted = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scripted") == 0 || strcmp(argv[i], "-s") == 0)
            scripted = true;
    }

    printf("GaYm Xbox 02FF Test Feeder v2.0\n");
    printf("Mode: %s\n", scripted ? "SCRIPTED (30s auto-test)" : "KEYBOARD (manual)");
    printf("sizeof(GAYM_REPORT) = %zu bytes\n\n", sizeof(GAYM_REPORT));
    printf("Control: producer path prefers \\\\.\\GaYmXInputFilterCtl; diagnostics may fall back to \\\\.\\GaYmFilterCtl\n");

    HANDLE hDevice = gaym::client::OpenSupportedAdapter(0);
    if (hDevice == INVALID_HANDLE_VALUE) {
        printf("ERROR: Cannot open supported adapter (error %lu)\n", GetLastError());
        return 1;
    }
    printf("Device opened successfully.\n");
    g_hDevice = hDevice;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    /* Query device info */
    GAYM_DEVICE_INFO info = {};
    if (gaym::client::QueryAdapterInfo(hDevice, &info)) {
        printf("Device: %s (VID_%04X PID_%04X), Override: %s, Reports sent: %lu\n",
               gaym::client::DeviceTypeName(info.DeviceType),
               info.VendorId, info.ProductId,
               info.OverrideActive ? "ON" : "OFF",
               info.ReportsSent);
    } else {
        printf("Note: QUERY_DEVICE failed (err %lu) - continuing anyway.\n", GetLastError());
    }

    GAYM_OBSERVATION_V1 observation = {};
    if (gaym::client::QueryObservation(hDevice, &observation)) {
        PrintObservation(observation);
    } else {
        printf("[Observation] unavailable (error %lu)\n", GetLastError());
    }

    if (!gaym::client::AcquireWriterSession(hDevice)) {
        printf("ERROR: Failed to acquire writer session (err %lu)\n", GetLastError());
        CloseHandle(hDevice);
        return 1;
    }

    /* Enable override */
    if (!gaym::client::EnableOverride(hDevice)) {
        printf("ERROR: Failed to enable override (err %lu)\n", GetLastError());
        gaym::client::ReleaseWriterSession(hDevice);
        CloseHandle(hDevice);
        return 1;
    }
    printf("\n*** Override ENABLED ***\n");
    printf("The real controller is now intercepted. Injected reports will appear in joy.cpl.\n");

    if (scripted) {
        RunScripted(hDevice);
    } else {
        RunKeyboard(hDevice);
    }

    /* Clean shutdown */
    printf("\nDisabling override...\n");
    gaym::client::DisableOverride(hDevice);
    gaym::client::ReleaseWriterSession(hDevice);
    CloseHandle(hDevice);
    printf("Override DISABLED. Controller returned to hardware. Done.\n");
    return 0;
}
