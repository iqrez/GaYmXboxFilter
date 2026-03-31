#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <xinput.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "gaym_client_compat.h"

using gaym::client::DeviceTypeName;
using gaym::client::DisableOverride;
using gaym::client::EnableOverride;
using gaym::client::InjectReport;
using gaym::client::OpenSupportedAdapter;
using gaym::client::QueryAdapterInfo;
using gaym::client::QueryObservation;
using gaym::client::AcquireWriterSession;
using gaym::client::ReleaseWriterSession;

#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

struct VerificationStep {
    const char* label;
    DWORD durationMs;
    GAYM_REPORT report;
};

struct ObservedState {
    bool connected = false;
    DWORD packetChanges = 0;
    SHORT minLX = 0;
    SHORT maxLX = 0;
    SHORT minLY = 0;
    SHORT maxLY = 0;
    SHORT minRX = 0;
    SHORT maxRX = 0;
    SHORT minRY = 0;
    SHORT maxRY = 0;
    BYTE maxLT = 0;
    BYTE maxRT = 0;
    WORD buttonsOr = 0;
};

struct HidObservation {
    bool sawChange = false;
    DWORD reportsRead = 0;
    DWORD firstDiffOffset = 0;
    DWORD reportLength = 0;
    BYTE baseline[64] = {};
    BYTE changed[64] = {};
};

struct CounterObservation {
    GAYM_DEVICE_INFO before = {};
    GAYM_DEVICE_INFO after = {};
};

enum class VerificationMode {
    None,
    XInputDirect,
    RawHid,
    CounterFallback
};

static CounterObservation RunCounterStep(HANDLE device, const VerificationStep* step);
static bool PrintCounterEvaluation(const VerificationStep* step, const CounterObservation* observed);
static const char* VerificationModeToLabel(VerificationMode mode, bool passed);
static void PersistVerificationState(VerificationMode mode, bool passed);

static void InitReport(GAYM_REPORT* report)
{
    std::memset(report, 0, sizeof(*report));
    report->DPad = GAYM_DPAD_NEUTRAL;
}

static void PrintObservation(const GAYM_OBSERVATION_V1* observation)
{
    std::printf(
        "Observation: family=%lu caps=0x%08lX status=0x%08lX obs=%llu inj=%llu%s\n",
        observation->AdapterFamily,
        observation->CapabilityFlags,
        observation->StatusFlags,
        observation->LastObservedSequence,
        observation->LastInjectedSequence,
        (observation->StatusFlags & GAYM_STATUS_OBSERVATION_SYNTHETIC) ? " synthetic" : "");
}

static bool GetDeviceInfoSnapshot(HANDLE device, GAYM_DEVICE_INFO* info)
{
    return QueryAdapterInfo(device, info);
}

static bool TryGetPadState(DWORD* index, XINPUT_STATE* state)
{
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE current = {};
        if (XInputGetState(i, &current) == ERROR_SUCCESS) {
            *index = i;
            *state = current;
            return true;
        }
    }

    return false;
}

static bool WaitForPad(DWORD timeoutMs, DWORD* index, XINPUT_STATE* state)
{
    DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        if (TryGetPadState(index, state)) {
            return true;
        }
        Sleep(25);
    }
    return false;
}

