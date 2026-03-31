#pragma once
/*
 * MouseProvider - Maps mouse movement to right stick, buttons to triggers/face.
 *
 *   Mouse move  → Right stick (relative delta, scaled)
 *   LMB         → RT (full pull)
 *   RMB         → LT (full pull)
 *   MMB         → A button
 *   Mouse4      → B button
 *   Mouse5      → Y button
 *   Scroll      → D-pad Up/Down
 *
 * Keyboard WASD still controls left stick (hybrid mode).
 */

#include "IInputProvider.h"

class MouseProvider : public IInputProvider {
public:
    bool Init() override;
    void GetReport(GAYM_REPORT* report) override;
    const char* Name() const override { return "Mouse"; }

private:
    POINT lastPos_ = {};
    bool  initialized_ = false;
    float sensitivity_ = 150.0f;   /* Pixels → stick axis scaling */
};
