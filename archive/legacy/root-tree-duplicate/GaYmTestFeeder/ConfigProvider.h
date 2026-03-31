#pragma once
/*
 * ConfigProvider - Outputs a fixed/static report for testing.
 * Useful for verifying the driver pipeline without any real input.
 * Sets left stick full-right + A button pressed.
 */

#include "IInputProvider.h"

class ConfigProvider : public IInputProvider {
public:
    void GetReport(GAYM_REPORT* report) override
    {
        RtlZeroMemory(report, sizeof(GAYM_REPORT));
        report->DPad        = GAYM_DPAD_NEUTRAL;
        report->ThumbLeftX  = 32767;       /* Full right */
        report->Buttons[0]  = GAYM_BTN_A;  /* A pressed  */
    }

    const char* Name() const override { return "Config (Static Test)"; }
};
