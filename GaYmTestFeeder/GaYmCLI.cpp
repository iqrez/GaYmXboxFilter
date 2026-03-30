/*
 * GaYm Controller CLI - Standalone command-line control tool.
 *
 * Sends IOCTLs to the GaYmFilter driver without running the feeder loop.
 *
 * Usage:
 *   GaYmCLI.exe status                    List devices and override state
 *   GaYmCLI.exe on    [device_index]      Enable override
 *   GaYmCLI.exe off   [device_index]      Disable override
 *   GaYmCLI.exe jitter <min_us> <max_us>  Set timing jitter (maintainer builds only)
 *   GaYmCLI.exe jitter off                Disable jitter (maintainer builds only)
 *   GaYmCLI.exe test  [device_index]      Send a test report (A + left stick right)
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <xinput.h>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>

#include "DeviceHelper.h"

#pragma comment(lib, "xinput.lib")

static const char* TracePhaseName(ULONG phase)
{
    switch (phase) {
    case GAYM_TRACE_PHASE_DISPATCH:     return "dispatch";
    case GAYM_TRACE_PHASE_COMPLETION:   return "complete";
    case GAYM_TRACE_PHASE_SEND_FAILURE: return "sendfail";
    default:                            return "none";
    }
}

static const char* TraceRequestTypeName(ULONG requestType)
{
    switch (requestType) {
    case GAYM_TRACE_REQUEST_READ:                    return "read";
    case GAYM_TRACE_REQUEST_WRITE:                   return "write";
    case GAYM_TRACE_REQUEST_DEVICE_CONTROL:          return "devctl";
    case GAYM_TRACE_REQUEST_INTERNAL_DEVICE_CONTROL: return "internal";
    default:                                         return "unknown";
    }
}

static void PrintTraceSample(const GAYM_TRACE_ENTRY& entry)
{
    if (entry.SampleLength == 0) {
        return;
    }

    printf(" sample=");
    for (UCHAR index = 0; index < entry.SampleLength; ++index) {
        printf("%02X", entry.Sample[index]);
    }
}

static void PrintHexBytes(const char* label, const UCHAR* bytes, ULONG length)
{
    if (length == 0) {
        return;
    }

    printf("       %s", label);
    for (ULONG index = 0; index < length; ++index) {
        printf("%02X", bytes[index]);
    }
    printf("\n");
}

static void PrintCaptureFlags(ULONG captureFlags)
{
    if (captureFlags == 0) {
        printf("none");
        return;
    }

    bool first = true;
    auto printFlag = [&](const char* text) {
        if (!first) {
            printf("|");
        }
        printf("%s", text);
        first = false;
    };

    if (captureFlags & GAYM_CAPTURE_FLAG_VALID) {
        printFlag("valid");
    }
    if (captureFlags & GAYM_CAPTURE_FLAG_PARTIAL) {
        printFlag("partial");
    }
    if (captureFlags & GAYM_CAPTURE_FLAG_TRIGGERS_COMBINED) {
        printFlag("combined-triggers");
    }
    if (captureFlags & GAYM_CAPTURE_FLAG_SOURCE_NATIVE_READ) {
        printFlag("native-read");
    }
}

struct CapabilityFlagName {
    GAYM_CAPABILITY_FLAGS flag;
    const char* name;
};

static const CapabilityFlagName kInputCapabilityNames[] = {
    { GAYM_INPUT_CAP_FACE_BUTTONS, "face-buttons" },
    { GAYM_INPUT_CAP_SHOULDERS, "shoulders" },
    { GAYM_INPUT_CAP_MENU_BUTTONS, "menu-buttons" },
    { GAYM_INPUT_CAP_GUIDE_BUTTON, "guide-button" },
    { GAYM_INPUT_CAP_MISC_BUTTON, "misc-button" },
    { GAYM_INPUT_CAP_STICK_CLICKS, "stick-clicks" },
    { GAYM_INPUT_CAP_DPAD_8WAY, "dpad-8way" },
    { GAYM_INPUT_CAP_ANALOG_TRIGGERS, "analog-triggers" },
    { GAYM_INPUT_CAP_LEFT_STICK, "left-stick" },
    { GAYM_INPUT_CAP_RIGHT_STICK, "right-stick" },
    { GAYM_INPUT_CAP_TOUCH_CLICK, "touch-click" },
    { GAYM_INPUT_CAP_TOUCH_SURFACE, "touch-surface" },
    { GAYM_INPUT_CAP_MOTION_SENSORS, "motion-sensors" },
};

static const CapabilityFlagName kOutputCapabilityNames[] = {
    { GAYM_OUTPUT_CAP_DUAL_MOTOR_RUMBLE, "dual-motor-rumble" },
    { GAYM_OUTPUT_CAP_TRIGGER_RUMBLE, "trigger-rumble" },
    { GAYM_OUTPUT_CAP_PLAYER_LIGHT, "player-light" },
    { GAYM_OUTPUT_CAP_RGB_LIGHT, "rgb-light" },
    { GAYM_OUTPUT_CAP_MUTE_LIGHT, "mute-light" },
    { GAYM_OUTPUT_CAP_TRIGGER_EFFECTS, "trigger-effects" },
    { GAYM_OUTPUT_CAP_VENDOR_EFFECTS, "vendor-effects" },
};

static void PrintCapabilityFlags(
    const char* label,
    GAYM_CAPABILITY_FLAGS flags,
    const CapabilityFlagName* names,
    size_t nameCount)
{
    printf("  %s0x%016llX", label, static_cast<unsigned long long>(flags));
    if (flags == 0) {
        printf(" none\n");
        return;
    }

    printf(" ");

    bool first = true;
    for (size_t index = 0; index < nameCount; ++index) {
        if ((flags & names[index].flag) == 0) {
            continue;
        }

        if (!first) {
            printf("|");
        }

        printf("%s", names[index].name);
        first = false;
    }

    if (first) {
        printf("unknown");
    }

    printf("\n");
}

static UCHAR ScaleMotorWordToByte(WORD value)
{
    return static_cast<UCHAR>((static_cast<ULONG>(value) * 255u + 32767u) / 65535u);
}

static void PrintOutputState(const GAYM_OUTPUT_STATE& outputState)
{
    printf(
        "mask=0x%08lX low=%u high=%u lt=%u rt=%u player=%u light=%u vendor0=0x%02X\n",
        outputState.ActiveMask,
        outputState.LowFrequencyMotor,
        outputState.HighFrequencyMotor,
        outputState.LeftTriggerMotor,
        outputState.RightTriggerMotor,
        outputState.PlayerIndex,
        outputState.LightMode,
        outputState.VendorDefined[0]);
}

static void PrintReportState(const GAYM_REPORT& report)
{
    printf(
        "btn0=0x%02X btn1=0x%02X dpad=%u lt=%u rt=%u lx=%d ly=%d rx=%d ry=%d\n",
        report.Buttons[0],
        report.Buttons[1],
        report.DPad,
        report.TriggerLeft,
        report.TriggerRight,
        report.ThumbLeftX,
        report.ThumbLeftY,
        report.ThumbRightX,
        report.ThumbRightY);
}

static void PrintUsage()
{
    printf("GaYm Controller CLI v2.0\n\n");
    printf("Usage:\n");
    printf("  GaYmCLI status                     Show all devices and state\n");
    printf("  GaYmCLI poll [device_index]        Show HID poll interval for the supported controller\n");
    printf("  GaYmCLI poll <ms> [device_index]   Set HID poll interval in milliseconds\n");
    printf("  GaYmCLI wakecheck [seconds] [interval_ms] [device_index]\n");
    printf("  GaYmCLI rumble <left> <right> [duration_ms] [pad_index]\n");
    printf("  GaYmCLI vrumble <left> <right> [duration_ms] [device_index]\n");
    printf("  GaYmCLI on    [device_index]       Enable input override\n");
    printf("  GaYmCLI off   [device_index]       Disable input override\n");
#if GAYM_ENABLE_DEV_DIAGNOSTICS
    printf("  GaYmCLI jitter <min_us> <max_us>   Set timing jitter range (dev builds only)\n");
    printf("  GaYmCLI jitter off                 Disable jitter (dev builds only)\n");
#endif
    printf("  GaYmCLI test  [device_index]       Inject a test report\n");
    printf("  GaYmCLI report [device_index] <btn0> <btn1> <dpad> <lt> <rt> <lx> <ly> <rx> <ry>\n");
}

static bool DeviceInfoHasField(DWORD bytesReturned, size_t fieldOffset, size_t fieldSize)
{
    return bytesReturned >= fieldOffset + fieldSize;
}

struct GaYmProtocolStatusBundle {
    GAYM_QUERY_DEVICE_SUMMARY_RESPONSE summary;
    GAYM_QUERY_RUNTIME_COUNTERS_RESPONSE runtime;
    GAYM_QUERY_LAST_IO_RESPONSE lastIo;
    GAYM_QUERY_TRACE_RESPONSE trace;
    GAYM_QUERY_OUTPUT_RESPONSE output;
    DWORD summaryBytes;
    DWORD runtimeBytes;
    DWORD lastIoBytes;
    DWORD traceBytes;
    DWORD outputBytes;
};

static void PrintTraceEntries(const GAYM_TRACE_SNAPSHOT& traceSnapshot)
{
    const ULONG traceCount = traceSnapshot.TraceCount > GAYM_TRACE_HISTORY_COUNT
        ? GAYM_TRACE_HISTORY_COUNT
        : traceSnapshot.TraceCount;

    for (ULONG traceIndex = 0; traceIndex < traceCount; ++traceIndex) {
        const GAYM_TRACE_ENTRY& entry = traceSnapshot.Trace[traceIndex];
        printf("  #%lu %-8s %-8s ioctl=0x%08lX in=%lu out=%lu xfer=%lu status=0x%08lX",
            entry.Sequence,
            TracePhaseName(entry.Phase),
            TraceRequestTypeName(entry.RequestType),
            entry.IoControlCode,
            entry.InputLength,
            entry.OutputLength,
            entry.TransferLength,
            entry.Status);
        PrintTraceSample(entry);
        printf("\n");
    }
}

static bool QueryProtocolStatusBundle(HANDLE device, GaYmProtocolStatusBundle* bundle)
{
    ZeroMemory(bundle, sizeof(*bundle));

    return QueryDeviceSummary(device, &bundle->summary, &bundle->summaryBytes) &&
        QueryRuntimeCounters(device, &bundle->runtime, &bundle->runtimeBytes) &&
        QueryLastIoSnapshot(device, &bundle->lastIo, &bundle->lastIoBytes) &&
        QueryTraceSnapshot(device, &bundle->trace, &bundle->traceBytes) &&
        QueryOutputSnapshot(device, &bundle->output, &bundle->outputBytes);
}

static bool TryPrintProtocolDeviceStatusBlock(
    const char* label,
    const GaYmDevicePath& devicePath,
    HANDLE device)
{
    GaYmProtocolStatusBundle bundle = {};
    const GAYM_DEVICE_SUMMARY* summary;
    const GAYM_RUNTIME_COUNTERS* runtime;
    const GAYM_LAST_IO_SNAPSHOT* lastIo;
    const GAYM_TRACE_SNAPSHOT* trace;
    const GAYM_OUTPUT_SNAPSHOT* output;

    if (!QueryProtocolStatusBundle(device, &bundle)) {
        return false;
    }

    summary = &bundle.summary.Payload;
    runtime = &bundle.runtime.Payload;
    lastIo = &bundle.lastIo.Payload;
    trace = &bundle.trace.Payload;
    output = &bundle.output.Payload;

    printf("%s [%d] %ls\n", label, devicePath.index, devicePath.path.c_str());
    printf("  %s  VID:%04X PID:%04X  Override:%s  Reports:%lu\n",
        DeviceTypeName(summary->DeviceType),
        summary->VendorId,
        summary->ProductId,
        summary->OverrideActive ? "ON" : "OFF",
        summary->ReportsSent);
    printf("  Read:%lu Write:%lu DevCtl:%lu IntCtl:%lu Pending:%lu Fwd:%lu Done:%lu\n",
        runtime->ReadRequestsSeen,
        runtime->WriteRequestsSeen,
        runtime->DeviceControlRequestsSeen,
        runtime->InternalDeviceControlRequestsSeen,
        runtime->PendingInputRequests,
        runtime->ForwardedInputRequests,
        runtime->CompletedInputRequests);
    printf("  QueryBytes:%lu/%lu/%lu/%lu/%lu Layout:protocol-v%u Build:0x%08lX\n",
        bundle.summaryBytes,
        bundle.runtimeBytes,
        bundle.lastIoBytes,
        bundle.traceBytes,
        bundle.outputBytes,
        GAYM_PROTOCOL_ABI_MAJOR,
        summary->DriverBuildStamp);

    PrintCapabilityFlags(
        "InputCaps : ",
        summary->InputCapabilities,
        kInputCapabilityNames,
        sizeof(kInputCapabilityNames) / sizeof(kInputCapabilityNames[0]));
    PrintCapabilityFlags(
        "OutputCaps: ",
        summary->OutputCapabilities,
        kOutputCapabilityNames,
        sizeof(kOutputCapabilityNames) / sizeof(kOutputCapabilityNames[0]));

    printf("  LastIoctl:0x%08lX LastStatus:0x%08lX LastInfo:%lu\n",
        runtime->LastInterceptedIoctl,
        lastIo->LastCompletedStatus,
        lastIo->LastCompletionInformation);
    printf("  LastLens: read=%lu write=%lu devctl=%lu/%lu internal=%lu/%lu\n",
        lastIo->LastReadLength,
        lastIo->LastWriteLength,
        lastIo->LastDeviceControlInputLength,
        lastIo->LastDeviceControlOutputLength,
        lastIo->LastInternalInputLength,
        lastIo->LastInternalOutputLength);

    if (lastIo->LastWriteLength != 0 || output->LastWriteSampleLength != 0) {
        printf("  OutputWrite: len=%lu sampleLen=%lu\n",
            lastIo->LastWriteLength,
            output->LastWriteSampleLength);
        PrintHexBytes("WriteSample : ", output->LastWriteSample, output->LastWriteSampleLength);
    }

    if (output->LastOutputCaptureLength != 0 || output->LastOutputCaptureSampleLength != 0) {
        printf(
            "  OutputCapture: ioctl=0x%08lX len=%lu sampleLen=%lu ",
            output->LastOutputCaptureIoctl,
            output->LastOutputCaptureLength,
            output->LastOutputCaptureSampleLength);
        PrintOutputState(output->LastOutputCaptureState);
        PrintHexBytes("OutputSample : ", output->LastOutputCaptureSample, output->LastOutputCaptureSampleLength);
    }

    if (lastIo->LastRawReadSampleLength != 0 || lastIo->LastPatchedReadSampleLength != 0) {
        printf("  NativeRead: rawLen=%lu rawInfo=%lu patchedLen=%lu patchedInfo=%lu applied=%lu bytes=%lu\n",
            lastIo->LastRawReadSampleLength,
            lastIo->LastRawReadCompletionLength,
            lastIo->LastPatchedReadSampleLength,
            lastIo->LastPatchedReadCompletionLength,
            lastIo->LastNativeOverrideApplied,
            lastIo->LastNativeOverrideBytesWritten);
        PrintHexBytes("RawSample    : ", lastIo->LastRawReadSample, lastIo->LastRawReadSampleLength);
        PrintHexBytes("PatchedSample: ", lastIo->LastPatchedReadSample, lastIo->LastPatchedReadSampleLength);
    }

    printf("  SemanticCapture: flags=");
    PrintCaptureFlags(lastIo->LastSemanticCaptureFlags);
    printf(" len=%lu ", lastIo->LastSemanticCaptureLength);
    PrintReportState(lastIo->LastSemanticCaptureReport);
    if (lastIo->LastSemanticCaptureSampleLength != 0) {
        printf("  SemanticSource: ioctl=0x%08lX len=%lu\n",
            lastIo->LastSemanticCaptureIoctl,
            lastIo->LastSemanticCaptureSampleLength);
        PrintHexBytes("SemanticSample: ", lastIo->LastSemanticCaptureSample, lastIo->LastSemanticCaptureSampleLength);
    }

    if (trace->TraceCount != 0) {
        PrintTraceEntries(*trace);
    }

    printf("\n");
    return true;
}

static void PrintDeviceStatusBlock(
    const char* label,
    const GaYmDevicePath& devicePath,
    HANDLE device)
{
    if (TryPrintProtocolDeviceStatusBlock(label, devicePath, device)) {
        return;
    }

    GAYM_DEVICE_INFO info;
    DWORD bytesReturned = 0;
    const bool queryOk = QueryDeviceInfo(device, &info, &bytesReturned);
    const bool hasLayoutField = queryOk &&
        DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, QueryLayoutVersion), sizeof(info.QueryLayoutVersion));
    const ULONG layoutVersion = hasLayoutField ? info.QueryLayoutVersion : 0;

    printf("%s [%d] %ls\n", label, devicePath.index, devicePath.path.c_str());
    if (!queryOk) {
        printf("  query failed (error %lu)\n\n", GetLastError());
        return;
    }

    printf("  %s  VID:%04X PID:%04X  Override:%s  Reports:%lu\n",
        DeviceTypeName(info.DeviceType),
        info.VendorId,
        info.ProductId,
        info.OverrideActive ? "ON" : "OFF",
        info.ReportsSent);
    printf("  Read:%lu Write:%lu DevCtl:%lu IntCtl:%lu Pending:%lu Fwd:%lu Done:%lu\n",
        info.ReadRequestsSeen,
        info.WriteRequestsSeen,
        info.DeviceControlRequestsSeen,
        info.InternalDeviceControlRequestsSeen,
        info.PendingInputRequests,
        info.ForwardedInputRequests,
        info.CompletedInputRequests);
    printf("  QueryBytes:%lu", bytesReturned);
    if (hasLayoutField) {
        printf(" Layout:%lu Build:0x%08lX", layoutVersion, info.DriverBuildStamp);
    } else {
        printf(" Layout:legacy");
    }
    printf("\n");

    if (layoutVersion >= 4 &&
        DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, OutputCapabilities), sizeof(info.OutputCapabilities))) {
        PrintCapabilityFlags(
            "InputCaps : ",
            info.InputCapabilities,
            kInputCapabilityNames,
            sizeof(kInputCapabilityNames) / sizeof(kInputCapabilityNames[0]));
        PrintCapabilityFlags(
            "OutputCaps: ",
            info.OutputCapabilities,
            kOutputCapabilityNames,
            sizeof(kOutputCapabilityNames) / sizeof(kOutputCapabilityNames[0]));
    }

    if (DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, LastCompletedStatus), sizeof(info.LastCompletionInformation))) {
        printf("  LastIoctl:0x%08lX LastStatus:0x%08lX LastInfo:%lu\n",
            info.LastInterceptedIoctl,
            info.LastCompletedStatus,
            info.LastCompletionInformation);
    } else {
        printf("  LastIoctl:0x%08lX\n", info.LastInterceptedIoctl);
    }

    if (DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, LastReadLength), sizeof(info.LastInternalOutputLength))) {
        printf("  LastLens: read=%lu write=%lu devctl=%lu/%lu internal=%lu/%lu\n",
            info.LastReadLength,
            info.LastWriteLength,
            info.LastDeviceControlInputLength,
            info.LastDeviceControlOutputLength,
            info.LastInternalInputLength,
            info.LastInternalOutputLength);
    }

    if (layoutVersion >= 4 &&
        DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, LastWriteSample), sizeof(info.LastWriteSample)) &&
        (info.LastWriteLength != 0 || info.LastWriteSampleLength != 0)) {
        printf("  OutputWrite: len=%lu sampleLen=%lu\n",
            info.LastWriteLength,
            info.LastWriteSampleLength);
        PrintHexBytes("WriteSample : ", info.LastWriteSample, info.LastWriteSampleLength);
    }

    if (layoutVersion >= 5 &&
        DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, LastOutputCaptureSample), sizeof(info.LastOutputCaptureSample)) &&
        (info.LastOutputCaptureLength != 0 || info.LastOutputCaptureSampleLength != 0)) {
        printf(
            "  OutputCapture: ioctl=0x%08lX len=%lu sampleLen=%lu ",
            info.LastOutputCaptureIoctl,
            info.LastOutputCaptureLength,
            info.LastOutputCaptureSampleLength);
        PrintOutputState(info.LastOutputCaptureState);
        PrintHexBytes("OutputSample : ", info.LastOutputCaptureSample, info.LastOutputCaptureSampleLength);
    }

    if (DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, LastRawReadSampleLength), sizeof(info.LastNativeOverrideBytesWritten)) &&
        (info.LastRawReadSampleLength != 0 || info.LastPatchedReadSampleLength != 0)) {
        printf("  NativeRead: rawLen=%lu rawInfo=%lu patchedLen=%lu patchedInfo=%lu applied=%lu bytes=%lu\n",
            info.LastRawReadSampleLength,
            info.LastRawReadCompletionLength,
            info.LastPatchedReadSampleLength,
            info.LastPatchedReadCompletionLength,
            info.LastNativeOverrideApplied,
            info.LastNativeOverrideBytesWritten);
        PrintHexBytes("RawSample    : ", info.LastRawReadSample, info.LastRawReadSampleLength);
        PrintHexBytes("PatchedSample: ", info.LastPatchedReadSample, info.LastPatchedReadSampleLength);
    }

    if (layoutVersion >= 2 &&
        DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, LastSemanticCaptureFlags), sizeof(info.LastSemanticCaptureReport))) {
        printf("  SemanticCapture: flags=");
        PrintCaptureFlags(info.LastSemanticCaptureFlags);
        printf(" len=%lu ", info.LastSemanticCaptureLength);
        PrintReportState(info.LastSemanticCaptureReport);
        if (layoutVersion >= 3 &&
            DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, LastSemanticCaptureIoctl), sizeof(info.LastSemanticCaptureSample))) {
            printf("  SemanticSource: ioctl=0x%08lX len=%lu\n",
                info.LastSemanticCaptureIoctl,
                info.LastSemanticCaptureSampleLength);
            PrintHexBytes("SemanticSample: ", info.LastSemanticCaptureSample, info.LastSemanticCaptureSampleLength);
        }
    }

    if (layoutVersion >= 1 &&
        DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, TraceCount), sizeof(info.TraceCount)) &&
        info.TraceCount != 0) {
        const ULONG traceCount = info.TraceCount > GAYM_TRACE_HISTORY_COUNT ? GAYM_TRACE_HISTORY_COUNT : info.TraceCount;
        for (ULONG traceIndex = 0; traceIndex < traceCount; ++traceIndex) {
            const GAYM_TRACE_ENTRY& entry = info.Trace[traceIndex];
            printf("  #%lu %-8s %-8s ioctl=0x%08lX in=%lu out=%lu xfer=%lu status=0x%08lX",
                entry.Sequence,
                TracePhaseName(entry.Phase),
                TraceRequestTypeName(entry.RequestType),
                entry.IoControlCode,
                entry.InputLength,
                entry.OutputLength,
                entry.TransferLength,
                entry.Status);
            PrintTraceSample(entry);
            printf("\n");
        }
    }

    printf("\n");
}

static void PrintStatusForTarget(const char* label, GaYmControlTarget target)
{
    auto devices = EnumerateGaYmDevicesForTarget(target);
    if (devices.empty()) {
        printf("%s: no endpoint found.\n\n", label);
        return;
    }

    for (const auto& devicePath : devices) {
        HANDLE device = OpenGaYmPathWithFallback(devicePath.path);
        if (device == INVALID_HANDLE_VALUE) {
            printf("%s [%d] %ls\n", label, devicePath.index, devicePath.path.c_str());
            printf("  open failed (error %lu)\n\n", GetLastError());
            continue;
        }

        PrintDeviceStatusBlock(label, devicePath, device);
        CloseHandle(device);
    }
}

static void CmdStatus()
{
    PrintStatusForTarget("Upper", GaYmControlTarget::Upper);
    PrintStatusForTarget("Lower", GaYmControlTarget::Lower);
}

static HANDLE OpenCommandDevice(int argc, char* argv[], int argIdx)
{
    int devIdx = 0;
    if (argIdx < argc) devIdx = atoi(argv[argIdx]);

    HANDLE h = OpenGaYmDeviceForTarget(GaYmControlTarget::Upper, devIdx);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ERROR: Cannot open upper control device %d (error %lu). Run as Administrator.\n",
            devIdx, GetLastError());
    }
    return h;
}

static void CmdOverride(int argc, char* argv[], bool enable)
{
    HANDLE h = OpenCommandDevice(argc, argv, 2);
    if (h == INVALID_HANDLE_VALUE) return;

    DWORD ioctl = enable ? IOCTL_GAYM_OVERRIDE_ON : IOCTL_GAYM_OVERRIDE_OFF;
    if (SendIoctl(h, ioctl)) {
        printf("Override %s.\n", enable ? "ENABLED" : "DISABLED");
    } else {
        fprintf(stderr, "Failed to %s override (error %lu).\n",
            enable ? "enable" : "disable", GetLastError());
    }
    CloseHandle(h);
}

#if GAYM_ENABLE_DEV_DIAGNOSTICS
static void CmdJitter(int argc, char* argv[])
{
    if (argc < 3) {
        PrintUsage();
        return;
    }

    HANDLE h = OpenGaYmDeviceForTarget(GaYmControlTarget::Upper, 0);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ERROR: Cannot open upper control device (error %lu).\n", GetLastError());
        return;
    }

    GAYM_JITTER_CONFIG jcfg = {};

    if (_stricmp(argv[2], "off") == 0) {
        jcfg.Enabled = FALSE;
        printf("Jitter disabled.\n");
    } else if (argc >= 4) {
        jcfg.Enabled   = TRUE;
        jcfg.MinDelayUs = (ULONG)atoi(argv[2]);
        jcfg.MaxDelayUs = (ULONG)atoi(argv[3]);
        printf("Jitter enabled: %lu - %lu us\n", jcfg.MinDelayUs, jcfg.MaxDelayUs);
    } else {
        PrintUsage();
        CloseHandle(h);
        return;
    }

    if (!SetJitter(h, &jcfg)) {
        const DWORD error = GetLastError();
        if (error == ERROR_NOT_SUPPORTED) {
            fprintf(stderr, "Jitter is not supported by this build.\n");
        } else {
            fprintf(stderr, "Failed to set jitter (error %lu).\n", error);
        }
    }
    CloseHandle(h);
}
#else
static void CmdJitter(int argc, char* argv[])
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);
    fprintf(stderr, "The jitter command is not available in the release bundle profile.\n");
}
#endif

static void CmdTest(int argc, char* argv[])
{
    HANDLE h = OpenCommandDevice(argc, argv, 2);
    if (h == INVALID_HANDLE_VALUE) return;

    /* Enable override first */
    SendIoctl(h, IOCTL_GAYM_OVERRIDE_ON);

    /* Send a test report: A button + left stick full right */
    GAYM_REPORT report;
    RtlZeroMemory(&report, sizeof(report));
    report.DPad       = GAYM_DPAD_NEUTRAL;
    report.Buttons[0] = GAYM_BTN_A;
    report.ThumbLeftX = 32767;

    if (InjectReport(h, &report)) {
        printf("Test report injected: A + LX=32767\n");
        printf("Holding for 2 seconds...\n");
        Sleep(2000);
    } else {
        fprintf(stderr, "InjectReport failed (error %lu).\n", GetLastError());
    }

    /* Release: send neutral report then disable override */
    RtlZeroMemory(&report, sizeof(report));
    report.DPad = GAYM_DPAD_NEUTRAL;
    InjectReport(h, &report);
    Sleep(50);

    SendIoctl(h, IOCTL_GAYM_OVERRIDE_OFF);
    printf("Override disabled. Test complete.\n");

    CloseHandle(h);
}

