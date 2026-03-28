/*
 * GaYmFilter - Device table and native HID report translators.
 *
 * Each supported controller has a translation function that converts
 * the generic GAYM_REPORT into the device's native HID input report format.
 *
 * NOTE: Native report byte layouts are based on documented HID report
 * descriptors. If a specific hardware revision differs, update the
 * translation function and NativeReportSize for that VID/PID entry.
 */

#include "devices.h"
#include "logging.h"

#define GAYM_INPUT_CAP_STANDARD_GAMEPAD \
    (GAYM_INPUT_CAP_FACE_BUTTONS | \
     GAYM_INPUT_CAP_SHOULDERS | \
     GAYM_INPUT_CAP_MENU_BUTTONS | \
     GAYM_INPUT_CAP_GUIDE_BUTTON | \
     GAYM_INPUT_CAP_STICK_CLICKS | \
     GAYM_INPUT_CAP_DPAD_8WAY | \
     GAYM_INPUT_CAP_ANALOG_TRIGGERS | \
     GAYM_INPUT_CAP_LEFT_STICK | \
     GAYM_INPUT_CAP_RIGHT_STICK)

#define GAYM_INPUT_CAP_XBOX_BT_GAMEPAD \
    (GAYM_INPUT_CAP_STANDARD_GAMEPAD)

#define GAYM_INPUT_CAP_XBOX_USB_GAMEPAD \
    (GAYM_INPUT_CAP_STANDARD_GAMEPAD | \
     GAYM_INPUT_CAP_MISC_BUTTON)

#define GAYM_INPUT_CAP_DUALSENSE_GAMEPAD \
    (GAYM_INPUT_CAP_STANDARD_GAMEPAD | \
     GAYM_INPUT_CAP_MISC_BUTTON)

#define GAYM_OUTPUT_CAP_XBOX_GAMEPAD \
    (GAYM_OUTPUT_CAP_DUAL_MOTOR_RUMBLE | \
     GAYM_OUTPUT_CAP_PLAYER_LIGHT)

#define GAYM_OUTPUT_CAP_DUALSENSE_GAMEPAD \
    (GAYM_OUTPUT_CAP_DUAL_MOTOR_RUMBLE | \
     GAYM_OUTPUT_CAP_PLAYER_LIGHT | \
     GAYM_OUTPUT_CAP_RGB_LIGHT | \
     GAYM_OUTPUT_CAP_MUTE_LIGHT | \
     GAYM_OUTPUT_CAP_TRIGGER_EFFECTS)

#define GAYM_XBOX_XINPUT_OUTPUT_PACKET_LENGTH 5u
#define GAYM_XBOX_XINPUT_OUTPUT_SELECTOR      0x02u

/* ═══════════════════════════════════════════════════════════════════
 * Helper: parse hex digits from a wide string
 * ═══════════════════════════════════════════════════════════════════ */
static ULONG ParseHexW(_In_ PCWSTR Str, _In_ ULONG MaxChars)
{
    ULONG result = 0;
    for (ULONG i = 0; i < MaxChars && Str[i]; i++) {
        WCHAR c = Str[i];
        ULONG digit;
        if      (c >= L'0' && c <= L'9') digit = c - L'0';
        else if (c >= L'A' && c <= L'F') digit = 10 + (c - L'A');
        else if (c >= L'a' && c <= L'f') digit = 10 + (c - L'a');
        else break;
        result = (result << 4) | digit;
    }
    return result;
}

