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

/* ═══════════════════════════════════════════════════════════════════
 * Xbox - USB XInput HID report translator (PID 0x02FF)
 *
 * Byte layout discovered via HidP_SetUsageValue probing:
 *   [0]     Report ID  (0x00 — no report ID on wire)
 *   [1-2]   LX         uint16 LE, 0-65535, center 32768 (Usage 0x30)
 *   [3-4]   LY         uint16 LE                        (Usage 0x31)
 *   [5-6]   RX         uint16 LE                        (Usage 0x33)
 *   [7-8]   RY         uint16 LE                        (Usage 0x34)
 *   [9-10]  Z          uint16 LE, combined triggers      (Usage 0x32)
 *                       center=32768, LT→0, RT→65535
 *   [11]    Buttons 1-8 bitmap:
 *             Bit 0: A     Bit 4: LB
 *             Bit 1: B     Bit 5: RB
 *             Bit 2: X     Bit 6: Back/View
 *             Bit 3: Y     Bit 7: Start/Menu
 *   [12]    Buttons 9-16 bitmap:
 *             Bit 0: L3    Bit 2: Guide
 *             Bit 1: R3    Bit 3: Share/Misc
 *   [13]    Hat switch (low nibble, 1-8 directions, 0=neutral)
 *   [14-15] Reserved (0)
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
    if (Sz < REPORT_SIZE) return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(N, REPORT_SIZE);

    /* Byte [0]: Report ID = 0x00 (no report ID on this device) */
    N[0] = 0x00;

    /* Bytes [1-8]: Stick axes — int16 → uint16 LE, center = 32768 */
    USHORT lx = AxisI16toU16(G->ThumbLeftX);
    USHORT ly = AxisI16toU16(G->ThumbLeftY);
    USHORT rx = AxisI16toU16(G->ThumbRightX);
    USHORT ry = AxisI16toU16(G->ThumbRightY);

    N[1]  = (UCHAR)(lx & 0xFF);  N[2]  = (UCHAR)(lx >> 8);
    N[3]  = (UCHAR)(ly & 0xFF);  N[4]  = (UCHAR)(ly >> 8);
    N[5]  = (UCHAR)(rx & 0xFF);  N[6]  = (UCHAR)(rx >> 8);
    N[7]  = (UCHAR)(ry & 0xFF);  N[8]  = (UCHAR)(ry >> 8);

    /* Bytes [9-10]: Z axis — combined triggers, center = 32768
     * LT pushes toward 0, RT pushes toward 65535 */
    LONG z = 32768L + ((LONG)G->TriggerRight - (LONG)G->TriggerLeft) * 128L;
    if (z < 0)     z = 0;
    if (z > 65535) z = 65535;
    USHORT zVal = (USHORT)z;
    N[9]  = (UCHAR)(zVal & 0xFF);
    N[10] = (UCHAR)(zVal >> 8);

    /* Byte [11]: Buttons 1-8
     * GAYM_BTN_* in Buttons[0] map 1:1 to HID buttons 1-8 */
    N[11] = G->Buttons[0];

    /* Byte [12]: Buttons 9-16
     * GAYM_BTN_LSTICK(0x01)→btn9, RSTICK(0x02)→btn10,
     * GUIDE(0x04)→btn11, MISC(0x08)→btn12 */
    N[12] = G->Buttons[1] & 0x0F;

    /* Byte [13]: Hat switch — GAYM 0-7 + 0x0F(neutral) → HID 1-8 + 0(neutral) */
    if (G->DPad == GAYM_DPAD_NEUTRAL)
        N[13] = 0;
    else
        N[13] = G->DPad + 1;

    *Written = REPORT_SIZE;
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

/* ═══════════════════════════════════════════════════════════════════
 * Device table - all supported VID/PID pairs
 * ═══════════════════════════════════════════════════════════════════ */
static const GAYM_DEVICE_DESCRIPTOR g_DeviceTable[] =
{
    /* ── Xbox One (Bluetooth) ── */
    { 0x045E, 0x02D1, GAYM_DEVICE_XBOX_ONE,    "Xbox One Controller",          0x01, 16, TranslateXboxBtReport },
    { 0x045E, 0x02DD, GAYM_DEVICE_XBOX_ONE,    "Xbox One Controller (S)",      0x01, 16, TranslateXboxBtReport },
    { 0x045E, 0x02E0, GAYM_DEVICE_XBOX_ONE,    "Xbox One Elite Controller",    0x01, 16, TranslateXboxBtReport },
    { 0x045E, 0x02EA, GAYM_DEVICE_XBOX_ONE,    "Xbox Adaptive Controller",     0x01, 16, TranslateXboxBtReport },

    /* ── Xbox Series X|S (Bluetooth) ── */
    { 0x045E, 0x0B12, GAYM_DEVICE_XBOX_SERIES, "Xbox Series X|S Controller",   0x01, 16, TranslateXboxBtReport },
    { 0x045E, 0x0B13, GAYM_DEVICE_XBOX_SERIES, "Xbox Series X|S Controller",   0x01, 16, TranslateXboxBtReport },
    { 0x045E, 0x0B20, GAYM_DEVICE_XBOX_SERIES, "Xbox Series S Controller",     0x01, 16, TranslateXboxBtReport },
    { 0x045E, 0x0B22, GAYM_DEVICE_XBOX_SERIES, "Xbox Elite Series 2",          0x01, 16, TranslateXboxBtReport },

    /* ── Xbox USB XInput HID compatibility (PID 0x02FF) ── */
    { 0x045E, 0x02FF, GAYM_DEVICE_XBOX_ONE,    "Xbox Controller (XInput HID)", 0x00, 16, TranslateXboxUsbHidReport },

    /* ── DualSense (PS5) ── */
    { 0x054C, 0x0CE6, GAYM_DEVICE_DUALSENSE,      "DualSense Wireless",        0x01, 64, TranslateDualSenseReport },

    /* ── DualSense Edge (PS5) ── */
    { 0x054C, 0x0DF2, GAYM_DEVICE_DUALSENSE_EDGE, "DualSense Edge",            0x01, 64, TranslateDualSenseReport },
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
