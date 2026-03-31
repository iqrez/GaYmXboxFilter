#include "../include/upper_trace.h"
#include "../include/upper_device.h"

static VOID UpperSetSemanticButtonsFromLegacyReport(
    _In_ const GAYM_REPORT* Report,
    _Out_ PULONG Buttons)
{
    *Buttons = 0;

    if (Report->Buttons[0] & GAYM_BTN_A) {
        *Buttons |= GAYM_SEMANTIC_BUTTON_A;
    }
    if (Report->Buttons[0] & GAYM_BTN_B) {
        *Buttons |= GAYM_SEMANTIC_BUTTON_B;
    }
    if (Report->Buttons[0] & GAYM_BTN_X) {
        *Buttons |= GAYM_SEMANTIC_BUTTON_X;
    }
    if (Report->Buttons[0] & GAYM_BTN_Y) {
        *Buttons |= GAYM_SEMANTIC_BUTTON_Y;
    }
    if (Report->Buttons[0] & GAYM_BTN_LB) {
        *Buttons |= GAYM_SEMANTIC_BUTTON_LB;
    }
    if (Report->Buttons[0] & GAYM_BTN_RB) {
        *Buttons |= GAYM_SEMANTIC_BUTTON_RB;
    }
    if (Report->Buttons[0] & GAYM_BTN_BACK) {
        *Buttons |= GAYM_SEMANTIC_BUTTON_BACK;
    }
    if (Report->Buttons[0] & GAYM_BTN_START) {
        *Buttons |= GAYM_SEMANTIC_BUTTON_START;
    }
    if (Report->Buttons[1] & GAYM_BTN_LSTICK) {
        *Buttons |= GAYM_SEMANTIC_BUTTON_LSTICK;
    }
    if (Report->Buttons[1] & GAYM_BTN_RSTICK) {
        *Buttons |= GAYM_SEMANTIC_BUTTON_RSTICK;
    }
    if (Report->Buttons[1] & GAYM_BTN_GUIDE) {
        *Buttons |= GAYM_SEMANTIC_BUTTON_GUIDE;
    }
    if (Report->Buttons[1] & GAYM_BTN_MISC) {
        *Buttons |= GAYM_SEMANTIC_BUTTON_MISC;
    }

    switch (Report->DPad) {
    case GAYM_DPAD_UP:
        *Buttons |= GAYM_SEMANTIC_BUTTON_DPAD_UP;
        break;
    case GAYM_DPAD_RIGHT:
        *Buttons |= GAYM_SEMANTIC_BUTTON_DPAD_RIGHT;
        break;
    case GAYM_DPAD_DOWN:
        *Buttons |= GAYM_SEMANTIC_BUTTON_DPAD_DOWN;
        break;
    case GAYM_DPAD_LEFT:
        *Buttons |= GAYM_SEMANTIC_BUTTON_DPAD_LEFT;
        break;
    case GAYM_DPAD_UPRIGHT:
        *Buttons |= GAYM_SEMANTIC_BUTTON_DPAD_UP | GAYM_SEMANTIC_BUTTON_DPAD_RIGHT;
        break;
    case GAYM_DPAD_DOWNRIGHT:
        *Buttons |= GAYM_SEMANTIC_BUTTON_DPAD_DOWN | GAYM_SEMANTIC_BUTTON_DPAD_RIGHT;
        break;
    case GAYM_DPAD_DOWNLEFT:
        *Buttons |= GAYM_SEMANTIC_BUTTON_DPAD_DOWN | GAYM_SEMANTIC_BUTTON_DPAD_LEFT;
        break;
    case GAYM_DPAD_UPLEFT:
        *Buttons |= GAYM_SEMANTIC_BUTTON_DPAD_UP | GAYM_SEMANTIC_BUTTON_DPAD_LEFT;
        break;
    default:
        break;
    }
}

VOID UpperTraceReset(VOID)
{
}

VOID UpperTraceRecord(_In_ ULONG EventCode, _In_ NTSTATUS Status)
{
    UNREFERENCED_PARAMETER(EventCode);
    UNREFERENCED_PARAMETER(Status);
}