static bool ParseLongArgument(const char* text, long minValue, long maxValue, long* value)
{
    char* end = nullptr;
    long parsed = strtol(text, &end, 0);
    if (end == text || *end != '\0' || parsed < minValue || parsed > maxValue) {
        return false;
    }

    *value = parsed;
    return true;
}

static void PrintPollInterval(ULONG intervalMs)
{
    const double rateHz = intervalMs != 0
        ? 1000.0 / static_cast<double>(intervalMs)
        : 0.0;

    printf(
        "HID poll interval: %lu ms (%.2f Hz)\n",
        intervalMs,
        rateHz);
}

static void CmdPoll(int argc, char* argv[])
{
    long intervalMs = -1;
    long deviceIndex = 0;
    ULONG currentIntervalMs = 0;
    int deviceArgIndex = 2;

    if (argc >= 3 && _stricmp(argv[2], "status") != 0 && _stricmp(argv[2], "get") != 0) {
        if (!ParseLongArgument(argv[2], 1, 1000, &intervalMs)) {
            fprintf(stderr, "Invalid poll interval value: %s\n", argv[2]);
            return;
        }
        deviceArgIndex = 3;
    }

    if (argc > deviceArgIndex) {
        if (!ParseLongArgument(argv[deviceArgIndex], 0, 64, &deviceIndex)) {
            fprintf(stderr, "Invalid device index: %s\n", argv[deviceArgIndex]);
            return;
        }
    }

    if (intervalMs >= 0) {
        if (!SetHidPollIntervalMs(static_cast<int>(deviceIndex), static_cast<ULONG>(intervalMs))) {
            const DWORD error = GetLastError();
            if (error == ERROR_INVALID_FUNCTION) {
                fprintf(
                    stderr,
                    "The active HID stack does not expose poll-frequency control on this path.\n");
            } else {
                fprintf(stderr, "Failed to set HID poll interval (error %lu).\n", error);
            }
            return;
        }

        printf(
            "Set HID poll interval for supported HID device %ld to %ld ms.\n",
            deviceIndex,
            intervalMs);
    }

    if (!QueryHidPollIntervalMs(static_cast<int>(deviceIndex), &currentIntervalMs)) {
        const DWORD error = GetLastError();
        if (error == ERROR_INVALID_FUNCTION) {
            fprintf(
                stderr,
                "The active HID stack does not expose poll-frequency control on this path.\n");
        } else {
            fprintf(stderr, "Failed to query HID poll interval (error %lu).\n", error);
        }
        return;
    }

    printf("Supported HID device %ld\n", deviceIndex);
    PrintPollInterval(currentIntervalMs);
}