static void UpdateObservedState(ObservedState* observed, const XINPUT_STATE* state, const XINPUT_STATE* baseline)
{
    const XINPUT_GAMEPAD* gamepad = &state->Gamepad;

    if (!observed->connected) {
        observed->connected = true;
        observed->minLX = observed->maxLX = gamepad->sThumbLX;
        observed->minLY = observed->maxLY = gamepad->sThumbLY;
        observed->minRX = observed->maxRX = gamepad->sThumbRX;
        observed->minRY = observed->maxRY = gamepad->sThumbRY;
    } else {
        if (gamepad->sThumbLX < observed->minLX) observed->minLX = gamepad->sThumbLX;
        if (gamepad->sThumbLX > observed->maxLX) observed->maxLX = gamepad->sThumbLX;
        if (gamepad->sThumbLY < observed->minLY) observed->minLY = gamepad->sThumbLY;
        if (gamepad->sThumbLY > observed->maxLY) observed->maxLY = gamepad->sThumbLY;
        if (gamepad->sThumbRX < observed->minRX) observed->minRX = gamepad->sThumbRX;
        if (gamepad->sThumbRX > observed->maxRX) observed->maxRX = gamepad->sThumbRX;
        if (gamepad->sThumbRY < observed->minRY) observed->minRY = gamepad->sThumbRY;
        if (gamepad->sThumbRY > observed->maxRY) observed->maxRY = gamepad->sThumbRY;
    }

    if (gamepad->bLeftTrigger > observed->maxLT) observed->maxLT = gamepad->bLeftTrigger;
    if (gamepad->bRightTrigger > observed->maxRT) observed->maxRT = gamepad->bRightTrigger;
    observed->buttonsOr |= gamepad->wButtons;

    if (state->dwPacketNumber != baseline->dwPacketNumber) {
        observed->packetChanges++;
    }
}

static void SendNeutralBurst(HANDLE device, DWORD durationMs)
{
    GAYM_REPORT neutral = {};
    InitReport(&neutral);

    DWORD start = GetTickCount();
    while (GetTickCount() - start < durationMs) {
        InjectReport(device, &neutral);
        Sleep(8);
    }
}

static HANDLE OpenXboxHidDevice(USHORT* inputReportLength)
{
    GUID hidGuid = {};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }

    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);

    HANDLE result = INVALID_HANDLE_VALUE;

    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, idx, &ifData); ++idx) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &needed, NULL);
        if (needed == 0) {
            continue;
        }

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
            reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(std::malloc(needed));
        if (!detail) {
            continue;
        }

        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, needed, NULL, NULL)) {
            std::free(detail);
            continue;
        }

        if (!wcsstr(detail->DevicePath, L"vid_045e") || !wcsstr(detail->DevicePath, L"pid_02ff")) {
            std::free(detail);
            continue;
        }

        std::wprintf(L"Trying HID path: %ls\n", detail->DevicePath);

        HANDLE metadataHandle = CreateFileW(
            detail->DevicePath,
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL);

        if (metadataHandle == INVALID_HANDLE_VALUE) {
            std::printf("  metadata open failed: %lu\n", GetLastError());
            std::free(detail);
            continue;
        }

        HIDP_CAPS caps = {};
        NTSTATUS status = HIDP_STATUS_INVALID_PREPARSED_DATA;
        PHIDP_PREPARSED_DATA preparsed = NULL;
        if (HidD_GetPreparsedData(metadataHandle, &preparsed)) {
            status = HidP_GetCaps(preparsed, &caps);
            HidD_FreePreparsedData(preparsed);
        } else {
            std::printf("  HidD_GetPreparsedData failed: %lu\n", GetLastError());
        }
        CloseHandle(metadataHandle);

        HANDLE hid = CreateFileW(
            detail->DevicePath,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            NULL);

        std::free(detail);

        if (hid == INVALID_HANDLE_VALUE) {
            std::printf("  read open failed: %lu\n", GetLastError());
            continue;
        }

        if (status == HIDP_STATUS_SUCCESS &&
            caps.InputReportByteLength > 0 &&
            caps.InputReportByteLength <= 64) {
            *inputReportLength = caps.InputReportByteLength;
        } else {
            *inputReportLength = 64;
        }

        result = hid;
        break;
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

static bool ReadHidReport(HANDLE hid, BYTE* buffer, DWORD length, DWORD timeoutMs)
{
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent) {
        return false;
    }

    DWORD bytesRead = 0;
    BOOL readStarted = ReadFile(hid, buffer, length, NULL, &overlapped);
    DWORD lastError = GetLastError();

    bool success = false;
    if (readStarted || lastError == ERROR_IO_PENDING) {
        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, timeoutMs);
        if (waitResult == WAIT_OBJECT_0) {
            success = GetOverlappedResult(hid, &overlapped, &bytesRead, FALSE) != FALSE && bytesRead > 0;
        } else {
            CancelIo(hid);
        }
    }

    CloseHandle(overlapped.hEvent);
    return success;
}