VOID UpperDeviceUpdateObservation(_In_ PUPPER_DEVICE_CONTEXT Context)
{
    GAYM_REPORT reportSnapshot;
    BOOLEAN writerHeld;
    BOOLEAN overrideEnabled;
    BOOLEAN hasInjectedReport;
    BOOLEAN hasObservedReport;
    BOOLEAN isAttached;
    BOOLEAN isInD0;
    ULONG reportsInjected;
    ULONG reportsObserved;
    LONG readRequestsSeen;
    KIRQL oldIrql;

    if (Context == NULL) {
        return;
    }

    RtlZeroMemory(&reportSnapshot, sizeof(reportSnapshot));

    KeAcquireSpinLock(&Context->StateLock, &oldIrql);
    writerHeld = Context->WriterSessionHeld;
    overrideEnabled = Context->OverrideEnabled;
    hasInjectedReport = Context->HasInjectedReport;
    hasObservedReport = Context->HasObservedReport;
    isAttached = Context->IsAttached;
    isInD0 = Context->IsInD0;
    reportsInjected = Context->ReportsInjected;
    reportsObserved = Context->ReportsObserved;
    readRequestsSeen = Context->ReadRequestsSeen;

    if (overrideEnabled && hasInjectedReport) {
        reportSnapshot = Context->LastInjectedReport;
    } else if (hasObservedReport) {
        reportSnapshot = Context->LastObservedReport;
    }
    KeReleaseSpinLock(&Context->StateLock, oldIrql);

    RtlZeroMemory(&Context->LastObservation, sizeof(Context->LastObservation));
    Context->LastObservation.Header.Magic = GAYM_PROTOCOL_MAGIC;
    Context->LastObservation.Header.AbiMajor = (USHORT)GAYM_ABI_MAJOR;
    Context->LastObservation.Header.AbiMinor = (USHORT)GAYM_ABI_MINOR;
    Context->LastObservation.Header.Size = (ULONG)sizeof(GAYM_OBSERVATION_V1);
    Context->LastObservation.Header.Flags = GAYM_PROTOCOL_FLAG_NONE;
    Context->LastObservation.Header.RequestId = 0;
    Context->LastObservation.AdapterFamily = isAttached ? GAYM_ADAPTER_FAMILY_XBOX_02FF : GAYM_ADAPTER_FAMILY_UNKNOWN;
    Context->LastObservation.CapabilityFlags =
        GAYM_CAPABILITY_SEMANTIC_CONTROL |
        GAYM_CAPABILITY_SEMANTIC_OBSERVATION |
        GAYM_CAPABILITY_SINGLE_WRITER_REQUIRED;
    Context->LastObservation.StatusFlags = isAttached ? GAYM_STATUS_DEVICE_PRESENT : 0;
    if (writerHeld) {
        Context->LastObservation.StatusFlags |= GAYM_STATUS_WRITER_HELD;
    }
    if (overrideEnabled) {
        Context->LastObservation.StatusFlags |= GAYM_STATUS_OVERRIDE_ACTIVE;
    }
    if (!(hasObservedReport || (overrideEnabled && hasInjectedReport))) {
        Context->LastObservation.StatusFlags |= GAYM_STATUS_OBSERVATION_SYNTHETIC;
    }
    Context->LastObservation.LastObservedSequence = (ULONGLONG)readRequestsSeen;
    Context->LastObservation.LastInjectedSequence = reportsInjected;
    Context->LastObservation.TimestampQpc = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
    if (hasObservedReport || (overrideEnabled && hasInjectedReport)) {
        UpperSetSemanticButtonsFromLegacyReport(&reportSnapshot, &Context->LastObservation.Buttons);
        Context->LastObservation.LeftStickX = reportSnapshot.ThumbLeftX;
        Context->LastObservation.LeftStickY = reportSnapshot.ThumbLeftY;
        Context->LastObservation.RightStickX = reportSnapshot.ThumbRightX;
        Context->LastObservation.RightStickY = reportSnapshot.ThumbRightY;
        Context->LastObservation.LeftTrigger = (USHORT)reportSnapshot.TriggerLeft;
        Context->LastObservation.RightTrigger = (USHORT)reportSnapshot.TriggerRight;
    }
    Context->LastObservation.BatteryPercent = 0xFF;
    Context->LastObservation.PowerFlags = isInD0 ? 1 : 0;
    Context->LastObservation.Reserved = (USHORT)reportsObserved;
}