static bool IsExpectedTransientQueryError(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_NOT_READY:
    case ERROR_GEN_FAILURE:
    case ERROR_DEV_NOT_EXIST:
        return true;
    default:
        return false;
    }
}

static bool TryFindConnectedPad(DWORD* padIndex)
{
    for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
        XINPUT_STATE state = {};
        if (XInputGetState(index, &state) == ERROR_SUCCESS) {
            *padIndex = index;
            return true;
        }
    }

    return false;
}

static void CmdRumble(int argc, char* argv[])
{
    long leftMotor = 65535;
    long rightMotor = 65535;
    long durationMs = 750;
    long requestedPadIndex = -1;
    DWORD padIndex;
    DWORD result;
    XINPUT_STATE state = {};
    XINPUT_VIBRATION vibration = {};

    if (argc < 4) {
        PrintUsage();
        return;
    }

    if (!ParseLongArgument(argv[2], 0, 65535, &leftMotor)) {
        std::fprintf(stderr, "Invalid left motor value: %s\n", argv[2]);
        return;
    }

    if (!ParseLongArgument(argv[3], 0, 65535, &rightMotor)) {
        std::fprintf(stderr, "Invalid right motor value: %s\n", argv[3]);
        return;
    }

    if (argc >= 5 && !ParseLongArgument(argv[4], 1, 10000, &durationMs)) {
        std::fprintf(stderr, "Invalid duration value: %s\n", argv[4]);
        return;
    }

    if (argc >= 6 && !ParseLongArgument(argv[5], 0, XUSER_MAX_COUNT - 1, &requestedPadIndex)) {
        std::fprintf(stderr, "Invalid pad index: %s\n", argv[5]);
        return;
    }

    if (requestedPadIndex >= 0) {
        padIndex = (DWORD)requestedPadIndex;
        result = XInputGetState(padIndex, &state);
        if (result != ERROR_SUCCESS) {
            std::fprintf(stderr, "XInput pad %lu is not connected (error %lu).\n", padIndex, result);
            return;
        }
    } else if (!TryFindConnectedPad(&padIndex)) {
        std::fprintf(stderr, "No connected XInput pad found.\n");
        return;
    }

    vibration.wLeftMotorSpeed = (WORD)leftMotor;
    vibration.wRightMotorSpeed = (WORD)rightMotor;

    result = XInputSetState(padIndex, &vibration);
    if (result != ERROR_SUCCESS) {
        std::fprintf(stderr, "XInputSetState failed for pad %lu (error %lu).\n", padIndex, result);
        return;
    }

    std::printf(
        "Rumble on pad %lu: left=%ld right=%ld duration=%ld ms\n",
        padIndex,
        leftMotor,
        rightMotor,
        durationMs);
    Sleep((DWORD)durationMs);

    std::memset(&vibration, 0, sizeof(vibration));
    result = XInputSetState(padIndex, &vibration);
    if (result != ERROR_SUCCESS) {
        std::fprintf(stderr, "Failed to stop rumble on pad %lu (error %lu).\n", padIndex, result);
        return;
    }

    std::printf("Rumble stopped. Current driver status:\n\n");
    CmdStatus();
}

