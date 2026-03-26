/*
 * GaYm Controller Feeder - Main entry point.
 *
 * Opens the GaYmFilter device interface, selects an input provider
 * based on config, and pumps reports at the configured rate.
 *
 * Usage:
 *   GaYmFeeder.exe                     (uses GaYmController.ini or defaults)
 *   GaYmFeeder.exe -p keyboard         (override provider)
 *   GaYmFeeder.exe -c myconfig.ini     (custom config path)
 *   GaYmFeeder.exe -d 1                (device index)
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>

#include "DeviceHelper.h"
#include "Config.h"
#include "IInputProvider.h"
#include "KeyboardProvider.h"
#include "MouseProvider.h"
#include "NetworkProvider.h"
#include "MacroProvider.h"
#include "ConfigProvider.h"

/* ─── Create provider by name ─── */
static std::unique_ptr<IInputProvider> CreateProvider(const std::string& name, const GaYmConfig& cfg)
{
    if (name == "keyboard") return std::make_unique<KeyboardProvider>();
    if (name == "mouse")    return std::make_unique<MouseProvider>();
    if (name == "network")  return std::make_unique<NetworkProvider>(cfg.netBindAddr, cfg.netPort);
    if (name == "macro")    return std::make_unique<MacroProvider>(cfg.macroFile, cfg.macroLoop);
    if (name == "config")   return std::make_unique<ConfigProvider>();
    return nullptr;
}

/* ─── Parse command-line overrides ─── */
static void ParseArgs(int argc, char* argv[], std::string& configPath,
                      std::string& providerOverride, int& deviceOverride)
{
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            configPath = argv[++i];
        }
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--provider") == 0) && i + 1 < argc) {
            providerOverride = argv[++i];
        }
        else if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--device") == 0) && i + 1 < argc) {
            deviceOverride = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("GaYm Controller Feeder\n\n");
            printf("Usage: GaYmFeeder.exe [options]\n");
            printf("  -c, --config <path>    Config file (default: GaYmController.ini)\n");
            printf("  -p, --provider <name>  Input provider: keyboard, mouse, network, macro, config\n");
            printf("  -d, --device <index>   Device index (0 = first)\n");
            printf("  -h, --help             Show this help\n");
            exit(0);
        }
    }
}

/* ─── Console Ctrl+C handler ─── */
static volatile BOOL g_Running = TRUE;

static BOOL WINAPI ConsoleHandler(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        g_Running = FALSE;
        return TRUE;
    }
    return FALSE;
}