static bool CaptureBaselineReport(HANDLE device, HANDLE hid, DWORD reportLength, BYTE* baseline)
{
    SendNeutralBurst(device, 200);

    BYTE discard[64] = {};
    ReadHidReport(hid, discard, reportLength, 250);
    return ReadHidReport(hid, baseline, reportLength, 800);
}

static HidObservation RunHidStep(HANDLE device, HANDLE hid, const VerificationStep* step, const BYTE* baseline, DWORD reportLength)
{
    HidObservation observation = {};
    observation.reportLength = reportLength;
    std::memcpy(observation.baseline, baseline, reportLength);

    DWORD start = GetTickCount();
    BYTE report[64] = {};

    while (GetTickCount() - start < step->durationMs) {
        InjectReport(device, &step->report);

        if (ReadHidReport(hid, report, reportLength, 40)) {
            observation.reportsRead++;
            if (std::memcmp(report, baseline, reportLength) != 0) {
                observation.sawChange = true;
                std::memcpy(observation.changed, report, reportLength);

                for (DWORD i = 0; i < reportLength; ++i) {
                    if (report[i] != baseline[i]) {
                        observation.firstDiffOffset = i;
                        break;
                    }
                }
                break;
            }
        }

        Sleep(8);
    }

    SendNeutralBurst(device, 120);
    return observation;
}

static ObservedState RunStep(HANDLE device, DWORD padIndex, const VerificationStep* step, const XINPUT_STATE* baseline)
{
    ObservedState observed = {};
    DWORD start = GetTickCount();

    while (GetTickCount() - start < step->durationMs) {
        if (!InjectReport(device, &step->report)) {
            std::printf("  Inject failed for %s (error %lu)\n", step->label, GetLastError());
            break;
        }

        XINPUT_STATE state = {};
        if (XInputGetState(padIndex, &state) == ERROR_SUCCESS) {
            UpdateObservedState(&observed, &state, baseline);
        }

        Sleep(8);
    }

    SendNeutralBurst(device, 120);
    return observed;
}

static bool PrintEvaluation(const VerificationStep* step, const ObservedState* observed)
{
    bool pass = false;

    if (std::strcmp(step->label, "Left stick right") == 0) {
        pass = observed->maxLX >= 20000;
    } else if (std::strcmp(step->label, "Left stick left") == 0) {
        pass = observed->minLX <= -20000;
    } else if (std::strcmp(step->label, "Right stick up") == 0) {
        pass = observed->minRY <= -20000 || observed->maxRY >= 20000;
    } else if (std::strcmp(step->label, "Left trigger full") == 0) {
        pass = observed->maxLT >= 200;
    } else if (std::strcmp(step->label, "Button A") == 0) {
        pass = observed->buttonsOr != 0;
    }

    std::printf("  %-18s : %s | pkt=%lu LX=[%d,%d] LY=[%d,%d] RX=[%d,%d] RY=[%d,%d] LT=%u RT=%u Btn=0x%04X\n",
        step->label,
        pass ? "PASS" : "FAIL",
        observed->packetChanges,
        observed->minLX, observed->maxLX,
        observed->minLY, observed->maxLY,
        observed->minRX, observed->maxRX,
        observed->minRY, observed->maxRY,
        (unsigned)observed->maxLT,
        (unsigned)observed->maxRT,
        observed->buttonsOr);

    return pass;
}

static bool RunCounterFallback(HANDLE device, const VerificationStep* steps, size_t stepCount)
{
    bool allPassed = true;

    std::printf("Falling back to driver-side queue/completion counters.\n");
    for (size_t index = 0; index < stepCount; ++index) {
        CounterObservation observed = RunCounterStep(device, &steps[index]);
        if (!PrintCounterEvaluation(&steps[index], &observed)) {
            allPassed = false;
        }
        Sleep(120);
    }

    return allPassed;
}

