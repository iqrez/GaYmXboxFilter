/*
 * XInputCheck.cpp - Polls XInput and prints meaningful state changes.
 * Compile: cl /EHsc XInputCheck.cpp /link xinput.lib
 */
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "xinput.lib")

static void PrintPadState(DWORD elapsedMs, DWORD index, const XINPUT_STATE* state)
{
    const XINPUT_GAMEPAD* g = &state->Gamepad;
    printf("[%6lu ms] Pad %lu pkt=%-6lu LX=%6d LY=%6d RX=%6d RY=%6d LT=%3u RT=%3u Btn=0x%04X\n",
        elapsedMs,
        index,
        state->dwPacketNumber,
        g->sThumbLX,
        g->sThumbLY,
        g->sThumbRX,
        g->sThumbRY,
        (unsigned)g->bLeftTrigger,
        (unsigned)g->bRightTrigger,
        g->wButtons);
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("Polling XInput for 30 seconds... (Ctrl+C to stop)\n");
    printf("Prints connection changes, packet/state changes, and reports when XInput is unavailable.\n");
    printf("Run MinimalTestFeeder.exe in another window.\n\n");

    DWORD start = GetTickCount();
    BOOL connected[4] = {};
    XINPUT_STATE lastState[4] = {};
    DWORD unavailableSince[4] = {};
    BOOL unavailableReported[4] = {};

    while (GetTickCount() - start < 30000) {
        DWORD now = GetTickCount();
        DWORD elapsedMs = now - start;

        for (DWORD i = 0; i < 4; ++i) {
            XINPUT_STATE state = {};
            DWORD result = XInputGetState(i, &state);

            if (result == ERROR_SUCCESS) {
                if (!connected[i]) {
                    connected[i] = TRUE;
                    lastState[i] = state;
                    unavailableReported[i] = FALSE;
                    unavailableSince[i] = 0;
                    printf("[%6lu ms] Pad %lu connected.\n", elapsedMs, i);
                    PrintPadState(elapsedMs, i, &state);
                    continue;
                }

                if (state.dwPacketNumber != lastState[i].dwPacketNumber ||
                    memcmp(&state.Gamepad, &lastState[i].Gamepad, sizeof(state.Gamepad)) != 0) {
                    lastState[i] = state;
                    PrintPadState(elapsedMs, i, &state);
                }
            } else {
                if (connected[i]) {
                    connected[i] = FALSE;
                    ZeroMemory(&lastState[i], sizeof(lastState[i]));
                    printf("[%6lu ms] Pad %lu disconnected (error %lu).\n", elapsedMs, i, result);
                }

                if (unavailableSince[i] == 0) {
                    unavailableSince[i] = now;
                } else if (!unavailableReported[i] && now - unavailableSince[i] >= 1000) {
                    unavailableReported[i] = TRUE;
                    printf("[%6lu ms] Pad %lu unavailable for %lu ms (error %lu).\n",
                        elapsedMs,
                        i,
                        now - unavailableSince[i],
                        result);
                }
            }
        }

        Sleep(16);
    }

    printf("\nDone.\n");
    return 0;
}