VOID GaYmParseHardwareId(
    _In_  PCWSTR  HardwareId,
    _Out_ PUSHORT VendorId,
    _Out_ PUSHORT ProductId)
{
    *VendorId  = 0;
    *ProductId = 0;

    if (!HardwareId) return;

    /* Scan for VID_ and PID_ substrings */
    for (PCWSTR p = HardwareId; *p; p++) {
        if (p[0] == L'V' && p[1] == L'I' && p[2] == L'D' && p[3] == L'_') {
            *VendorId = (USHORT)ParseHexW(p + 4, 4);
        }
        if (p[0] == L'P' && p[1] == L'I' && p[2] == L'D' && p[3] == L'_') {
            *ProductId = (USHORT)ParseHexW(p + 4, 4);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Axis conversion helpers
 * ═══════════════════════════════════════════════════════════════════ */

/* int16 (-32767..32767) → uint8 (0..255), center = 128 */
static __inline UCHAR AxisI16toU8(SHORT v)
{
    return (UCHAR)(((LONG)v + 32767L) * 255L / 65534L);
}

/* int16 (-32767..32767) → uint16 (0..65535), center = 32768 */
static __inline USHORT AxisI16toU16(SHORT v)
{
    return (USHORT)((LONG)v + 32768L);
}

static __inline SHORT AxisU16toI16(USHORT v)
{
    LONG value = (LONG)v - 32768L;
    if (value < -32767L) value = -32767L;
    if (value > 32767L) value = 32767L;
    return (SHORT)value;
}

static __inline SHORT AxisU8toI16(UCHAR v)
{
    LONG value = ((LONG)v * 65534L + 127L) / 255L - 32767L;
    if (value < -32767L) value = -32767L;
    if (value > 32767L) value = 32767L;
    return (SHORT)value;
}

static __inline USHORT ReadLe16(_In_reads_(2) const UCHAR* bytes)
{
    return (USHORT)(bytes[0] | (bytes[1] << 8));
}

static __inline UCHAR XboxTriggerWordToU8(USHORT rawValue)
{
    ULONG clamped = rawValue;

    if (clamped > 0x03FFu) {
        clamped = 0x03FFu;
    }

    return (UCHAR)((clamped * 255u + 511u) / 1023u);
}

static __inline USHORT XboxTriggerU8ToWord(UCHAR value)
{
    return (USHORT)(((ULONG)value * 1023u + 127u) / 255u);
}

static __inline USHORT XboxCombinedTriggerU8ToU16(UCHAR leftTrigger, UCHAR rightTrigger)
{
    LONG centered = 32768L + ((LONG)rightTrigger - (LONG)leftTrigger) * 128L;

    if (centered < 0L) {
        centered = 0L;
    } else if (centered > 65535L) {
        centered = 65535L;
    }

    return (USHORT)centered;
}

static VOID XboxCombinedTriggerU16ToPair(
    _In_ USHORT combinedValue,
    _Out_ PUCHAR leftTrigger,
    _Out_ PUCHAR rightTrigger)
{
    LONG delta = (LONG)combinedValue - 32768L;

    *leftTrigger = 0;
    *rightTrigger = 0;

    if (delta < 0) {
        LONG magnitude = -delta;
        if (magnitude > 32768L) {
            magnitude = 32768L;
        }

        *leftTrigger = (UCHAR)((magnitude * 255L + 16384L) / 32768L);
        return;
    }

    if (delta > 0) {
        if (delta > 32767L) {
            delta = 32767L;
        }

        *rightTrigger = (UCHAR)((delta * 255L + 16383L) / 32767L);
    }
}

static __inline UCHAR XboxMotorWordToU8(USHORT rawValue)
{
    return (UCHAR)(((ULONG)rawValue * 255u + 32767u) / 65535u);
}

static __inline USHORT XboxMotorU8ToWord(UCHAR value)
{
    return (USHORT)(((ULONG)value * 65535u + 127u) / 255u);
}

static VOID ResetParsedReport(_Out_ PGAYM_REPORT report)
{
    RtlZeroMemory(report, sizeof(*report));
    report->DPad = GAYM_DPAD_NEUTRAL;
}

/* ═══════════════════════════════════════════════════════════════════
 * Xbox - USB XInput HID report translator (PID 0x02FF)
 *
 * The live upper HID read path exposes a 16-byte compatibility packet:
 *   [0]     Report ID        0x00
 *   [1-8]   LX, LY, RX, RY   uint16 LE, center 32768
 *   [9-10]  Combined triggers uint16 LE, center 32768
 *   [11]    Buttons 1-8 bitmap (A/B/X/Y/LB/RB/Back/Start)
 *   [12]    Buttons 9-12 low nibble (L3/R3/Guide/Misc)
 *   [13]    Hat switch       0 = neutral, 1-8 directions
 *   [14-15] Reserved         0
 *
 * The lower diagnostic path still sees an 18-byte transport-flavored packet.
 * Accept that shape on parse, but emit the 16-byte compatibility packet here
 * so the upper native rewrite can patch WinMM/DirectInput-visible reads.
 * ═══════════════════════════════════════════════════════════════════ */
static NTSTATUS TranslateXboxUsbHidReport(
    _In_  const GAYM_REPORT* G,
    _Out_ PUCHAR             N,
    _In_  ULONG              Sz,
    _Out_ PULONG             Written,
    _Inout_ PUCHAR           Seq)
{
    UNREFERENCED_PARAMETER(Seq);

    const ULONG REPORT_SIZE = 16;
    USHORT lx;
    USHORT ly;
    USHORT rx;
    USHORT ry;
    USHORT combinedTriggers;

    if (Sz < REPORT_SIZE) return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(N, REPORT_SIZE);
    N[0] = 0x00;

    lx = AxisI16toU16(G->ThumbLeftX);
    ly = AxisI16toU16(G->ThumbLeftY);
    rx = AxisI16toU16(G->ThumbRightX);
    ry = AxisI16toU16(G->ThumbRightY);
    combinedTriggers = XboxCombinedTriggerU8ToU16(G->TriggerLeft, G->TriggerRight);

    N[1]  = (UCHAR)(lx & 0xFF);  N[2]  = (UCHAR)(lx >> 8);
    N[3]  = (UCHAR)(ly & 0xFF);  N[4]  = (UCHAR)(ly >> 8);
    N[5]  = (UCHAR)(rx & 0xFF);  N[6]  = (UCHAR)(rx >> 8);
    N[7]  = (UCHAR)(ry & 0xFF);  N[8]  = (UCHAR)(ry >> 8);

    N[9]  = (UCHAR)(combinedTriggers & 0xFF);
    N[10] = (UCHAR)(combinedTriggers >> 8);
    N[11] = G->Buttons[0];
    N[12] = (UCHAR)(G->Buttons[1] & 0x0F);

    if (G->DPad == GAYM_DPAD_NEUTRAL)
        N[13] = 0;
    else
        N[13] = G->DPad + 1;

    *Written = REPORT_SIZE;
    return STATUS_SUCCESS;
}

static NTSTATUS ParseXboxUsbHidReport(
    _In_reads_bytes_(NativeBufferSize) const UCHAR* NativeReport,
    _In_ ULONG NativeBufferSize,
    _Out_ PGAYM_REPORT GenericReport,
    _Out_ PULONG CaptureFlags)
{
    USHORT buttons;
    UCHAR hat;

    if (NativeReport == NULL || GenericReport == NULL || CaptureFlags == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (NativeBufferSize < 16) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    ResetParsedReport(GenericReport);
    if (NativeBufferSize >= 18) {
        if (NativeReport[17] == 0x83) {
            *CaptureFlags = GAYM_CAPTURE_FLAG_VALID |
                GAYM_CAPTURE_FLAG_PARTIAL |
                GAYM_CAPTURE_FLAG_SOURCE_NATIVE_READ;

            GenericReport->ReportId = NativeReport[0];
            GenericReport->ThumbLeftX = AxisU16toI16(ReadLe16(&NativeReport[1]));
            GenericReport->ThumbLeftY = AxisU16toI16(ReadLe16(&NativeReport[3]));
            GenericReport->ThumbRightX = AxisU16toI16(ReadLe16(&NativeReport[5]));
            GenericReport->ThumbRightY = AxisU16toI16(ReadLe16(&NativeReport[7]));
            GenericReport->TriggerLeft = XboxTriggerWordToU8(ReadLe16(&NativeReport[9]));
            GenericReport->TriggerRight = 0;

            buttons = ReadLe16(&NativeReport[13]);
            if (buttons & (1u << 0)) GenericReport->Buttons[0] |= GAYM_BTN_A;
            if (buttons & (1u << 1)) GenericReport->Buttons[0] |= GAYM_BTN_B;
            if (buttons & (1u << 2)) GenericReport->Buttons[0] |= GAYM_BTN_X;
            if (buttons & (1u << 3)) GenericReport->Buttons[0] |= GAYM_BTN_Y;
            if (buttons & (1u << 4)) GenericReport->Buttons[0] |= GAYM_BTN_LB;
            if (buttons & (1u << 5)) GenericReport->Buttons[0] |= GAYM_BTN_RB;
            if (buttons & (1u << 6)) GenericReport->Buttons[0] |= GAYM_BTN_BACK;
            if (buttons & (1u << 7)) GenericReport->Buttons[0] |= GAYM_BTN_START;
            if (buttons & (1u << 8)) GenericReport->Buttons[1] |= GAYM_BTN_LSTICK;
            if (buttons & (1u << 9)) GenericReport->Buttons[1] |= GAYM_BTN_RSTICK;
            if (buttons & (1u << 10)) GenericReport->Buttons[1] |= GAYM_BTN_GUIDE;
            if (buttons & (1u << 11)) GenericReport->Buttons[1] |= GAYM_BTN_MISC;

            hat = NativeReport[15] & 0x0F;
            if (hat != 0 && hat <= 8) {
                GenericReport->DPad = hat - 1;
            }

            return STATUS_SUCCESS;
        }
    }

    *CaptureFlags = GAYM_CAPTURE_FLAG_VALID |
        GAYM_CAPTURE_FLAG_PARTIAL |
        GAYM_CAPTURE_FLAG_TRIGGERS_COMBINED |
        GAYM_CAPTURE_FLAG_SOURCE_NATIVE_READ;

    GenericReport->ReportId = NativeReport[0];
    GenericReport->ThumbLeftX = AxisU16toI16(ReadLe16(&NativeReport[1]));
    GenericReport->ThumbLeftY = AxisU16toI16(ReadLe16(&NativeReport[3]));
    GenericReport->ThumbRightX = AxisU16toI16(ReadLe16(&NativeReport[5]));
    GenericReport->ThumbRightY = AxisU16toI16(ReadLe16(&NativeReport[7]));
    XboxCombinedTriggerU16ToPair(
        ReadLe16(&NativeReport[9]),
        &GenericReport->TriggerLeft,
        &GenericReport->TriggerRight);

    if (NativeReport[11] & GAYM_BTN_A)     GenericReport->Buttons[0] |= GAYM_BTN_A;
    if (NativeReport[11] & GAYM_BTN_B)     GenericReport->Buttons[0] |= GAYM_BTN_B;
    if (NativeReport[11] & GAYM_BTN_X)     GenericReport->Buttons[0] |= GAYM_BTN_X;
    if (NativeReport[11] & GAYM_BTN_Y)     GenericReport->Buttons[0] |= GAYM_BTN_Y;
    if (NativeReport[11] & GAYM_BTN_LB)    GenericReport->Buttons[0] |= GAYM_BTN_LB;
    if (NativeReport[11] & GAYM_BTN_RB)    GenericReport->Buttons[0] |= GAYM_BTN_RB;
    if (NativeReport[11] & GAYM_BTN_BACK)  GenericReport->Buttons[0] |= GAYM_BTN_BACK;
    if (NativeReport[11] & GAYM_BTN_START) GenericReport->Buttons[0] |= GAYM_BTN_START;

    if (NativeReport[12] & GAYM_BTN_LSTICK) GenericReport->Buttons[1] |= GAYM_BTN_LSTICK;
    if (NativeReport[12] & GAYM_BTN_RSTICK) GenericReport->Buttons[1] |= GAYM_BTN_RSTICK;
    if (NativeReport[12] & GAYM_BTN_GUIDE)  GenericReport->Buttons[1] |= GAYM_BTN_GUIDE;
    if (NativeReport[12] & GAYM_BTN_MISC)   GenericReport->Buttons[1] |= GAYM_BTN_MISC;

    hat = NativeReport[13] & 0x0F;
    if (hat != 0 && hat <= 8) {
        GenericReport->DPad = hat - 1;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS TranslateXboxXInputOutputState(
    _In_ const GAYM_OUTPUT_STATE* OutputState,
    _Out_writes_bytes_(NativeBufferSize) PUCHAR NativeReport,
    _In_ ULONG NativeBufferSize,
    _Out_ PULONG BytesWritten)
{
    USHORT lowMotor;
    USHORT highMotor;
    UCHAR selector;
    ULONG unsupportedMask;

    if (OutputState == NULL || NativeReport == NULL || BytesWritten == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (NativeBufferSize < GAYM_XBOX_XINPUT_OUTPUT_PACKET_LENGTH) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    unsupportedMask = OutputState->ActiveMask &
        ~(GAYM_OUTPUT_UPDATE_RUMBLE | GAYM_OUTPUT_UPDATE_VENDOR_DATA);
    if (unsupportedMask != 0) {
        return STATUS_NOT_SUPPORTED;
    }

    lowMotor = 0;
    highMotor = 0;
    if ((OutputState->ActiveMask & GAYM_OUTPUT_UPDATE_RUMBLE) != 0) {
        lowMotor = XboxMotorU8ToWord(OutputState->LowFrequencyMotor);
        highMotor = XboxMotorU8ToWord(OutputState->HighFrequencyMotor);
    }

    selector = GAYM_XBOX_XINPUT_OUTPUT_SELECTOR;
    if ((OutputState->ActiveMask & GAYM_OUTPUT_UPDATE_VENDOR_DATA) != 0 &&
        OutputState->VendorDefined[0] != 0) {
        selector = OutputState->VendorDefined[0];
    }

    RtlZeroMemory(NativeReport, GAYM_XBOX_XINPUT_OUTPUT_PACKET_LENGTH);
    NativeReport[0] = (UCHAR)(lowMotor & 0xFF);
    NativeReport[1] = (UCHAR)(lowMotor >> 8);
    NativeReport[2] = (UCHAR)(highMotor & 0xFF);
    NativeReport[3] = (UCHAR)(highMotor >> 8);
    NativeReport[4] = selector;

    *BytesWritten = GAYM_XBOX_XINPUT_OUTPUT_PACKET_LENGTH;
    return STATUS_SUCCESS;
}

static NTSTATUS ParseXboxXInputOutputReport(
    _In_reads_bytes_(NativeBufferSize) const UCHAR* NativeReport,
    _In_ ULONG NativeBufferSize,
    _Out_ PGAYM_OUTPUT_STATE OutputState)
{
    if (NativeReport == NULL || OutputState == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (NativeBufferSize < GAYM_XBOX_XINPUT_OUTPUT_PACKET_LENGTH) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputState, sizeof(*OutputState));
    OutputState->ActiveMask = GAYM_OUTPUT_UPDATE_RUMBLE;
    OutputState->LowFrequencyMotor = XboxMotorWordToU8(ReadLe16(NativeReport));
    OutputState->HighFrequencyMotor = XboxMotorWordToU8(ReadLe16(NativeReport + 2));
    OutputState->VendorDefined[0] = NativeReport[4];

    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════
 * Xbox - HID Bluetooth report translator (legacy, for BT-connected)
 *
 * Format (16 bytes):
 *   [0]     Report ID  (0x01)
 *   [1-2]   LX         uint16 LE, 0-65535, center 32768
 *   [3-4]   LY         uint16 LE
 *   [5-6]   RX         uint16 LE
 *   [7-8]   RY         uint16 LE
 *   [9]     LT         0-255
 *   [10]    RT         0-255
 *   [11]    D-pad      hat switch (1-8, 0=neutral)
 *   [12-13] Buttons    16 bits LE
 *   [14-15] Reserved   0
 *
 * Button bits (Xbox HID BT):
 *   Bit 0:  A          Bit 4:  LB
 *   Bit 1:  B          Bit 5:  RB
 *   Bit 2:  (rsvd)     Bit 6:  Back/View
 *   Bit 3:  X          Bit 7:  Start/Menu
 *   Bit 8:  Y          Bit 9:  L3
 *   Bit 10: R3         Bit 11: (rsvd)
 *   Bit 12: Guide
 * ═══════════════════════════════════════════════════════════════════ */
static NTSTATUS TranslateXboxBtReport(
    _In_  const GAYM_REPORT* G,
    _Out_ PUCHAR             N,
    _In_  ULONG              Sz,
    _Out_ PULONG             Written,
    _Inout_ PUCHAR           Seq)
{
    UNREFERENCED_PARAMETER(Seq);

    const ULONG XBOX_BT_REPORT_SIZE = 16;
    if (Sz < XBOX_BT_REPORT_SIZE) return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(N, XBOX_BT_REPORT_SIZE);

    N[0] = 0x01;

    USHORT lx = AxisI16toU16(G->ThumbLeftX);
    USHORT ly = AxisI16toU16(G->ThumbLeftY);
    USHORT rx = AxisI16toU16(G->ThumbRightX);
    USHORT ry = AxisI16toU16(G->ThumbRightY);

    N[1]  = (UCHAR)(lx & 0xFF);  N[2]  = (UCHAR)(lx >> 8);
    N[3]  = (UCHAR)(ly & 0xFF);  N[4]  = (UCHAR)(ly >> 8);
    N[5]  = (UCHAR)(rx & 0xFF);  N[6]  = (UCHAR)(rx >> 8);
    N[7]  = (UCHAR)(ry & 0xFF);  N[8]  = (UCHAR)(ry >> 8);

    N[9]  = G->TriggerLeft;
    N[10] = G->TriggerRight;

    if (G->DPad == GAYM_DPAD_NEUTRAL)
        N[11] = 0;
    else
        N[11] = G->DPad + 1;

    USHORT btns = 0;
    if (G->Buttons[0] & GAYM_BTN_A)     btns |= (1 << 0);
    if (G->Buttons[0] & GAYM_BTN_B)     btns |= (1 << 1);
    if (G->Buttons[0] & GAYM_BTN_X)     btns |= (1 << 3);
    if (G->Buttons[0] & GAYM_BTN_Y)     btns |= (1 << 8);
    if (G->Buttons[0] & GAYM_BTN_LB)    btns |= (1 << 4);
    if (G->Buttons[0] & GAYM_BTN_RB)    btns |= (1 << 5);
    if (G->Buttons[0] & GAYM_BTN_BACK)  btns |= (1 << 6);
    if (G->Buttons[0] & GAYM_BTN_START) btns |= (1 << 7);
    if (G->Buttons[1] & GAYM_BTN_LSTICK) btns |= (1 << 9);
    if (G->Buttons[1] & GAYM_BTN_RSTICK) btns |= (1 << 10);
    if (G->Buttons[1] & GAYM_BTN_GUIDE)  btns |= (1 << 12);

    N[12] = (UCHAR)(btns & 0xFF);
    N[13] = (UCHAR)(btns >> 8);

    *Written = XBOX_BT_REPORT_SIZE;
    return STATUS_SUCCESS;
}

static NTSTATUS ParseXboxBtReport(
    _In_reads_bytes_(NativeBufferSize) const UCHAR* NativeReport,
    _In_ ULONG NativeBufferSize,
    _Out_ PGAYM_REPORT GenericReport,
    _Out_ PULONG CaptureFlags)
{
    USHORT buttons;

    if (NativeReport == NULL || GenericReport == NULL || CaptureFlags == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (NativeBufferSize < 14) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    ResetParsedReport(GenericReport);
    *CaptureFlags = GAYM_CAPTURE_FLAG_VALID | GAYM_CAPTURE_FLAG_SOURCE_NATIVE_READ;

    GenericReport->ReportId = NativeReport[0];
    GenericReport->ThumbLeftX = AxisU16toI16((USHORT)(NativeReport[1] | (NativeReport[2] << 8)));
    GenericReport->ThumbLeftY = AxisU16toI16((USHORT)(NativeReport[3] | (NativeReport[4] << 8)));
    GenericReport->ThumbRightX = AxisU16toI16((USHORT)(NativeReport[5] | (NativeReport[6] << 8)));
    GenericReport->ThumbRightY = AxisU16toI16((USHORT)(NativeReport[7] | (NativeReport[8] << 8)));
    GenericReport->TriggerLeft = NativeReport[9];
    GenericReport->TriggerRight = NativeReport[10];
    if (NativeReport[11] != 0 && NativeReport[11] <= 8) {
        GenericReport->DPad = NativeReport[11] - 1;
    }

    buttons = (USHORT)(NativeReport[12] | (NativeReport[13] << 8));
    if (buttons & (1u << 0)) GenericReport->Buttons[0] |= GAYM_BTN_A;
    if (buttons & (1u << 1)) GenericReport->Buttons[0] |= GAYM_BTN_B;
    if (buttons & (1u << 3)) GenericReport->Buttons[0] |= GAYM_BTN_X;
    if (buttons & (1u << 8)) GenericReport->Buttons[0] |= GAYM_BTN_Y;
    if (buttons & (1u << 4)) GenericReport->Buttons[0] |= GAYM_BTN_LB;
    if (buttons & (1u << 5)) GenericReport->Buttons[0] |= GAYM_BTN_RB;
    if (buttons & (1u << 6)) GenericReport->Buttons[0] |= GAYM_BTN_BACK;
    if (buttons & (1u << 7)) GenericReport->Buttons[0] |= GAYM_BTN_START;
    if (buttons & (1u << 9)) GenericReport->Buttons[1] |= GAYM_BTN_LSTICK;
    if (buttons & (1u << 10)) GenericReport->Buttons[1] |= GAYM_BTN_RSTICK;
    if (buttons & (1u << 12)) GenericReport->Buttons[1] |= GAYM_BTN_GUIDE;

    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════
 * DualSense / DualSense Edge - USB HID report translator
 *
 * Format (64 bytes, Report ID 0x01):
 *   [0]     Report ID  (0x01)
 *   [1]     LX         0-255, center 0x80
 *   [2]     LY         0-255, center 0x80
 *   [3]     RX         0-255, center 0x80
 *   [4]     RY         0-255, center 0x80
 *   [5]     L2 trigger 0-255
 *   [6]     R2 trigger 0-255
 *   [7]     Sequence#  (auto-incremented)
 *   [8]     Buttons 0:
 *             Bits 3-0: D-pad hat (0-7, 8=neutral)
 *             Bit 4: Square  (→ X)
 *             Bit 5: Cross   (→ A)
 *             Bit 6: Circle  (→ B)
 *             Bit 7: Triangle(→ Y)
 *   [9]     Buttons 1:
 *             Bit 0: L1      Bit 4: Create (Back)
 *             Bit 1: R1      Bit 5: Options (Start)
 *             Bit 2: L2 btn  Bit 6: L3
 *             Bit 3: R2 btn  Bit 7: R3
 *   [10]    Buttons 2:
 *             Bit 0: PS button
 *             Bit 1: Touchpad click
 *             Bit 2: Mute button
 *   [11-63] IMU, touchpad, battery (zeroed)
 * ═══════════════════════════════════════════════════════════════════ */
static NTSTATUS TranslateDualSenseReport(
    _In_  const GAYM_REPORT* G,
    _Out_ PUCHAR             N,
    _In_  ULONG              Sz,
    _Out_ PULONG             Written,
    _Inout_ PUCHAR           Seq)
{
    const ULONG DS_REPORT_SIZE = 64;
    if (Sz < DS_REPORT_SIZE) return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(N, DS_REPORT_SIZE);

    /* Report ID */
    N[0] = 0x01;

    /* Sticks - int16 → uint8 */
    N[1] = AxisI16toU8(G->ThumbLeftX);
    N[2] = AxisI16toU8(G->ThumbLeftY);
    N[3] = AxisI16toU8(G->ThumbRightX);
    N[4] = AxisI16toU8(G->ThumbRightY);

    /* Triggers */
    N[5] = G->TriggerLeft;
    N[6] = G->TriggerRight;

    /* Sequence counter */
    N[7] = (*Seq)++;

    /* Buttons byte 0: d-pad hat + face buttons */
    UCHAR btn0 = 0;
    if (G->DPad == GAYM_DPAD_NEUTRAL)
        btn0 = 8;   /* DualSense uses 8 for neutral, not 0x0F */
    else
        btn0 = G->DPad;

    if (G->Buttons[0] & GAYM_BTN_X) btn0 |= 0x10;  /* Square  */
    if (G->Buttons[0] & GAYM_BTN_A) btn0 |= 0x20;  /* Cross   */
    if (G->Buttons[0] & GAYM_BTN_B) btn0 |= 0x40;  /* Circle  */
    if (G->Buttons[0] & GAYM_BTN_Y) btn0 |= 0x80;  /* Triangle*/
    N[8] = btn0;

    /* Buttons byte 1: shoulders, system, sticks */
    UCHAR btn1 = 0;
    if (G->Buttons[0] & GAYM_BTN_LB)    btn1 |= 0x01;  /* L1      */
    if (G->Buttons[0] & GAYM_BTN_RB)    btn1 |= 0x02;  /* R1      */
    if (G->TriggerLeft  > 0)             btn1 |= 0x04;  /* L2 digi */
    if (G->TriggerRight > 0)             btn1 |= 0x08;  /* R2 digi */
    if (G->Buttons[0] & GAYM_BTN_BACK)  btn1 |= 0x10;  /* Create  */
    if (G->Buttons[0] & GAYM_BTN_START) btn1 |= 0x20;  /* Options */
    if (G->Buttons[1] & GAYM_BTN_LSTICK) btn1 |= 0x40; /* L3      */
    if (G->Buttons[1] & GAYM_BTN_RSTICK) btn1 |= 0x80; /* R3      */
    N[9] = btn1;

    /* Buttons byte 2: system buttons */
    UCHAR btn2 = 0;
    if (G->Buttons[1] & GAYM_BTN_GUIDE) btn2 |= 0x01;  /* PS      */
    if (G->Buttons[1] & GAYM_BTN_MISC)  btn2 |= 0x04;  /* Mute    */
    N[10] = btn2;

    *Written = DS_REPORT_SIZE;
    return STATUS_SUCCESS;
}

static NTSTATUS ParseDualSenseReport(
    _In_reads_bytes_(NativeBufferSize) const UCHAR* NativeReport,
    _In_ ULONG NativeBufferSize,
    _Out_ PGAYM_REPORT GenericReport,
    _Out_ PULONG CaptureFlags)
{
    UCHAR hat;
    UCHAR buttons0;
    UCHAR buttons1;
    UCHAR buttons2;

    if (NativeReport == NULL || GenericReport == NULL || CaptureFlags == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (NativeBufferSize < 11) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    ResetParsedReport(GenericReport);
    *CaptureFlags = GAYM_CAPTURE_FLAG_VALID | GAYM_CAPTURE_FLAG_SOURCE_NATIVE_READ;

    GenericReport->ReportId = NativeReport[0];
    GenericReport->ThumbLeftX = AxisU8toI16(NativeReport[1]);
    GenericReport->ThumbLeftY = AxisU8toI16(NativeReport[2]);
    GenericReport->ThumbRightX = AxisU8toI16(NativeReport[3]);
    GenericReport->ThumbRightY = AxisU8toI16(NativeReport[4]);
    GenericReport->TriggerLeft = NativeReport[5];
    GenericReport->TriggerRight = NativeReport[6];

    buttons0 = NativeReport[8];
    buttons1 = NativeReport[9];
    buttons2 = NativeReport[10];
    hat = buttons0 & 0x0F;
    if (hat <= 7) {
        GenericReport->DPad = hat;
    }

    if (buttons0 & 0x10) GenericReport->Buttons[0] |= GAYM_BTN_X;
    if (buttons0 & 0x20) GenericReport->Buttons[0] |= GAYM_BTN_A;
    if (buttons0 & 0x40) GenericReport->Buttons[0] |= GAYM_BTN_B;
    if (buttons0 & 0x80) GenericReport->Buttons[0] |= GAYM_BTN_Y;

    if (buttons1 & 0x01) GenericReport->Buttons[0] |= GAYM_BTN_LB;
    if (buttons1 & 0x02) GenericReport->Buttons[0] |= GAYM_BTN_RB;
    if (buttons1 & 0x10) GenericReport->Buttons[0] |= GAYM_BTN_BACK;
    if (buttons1 & 0x20) GenericReport->Buttons[0] |= GAYM_BTN_START;
    if (buttons1 & 0x40) GenericReport->Buttons[1] |= GAYM_BTN_LSTICK;
    if (buttons1 & 0x80) GenericReport->Buttons[1] |= GAYM_BTN_RSTICK;
    if (buttons2 & 0x01) GenericReport->Buttons[1] |= GAYM_BTN_GUIDE;
    if (buttons2 & 0x04) GenericReport->Buttons[1] |= GAYM_BTN_MISC;

    return STATUS_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════
 * Device table - all supported VID/PID pairs
 * ═══════════════════════════════════════════════════════════════════ */
static const GAYM_DEVICE_DESCRIPTOR g_DeviceTable[] =
{
    /* ── Xbox One (Bluetooth) ── */
    { 0x045E, 0x02D1, GAYM_DEVICE_XBOX_ONE,    "Xbox One Controller",          0x01, 16, TranslateXboxBtReport, ParseXboxBtReport, GAYM_XBOX_XINPUT_OUTPUT_SELECTOR, GAYM_XBOX_XINPUT_OUTPUT_PACKET_LENGTH, GAYM_INPUT_CAP_XBOX_BT_GAMEPAD,  GAYM_OUTPUT_CAP_XBOX_GAMEPAD,      TranslateXboxXInputOutputState, ParseXboxXInputOutputReport },
    { 0x045E, 0x02DD, GAYM_DEVICE_XBOX_ONE,    "Xbox One Controller (S)",      0x01, 16, TranslateXboxBtReport, ParseXboxBtReport, GAYM_XBOX_XINPUT_OUTPUT_SELECTOR, GAYM_XBOX_XINPUT_OUTPUT_PACKET_LENGTH, GAYM_INPUT_CAP_XBOX_BT_GAMEPAD,  GAYM_OUTPUT_CAP_XBOX_GAMEPAD,      TranslateXboxXInputOutputState, ParseXboxXInputOutputReport },
    { 0x045E, 0x02E0, GAYM_DEVICE_XBOX_ONE,    "Xbox One Elite Controller",    0x01, 16, TranslateXboxBtReport, ParseXboxBtReport, GAYM_XBOX_XINPUT_OUTPUT_SELECTOR, GAYM_XBOX_XINPUT_OUTPUT_PACKET_LENGTH, GAYM_INPUT_CAP_XBOX_BT_GAMEPAD,  GAYM_OUTPUT_CAP_XBOX_GAMEPAD,      TranslateXboxXInputOutputState, ParseXboxXInputOutputReport },
    { 0x045E, 0x02EA, GAYM_DEVICE_XBOX_ONE,    "Xbox Adaptive Controller",     0x01, 16, TranslateXboxBtReport, ParseXboxBtReport, GAYM_XBOX_XINPUT_OUTPUT_SELECTOR, GAYM_XBOX_XINPUT_OUTPUT_PACKET_LENGTH, GAYM_INPUT_CAP_XBOX_BT_GAMEPAD,  GAYM_OUTPUT_CAP_XBOX_GAMEPAD,      TranslateXboxXInputOutputState, ParseXboxXInputOutputReport },

    /* ── Xbox Series X|S (Bluetooth) ── */
    { 0x045E, 0x0B12, GAYM_DEVICE_XBOX_SERIES, "Xbox Series X|S Controller",   0x01, 16, TranslateXboxBtReport, ParseXboxBtReport, GAYM_XBOX_XINPUT_OUTPUT_SELECTOR, GAYM_XBOX_XINPUT_OUTPUT_PACKET_LENGTH, GAYM_INPUT_CAP_XBOX_BT_GAMEPAD,  GAYM_OUTPUT_CAP_XBOX_GAMEPAD,      TranslateXboxXInputOutputState, ParseXboxXInputOutputReport },
    { 0x045E, 0x0B13, GAYM_DEVICE_XBOX_SERIES, "Xbox Series X|S Controller",   0x01, 16, TranslateXboxBtReport, ParseXboxBtReport, GAYM_XBOX_XINPUT_OUTPUT_SELECTOR, GAYM_XBOX_XINPUT_OUTPUT_PACKET_LENGTH, GAYM_INPUT_CAP_XBOX_BT_GAMEPAD,  GAYM_OUTPUT_CAP_XBOX_GAMEPAD,      TranslateXboxXInputOutputState, ParseXboxXInputOutputReport },
    { 0x045E, 0x0B20, GAYM_DEVICE_XBOX_SERIES, "Xbox Series S Controller",     0x01, 16, TranslateXboxBtReport, ParseXboxBtReport, GAYM_XBOX_XINPUT_OUTPUT_SELECTOR, GAYM_XBOX_XINPUT_OUTPUT_PACKET_LENGTH, GAYM_INPUT_CAP_XBOX_BT_GAMEPAD,  GAYM_OUTPUT_CAP_XBOX_GAMEPAD,      TranslateXboxXInputOutputState, ParseXboxXInputOutputReport },
    { 0x045E, 0x0B22, GAYM_DEVICE_XBOX_SERIES, "Xbox Elite Series 2",          0x01, 16, TranslateXboxBtReport, ParseXboxBtReport, GAYM_XBOX_XINPUT_OUTPUT_SELECTOR, GAYM_XBOX_XINPUT_OUTPUT_PACKET_LENGTH, GAYM_INPUT_CAP_XBOX_BT_GAMEPAD,  GAYM_OUTPUT_CAP_XBOX_GAMEPAD,      TranslateXboxXInputOutputState, ParseXboxXInputOutputReport },

    /* ── Xbox USB XInput HID compatibility (PID 0x02FF) ── */
    { 0x045E, 0x02FF, GAYM_DEVICE_XBOX_ONE,    "Xbox Controller (XInput HID)", 0x00, 16, TranslateXboxUsbHidReport, ParseXboxUsbHidReport, GAYM_XBOX_XINPUT_OUTPUT_SELECTOR, GAYM_XBOX_XINPUT_OUTPUT_PACKET_LENGTH, GAYM_INPUT_CAP_XBOX_USB_GAMEPAD, GAYM_OUTPUT_CAP_XBOX_GAMEPAD,      TranslateXboxXInputOutputState, ParseXboxXInputOutputReport },

    /* ── DualSense (PS5) ── */
    { 0x054C, 0x0CE6, GAYM_DEVICE_DUALSENSE,      "DualSense Wireless",        0x01, 64, TranslateDualSenseReport, ParseDualSenseReport, 0x00, 0, GAYM_INPUT_CAP_DUALSENSE_GAMEPAD, GAYM_OUTPUT_CAP_DUALSENSE_GAMEPAD, NULL, NULL },

    /* ── DualSense Edge (PS5) ── */
    { 0x054C, 0x0DF2, GAYM_DEVICE_DUALSENSE_EDGE, "DualSense Edge",            0x01, 64, TranslateDualSenseReport, ParseDualSenseReport, 0x00, 0, GAYM_INPUT_CAP_DUALSENSE_GAMEPAD, GAYM_OUTPUT_CAP_DUALSENSE_GAMEPAD, NULL, NULL },
};

static const ULONG g_DeviceTableCount = sizeof(g_DeviceTable) / sizeof(g_DeviceTable[0]);

/* ═══════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════ */

const GAYM_DEVICE_DESCRIPTOR* GaYmLookupDevice(_In_ USHORT VendorId, _In_ USHORT ProductId)
{
    for (ULONG i = 0; i < g_DeviceTableCount; i++) {
        if (g_DeviceTable[i].VendorId  == VendorId &&
            g_DeviceTable[i].ProductId == ProductId) {
            return &g_DeviceTable[i];
        }
    }
    return NULL;
}

const char* GaYmDeviceTypeName(_In_ GAYM_DEVICE_TYPE Type)
{
    switch (Type) {
    case GAYM_DEVICE_XBOX_ONE:       return "Xbox One";
    case GAYM_DEVICE_XBOX_SERIES:    return "Xbox Series";
    case GAYM_DEVICE_DUALSENSE:      return "DualSense";
    case GAYM_DEVICE_DUALSENSE_EDGE: return "DualSense Edge";
    default:                         return "Unknown";
    }
}

NTSTATUS GaYmParseNativeReport(
    _In_ const GAYM_DEVICE_DESCRIPTOR* DeviceDesc,
    _In_reads_bytes_(NativeBufferSize) const UCHAR* NativeReport,
    _In_ ULONG NativeBufferSize,
    _Out_ PGAYM_REPORT GenericReport,
    _Out_ PULONG CaptureFlags)
{
    if (DeviceDesc == NULL || DeviceDesc->ParseReport == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    return DeviceDesc->ParseReport(
        NativeReport,
        NativeBufferSize,
        GenericReport,
        CaptureFlags);
}

NTSTATUS GaYmTranslateOutputState(
    _In_ const GAYM_DEVICE_DESCRIPTOR* DeviceDesc,
    _In_ const GAYM_OUTPUT_STATE* OutputState,
    _Out_writes_bytes_(NativeBufferSize) PUCHAR NativeReport,
    _In_ ULONG NativeBufferSize,
    _Out_ PULONG BytesWritten)
{
    if (DeviceDesc == NULL || DeviceDesc->TranslateOutputState == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    return DeviceDesc->TranslateOutputState(
        OutputState,
        NativeReport,
        NativeBufferSize,
        BytesWritten);
}

NTSTATUS GaYmParseOutputReport(
    _In_ const GAYM_DEVICE_DESCRIPTOR* DeviceDesc,
    _In_reads_bytes_(NativeBufferSize) const UCHAR* NativeReport,
    _In_ ULONG NativeBufferSize,
    _Out_ PGAYM_OUTPUT_STATE OutputState)
{
    if (DeviceDesc == NULL || DeviceDesc->ParseOutputReport == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    return DeviceDesc->ParseOutputReport(
        NativeReport,
        NativeBufferSize,
        OutputState);
}

GAYM_CAPABILITY_FLAGS GaYmGetInputCapabilities(_In_opt_ const GAYM_DEVICE_DESCRIPTOR* DeviceDesc)
{
    if (DeviceDesc == NULL) {
        return 0;
    }

    return DeviceDesc->InputCapabilities;
}

GAYM_CAPABILITY_FLAGS GaYmGetOutputCapabilities(_In_opt_ const GAYM_DEVICE_DESCRIPTOR* DeviceDesc)
{
    if (DeviceDesc == NULL) {
        return 0;
    }

    return DeviceDesc->OutputCapabilities;
}
