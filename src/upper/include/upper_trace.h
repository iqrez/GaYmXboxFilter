#pragma once

#include <ntddk.h>

typedef struct _UPPER_TRACE_ENTRY {
    ULONGLONG TimestampQpc;
    ULONG EventCode;
    NTSTATUS Status;
} UPPER_TRACE_ENTRY, *PUPPER_TRACE_ENTRY;

VOID UpperTraceReset(VOID);
VOID UpperTraceRecord(_In_ ULONG EventCode, _In_ NTSTATUS Status);