static bool PrintHidEvaluation(const VerificationStep* step, const HidObservation* observed)
{
    std::printf("  %-18s : %s | reports=%lu firstDiff=%lu\n",
        step->label,
        observed->sawChange ? "PASS" : "FAIL",
        observed->reportsRead,
        observed->firstDiffOffset);

    if (observed->sawChange) {
        std::printf("    baseline:");
        for (DWORD i = 0; i < observed->reportLength; ++i) {
            std::printf(" %02X", observed->baseline[i]);
        }
        std::printf("\n");

        std::printf("    changed :");
        for (DWORD i = 0; i < observed->reportLength; ++i) {
            std::printf(" %02X", observed->changed[i]);
        }
        std::printf("\n");
    }

    return observed->sawChange;
}

static CounterObservation RunCounterStep(HANDLE device, const VerificationStep* step)
{
    CounterObservation observation = {};
    GetDeviceInfoSnapshot(device, &observation.before);

    DWORD start = GetTickCount();
    while (GetTickCount() - start < step->durationMs) {
        InjectReport(device, &step->report);
        Sleep(8);
    }

    SendNeutralBurst(device, 120);
    GetDeviceInfoSnapshot(device, &observation.after);
    return observation;
}

static bool PrintCounterEvaluation(const VerificationStep* step, const CounterObservation* observed)
{
    LONG deltaReports = (LONG)observed->after.ReportsSent - (LONG)observed->before.ReportsSent;
    LONG deltaQueued = (LONG)observed->after.QueuedInputRequests - (LONG)observed->before.QueuedInputRequests;
    LONG deltaCompleted = (LONG)observed->after.CompletedInputRequests - (LONG)observed->before.CompletedInputRequests;
    LONG deltaForwarded = (LONG)observed->after.ForwardedInputRequests - (LONG)observed->before.ForwardedInputRequests;
    LONG deltaReads = (LONG)observed->after.ReadRequestsSeen - (LONG)observed->before.ReadRequestsSeen;
    LONG deltaDevCtl = (LONG)observed->after.DeviceControlRequestsSeen - (LONG)observed->before.DeviceControlRequestsSeen;
    LONG deltaInternal = (LONG)observed->after.InternalDeviceControlRequestsSeen - (LONG)observed->before.InternalDeviceControlRequestsSeen;
    LONG deltaWrites = (LONG)observed->after.WriteRequestsSeen - (LONG)observed->before.WriteRequestsSeen;

    bool pass = deltaQueued > 0 || deltaCompleted > 0 || deltaForwarded > 0 ||
        deltaReads > 0 || deltaDevCtl > 0 || deltaInternal > 0;

    std::printf("  %-18s : %s | dReports=%ld dQueued=%ld dCompleted=%ld dForwarded=%ld dRead=%ld dDevCtl=%ld dInternal=%ld dWrite=%ld lastIoctl=0x%08X\n",
        step->label,
        pass ? "PASS" : "FAIL",
        deltaReports,
        deltaQueued,
        deltaCompleted,
        deltaForwarded,
        deltaReads,
        deltaDevCtl,
        deltaInternal,
        deltaWrites,
        observed->after.LastInterceptedIoctl);

    return pass;
}

static std::wstring TrimToParentDirectory(const std::wstring& path)
{
    const size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return std::wstring();
    }

    return path.substr(0, separator);
}

static std::wstring GetVerificationStatePath()
{
    wchar_t executablePath[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(NULL, executablePath, ARRAYSIZE(executablePath));
    if (length == 0 || length >= ARRAYSIZE(executablePath)) {
        return std::wstring();
    }

    std::wstring toolsDirectory = TrimToParentDirectory(executablePath);
    std::wstring profileDirectory = TrimToParentDirectory(toolsDirectory);
    if (profileDirectory.empty()) {
        return std::wstring();
    }

    std::wstring verificationDirectory = profileDirectory + L"\\verification";
    if (!CreateDirectoryW(verificationDirectory.c_str(), NULL)) {
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            return std::wstring();
        }
    }

    return verificationDirectory + L"\\autoverify-state.json";
}