static void CmdVirtualRumble(int argc, char* argv[])
{
    long leftMotor = 65535;
    long rightMotor = 65535;
    long durationMs = 750;
    HANDLE device;
    GAYM_OUTPUT_STATE outputState = {};

    if (argc < 4) {
        PrintUsage();
        return;
    }

    if (!ParseLongArgument(argv[2], 0, 65535, &leftMotor)) {
        std::fprintf(stderr, "Invalid left motor value: %s\n", argv[2]);
        return;
    }
    if (!ParseLongArgument(argv[3], 0, 65535, &rightMotor)) {
        std::fprintf(stderr, "Invalid right motor value: %s\n", argv[3]);
        return;
    }
    if (argc >= 5 && !ParseLongArgument(argv[4], 0, 60000, &durationMs)) {
        std::fprintf(stderr, "Invalid duration value: %s\n", argv[4]);
        return;
    }

    device = OpenCommandDevice(argc, argv, 5);
    if (device == INVALID_HANDLE_VALUE) {
        return;
    }

    outputState.ActiveMask = GAYM_OUTPUT_UPDATE_RUMBLE;
    outputState.LowFrequencyMotor = ScaleMotorWordToByte(static_cast<WORD>(leftMotor));
    outputState.HighFrequencyMotor = ScaleMotorWordToByte(static_cast<WORD>(rightMotor));

    if (!ApplyOutputState(device, &outputState)) {
        std::fprintf(stderr, "ApplyOutputState failed (error %lu).\n", GetLastError());
        CloseHandle(device);
        return;
    }

    std::printf(
        "Virtual rumble applied: left=%ld right=%ld duration=%ld ms\n",
        leftMotor,
        rightMotor,
        durationMs);
    Sleep(static_cast<DWORD>(durationMs));

    outputState.LowFrequencyMotor = 0;
    outputState.HighFrequencyMotor = 0;
    if (!ApplyOutputState(device, &outputState)) {
        std::fprintf(stderr, "Failed to stop virtual rumble (error %lu).\n", GetLastError());
        CloseHandle(device);
        return;
    }

    CloseHandle(device);
    std::printf("Virtual rumble stopped. Current driver status:\n\n");
    CmdStatus();
}

