#pragma once
/*
 * GaYmTestFeeder - INI configuration file parser.
 *
 * Reads GaYmController.ini with sections:
 *   [General]     - Provider selection, poll rate, supported adapter slot
 *   [Jitter]      - Timing jitter settings
 *   [Network]     - UDP listener settings
 *   [Macros]      - Macro file path and settings
 *   [KeyBindings] - Custom keyboard→button mappings (future)
 */

#include <string>
#include <map>

struct GaYmConfig {
    /* [General] */
    std::string provider   = "keyboard";   /* keyboard, mouse, network, macro, config */
    int         pollRateHz = 125;          /* Report injection rate (Hz)              */
    int         deviceIndex = 0;           /* Supported adapter slot (0 = first)      */

    /* [Jitter] */
    bool        jitterEnabled = false;
    int         jitterMinUs   = 0;         /* Microseconds */
    int         jitterMaxUs   = 0;

    /* [Network] */
    std::string netBindAddr = "127.0.0.1";
    int         netPort     = 43210;

    /* [Macros] */
    std::string macroFile   = "macro.csv";
    bool        macroLoop   = false;

    /* Derived */
    int PollIntervalMs() const {
        return (pollRateHz > 0) ? (1000 / pollRateHz) : 8;
    }
};

/*
 * Parse a .ini file into GaYmConfig.
 * Returns true on success. Missing keys keep their defaults.
 */
bool LoadConfig(const std::string& path, GaYmConfig& cfg);

/*
 * Write a default config file (for first-run generation).
 */
bool WriteDefaultConfig(const std::string& path);
