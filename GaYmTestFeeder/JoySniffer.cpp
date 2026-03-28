#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#pragma comment(lib, "winmm.lib")

struct JoySnapshot {
    JOYINFOEX info = {};
    bool valid = false;
};

struct JoystickCandidate {
    UINT id = 0;
    JOYCAPSW caps = {};
    JoySnapshot previous = {};
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

static bool QueryJoystick(UINT id, JoySnapshot* snapshot)
{
    std::memset(snapshot, 0, sizeof(*snapshot));
    snapshot->info.dwSize = sizeof(snapshot->info);
    snapshot->info.dwFlags = JOY_RETURNALL;
    snapshot->valid = joyGetPosEx(id, &snapshot->info) == JOYERR_NOERROR;
    return snapshot->valid;
}

static std::vector<JoystickCandidate> EnumerateJoysticks()
{
    std::vector<JoystickCandidate> candidates;
    const UINT count = joyGetNumDevs();

    for (UINT id = 0; id < count; ++id) {
        JOYCAPSW localCaps = {};
        if (joyGetDevCapsW(id, &localCaps, sizeof(localCaps)) != JOYERR_NOERROR) {
            continue;
        }

        JoySnapshot snapshot = {};
        if (!QueryJoystick(id, &snapshot)) {
            continue;
        }

        JoystickCandidate candidate = {};
        candidate.id = id;
        candidate.caps = localCaps;
        candidate.previous = snapshot;
        candidates.push_back(candidate);
    }

    return candidates;
}

static bool SnapshotsEqual(const JoySnapshot& left, const JoySnapshot& right)
{
    return left.valid == right.valid &&
        std::memcmp(&left.info, &right.info, sizeof(JOYINFOEX)) == 0;
}

static void PrintSnapshot(const JoySnapshot& snapshot)
{
    std::printf(
        "X=%lu Y=%lu Z=%lu R=%lu U=%lu V=%lu POV=%lu Buttons=0x%08lX\n",
        snapshot.info.dwXpos,
        snapshot.info.dwYpos,
        snapshot.info.dwZpos,
        snapshot.info.dwRpos,
        snapshot.info.dwUpos,
        snapshot.info.dwVpos,
        snapshot.info.dwPOV,
        snapshot.info.dwButtons);
}

static void PrintSnapshotDiff(const JoySnapshot& previous, const JoySnapshot& current)
{
    bool printed = false;

    auto printField = [&](const char* name, DWORD oldValue, DWORD newValue) {
        if (oldValue == newValue) {
            return;
        }

        if (!printed) {
            std::printf("  diff:");
            printed = true;
        }

        std::printf(" %s=%lu->%lu", name, oldValue, newValue);
    };

    printField("X", previous.info.dwXpos, current.info.dwXpos);
    printField("Y", previous.info.dwYpos, current.info.dwYpos);
    printField("Z", previous.info.dwZpos, current.info.dwZpos);
    printField("R", previous.info.dwRpos, current.info.dwRpos);
    printField("U", previous.info.dwUpos, current.info.dwUpos);
    printField("V", previous.info.dwVpos, current.info.dwVpos);
    printField("POV", previous.info.dwPOV, current.info.dwPOV);
    printField("Buttons", previous.info.dwButtons, current.info.dwButtons);

    if (!printed) {
        std::printf("  diff: none");
    }

    std::printf("\n");
}

int main(int argc, char* argv[])
{
    const DWORD durationMs = ParsePositiveArg(argc, argv, "--duration-ms", 45000);
    const DWORD pollMs = ParsePositiveArg(argc, argv, "--poll-ms", 16);

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    std::vector<JoystickCandidate> joysticks = EnumerateJoysticks();
    if (joysticks.empty()) {
        std::fprintf(stderr, "ERROR: No joystick visible via joyGetPosEx.\n");
        return 1;
    }

    bool sawChange = false;

    std::printf("joy.cpl sniffer\n");
    for (const JoystickCandidate& joystick : joysticks) {
        std::printf("Joystick %u: %ls\n", joystick.id, joystick.caps.szPname);
        std::printf("[baseline] ");
        PrintSnapshot(joystick.previous);
    }
    std::printf("Listening for %lu ms. Spam buttons, triggers, or sticks now.\n\n", durationMs);

    const DWORD startTick = GetTickCount();
    while (InterlockedCompareExchange(&g_Running, TRUE, TRUE) != FALSE &&
           GetTickCount() - startTick < durationMs) {
        Sleep(pollMs);

        for (JoystickCandidate& joystick : joysticks) {
            JoySnapshot current = {};
            if (!QueryJoystick(joystick.id, &current)) {
                continue;
            }

            if (SnapshotsEqual(joystick.previous, current)) {
                continue;
            }

            sawChange = true;
            std::printf("[%6lu ms] Joystick %u\n", GetTickCount() - startTick, joystick.id);
            PrintSnapshot(current);
            PrintSnapshotDiff(joystick.previous, current);
            joystick.previous = current;
        }
    }

    if (!sawChange) {
        std::printf("No joystick changes observed.\n");
        return 2;
    }

    std::printf("Capture complete.\n");
    return 0;
}
