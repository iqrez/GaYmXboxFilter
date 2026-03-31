#include "gaym_client_internal.h"

BOOL gaym_client_query_diagnostic_snapshot_handle(HANDLE device, void* buffer, DWORD bufferSize, DWORD* bytesReturned)
{
    return gaym_client_send_ioctl_with_upper_fallback(
        device,
        (DWORD)IOCTL_GAYM_QUERY_SNAPSHOT,
        NULL,
        0,
        buffer,
        bufferSize,
        bytesReturned);
}

BOOL gaym_client_capture_native_observation_handle(HANDLE device, void* buffer, DWORD bufferSize, DWORD* bytesReturned)
{
    return gaym_client_send_ioctl_with_upper_fallback(
        device,
        (DWORD)IOCTL_GAYM_CAPTURE_OBSERVATION,
        NULL,
        0,
        buffer,
        bufferSize,
        bytesReturned);
}