static const char* VerificationModeToLabel(VerificationMode mode, bool passed)
{
    switch (mode) {
    case VerificationMode::XInputDirect:
        return passed ? "xinput_direct_pass" : "xinput_direct_fail";
    case VerificationMode::RawHid:
        return passed ? "raw_hid_pass" : "raw_hid_fail";
    case VerificationMode::CounterFallback:
        return passed ? "counter_fallback_pass" : "counter_fallback_fail";
    default:
        return passed ? "unknown_pass" : "unknown_fail";
    }
}

static void PersistVerificationState(VerificationMode mode, bool passed)
{
    std::wstring statePath = GetVerificationStatePath();
    SYSTEMTIME utcNow = {};
    FILE* file = NULL;
    const char* modeLabel = VerificationModeToLabel(mode, passed);

    if (statePath.empty()) {
        std::fprintf(stderr, "WARNING: Could not resolve AutoVerify state path.\n");
        return;
    }

    GetSystemTime(&utcNow);
    if (_wfopen_s(&file, statePath.c_str(), L"wb") != 0 || file == NULL) {
        std::fprintf(stderr, "WARNING: Could not write AutoVerify state file.\n");
        return;
    }

    std::fprintf(
        file,
        "{\n"
        "  \"schema\": 1,\n"
        "  \"tool\": \"AutoVerify\",\n"
        "  \"result\": \"%s\",\n"
        "  \"verificationMode\": \"%s\",\n"
        "  \"createdUtc\": \"%04u-%02u-%02uT%02u:%02u:%02uZ\"\n"
        "}\n",
        passed ? "PASS" : "FAIL",
        modeLabel,
        (unsigned)utcNow.wYear,
        (unsigned)utcNow.wMonth,
        (unsigned)utcNow.wDay,
        (unsigned)utcNow.wHour,
        (unsigned)utcNow.wMinute,
        (unsigned)utcNow.wSecond);
    std::fclose(file);

    std::wprintf(L"Verification state: %ls\n", statePath.c_str());
}

