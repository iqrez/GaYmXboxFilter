#pragma once

#include "gaym_client.h"

BOOL gaym_client_send_ioctl(
    HANDLE device,
    DWORD code,
    LPVOID inputBuffer,
    DWORD inputSize,
    LPVOID outputBuffer,
    DWORD outputSize,
    LPDWORD bytesReturned);
HANDLE gaym_client_open_control_handle(void);
BOOL gaym_client_send_ioctl_with_upper_fallback(
    HANDLE device,
    DWORD code,
    LPVOID inputBuffer,
    DWORD inputSize,
    LPVOID outputBuffer,
    DWORD outputSize,
    LPDWORD bytesReturned);

ULONGLONG gaym_client_query_qpc(void);
void gaym_client_populate_protocol_header(PGAYM_PROTOCOL_HEADER header, ULONG size);