static bool IsReasonableDeviceType(ULONG deviceType)
{
    return deviceType >= GAYM_DEVICE_UNKNOWN && deviceType <= GAYM_DEVICE_DUALSENSE_EDGE;
}

static bool ValidateDeviceInfoSnapshot(
    const GAYM_DEVICE_INFO& info,
    DWORD bytesReturned,
    char* reason,
    size_t reasonCount)
{
    const bool hasLayoutField = DeviceInfoHasField(
        bytesReturned,
        offsetof(GAYM_DEVICE_INFO, QueryLayoutVersion),
        sizeof(info.QueryLayoutVersion));

    if (bytesReturned < sizeof(GAYM_DEVICE_TYPE) + sizeof(USHORT) * 2) {
        std::snprintf(reason, reasonCount, "short response (%lu bytes)", bytesReturned);
        return false;
    }

    if (!IsReasonableDeviceType(info.DeviceType)) {
        std::snprintf(reason, reasonCount, "invalid device type %lu", info.DeviceType);
        return false;
    }

    if (info.VendorId == 0 || info.ProductId == 0) {
        std::snprintf(reason, reasonCount, "zero VID/PID (%04X/%04X)", info.VendorId, info.ProductId);
        return false;
    }

    if (hasLayoutField) {
        if (info.QueryLayoutVersion == 0 || info.QueryLayoutVersion > 5) {
            std::snprintf(reason, reasonCount, "invalid layout version %lu", info.QueryLayoutVersion);
            return false;
        }

        if (info.DriverBuildStamp != 0x20260327u) {
            std::snprintf(reason, reasonCount, "unexpected build stamp 0x%08lX", info.DriverBuildStamp);
            return false;
        }
    }

    if (DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, PendingInputRequests), sizeof(info.WriteRequestsSeen))) {
        if (info.PendingInputRequests > info.QueuedInputRequests) {
            std::snprintf(reason, reasonCount, "pending exceeds queued (%lu > %lu)",
                info.PendingInputRequests, info.QueuedInputRequests);
            return false;
        }

        if (info.ReadRequestsSeen > 1000000 ||
            info.DeviceControlRequestsSeen > 1000000 ||
            info.InternalDeviceControlRequestsSeen > 1000000 ||
            info.WriteRequestsSeen > 1000000 ||
            info.PendingInputRequests > 1000000 ||
            info.QueuedInputRequests > 1000000 ||
            info.CompletedInputRequests > 1000000 ||
            info.ForwardedInputRequests > 1000000) {
            std::snprintf(reason, reasonCount, "counter range looks corrupted");
            return false;
        }
    }

    if (DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, LastReadLength), sizeof(info.LastInternalOutputLength))) {
        if (info.LastReadLength > 65536 ||
            info.LastWriteLength > 65536 ||
            info.LastDeviceControlInputLength > 65536 ||
            info.LastDeviceControlOutputLength > 65536 ||
            info.LastInternalInputLength > 65536 ||
            info.LastInternalOutputLength > 65536) {
            std::snprintf(reason, reasonCount, "buffer length range looks corrupted");
            return false;
        }
    }

    if (DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, LastWriteSample), sizeof(info.LastWriteSample))) {
        if (info.LastWriteSampleLength > GAYM_TRACE_SAMPLE_BYTES) {
            std::snprintf(reason, reasonCount, "write sample length looks corrupted (%lu)", info.LastWriteSampleLength);
            return false;
        }

        if (info.LastWriteSampleLength > info.LastWriteLength) {
            std::snprintf(reason, reasonCount, "write sample exceeds write length (%lu > %lu)",
                info.LastWriteSampleLength, info.LastWriteLength);
            return false;
        }
    }

    if (DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, LastOutputCaptureSample), sizeof(info.LastOutputCaptureSample))) {
        if (info.LastOutputCaptureSampleLength > GAYM_TRACE_SAMPLE_BYTES) {
            std::snprintf(reason, reasonCount, "output capture sample length looks corrupted (%lu)", info.LastOutputCaptureSampleLength);
            return false;
        }

        if (info.LastOutputCaptureSampleLength > info.LastOutputCaptureLength) {
            std::snprintf(reason, reasonCount, "output capture sample exceeds capture length (%lu > %lu)",
                info.LastOutputCaptureSampleLength, info.LastOutputCaptureLength);
            return false;
        }
    }

    reason[0] = '\0';
    return true;
}

