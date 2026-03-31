/*
 * XInputCheck.cpp - Monitors the live XInput surface that games actually see.
 *
 * Default mode is continuous monitoring until the user presses any key.
 * Use --duration <seconds> for a bounded run or --port <0-3> to watch a
 * single controller slot.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <conio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <xinput.h>

#include "gaym_client_compat.h"

#pragma comment(lib, "xinput.lib")

namespace {

struct Options {
    DWORD durationSeconds = 0;
    bool continuous = true;
    int port = -1;
    bool showHelp = false;
};

struct PadSemanticState {
    int leftStickXPercent = 0;
    int leftStickYPercent = 0;
    int rightStickXPercent = 0;
    int rightStickYPercent = 0;
    int leftTriggerPercent = 0;
    int rightTriggerPercent = 0;
    WORD buttons = 0;
};

struct PadMonitorState {
    bool connected = false;
    bool hasRawState = false;
    XINPUT_STATE lastRawState = {};
    PadSemanticState lastSemanticState = {};
    DWORD rawChangeCount = 0;
    DWORD semanticChangeCount = 0;
};

struct OverrideSnapshot {
    bool available = false;
    bool overrideActive = false;
    GAYM_DEVICE_INFO deviceInfo = {};
};

struct ConsoleTheme {
    HANDLE outputHandle = INVALID_HANDLE_VALUE;
    WORD defaultAttributes = 0;
    bool enabled = false;
};

static ConsoleTheme gConsoleTheme = {};

static const DWORD kPollSleepMs = 16;
static const SHORT kStickMax = 32767;
static const BYTE kTriggerMax = 255;
static const SHORT kLeftStickDeadzone = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
static const SHORT kRightStickDeadzone = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
static const BYTE kTriggerDeadzone = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;

static const WORD kColorDim = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
static const WORD kColorInfo = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
static const WORD kColorGood = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static const WORD kColorWarn = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static const WORD kColorBad = FOREGROUND_RED | FOREGROUND_INTENSITY;
static const WORD kColorAccent = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY;

static bool ParseUnsignedLong(const char* text, unsigned long* value)
{
    char* end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }

    *value = parsed;
    return true;
}

static void PrintUsage()
{
    printf("XInputCheck - Monitoring GaYm upper-driver XInput override (real game path)\n\n");
    printf("Usage: XInputCheck.exe [options]\n");
    printf("  --duration <seconds>  Run for a bounded number of seconds\n");
    printf("  --continuous          Run until any key is pressed (default)\n");
    printf("  --port <0-3>          Watch only one XInput slot\n");
    printf("  -h, --help            Show this help\n");
}

static bool ParseOptions(int argc, char* argv[], Options* options)
{
    for (int argumentIndex = 1; argumentIndex < argc; ++argumentIndex) {
        const char* argument = argv[argumentIndex];

        if ((strcmp(argument, "--duration") == 0 || strcmp(argument, "-d") == 0) &&
            argumentIndex + 1 < argc) {
            unsigned long durationSeconds = 0;
            if (!ParseUnsignedLong(argv[++argumentIndex], &durationSeconds) || durationSeconds == 0) {
                fprintf(stderr, "ERROR: --duration expects a positive integer number of seconds.\n");
                return false;
            }

            options->durationSeconds = (DWORD)durationSeconds;
            options->continuous = false;
        } else if (strcmp(argument, "--continuous") == 0) {
            options->durationSeconds = 0;
            options->continuous = true;
        } else if ((strcmp(argument, "--port") == 0 || strcmp(argument, "-p") == 0) &&
            argumentIndex + 1 < argc) {
            unsigned long port = 0;
            if (!ParseUnsignedLong(argv[++argumentIndex], &port) || port > 3) {
                fprintf(stderr, "ERROR: --port expects an integer from 0 to 3.\n");
                return false;
            }

            options->port = (int)port;
        } else if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0) {
            options->showHelp = true;
        } else {
            fprintf(stderr, "ERROR: Unknown argument '%s'.\n", argument);
            return false;
        }
    }

    return true;
}

static void InitializeConsoleTheme()
{
    HANDLE outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (outputHandle == INVALID_HANDLE_VALUE || outputHandle == NULL) {
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFO bufferInfo = {};
    if (!GetConsoleScreenBufferInfo(outputHandle, &bufferInfo)) {
        return;
    }

    gConsoleTheme.outputHandle = outputHandle;
    gConsoleTheme.defaultAttributes = bufferInfo.wAttributes;
    gConsoleTheme.enabled = true;
}

static void SetConsoleColor(WORD attributes)
{
    if (!gConsoleTheme.enabled) {
        return;
    }

    SetConsoleTextAttribute(gConsoleTheme.outputHandle, attributes);
}

static void ResetConsoleColor()
{
    if (!gConsoleTheme.enabled) {
        return;
    }

    SetConsoleTextAttribute(gConsoleTheme.outputHandle, gConsoleTheme.defaultAttributes);
}

static void PrintColoredLine(WORD attributes, const char* format, ...)
{
    SetConsoleColor(attributes);

    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    ResetConsoleColor();
}

static void AppendText(char* buffer, size_t bufferSize, const char* text)
{
    size_t currentLength = strlen(buffer);
    if (currentLength + 1 >= bufferSize) {
        return;
    }

    size_t textLength = strlen(text);
    size_t remaining = bufferSize - currentLength - 1;
    if (textLength > remaining) {
        textLength = remaining;
    }

    memcpy(buffer + currentLength, text, textLength);
    buffer[currentLength + textLength] = '\0';
}

static int NormalizeStickPercent(SHORT value, SHORT deadzone)
{
    LONG rawValue = (LONG)value;
    LONG absoluteValue = rawValue < 0 ? -rawValue : rawValue;
    if (absoluteValue <= deadzone) {
        return 0;
    }

    LONG adjusted = absoluteValue - deadzone;
    LONG span = kStickMax - deadzone;
    if (span <= 0) {
        return 0;
    }

    int percent = (int)((adjusted * 100 + span / 2) / span);
    if (percent > 100) {
        percent = 100;
    }

    return rawValue < 0 ? -percent : percent;
}

static int NormalizeTriggerPercent(BYTE value)
{
    if (value <= kTriggerDeadzone) {
        return 0;
    }

    int adjusted = (int)value - (int)kTriggerDeadzone;
    int span = (int)kTriggerMax - (int)kTriggerDeadzone;
    if (span <= 0) {
        return 0;
    }

    int percent = (adjusted * 100 + span / 2) / span;
    if (percent > 100) {
        percent = 100;
    }

    return percent;
}

static PadSemanticState BuildSemanticState(const XINPUT_STATE& state)
{
    const XINPUT_GAMEPAD& gamepad = state.Gamepad;

    PadSemanticState semanticState = {};
    semanticState.leftStickXPercent = NormalizeStickPercent(gamepad.sThumbLX, kLeftStickDeadzone);
    semanticState.leftStickYPercent = NormalizeStickPercent(gamepad.sThumbLY, kLeftStickDeadzone);
    semanticState.rightStickXPercent = NormalizeStickPercent(gamepad.sThumbRX, kRightStickDeadzone);
    semanticState.rightStickYPercent = NormalizeStickPercent(gamepad.sThumbRY, kRightStickDeadzone);
    semanticState.leftTriggerPercent = NormalizeTriggerPercent(gamepad.bLeftTrigger);
    semanticState.rightTriggerPercent = NormalizeTriggerPercent(gamepad.bRightTrigger);
    semanticState.buttons = gamepad.wButtons;
    return semanticState;
}

static bool SemanticStatesDiffer(const PadSemanticState& left, const PadSemanticState& right)
{
    return left.leftStickXPercent != right.leftStickXPercent ||
        left.leftStickYPercent != right.leftStickYPercent ||
        left.rightStickXPercent != right.rightStickXPercent ||
        left.rightStickYPercent != right.rightStickYPercent ||
        left.leftTriggerPercent != right.leftTriggerPercent ||
        left.rightTriggerPercent != right.rightTriggerPercent ||
        left.buttons != right.buttons;
}

static const char* GetDPadLabel(WORD buttons)
{
    const bool up = (buttons & XINPUT_GAMEPAD_DPAD_UP) != 0;
    const bool down = (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    const bool left = (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
    const bool right = (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;

    if (up && right) return "UpRight";
    if (up && left) return "UpLeft";
    if (down && right) return "DownRight";
    if (down && left) return "DownLeft";
    if (up) return "Up";
    if (down) return "Down";
    if (left) return "Left";
    if (right) return "Right";
    return "Neutral";
}

static void BuildButtonLabelList(WORD buttons, char* buffer, size_t bufferSize)
{
    buffer[0] = '\0';

    struct ButtonName {
        WORD mask;
        const char* label;
    };

    static const ButtonName kButtonNames[] = {
        { XINPUT_GAMEPAD_A, "A" },
        { XINPUT_GAMEPAD_B, "B" },
        { XINPUT_GAMEPAD_X, "X" },
        { XINPUT_GAMEPAD_Y, "Y" },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, "LB" },
        { XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB" },
        { XINPUT_GAMEPAD_BACK, "Back" },
        { XINPUT_GAMEPAD_START, "Start" },
        { XINPUT_GAMEPAD_LEFT_THUMB, "LS" },
        { XINPUT_GAMEPAD_RIGHT_THUMB, "RS" },
#ifdef XINPUT_GAMEPAD_GUIDE
        { XINPUT_GAMEPAD_GUIDE, "Guide" },
#endif
    };

    bool first = true;
    for (size_t buttonIndex = 0; buttonIndex < ARRAYSIZE(kButtonNames); ++buttonIndex) {
        if ((buttons & kButtonNames[buttonIndex].mask) == 0) {
            continue;
        }

        if (!first) {
            AppendText(buffer, bufferSize, "|");
        }

        AppendText(buffer, bufferSize, kButtonNames[buttonIndex].label);
        first = false;
    }

    if (first) {
        AppendText(buffer, bufferSize, "--");
    }
}

static void FormatPercentColumn(char* buffer, size_t bufferSize, int percent)
{
    snprintf(buffer, bufferSize, "%+4d%%", percent);
}

static void FormatTriggerColumn(char* buffer, size_t bufferSize, int percent)
{
    snprintf(buffer, bufferSize, "%3d%%", percent);
}

static void PrintPadLine(
    DWORD elapsedMs,
    DWORD portIndex,
    const XINPUT_STATE& state,
    const PadSemanticState& semanticState,
    bool overrideActive)
{
    char leftStickX[16];
    char leftStickY[16];
    char rightStickX[16];
    char rightStickY[16];
    char leftTrigger[16];
    char rightTrigger[16];
    char buttonList[128];

    FormatPercentColumn(leftStickX, sizeof(leftStickX), semanticState.leftStickXPercent);
    FormatPercentColumn(leftStickY, sizeof(leftStickY), semanticState.leftStickYPercent);
    FormatPercentColumn(rightStickX, sizeof(rightStickX), semanticState.rightStickXPercent);
    FormatPercentColumn(rightStickY, sizeof(rightStickY), semanticState.rightStickYPercent);
    FormatTriggerColumn(leftTrigger, sizeof(leftTrigger), semanticState.leftTriggerPercent);
    FormatTriggerColumn(rightTrigger, sizeof(rightTrigger), semanticState.rightTriggerPercent);
    BuildButtonLabelList(semanticState.buttons, buttonList, sizeof(buttonList));

    const char* dpadLabel = GetDPadLabel(semanticState.buttons);
    WORD lineColor = overrideActive ? kColorGood : kColorInfo;

    PrintColoredLine(
        lineColor,
        "[%6lu ms] Pad %lu pkt=%-8lu | LX=%s LY=%s | RX=%s RY=%s | LT=%s RT=%s | DPad=%-8s | Btn=%s%s\n",
        elapsedMs,
        portIndex,
        state.dwPacketNumber,
        leftStickX,
        leftStickY,
        rightStickX,
        rightStickY,
        leftTrigger,
        rightTrigger,
        dpadLabel,
        buttonList,
        overrideActive ? " | Override=ON" : " | Override=OFF");
}

static bool TryQueryOverrideSnapshot(HANDLE adapterHandle, OverrideSnapshot* snapshot)
{
    if (adapterHandle == INVALID_HANDLE_VALUE || snapshot == NULL) {
        return false;
    }

    GAYM_DEVICE_INFO deviceInfo = {};
    if (!gaym::client::QueryAdapterInfo(adapterHandle, &deviceInfo)) {
        snapshot->available = false;
        snapshot->overrideActive = false;
        ZeroMemory(&snapshot->deviceInfo, sizeof(snapshot->deviceInfo));
        return false;
    }

    snapshot->available = true;
    snapshot->overrideActive = deviceInfo.OverrideActive ? true : false;
    snapshot->deviceInfo = deviceInfo;
    return true;
}

static void PrintHeader(const Options& options, const OverrideSnapshot* overrideSnapshot)
{
    printf("XInputCheck \xE2\x80\x94 Monitoring GaYm upper-driver XInput override (real game path)\n");
    printf("Press any key to stop. Use --duration <seconds> or --continuous; optional --port <0-3>.\n");

    if (options.port >= 0) {
        printf("Monitoring: port %d only\n", options.port);
    } else {
        printf("Monitoring: ports 0-3\n");
    }

    if (options.durationSeconds > 0) {
        printf("Stop mode: duration %lu second(s)\n", options.durationSeconds);
    } else {
        printf("Stop mode: continuous until any key is pressed\n");
    }

    if (overrideSnapshot != NULL && overrideSnapshot->available) {
        printf("Control-side override: %s | Reports=%lu\n",
            overrideSnapshot->overrideActive ? "ON" : "OFF",
            overrideSnapshot->deviceInfo.ReportsSent);
    } else {
        printf("Control-side override: unavailable\n");
    }

    printf("\n");
}

static void PrintHeartbeat(
    ULONGLONG elapsedMs,
    ULONGLONG pollsDelta,
    ULONGLONG semanticDelta,
    const OverrideSnapshot* overrideSnapshot,
    int connectedPads,
    int monitoredPads)
{
    double seconds = elapsedMs > 0 ? (double)elapsedMs / 1000.0 : 0.0;
    double pollsPerSecond = seconds > 0.0 ? (double)pollsDelta / seconds : 0.0;
    double semanticChangesPerSecond = seconds > 0.0 ? (double)semanticDelta / seconds : 0.0;
    const char* overrideLabel = "n/a";
    WORD lineColor = kColorDim;

    if (overrideSnapshot != NULL && overrideSnapshot->available) {
        overrideLabel = overrideSnapshot->overrideActive ? "ON" : "OFF";
        lineColor = overrideSnapshot->overrideActive ? kColorWarn : kColorAccent;
    }

    PrintColoredLine(
        lineColor,
        "[Stats] polls/s=%6.1f  semantic/s=%5.1f  connected=%d/%d  override=%s  reports=%lu\n",
        pollsPerSecond,
        semanticChangesPerSecond,
        connectedPads,
        monitoredPads,
        overrideLabel,
        (overrideSnapshot != NULL && overrideSnapshot->available) ? overrideSnapshot->deviceInfo.ReportsSent : 0UL);
}

static bool IsAnyKeyPressed()
{
    if (_kbhit()) {
        (void)_getch();
        return true;
    }

    return false;
}

static bool ShouldMonitorPort(int configuredPort, DWORD portIndex)
{
    return configuredPort < 0 || (DWORD)configuredPort == portIndex;
}

} // namespace

int main(int argc, char* argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);

    Options options = {};
    if (!ParseOptions(argc, argv, &options)) {
        PrintUsage();
        return 1;
    }

    if (options.showHelp) {
        PrintUsage();
        return 0;
    }

    InitializeConsoleTheme();

    HANDLE controlHandle = gaym::client::OpenSupportedAdapter(0);
    OverrideSnapshot overrideSnapshot = {};
    TryQueryOverrideSnapshot(controlHandle, &overrideSnapshot);

    PrintHeader(options, &overrideSnapshot);

    PadMonitorState padStates[XUSER_MAX_COUNT] = {};
    DWORD monitoredPadCount = 0;
    for (DWORD portIndex = 0; portIndex < XUSER_MAX_COUNT; ++portIndex) {
        if (ShouldMonitorPort(options.port, portIndex)) {
            ++monitoredPadCount;
        }
    }

    ULONGLONG startTick = GetTickCount64();
    ULONGLONG lastHeartbeatTick = startTick;
    ULONGLONG lastHeartbeatPolls = 0;
    ULONGLONG lastHeartbeatSemanticChanges = 0;
    ULONGLONG totalPolls = 0;
    ULONGLONG totalSemanticChanges = 0;
    bool stopByKeypress = false;

    if (monitoredPadCount == 0) {
        fprintf(stderr, "ERROR: No controller ports selected.\n");
        if (controlHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(controlHandle);
        }
        return 1;
    }

    while (true) {
        ULONGLONG now = GetTickCount64();
        ULONGLONG elapsedMs = now - startTick;

        if (options.durationSeconds > 0 && elapsedMs >= (ULONGLONG)options.durationSeconds * 1000ULL) {
            break;
        }

        if (IsAnyKeyPressed()) {
            stopByKeypress = true;
            break;
        }

        int connectedPads = 0;

        for (DWORD portIndex = 0; portIndex < XUSER_MAX_COUNT; ++portIndex) {
            if (!ShouldMonitorPort(options.port, portIndex)) {
                continue;
            }

            ++totalPolls;

            XINPUT_STATE currentState = {};
            DWORD result = XInputGetState(portIndex, &currentState);

            if (result == ERROR_SUCCESS) {
                ++connectedPads;

                PadMonitorState& padState = padStates[portIndex];
                PadSemanticState currentSemanticState = BuildSemanticState(currentState);

                if (!padState.connected) {
                    padState.connected = true;
                    padState.hasRawState = true;
                    padState.lastRawState = currentState;
                    padState.lastSemanticState = currentSemanticState;
                    padState.rawChangeCount = 1;
                    padState.semanticChangeCount = 1;
                    ++totalSemanticChanges;

                    PrintColoredLine(kColorGood, "[%6lu ms] Pad %lu connected.\n", (DWORD)elapsedMs, portIndex);
                    PrintPadLine((DWORD)elapsedMs, portIndex, currentState, currentSemanticState, overrideSnapshot.available && overrideSnapshot.overrideActive);
                    continue;
                }

                bool rawStateChanged = currentState.dwPacketNumber != padState.lastRawState.dwPacketNumber ||
                    memcmp(&currentState.Gamepad, &padState.lastRawState.Gamepad, sizeof(currentState.Gamepad)) != 0;
                if (!rawStateChanged) {
                    continue;
                }

                ++padState.rawChangeCount;
                padState.lastRawState = currentState;

                if (SemanticStatesDiffer(currentSemanticState, padState.lastSemanticState)) {
                    ++padState.semanticChangeCount;
                    ++totalSemanticChanges;
                    padState.lastSemanticState = currentSemanticState;
                    PrintPadLine((DWORD)elapsedMs, portIndex, currentState, currentSemanticState, overrideSnapshot.available && overrideSnapshot.overrideActive);
                }
            } else if (padStates[portIndex].connected) {
                padStates[portIndex].connected = false;
                padStates[portIndex].hasRawState = false;
                ZeroMemory(&padStates[portIndex].lastRawState, sizeof(padStates[portIndex].lastRawState));
                ZeroMemory(&padStates[portIndex].lastSemanticState, sizeof(padStates[portIndex].lastSemanticState));
                PrintColoredLine(kColorBad, "[%6lu ms] Pad %lu disconnected (error %lu).\n", (DWORD)elapsedMs, portIndex, result);
            }
        }

        now = GetTickCount64();
        elapsedMs = now - startTick;

        if (now - lastHeartbeatTick >= 1000ULL) {
            lastHeartbeatTick = now;
            TryQueryOverrideSnapshot(controlHandle, &overrideSnapshot);
            PrintHeartbeat(
                elapsedMs,
                totalPolls - lastHeartbeatPolls,
                totalSemanticChanges - lastHeartbeatSemanticChanges,
                &overrideSnapshot,
                connectedPads,
                monitoredPadCount);
            lastHeartbeatPolls = totalPolls;
            lastHeartbeatSemanticChanges = totalSemanticChanges;
        }

        Sleep(kPollSleepMs);
    }

    printf("\n");
    if (stopByKeypress) {
        printf("Stopped by keypress.\n");
    } else if (options.durationSeconds > 0) {
        printf("Stopped after %lu second(s).\n", options.durationSeconds);
    } else {
        printf("Stopped.\n");
    }

    if (controlHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(controlHandle);
    }

    return 0;
}