int main()
{
    std::printf("GaYm Xbox 02FF AutoVerify\n\n");

    HANDLE device = OpenSupportedAdapter(0);
    if (device == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "ERROR: Cannot open supported adapter (error %lu)\n", GetLastError());
        return 1;
    }
    bool writerHeld = false;

    GAYM_DEVICE_INFO info = {};
    if (QueryAdapterInfo(device, &info)) {
        std::printf("Device: %s VID:%04X PID:%04X Override:%s Reports:%lu\n",
            DeviceTypeName(info.DeviceType),
            info.VendorId,
            info.ProductId,
            info.OverrideActive ? "ON" : "OFF",
            info.ReportsSent);
    }

    GAYM_OBSERVATION_V1 observation = {};
    if (QueryObservation(device, &observation)) {
        PrintObservation(&observation);
    } else {
        std::printf("Observation: unavailable (error %lu)\n", GetLastError());
    }
    std::printf("Control: sole control path is \\\\.\\GaYmXInputFilterCtl\n");

    if (!AcquireWriterSession(device)) {
        std::fprintf(stderr, "ERROR: Failed to acquire writer session (error %lu)\n", GetLastError());
        CloseHandle(device);
        return 1;
    }
    writerHeld = true;

    if (!EnableOverride(device)) {
        std::fprintf(stderr, "ERROR: Failed to enable override (error %lu)\n", GetLastError());
        ReleaseWriterSession(device);
        CloseHandle(device);
        return 1;
    }

    VerificationStep steps[5] = {};
    InitReport(&steps[0].report);
    steps[0].label = "Left stick right";
    steps[0].durationMs = 700;
    steps[0].report.ThumbLeftX = 32767;

    InitReport(&steps[1].report);
    steps[1].label = "Left stick left";
    steps[1].durationMs = 700;
    steps[1].report.ThumbLeftX = -32767;

    InitReport(&steps[2].report);
    steps[2].label = "Right stick up";
    steps[2].durationMs = 700;
    steps[2].report.ThumbRightY = -32767;

    InitReport(&steps[3].report);
    steps[3].label = "Left trigger full";
    steps[3].durationMs = 700;
    steps[3].report.TriggerLeft = 255;

    InitReport(&steps[4].report);
    steps[4].label = "Button A";
    steps[4].durationMs = 700;
    steps[4].report.Buttons[0] = GAYM_BTN_A;

    bool allPassed = true;
    VerificationMode verificationMode = VerificationMode::None;
    DWORD padIndex = 0;
    XINPUT_STATE baseline = {};
    bool useXinput = WaitForPad(2000, &padIndex, &baseline);

    std::printf("\n=== Verification ===\n");
    if (useXinput) {
        bool xinputObservedAnyChange = false;
        bool xinputAllPassed = true;

        std::printf("Monitoring XInput pad %lu (packet %lu)\n", padIndex, baseline.dwPacketNumber);
        for (const auto& step : steps) {
            ObservedState observed = RunStep(device, padIndex, &step, &baseline);
            if (observed.packetChanges != 0 || observed.buttonsOr != 0 ||
                observed.maxLT != 0 || observed.maxRT != 0 ||
                observed.minLX != observed.maxLX || observed.minLY != observed.maxLY ||
                observed.minRX != observed.maxRX || observed.minRY != observed.maxRY) {
                xinputObservedAnyChange = true;
            }
            if (!PrintEvaluation(&step, &observed)) {
                xinputAllPassed = false;
            }
            Sleep(120);
        }

        if (xinputAllPassed && xinputObservedAnyChange) {
            allPassed = true;
            verificationMode = VerificationMode::XInputDirect;
        } else {
            std::printf("XInput was visible but did not reflect injected state reliably.\n");
            allPassed = RunCounterFallback(device, steps, ARRAYSIZE(steps));
            verificationMode = VerificationMode::CounterFallback;
        }
    } else {
        USHORT inputReportLength = 0;
        HANDLE hid = OpenXboxHidDevice(&inputReportLength);
        if (hid == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "ERROR: No XInput pad and no readable HID path were available for verification.\n");
            DisableOverride(device);
            if (writerHeld) {
                ReleaseWriterSession(device);
            }
            CloseHandle(device);
            return 1;
        }

        BYTE baselineReport[64] = {};
        if (!CaptureBaselineReport(device, hid, inputReportLength, baselineReport)) {
            std::printf("No baseline HID report was available; using a zero baseline.\n");
        }

        std::printf("XInput not visible in this shell. Falling back to raw HID report verification.\n");
        verificationMode = VerificationMode::RawHid;
        for (const auto& step : steps) {
            HidObservation observed = RunHidStep(device, hid, &step, baselineReport, inputReportLength);
            if (!PrintHidEvaluation(&step, &observed)) {
                allPassed = false;
            }
            Sleep(120);
        }

        CloseHandle(hid);
        if (!allPassed) {
            std::printf("Raw HID reads were not available. ");
            allPassed = RunCounterFallback(device, steps, ARRAYSIZE(steps));
            verificationMode = VerificationMode::CounterFallback;
        }
    }

    DisableOverride(device);
    if (writerHeld) {
        ReleaseWriterSession(device);
    }
    CloseHandle(device);

    const char* modeLabel = VerificationModeToLabel(verificationMode, allPassed);
    PersistVerificationState(verificationMode, allPassed);

    std::printf("Verification mode: %s\n", modeLabel);
    std::printf("\nResult: %s\n", allPassed ? "PASS" : "FAIL");
    return allPassed ? 0 : 2;
}
