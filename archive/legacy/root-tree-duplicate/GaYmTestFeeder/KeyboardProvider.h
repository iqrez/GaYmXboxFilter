#pragma once
/*
 * KeyboardProvider - Maps keyboard keys to gamepad report.
 *
 * Default bindings:
 *   WASD       → Left stick
 *   Arrows     → Right stick
 *   Space      → A        Enter → B
 *   E          → X        Q     → Y
 *   Shift      → LB       Ctrl  → RB
 *   Z          → LT (full)  C   → RT (full)
 *   Tab        → Back     Esc   → Start
 *   F          → L3       G     → R3
 *   1/2/3/4    → D-pad Up/Down/Left/Right
 *   Home       → Guide
 */

#include "IInputProvider.h"

class KeyboardProvider : public IInputProvider {
public:
    void GetReport(GAYM_REPORT* report) override;
    const char* Name() const override { return "Keyboard"; }
};
