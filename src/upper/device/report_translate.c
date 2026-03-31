#include "../include/upper_device.h"

static __forceinline USHORT UpperAxisI16ToU16(SHORT value)
{
    return (USHORT)((LONG)value + 32768L);
}

static __forceinline SHORT UpperAxisU16ToI16(USHORT value)
{
    LONG signedValue = (LONG)value - 32768L;
    if (signedValue < -32768L) {
        signedValue = -32768L;
    } else if (signedValue > 32767L) {
        signedValue = 32767L;
    }

    return (SHORT)signedValue;
}

NTSTATUS UpperDeviceTranslateReport(
    _In_ PUPPER_DEVICE_CONTEXT Context,
    _In_ ULONG InputSize,
    _In_reads_bytes_(InputSize) const GAYM_REPORT* Report,
    _Out_writes_bytes_(OutputSize) PVOID OutputBuffer,
    _In_ ULONG OutputSize,
    _Out_ PULONG BytesWritten)
{
    PUCHAR nativeReport;
    LONG zAxis;
    USHORT leftX;
    USHORT leftY;
    USHORT rightX;
    USHORT rightY;
    USHORT zValue;

    UNREFERENCED_PARAMETER(Context);

    if (BytesWritten != NULL) {
        *BytesWritten = 0;
    }

    if (Report == NULL || OutputBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (InputSize < sizeof(*Report)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (OutputSize < 16) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    nativeReport = (PUCHAR)OutputBuffer;
    RtlZeroMemory(nativeReport, 16);

    leftX = UpperAxisI16ToU16(Report->ThumbLeftX);
    leftY = UpperAxisI16ToU16(Report->ThumbLeftY);
    rightX = UpperAxisI16ToU16(Report->ThumbRightX);
    rightY = UpperAxisI16ToU16(Report->ThumbRightY);

    nativeReport[0] = 0x00;
    nativeReport[1] = (UCHAR)(leftX & 0xFF);
    nativeReport[2] = (UCHAR)(leftX >> 8);
    nativeReport[3] = (UCHAR)(leftY & 0xFF);
    nativeReport[4] = (UCHAR)(leftY >> 8);
    nativeReport[5] = (UCHAR)(rightX & 0xFF);
    nativeReport[6] = (UCHAR)(rightX >> 8);
    nativeReport[7] = (UCHAR)(rightY & 0xFF);
    nativeReport[8] = (UCHAR)(rightY >> 8);

    zAxis = 32768L + ((LONG)Report->TriggerRight - (LONG)Report->TriggerLeft) * 128L;
    if (zAxis < 0) {
        zAxis = 0;
    } else if (zAxis > 65535L) {
        zAxis = 65535L;
    }

    zValue = (USHORT)zAxis;
    nativeReport[9] = (UCHAR)(zValue & 0xFF);
    nativeReport[10] = (UCHAR)(zValue >> 8);
    nativeReport[11] = Report->Buttons[0];
    nativeReport[12] = Report->Buttons[1] & 0x0F;
    nativeReport[13] = (Report->DPad == GAYM_DPAD_NEUTRAL) ? 0 : (UCHAR)(Report->DPad + 1);

    if (BytesWritten != NULL) {
        *BytesWritten = 16;
    }

    return STATUS_SUCCESS;
}

NTSTATUS UpperDeviceParseNativeReport(
    _In_ PUPPER_DEVICE_CONTEXT Context,
    _In_reads_bytes_(InputSize) const UCHAR* InputBuffer,
    _In_ ULONG InputSize,
    _Out_ GAYM_REPORT* Report)
{
    USHORT leftX;
    USHORT leftY;
    USHORT rightX;
    USHORT rightY;
    USHORT zValue;

    UNREFERENCED_PARAMETER(Context);

    if (InputBuffer == NULL || Report == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (InputSize < 16) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(Report, sizeof(*Report));

    leftX = (USHORT)(InputBuffer[1] | (InputBuffer[2] << 8));
    leftY = (USHORT)(InputBuffer[3] | (InputBuffer[4] << 8));
    rightX = (USHORT)(InputBuffer[5] | (InputBuffer[6] << 8));
    rightY = (USHORT)(InputBuffer[7] | (InputBuffer[8] << 8));
    zValue = (USHORT)(InputBuffer[9] | (InputBuffer[10] << 8));

    Report->ThumbLeftX = UpperAxisU16ToI16(leftX);
    Report->ThumbLeftY = UpperAxisU16ToI16(leftY);
    Report->ThumbRightX = UpperAxisU16ToI16(rightX);
    Report->ThumbRightY = UpperAxisU16ToI16(rightY);
    Report->Buttons[0] = InputBuffer[11];
    Report->Buttons[1] = InputBuffer[12] & 0x0F;

    if (InputBuffer[13] == 0 || InputBuffer[13] > 8) {
        Report->DPad = GAYM_DPAD_NEUTRAL;
    } else {
        Report->DPad = (UCHAR)(InputBuffer[13] - 1);
    }

    if (zValue < 32768U) {
        ULONG leftTrigger = (32768U - zValue) / 128U;
        if (leftTrigger > 255U) {
            leftTrigger = 255U;
        }
        Report->TriggerLeft = (UCHAR)leftTrigger;
        Report->TriggerRight = 0;
    } else if (zValue > 32768U) {
        ULONG rightTrigger = (zValue - 32768U) / 128U;
        if (rightTrigger > 255U) {
            rightTrigger = 255U;
        }
        Report->TriggerLeft = 0;
        Report->TriggerRight = (UCHAR)rightTrigger;
    } else {
        Report->TriggerLeft = 0;
        Report->TriggerRight = 0;
    }

    return STATUS_SUCCESS;
}
