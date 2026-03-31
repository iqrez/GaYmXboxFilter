#pragma once
/*
 * MacroProvider - Plays back recorded input sequences from a CSV file.
 *
 * CSV format (no header row):
 *   delay_ms, buttons0, buttons1, dpad, lt, rt, lx, ly, rx, ry
 *
 * Example:
 *   0,   1, 0, 15, 0, 0,     0,     0,     0,     0
 *   100, 0, 0, 15, 0, 0, 32767,     0,     0,     0
 *   200, 1, 0, 15, 0, 0,     0, 32767,     0,     0
 *
 * delay_ms is relative to the START of playback (absolute timestamp).
 */

#include "IInputProvider.h"
#include <string>
#include <vector>

struct MacroFrame {
    DWORD       timestampMs;   /* Absolute time from start */
    GAYM_REPORT report;
};

class MacroProvider : public IInputProvider {
public:
    MacroProvider(const std::string& csvPath, bool loop = false);

    bool Init() override;
    void GetReport(GAYM_REPORT* report) override;
    const char* Name() const override { return "Macro Playback"; }

    bool IsFinished() const { return finished_; }

private:
    std::string              csvPath_;
    bool                     loop_;
    std::vector<MacroFrame>  frames_;
    size_t                   currentIdx_ = 0;
    DWORD                    startTick_  = 0;
    bool                     finished_   = false;
};