/* ─── Main ─── */
int main(int argc, char* argv[])
{
    printf("=== GaYm Controller Feeder v2.0 ===\n\n");

    /* ── Load config ── */
    std::string configPath = "GaYmController.ini";
    std::string providerOverride;
    int deviceOverride = -1;
    ParseArgs(argc, argv, configPath, providerOverride, deviceOverride);

    GaYmConfig cfg;
    if (LoadConfig(configPath, cfg)) {
        printf("[Config] Loaded: %s\n", configPath.c_str());
    } else {
        printf("[Config] Not found (%s), using defaults.\n", configPath.c_str());
    }

    /* Apply command-line overrides */
    if (!providerOverride.empty()) cfg.provider = providerOverride;
    if (deviceOverride >= 0)       cfg.deviceIndex = deviceOverride;

    /* ── Enumerate devices ── */
    auto devices = EnumerateGaYmDevices();
    if (devices.empty()) {
        fprintf(stderr, "ERROR: No GaYmFilter devices found.\n");
        fprintf(stderr, "  - Is the driver installed and a controller plugged in?\n");
        fprintf(stderr, "  - Run as Administrator.\n");
        return 1;
    }

    printf("[Devices] Found %zu GaYmFilter device(s):\n", devices.size());
    for (auto& d : devices) {
        printf("  [%d] %ls\n", d.index, d.path.c_str());
    }

    /* ── Open device ── */
    HANDLE hDevice = OpenGaYmDevice(cfg.deviceIndex);
    if (hDevice == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ERROR: Failed to open device %d (error %lu). Run as Administrator.\n",
            cfg.deviceIndex, GetLastError());
        return 1;
    }

    /* Query and display device info */
    GAYM_DEVICE_INFO devInfo;
    if (QueryDeviceInfo(hDevice, &devInfo)) {
        printf("[Device] %s (VID:%04X PID:%04X)\n",
            DeviceTypeName(devInfo.DeviceType), devInfo.VendorId, devInfo.ProductId);
    }

    /* ── Create input provider ── */
    auto provider = CreateProvider(cfg.provider, cfg);
    if (!provider) {
        fprintf(stderr, "ERROR: Unknown provider '%s'.\n", cfg.provider.c_str());
        fprintf(stderr, "  Valid: keyboard, mouse, network, macro, config\n");
        CloseHandle(hDevice);
        return 1;
    }

    if (!provider->Init()) {
        fprintf(stderr, "ERROR: Provider '%s' failed to initialize.\n", provider->Name());
        CloseHandle(hDevice);
        return 1;
    }

    printf("[Provider] %s\n", provider->Name());

    /* ── Set jitter config on driver ── */
    if (cfg.jitterEnabled) {
        GAYM_JITTER_CONFIG jcfg;
        jcfg.Enabled   = TRUE;
        jcfg.MinDelayUs = cfg.jitterMinUs;
        jcfg.MaxDelayUs = cfg.jitterMaxUs;
        SetJitter(hDevice, &jcfg);
        printf("[Jitter] Enabled: %d-%d us\n", cfg.jitterMinUs, cfg.jitterMaxUs);
    }

    /* ── Enable override ── */
    if (!SendIoctl(hDevice, IOCTL_GAYM_OVERRIDE_ON)) {
        fprintf(stderr, "ERROR: Failed to enable override (error %lu).\n", GetLastError());
        CloseHandle(hDevice);
        return 1;
    }

    printf("[Override] ACTIVE\n");

    /* ── Jitter RNG setup ── */
    std::mt19937 rng(GetTickCount());
    std::uniform_int_distribution<int> jitterDist(cfg.jitterMinUs, std::max(cfg.jitterMinUs, cfg.jitterMaxUs));

    /* ── Main loop ── */
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    printf("\nFeeding at %d Hz (%d ms). Press Ctrl+C to stop.\n\n", cfg.pollRateHz, cfg.PollIntervalMs());

    GAYM_REPORT report;
    DWORD reportCount = 0;
    DWORD lastStatTick = GetTickCount();

    while (g_Running) {
        provider->GetReport(&report);

        if (!InjectReport(hDevice, &report)) {
            fprintf(stderr, "\nERROR: InjectReport failed (error %lu). Device disconnected?\n", GetLastError());
            break;
        }

        reportCount++;

        /* Print stats every 5 seconds */
        DWORD now = GetTickCount();
        if (now - lastStatTick >= 5000) {
            float rate = (float)reportCount * 1000.0f / (float)(now - lastStatTick);
            printf("[Stats] %.1f reports/sec | LX:%6d LY:%6d RX:%6d RY:%6d | Btn:%02X,%02X DPad:%X LT:%3d RT:%3d\r",
                rate,
                report.ThumbLeftX, report.ThumbLeftY,
                report.ThumbRightX, report.ThumbRightY,
                report.Buttons[0], report.Buttons[1],
                report.DPad, report.TriggerLeft, report.TriggerRight);
            reportCount = 0;
            lastStatTick = now;
        }

        /* Sleep with optional jitter */
        int sleepMs = cfg.PollIntervalMs();
        if (cfg.jitterEnabled) {
            int jitterUs = jitterDist(rng);
            /* Add jitter as sub-ms variation. For >1ms jitter, add whole ms. */
            sleepMs += jitterUs / 1000;
        }
        Sleep(sleepMs);
    }

    /* ── Cleanup ── */
    printf("\n\n[Shutdown] Disabling override...\n");
    SendIoctl(hDevice, IOCTL_GAYM_OVERRIDE_OFF);
    provider->Shutdown();
    CloseHandle(hDevice);

    printf("[Shutdown] Done.\n");
    return 0;
}
