/*
 * MouseProvider - Mouse + WASD hybrid input.
 */

#include "MouseProvider.h"
#include <algorithm>

static inline bool KeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static inline SHORT ClampAxis(float v) {
    if (v >  32767.0f) return  32767;
    if (v < -32767.0f) return -32767;
    return (SHORT)v;
}

bool MouseProvider::Init()
{
    GetCursorPos(&lastPos_);
    initialized_ = true;
    return true;
}

void MouseProvider::GetReport(GAYM_REPORT* report)
{
    RtlZeroMemory(report, sizeof(GAYM_REPORT));
    report->DPad = GAYM_DPAD_NEUTRAL;

    /* ── Right stick from mouse delta ── */
    POINT curPos;
    GetCursorPos(&curPos);

    if (initialized_) {
        float dx = (float)(curPos.x - lastPos_.x);
        float dy = (float)(curPos.y - lastPos_.y);

        report->ThumbRightX = ClampAxis(dx * sensitivity_);
        report->ThumbRightY = ClampAxis(dy * sensitivity_);
    }

    lastPos_ = curPos;

    /* ── Left stick from WASD ── */
    if (KeyDown('W')) report->ThumbLeftY = -32767;
    if (KeyDown('S')) report->ThumbLeftY =  32767;
    if (KeyDown('A')) report->ThumbLeftX = -32767;
    if (KeyDown('D')) report->ThumbLeftX =  32767;

    if (report->ThumbLeftX != 0 && report->ThumbLeftY != 0) {
        report->ThumbLeftX = (SHORT)(report->ThumbLeftX * 7 / 10);
        report->ThumbLeftY = (SHORT)(report->ThumbLeftY * 7 / 10);
    }

    /* ── Mouse buttons → triggers and face buttons ── */
    if (KeyDown(VK_LBUTTON))  report->TriggerRight = 255;     /* LMB → RT */
    if (KeyDown(VK_RBUTTON))  report->TriggerLeft  = 255;     /* RMB → LT */
    if (KeyDown(VK_MBUTTON))  report->Buttons[0]  |= GAYM_BTN_A;
    if (KeyDown(VK_XBUTTON1)) report->Buttons[0]  |= GAYM_BTN_B;
    if (KeyDown(VK_XBUTTON2)) report->Buttons[0]  |= GAYM_BTN_Y;

    /* ── Bumpers from keyboard ── */
    if (KeyDown(VK_LSHIFT))   report->Buttons[0] |= GAYM_BTN_LB;
    if (KeyDown(VK_LCONTROL)) report->Buttons[0] |= GAYM_BTN_RB;

    /* ── System buttons ── */
    if (KeyDown(VK_TAB))      report->Buttons[0] |= GAYM_BTN_BACK;
    if (KeyDown(VK_ESCAPE))   report->Buttons[0] |= GAYM_BTN_START;
}
