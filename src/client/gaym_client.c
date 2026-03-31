#include "gaym_client_internal.h"

#include <string.h>

BOOL gaym_client_send_ioctl(
    HANDLE device,
    DWORD code,
    LPVOID inputBuffer,
    DWORD inputSize,
    LPVOID outputBuffer,
    DWORD outputSize,
    LPDWORD bytesReturned)
{
    DWORD localBytes = 0;

    if (bytesReturned == NULL) {
        bytesReturned = &localBytes;
    }

    return DeviceIoControl(
        device,
        code,
        inputBuffer,
        inputSize,
        outputBuffer,
        outputSize,
        bytesReturned,
        NULL);
}

BOOL gaym_client_send_ioctl_with_upper_fallback(
    HANDLE device,
    DWORD code,
    LPVOID inputBuffer,
    DWORD inputSize,
    LPVOID outputBuffer,
    DWORD outputSize,
    LPDWORD bytesReturned)
{
    DWORD originalError;
    DWORD fallbackError;
    HANDLE fallbackHandle;

    if (gaym_client_send_ioctl(
            device,
            code,
            inputBuffer,
            inputSize,
            outputBuffer,
            outputSize,
            bytesReturned)) {
        return TRUE;
    }

    originalError = GetLastError();
    fallbackHandle = gaym_client_open_control_handle();
    if (fallbackHandle == INVALID_HANDLE_VALUE) {
        SetLastError(originalError);
        return FALSE;
    }

    if (gaym_client_send_ioctl(
            fallbackHandle,
            code,
            inputBuffer,
            inputSize,
            outputBuffer,
            outputSize,
            bytesReturned)) {
        CloseHandle(fallbackHandle);
        return TRUE;
    }

    fallbackError = GetLastError();
    CloseHandle(fallbackHandle);
    SetLastError(fallbackError);
    return FALSE;
}

ULONGLONG gaym_client_query_qpc(void)
{
    LARGE_INTEGER counter;

    if (!QueryPerformanceCounter(&counter)) {
        return 0;
    }

    return (ULONGLONG)counter.QuadPart;
}

void gaym_client_populate_protocol_header(PGAYM_PROTOCOL_HEADER header, ULONG size)
{
    if (header == NULL) {
        return;
    }

    memset(header, 0, sizeof(*header));
    header->Magic = GAYM_PROTOCOL_MAGIC;
    header->AbiMajor = (USHORT)GAYM_ABI_MAJOR;
    header->AbiMinor = (USHORT)GAYM_ABI_MINOR;
    header->Size = size;
    header->Flags = GAYM_PROTOCOL_FLAG_NONE;
    header->RequestId = 0;
}

ULONG gaym_client_adapter_family_from_device_type(GAYM_DEVICE_TYPE type)
{
    switch (type) {
    case GAYM_DEVICE_XBOX_ONE:
    case GAYM_DEVICE_XBOX_SERIES:
        return GAYM_ADAPTER_FAMILY_XBOX_02FF;
    default:
        return GAYM_ADAPTER_FAMILY_UNKNOWN;
    }
}

const char* gaym_client_device_type_name(GAYM_DEVICE_TYPE type)
{
    switch (type) {
    case GAYM_DEVICE_XBOX_ONE:
        return "Xbox One";
    case GAYM_DEVICE_XBOX_SERIES:
        return "Xbox Series";
    case GAYM_DEVICE_DUALSENSE:
        return "DualSense";
    case GAYM_DEVICE_DUALSENSE_EDGE:
        return "DualSense Edge";
    default:
        return "Unknown";
    }
}
