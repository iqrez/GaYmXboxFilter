/*
 * KeyboardProvider - Full keyboard-to-gamepad mapping.
 */

#include "KeyboardProvider.h"

static inline bool KeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void KeyboardProvider::GetReport(GAYM_REPORT* report)
{
    RtlZeroMemory(report, sizeof(GAYM_REPORT));
    report->DPad = GAYM_DPAD_NEUTRAL;

    /* ── Left stick (WASD) ── */
    if (KeyDown('W')) report->ThumbLeftY = -32767;
    if (KeyDown('S')) report->ThumbLeftY =  32767;
    if (KeyDown('A')) report->ThumbLeftX = -32767;
    if (KeyDown('D')) report->ThumbLeftX =  32767;

    /* Diagonal: if both axes are maxed, scale to ~70% for proper diagonal */
    if (report->ThumbLeftX != 0 && report->ThumbLeftY != 0) {
        report->ThumbLeftX = (SHORT)(report->ThumbLeftX * 7 / 10);
        report->ThumbLeftY = (SHORT)(report->ThumbLeftY * 7 / 10);
    }

    /* ── Right stick (Arrows) ── */
    if (KeyDown(VK_UP))    report->ThumbRightY = -32767;
    if (KeyDown(VK_DOWN))  report->ThumbRightY =  32767;
    if (KeyDown(VK_LEFT))  report->ThumbRightX = -32767;
    if (KeyDown(VK_RIGHT)) report->ThumbRightX =  32767;

    if (report->ThumbRightX != 0 && report->ThumbRightY != 0) {
        report->ThumbRightX = (SHORT)(report->ThumbRightX * 7 / 10);
        report->ThumbRightY = (SHORT)(report->ThumbRightY * 7 / 10);
    }

    /* ── Face buttons ── */
    if (KeyDown(VK_SPACE))  report->Buttons[0] |= GAYM_BTN_A;
    if (KeyDown(VK_RETURN)) report->Buttons[0] |= GAYM_BTN_B;
    if (KeyDown('E'))       report->Buttons[0] |= GAYM_BTN_X;
    if (KeyDown('Q'))       report->Buttons[0] |= GAYM_BTN_Y;

    /* ── Bumpers ── */
    if (KeyDown(VK_LSHIFT) || KeyDown(VK_RSHIFT)) report->Buttons[0] |= GAYM_BTN_LB;
    if (KeyDown(VK_LCONTROL) || KeyDown(VK_RCONTROL)) report->Buttons[0] |= GAYM_BTN_RB;

    /* ── Triggers (binary: 0 or 255) ── */
    if (KeyDown('Z')) report->TriggerLeft  = 255;
    if (KeyDown('C')) report->TriggerRight = 255;

    /* ── System buttons ── */
    if (KeyDown(VK_TAB))    report->Buttons[0] |= GAYM_BTN_BACK;
    if (KeyDown(VK_ESCAPE)) report->Buttons[0] |= GAYM_BTN_START;

    /* ── Stick clicks ── */
    if (KeyDown('F')) report->Buttons[1] |= GAYM_BTN_LSTICK;
    if (KeyDown('G')) report->Buttons[1] |= GAYM_BTN_RSTICK;

    /* ── Guide ── */
    if (KeyDown(VK_HOME)) report->Buttons[1] |= GAYM_BTN_GUIDE;

    /* ── D-pad (number keys) ── */
    bool dU = KeyDown('1');
    bool dD = KeyDown('2');
    bool dL = KeyDown('3');
    bool dR = KeyDown('4');

    if (dU && dR)      report->DPad = GAYM_DPAD_UPRIGHT;
    else if (dU && dL) report->DPad = GAYM_DPAD_UPLEFT;
    else if (dD && dR) report->DPad = GAYM_DPAD_DOWNRIGHT;
    else if (dD && dL) report->DPad = GAYM_DPAD_DOWNLEFT;
    else if (dU)       report->DPad = GAYM_DPAD_UP;
    else if (dD)       report->DPad = GAYM_DPAD_DOWN;
    else if (dL)       report->DPad = GAYM_DPAD_LEFT;
    else if (dR)       report->DPad = GAYM_DPAD_RIGHT;
}