static void CmdWakeCheck(int argc, char* argv[])
{
    long durationSeconds = 45;
    long intervalMs = 500;
    long deviceIndex = 0;
    DWORD startedAt = GetTickCount();
    DWORD deadline;
    DWORD goodSnapshots = 0;
    DWORD transientFailures = 0;
    DWORD invalidSnapshots = 0;

    if (argc >= 3 && !ParseLongArgument(argv[2], 1, 3600, &durationSeconds)) {
        fprintf(stderr, "Invalid seconds value: %s\n", argv[2]);
        return;
    }
    if (argc >= 4 && !ParseLongArgument(argv[3], 50, 10000, &intervalMs)) {
        fprintf(stderr, "Invalid interval value: %s\n", argv[3]);
        return;
    }
    if (argc >= 5 && !ParseLongArgument(argv[4], 0, 64, &deviceIndex)) {
        fprintf(stderr, "Invalid device index: %s\n", argv[4]);
        return;
    }

    deadline = startedAt + static_cast<DWORD>(durationSeconds * 1000);

    printf("Wake check running for %ld second(s), poll interval %ld ms, device %ld.\n",
        durationSeconds, intervalMs, deviceIndex);
    printf("Accepts temporary not-ready/open failures. Fails on corrupted query snapshots.\n\n");

    while ((LONG)(GetTickCount() - deadline) < 0) {
        HANDLE device = OpenGaYmDeviceForTarget(GaYmControlTarget::Upper, static_cast<int>(deviceIndex));
        if (device == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (IsExpectedTransientQueryError(error)) {
                ++transientFailures;
                printf("[transient-open] error=%lu\n", error);
                Sleep(static_cast<DWORD>(intervalMs));
                continue;
            }

            fprintf(stderr, "[fail-open] error=%lu\n", error);
            return;
        }

        GAYM_DEVICE_INFO info = {};
        DWORD bytesReturned = 0;
        if (!QueryDeviceInfo(device, &info, &bytesReturned)) {
            const DWORD error = GetLastError();
            CloseHandle(device);

            if (IsExpectedTransientQueryError(error)) {
                ++transientFailures;
                printf("[transient-query] error=%lu\n", error);
                Sleep(static_cast<DWORD>(intervalMs));
                continue;
            }

            fprintf(stderr, "[fail-query] error=%lu\n", error);
            return;
        }

        CloseHandle(device);

        char reason[128] = {};
        if (!ValidateDeviceInfoSnapshot(info, bytesReturned, reason, sizeof(reason))) {
            ++invalidSnapshots;
            fprintf(stderr,
                "[fail-data] %s | bytes=%lu vid=%04X pid=%04X type=%lu layout=%lu build=0x%08lX "
                "read=%lu devctl=%lu pending=%lu queued=%lu lastIoctl=0x%08lX lens=%lu/%lu/%lu/%lu/%lu/%lu\n",
                reason,
                bytesReturned,
                info.VendorId,
                info.ProductId,
                info.DeviceType,
                DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, QueryLayoutVersion), sizeof(info.QueryLayoutVersion)) ? info.QueryLayoutVersion : 0,
                DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, DriverBuildStamp), sizeof(info.DriverBuildStamp)) ? info.DriverBuildStamp : 0,
                info.ReadRequestsSeen,
                info.DeviceControlRequestsSeen,
                info.PendingInputRequests,
                info.QueuedInputRequests,
                info.LastInterceptedIoctl,
                info.LastReadLength,
                info.LastWriteLength,
                info.LastDeviceControlInputLength,
                info.LastDeviceControlOutputLength,
                info.LastInternalInputLength,
                info.LastInternalOutputLength);
            return;
        }

        ++goodSnapshots;
        printf("[ok] bytes=%lu vid=%04X pid=%04X override=%s read=%lu devctl=%lu pending=%lu queued=%lu layout=%lu\n",
            bytesReturned,
            info.VendorId,
            info.ProductId,
            info.OverrideActive ? "on" : "off",
            info.ReadRequestsSeen,
            info.DeviceControlRequestsSeen,
            info.PendingInputRequests,
            info.QueuedInputRequests,
            DeviceInfoHasField(bytesReturned, offsetof(GAYM_DEVICE_INFO, QueryLayoutVersion), sizeof(info.QueryLayoutVersion)) ? info.QueryLayoutVersion : 0);

        Sleep(static_cast<DWORD>(intervalMs));
    }

    if (goodSnapshots == 0) {
        fprintf(stderr, "Wake check failed: no valid snapshots captured.\n");
        return;
    }

    printf("\nWake check PASS: valid snapshots=%lu transient failures=%lu invalid snapshots=%lu\n",
        goodSnapshots,
        transientFailures,
        invalidSnapshots);
}

