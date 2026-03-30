/*
 * GaYm Controller Feeder - Main entry point.
 *
 * Opens the GaYm filter device, selects an input provider, and pumps reports
 * at the configured rate. Runtime hotkeys:
 *   F8  Toggle override on or off
 *   F9  Force neutral report and disable override
 *   F10 Exit feeder
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>

#include "Config.h"
#include "ConfigProvider.h"
#include "DeviceHelper.h"
#include "IInputProvider.h"
#include "KeyboardProvider.h"
#include "MacroProvider.h"
#include "MouseProvider.h"
#include "NetworkProvider.h"

namespace {

constexpr int kToggleOverrideVk = VK_F8;
constexpr int kEmergencyOffVk = VK_F9;
constexpr int kQuitVk = VK_F10;
constexpr DWORD kStatsIntervalMs = 5000;
constexpr DWORD kNeutralFlushDelayMs = 20;

volatile BOOL g_Running = TRUE;
volatile LONG g_OverrideActive = FALSE;
volatile LONG g_EmergencyCleanupPerformed = FALSE;
HANDLE g_ShutdownDevice = INVALID_HANDLE_VALUE;

struct HotkeyState {
    bool ToggleDown = false;
    bool EmergencyOffDown = false;
    bool QuitDown = false;
};

static void BuildNeutralReport(GAYM_REPORT* report)
{
    std::memset(report, 0, sizeof(*report));
    report->DPad = GAYM_DPAD_NEUTRAL;
}

static bool IsKeyDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

static bool ConsumeKeyEdge(int virtualKey, bool* wasDown)
{
    const bool isDown = IsKeyDown(virtualKey);
    const bool pressed = isDown && !*wasDown;
    *wasDown = isDown;
    return pressed;
}

static bool EnableOverride(HANDLE hDevice)
{
    if (SendIoctl(hDevice, IOCTL_GAYM_OVERRIDE_ON)) {
        InterlockedExchange(&g_OverrideActive, TRUE);
        return true;
    }

    return false;
}

static void DisableOverrideBestEffort(HANDLE hDevice)
{
    if (hDevice == INVALID_HANDLE_VALUE) {
        return;
    }

    if (InterlockedExchange(&g_OverrideActive, FALSE) == FALSE) {
        return;
    }

    GAYM_REPORT neutralReport;
    BuildNeutralReport(&neutralReport);
    InjectReport(hDevice, &neutralReport);
    Sleep(kNeutralFlushDelayMs);
    SendIoctl(hDevice, IOCTL_GAYM_OVERRIDE_OFF);
}

static void PerformEmergencyCleanup()
{
    if (InterlockedExchange(&g_EmergencyCleanupPerformed, TRUE) != FALSE) {
        return;
    }

    DisableOverrideBestEffort(g_ShutdownDevice);
}

static BOOL WINAPI ConsoleHandler(DWORD event)
{
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_Running = FALSE;
        PerformEmergencyCleanup();
        return TRUE;
    default:
        return FALSE;
    }
}

static LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS*)
{
    g_Running = FALSE;
    PerformEmergencyCleanup();
    return EXCEPTION_EXECUTE_HANDLER;
}

static std::unique_ptr<IInputProvider> CreateProvider(const std::string& name, const GaYmConfig& cfg)
{
    if (name == "keyboard") {
        return std::make_unique<KeyboardProvider>();
    }
    if (name == "mouse") {
        return std::make_unique<MouseProvider>();
    }
    if (name == "network") {
        return std::make_unique<NetworkProvider>(cfg.netBindAddr, cfg.netPort);
    }
    if (name == "macro") {
        return std::make_unique<MacroProvider>(cfg.macroFile, cfg.macroLoop);
    }
    if (name == "config") {
        return std::make_unique<ConfigProvider>();
    }

    return nullptr;
}

static void PrintUsageAndExit()
{
    std::printf("GaYm Controller Feeder\n\n");
    std::printf("Usage: GaYmFeeder.exe [options]\n");
    std::printf("  -c, --config <path>    Config file (default: GaYmController.ini)\n");
    std::printf("  -p, --provider <name>  Input provider: keyboard, mouse, network, macro, config\n");
    std::printf("  -d, --device <index>   Device index (0 = first)\n");
    std::printf("      --poll-rate-hz <n> Override feeder injection rate in Hz\n");
    std::printf("      --poll-interval-ms <n> Override feeder injection interval in milliseconds\n");
    std::printf("      --hid-poll-ms <n>  Request HID poll interval in milliseconds\n");
    std::printf("      --duration-ms <n>  Run for n milliseconds, then exit cleanly\n");
    std::printf("  -h, --help             Show this help\n");
    std::printf("\nRuntime hotkeys:\n");
    std::printf("  F8                     Toggle override on or off\n");
    std::printf("  F9                     Send neutral report and disable override\n");
    std::printf("  F10                    Exit feeder\n");
    std::exit(0);
}

static bool ParsePositiveLongArgument(const char* text, long minValue, long maxValue, long* value)
{
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < minValue || parsed > maxValue) {
        return false;
    }

    *value = parsed;
    return true;
}

static void ParseArgs(
    int argc,
    char* argv[],
    std::string* configPath,
    std::string* providerOverride,
    int* deviceOverride,
    DWORD* runDurationMs,
    int* pollRateHzOverride,
    long* hidPollIntervalMsOverride)
{
    for (int argIndex = 1; argIndex < argc; ++argIndex) {
        if ((std::strcmp(argv[argIndex], "-c") == 0 ||
             std::strcmp(argv[argIndex], "--config") == 0) &&
            argIndex + 1 < argc) {
            *configPath = argv[++argIndex];
            continue;
        }

        if ((std::strcmp(argv[argIndex], "-p") == 0 ||
             std::strcmp(argv[argIndex], "--provider") == 0) &&
            argIndex + 1 < argc) {
            *providerOverride = argv[++argIndex];
            continue;
        }

        if ((std::strcmp(argv[argIndex], "-d") == 0 ||
             std::strcmp(argv[argIndex], "--device") == 0) &&
            argIndex + 1 < argc) {
            *deviceOverride = std::atoi(argv[++argIndex]);
            continue;
        }

        if ((std::strcmp(argv[argIndex], "--duration-ms") == 0 ||
             std::strcmp(argv[argIndex], "--duration") == 0) &&
            argIndex + 1 < argc) {
            *runDurationMs = static_cast<DWORD>(std::strtoul(argv[++argIndex], nullptr, 10));
            continue;
        }

        if (std::strcmp(argv[argIndex], "--poll-rate-hz") == 0 &&
            argIndex + 1 < argc) {
            long parsedHz = 0;
            if (!ParsePositiveLongArgument(argv[++argIndex], 1, 2000, &parsedHz)) {
                std::fprintf(stderr, "ERROR: Invalid poll rate: %s\n", argv[argIndex]);
                std::exit(1);
            }

            *pollRateHzOverride = static_cast<int>(parsedHz);
            continue;
        }

        if (std::strcmp(argv[argIndex], "--poll-interval-ms") == 0 &&
            argIndex + 1 < argc) {
            long parsedIntervalMs = 0;
            if (!ParsePositiveLongArgument(argv[++argIndex], 1, 1000, &parsedIntervalMs)) {
                std::fprintf(stderr, "ERROR: Invalid poll interval: %s\n", argv[argIndex]);
                std::exit(1);
            }

            *pollRateHzOverride = std::max(1L, 1000L / parsedIntervalMs);
            continue;
        }

        if (std::strcmp(argv[argIndex], "--hid-poll-ms") == 0 &&
            argIndex + 1 < argc) {
            if (!ParsePositiveLongArgument(argv[++argIndex], 1, 1000, hidPollIntervalMsOverride)) {
                std::fprintf(stderr, "ERROR: Invalid HID poll interval: %s\n", argv[argIndex]);
                std::exit(1);
            }
            continue;
        }

        if (std::strcmp(argv[argIndex], "-h") == 0 ||
            std::strcmp(argv[argIndex], "--help") == 0) {
            PrintUsageAndExit();
        }
    }
}

static void PrintStats(const GAYM_REPORT& report, DWORD reportCount, DWORD elapsedMs, bool overrideActive)
{
    const float rate = elapsedMs != 0
        ? static_cast<float>(reportCount) * 1000.0f / static_cast<float>(elapsedMs)
        : 0.0f;

    std::printf(
        "[Stats] Override:%s %.1f reports/sec | LX:%6d LY:%6d RX:%6d RY:%6d | "
        "Btn:%02X,%02X DPad:%X LT:%3d RT:%3d\r",
        overrideActive ? "ON " : "OFF",
        rate,
        report.ThumbLeftX,
        report.ThumbLeftY,
        report.ThumbRightX,
        report.ThumbRightY,
        report.Buttons[0],
        report.Buttons[1],
        report.DPad,
        report.TriggerLeft,
        report.TriggerRight);
}

static int RunFeeder(int argc, char* argv[])
{
    std::printf("=== GaYm Controller Feeder v2.1 ===\n\n");

    std::string configPath = "GaYmController.ini";
    std::string providerOverride;
    int deviceOverride = -1;
    DWORD runDurationMs = 0;
    int pollRateHzOverride = -1;
    long hidPollIntervalMsOverride = -1;
    ParseArgs(
        argc,
        argv,
        &configPath,
        &providerOverride,
        &deviceOverride,
        &runDurationMs,
        &pollRateHzOverride,
        &hidPollIntervalMsOverride);

    GaYmConfig cfg;
    if (LoadConfig(configPath, cfg)) {
        std::printf("[Config] Loaded: %s\n", configPath.c_str());
    } else {
        std::printf("[Config] Not found (%s), using defaults.\n", configPath.c_str());
        if (WriteDefaultConfig(configPath)) {
            std::printf("[Config] Wrote default config: %s\n", configPath.c_str());
        }
    }

    if (!providerOverride.empty()) {
        cfg.provider = providerOverride;
    }
    if (deviceOverride >= 0) {
        cfg.deviceIndex = deviceOverride;
    }
    if (pollRateHzOverride > 0) {
        cfg.pollRateHz = pollRateHzOverride;
    }

    auto devices = EnumerateGaYmDevices();
    if (devices.empty()) {
        std::fprintf(stderr, "ERROR: No GaYm filter devices found.\n");
        std::fprintf(stderr, "  - Is the driver installed and a controller plugged in?\n");
        std::fprintf(stderr, "  - Run as Administrator.\n");
        return 1;
    }

    std::printf("[Devices] Found %zu GaYm filter device(s):\n", devices.size());
    for (const auto& device : devices) {
        std::printf("  [%d] %ls\n", device.index, device.path.c_str());
    }

    HANDLE hDevice = OpenGaYmDevice(cfg.deviceIndex);
    if (hDevice == INVALID_HANDLE_VALUE) {
        std::fprintf(
            stderr,
            "ERROR: Failed to open device %d (error %lu). Run as Administrator.\n",
            cfg.deviceIndex,
            GetLastError());
        return 1;
    }

    g_ShutdownDevice = hDevice;
    GAYM_DEVICE_INFO deviceInfo = {};
    if (QueryDeviceInfo(hDevice, &deviceInfo)) {
        std::printf(
            "[Device] %s (VID:%04X PID:%04X)\n",
            DeviceTypeName(deviceInfo.DeviceType),
            deviceInfo.VendorId,
            deviceInfo.ProductId);
    }

    if (hidPollIntervalMsOverride > 0) {
        ULONG currentPollIntervalMs = 0;
        if (SetHidPollIntervalMs(cfg.deviceIndex, static_cast<ULONG>(hidPollIntervalMsOverride))) {
            std::printf(
                "[HID Poll] Requested %ld ms on supported HID device %d\n",
                hidPollIntervalMsOverride,
                cfg.deviceIndex);

            if (QueryHidPollIntervalMs(cfg.deviceIndex, &currentPollIntervalMs)) {
                std::printf(
                    "[HID Poll] Active %lu ms (%.2f Hz)\n",
                    currentPollIntervalMs,
                    1000.0 / static_cast<double>(currentPollIntervalMs));
            }
        } else {
            const DWORD error = GetLastError();
            if (error == ERROR_INVALID_FUNCTION) {
                std::fprintf(
                    stderr,
                    "WARNING: The active HID stack does not expose poll-frequency control on this path.\n");
            } else {
                std::fprintf(
                    stderr,
                    "WARNING: Failed to set HID poll interval to %ld ms (error %lu).\n",
                    hidPollIntervalMsOverride,
                    error);
            }
        }
    }

    auto provider = CreateProvider(cfg.provider, cfg);
    if (!provider) {
        std::fprintf(stderr, "ERROR: Unknown provider '%s'.\n", cfg.provider.c_str());
        std::fprintf(stderr, "  Valid: keyboard, mouse, network, macro, config\n");
        CloseHandle(hDevice);
        g_ShutdownDevice = INVALID_HANDLE_VALUE;
        return 1;
    }

    if (!provider->Init()) {
        std::fprintf(stderr, "ERROR: Provider '%s' failed to initialize.\n", provider->Name());
        CloseHandle(hDevice);
        g_ShutdownDevice = INVALID_HANDLE_VALUE;
        return 1;
    }

    std::printf("[Provider] %s\n", provider->Name());

    if (cfg.jitterEnabled) {
        GAYM_JITTER_CONFIG jitterConfig = {};
        jitterConfig.Enabled = TRUE;
        jitterConfig.MinDelayUs = cfg.jitterMinUs;
        jitterConfig.MaxDelayUs = cfg.jitterMaxUs;
        if (SetJitter(hDevice, &jitterConfig)) {
            std::printf("[Jitter] Enabled: %d-%d us\n", cfg.jitterMinUs, cfg.jitterMaxUs);
        } else {
            std::fprintf(stderr, "WARNING: Failed to set jitter (error %lu).\n", GetLastError());
        }
    }

    if (!EnableOverride(hDevice)) {
        std::fprintf(stderr, "ERROR: Failed to enable override (error %lu).\n", GetLastError());
        provider->Shutdown();
        CloseHandle(hDevice);
        g_ShutdownDevice = INVALID_HANDLE_VALUE;
        return 1;
    }

    std::printf("[Override] ACTIVE\n");
    std::printf("[Hotkeys] F8 toggle | F9 neutral+off | F10 exit\n");
    if (runDurationMs != 0) {
        std::printf("[Runtime] Auto-exit after %lu ms\n", runDurationMs);
    }

    std::mt19937 rng(GetTickCount());
    std::uniform_int_distribution<int> jitterDist(
        cfg.jitterMinUs,
        std::max(cfg.jitterMinUs, cfg.jitterMaxUs));

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    std::printf(
        "\nFeeding at %d Hz (%d ms). Press Ctrl+C or F10 to stop.\n\n",
        cfg.pollRateHz,
        cfg.PollIntervalMs());

    GAYM_REPORT report = {};
    BuildNeutralReport(&report);
    HotkeyState hotkeys = {};
    DWORD reportCount = 0;
    DWORD lastStatTick = GetTickCount();
    const DWORD runStartTick = GetTickCount();

    while (g_Running) {
        if (runDurationMs != 0 && GetTickCount() - runStartTick >= runDurationMs) {
            g_Running = FALSE;
            continue;
        }

        if (ConsumeKeyEdge(kQuitVk, &hotkeys.QuitDown)) {
            g_Running = FALSE;
            continue;
        }

        if (ConsumeKeyEdge(kEmergencyOffVk, &hotkeys.EmergencyOffDown)) {
            DisableOverrideBestEffort(hDevice);
            std::printf("\n[Override] DISABLED via F9\n");
        }

        if (ConsumeKeyEdge(kToggleOverrideVk, &hotkeys.ToggleDown)) {
            if (InterlockedCompareExchange(&g_OverrideActive, TRUE, TRUE) == TRUE) {
                DisableOverrideBestEffort(hDevice);
                std::printf("\n[Override] DISABLED via F8\n");
            } else if (EnableOverride(hDevice)) {
                std::printf("\n[Override] ENABLED via F8\n");
            } else {
                std::fprintf(stderr, "\nERROR: Failed to enable override (error %lu).\n", GetLastError());
            }
        }

        provider->GetReport(&report);

        if (InterlockedCompareExchange(&g_OverrideActive, TRUE, TRUE) == TRUE) {
            if (!InjectReport(hDevice, &report)) {
                std::fprintf(
                    stderr,
                    "\nERROR: InjectReport failed (error %lu). Device disconnected?\n",
                    GetLastError());
                break;
            }

            ++reportCount;
        }

        const DWORD now = GetTickCount();
        if (now - lastStatTick >= kStatsIntervalMs) {
            PrintStats(
                report,
                reportCount,
                now - lastStatTick,
                InterlockedCompareExchange(&g_OverrideActive, TRUE, TRUE) == TRUE);
            reportCount = 0;
            lastStatTick = now;
        }

        int sleepMs = std::max(1, cfg.PollIntervalMs());
        if (cfg.jitterEnabled) {
            sleepMs += jitterDist(rng) / 1000;
        }

        Sleep(sleepMs);
    }

    std::printf("\n\n[Shutdown] Releasing override...\n");
    DisableOverrideBestEffort(hDevice);
    provider->Shutdown();
    CloseHandle(hDevice);
    g_ShutdownDevice = INVALID_HANDLE_VALUE;

    std::printf("[Shutdown] Done.\n");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);

    const int exitCode = RunFeeder(argc, argv);
    SetConsoleCtrlHandler(ConsoleHandler, FALSE);
    return exitCode;
}
