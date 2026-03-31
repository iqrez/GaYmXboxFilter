/*
 * MacroProvider - CSV macro playback.
 */

#include "MacroProvider.h"
#include <fstream>
#include <sstream>
#include <cstdio>

MacroProvider::MacroProvider(const std::string& csvPath, bool loop)
    : csvPath_(csvPath), loop_(loop)
{}

bool MacroProvider::Init()
{
    std::ifstream file(csvPath_);
    if (!file.is_open()) {
        fprintf(stderr, "[Macro] Failed to open: %s\n", csvPath_.c_str());
        return false;
    }

    std::string line;
    int lineNum = 0;

    while (std::getline(file, line)) {
        lineNum++;

        /* Skip empty lines and comments */
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        std::istringstream ss(line);
        std::string token;
        MacroFrame frame;
        RtlZeroMemory(&frame.report, sizeof(GAYM_REPORT));
        frame.report.DPad = GAYM_DPAD_NEUTRAL;

        int field = 0;
        while (std::getline(ss, token, ',')) {
            int val = 0;
            try { val = std::stoi(token); } catch (...) { continue; }

            switch (field) {
            case 0: frame.timestampMs       = (DWORD)val;     break;
            case 1: frame.report.Buttons[0] = (UCHAR)val;     break;
            case 2: frame.report.Buttons[1] = (UCHAR)val;     break;
            case 3: frame.report.DPad        = (UCHAR)val;     break;
            case 4: frame.report.TriggerLeft = (UCHAR)val;     break;
            case 5: frame.report.TriggerRight= (UCHAR)val;     break;
            case 6: frame.report.ThumbLeftX  = (SHORT)val;     break;
            case 7: frame.report.ThumbLeftY  = (SHORT)val;     break;
            case 8: frame.report.ThumbRightX = (SHORT)val;     break;
            case 9: frame.report.ThumbRightY = (SHORT)val;     break;
            }
            field++;
        }

        if (field >= 1) {
            frames_.push_back(frame);
        }
    }

    if (frames_.empty()) {
        fprintf(stderr, "[Macro] No frames loaded from %s\n", csvPath_.c_str());
        return false;
    }

    printf("[Macro] Loaded %zu frames from %s (loop=%s)\n",
        frames_.size(), csvPath_.c_str(), loop_ ? "true" : "false");

    startTick_ = GetTickCount();
    currentIdx_ = 0;
    finished_ = false;

    return true;
}

void MacroProvider::GetReport(GAYM_REPORT* report)
{
    if (frames_.empty() || finished_) {
        RtlZeroMemory(report, sizeof(GAYM_REPORT));
        report->DPad = GAYM_DPAD_NEUTRAL;
        return;
    }

    DWORD elapsed = GetTickCount() - startTick_;

    /* Advance to the correct frame based on elapsed time */
    while (currentIdx_ < frames_.size() &&
           frames_[currentIdx_].timestampMs <= elapsed) {
        currentIdx_++;
    }

    if (currentIdx_ >= frames_.size()) {
        if (loop_) {
            /* Restart */
            startTick_ = GetTickCount();
            currentIdx_ = 0;
        } else {
            finished_ = true;
        }
    }

    /* Return the most recent frame before current time */
    size_t frameIdx = (currentIdx_ > 0) ? currentIdx_ - 1 : 0;
    *report = frames_[frameIdx].report;
}
