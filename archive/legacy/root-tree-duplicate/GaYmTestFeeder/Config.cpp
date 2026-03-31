/*
 * GaYmTestFeeder - INI configuration parser implementation.
 * Simple line-based parser. No external dependencies.
 */

#include "Config.h"
#include <fstream>
#include <sstream>
#include <algorithm>

/* Trim whitespace from both ends */
static std::string Trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/* Case-insensitive compare */
static std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return (char)tolower(c); });
    return s;
}

bool LoadConfig(const std::string& path, GaYmConfig& cfg)
{
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string currentSection;
    std::string line;

    while (std::getline(file, line)) {
        line = Trim(line);

        /* Skip empty lines and comments */
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;

        /* Section header */
        if (line[0] == '[') {
            size_t end = line.find(']');
            if (end != std::string::npos) {
                currentSection = ToLower(Trim(line.substr(1, end - 1)));
            }
            continue;
        }

        /* Key = Value */
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = ToLower(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));

        /* Strip inline comments */
        size_t commentPos = val.find(';');
        if (commentPos != std::string::npos)
            val = Trim(val.substr(0, commentPos));
        commentPos = val.find('#');
        if (commentPos != std::string::npos)
            val = Trim(val.substr(0, commentPos));

        /* Map to config struct */
        if (currentSection == "general") {
            if      (key == "provider")    cfg.provider    = ToLower(val);
            else if (key == "pollratehz")  cfg.pollRateHz  = std::stoi(val);
            else if (key == "deviceindex") cfg.deviceIndex = std::stoi(val);
        }
        else if (currentSection == "jitter") {
            if      (key == "enabled") cfg.jitterEnabled = (ToLower(val) == "true" || val == "1");
            else if (key == "minus")   cfg.jitterMinUs   = std::stoi(val);
            else if (key == "maxus")   cfg.jitterMaxUs   = std::stoi(val);
        }
        else if (currentSection == "network") {
            if      (key == "bindaddr" || key == "bind") cfg.netBindAddr = val;
            else if (key == "port")                      cfg.netPort     = std::stoi(val);
        }
        else if (currentSection == "macros") {
            if      (key == "file")  cfg.macroFile = val;
            else if (key == "loop")  cfg.macroLoop = (ToLower(val) == "true" || val == "1");
        }
    }

    return true;
}

bool WriteDefaultConfig(const std::string& path)
{
    std::ofstream file(path);
    if (!file.is_open()) return false;

    file <<
        "; GaYm Controller Configuration\n"
        "; Edit this file to change feeder behavior.\n"
        "\n"
        "[General]\n"
        "; Input provider: keyboard, mouse, network, macro, config\n"
        "Provider    = keyboard\n"
        "; Report injection rate in Hz (125 = 8ms interval)\n"
        "PollRateHz  = 125\n"
        "; Which controller to attach to (0 = first found)\n"
        "DeviceIndex = 0\n"
        "\n"
        "[Jitter]\n"
        "; Add random timing variation to avoid detection\n"
        "Enabled = false\n"
        "MinUs   = 500\n"
        "MaxUs   = 2000\n"
        "\n"
        "[Network]\n"
        "; UDP listener for remote input injection\n"
        "BindAddr = 127.0.0.1\n"
        "Port     = 43210\n"
        "\n"
        "[Macros]\n"
        "; CSV macro file for playback (columns: delay_ms, buttons, dpad, lt, rt, lx, ly, rx, ry)\n"
        "File = macro.csv\n"
        "Loop = false\n";

    return true;
}