static void CmdReport(int argc, char* argv[])
{
    if (argc < 12) {
        PrintUsage();
        return;
    }

    HANDLE h = OpenCommandDevice(argc, argv, 2);
    if (h == INVALID_HANDLE_VALUE) return;

    long values[9] = {};
    const long mins[9] = { 0, 0, 0, 0, 0, -32767, -32767, -32767, -32767 };
    const long maxs[9] = { 255, 255, 15, 255, 255, 32767, 32767, 32767, 32767 };

    for (int index = 0; index < 9; ++index) {
        if (!ParseLongArgument(argv[3 + index], mins[index], maxs[index], &values[index])) {
            std::fprintf(stderr, "Invalid report field at argument %d: %s\n", 3 + index, argv[3 + index]);
            CloseHandle(h);
            return;
        }
    }

    GAYM_REPORT report = {};
    std::memset(&report, 0, sizeof(report));
    report.Buttons[0] = static_cast<UCHAR>(values[0]);
    report.Buttons[1] = static_cast<UCHAR>(values[1]);
    report.DPad = static_cast<UCHAR>(values[2]);
    report.TriggerLeft = static_cast<UCHAR>(values[3]);
    report.TriggerRight = static_cast<UCHAR>(values[4]);
    report.ThumbLeftX = static_cast<SHORT>(values[5]);
    report.ThumbLeftY = static_cast<SHORT>(values[6]);
    report.ThumbRightX = static_cast<SHORT>(values[7]);
    report.ThumbRightY = static_cast<SHORT>(values[8]);

    if (InjectReport(h, &report)) {
        printf("Report injected.\n");
    } else {
        fprintf(stderr, "InjectReport failed (error %lu).\n", GetLastError());
    }

    CloseHandle(h);
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        PrintUsage();
        return 0;
    }

    const char* cmd = argv[1];

    if      (_stricmp(cmd, "status") == 0) CmdStatus();
    else if (_stricmp(cmd, "poll")   == 0) CmdPoll(argc, argv);
    else if (_stricmp(cmd, "wakecheck") == 0) CmdWakeCheck(argc, argv);
    else if (_stricmp(cmd, "rumble") == 0) CmdRumble(argc, argv);
    else if (_stricmp(cmd, "vrumble") == 0) CmdVirtualRumble(argc, argv);
    else if (_stricmp(cmd, "on")     == 0) CmdOverride(argc, argv, true);
    else if (_stricmp(cmd, "off")    == 0) CmdOverride(argc, argv, false);
    else if (_stricmp(cmd, "jitter") == 0) CmdJitter(argc, argv);
    else if (_stricmp(cmd, "test")   == 0) CmdTest(argc, argv);
    else if (_stricmp(cmd, "report") == 0) CmdReport(argc, argv);
    else {
        fprintf(stderr, "Unknown command: %s\n\n", cmd);
        PrintUsage();
        return 1;
    }

    return 0;
}
