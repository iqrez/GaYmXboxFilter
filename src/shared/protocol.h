#pragma once

#ifndef GAYM_PROTOCOL_H
#define GAYM_PROTOCOL_H

#ifndef GAYM_STATIC_ASSERT
#if defined(__cplusplus)
#define GAYM_STATIC_ASSERT(expr, name) static_assert((expr), #name)
#else
#define GAYM_STATIC_ASSERT(expr, name) typedef char name[(expr) ? 1 : -1]
#endif
#endif

#define GAYM_PROTOCOL_MAGIC 0x4D594147UL
#define GAYM_ABI_MAJOR 1U
#define GAYM_ABI_MINOR 0U

#define GAYM_PROTOCOL_FLAG_NONE 0x00000000UL

typedef struct _GAYM_PROTOCOL_HEADER {
    ULONG Magic;
    USHORT AbiMajor;
    USHORT AbiMinor;
    ULONG Size;
    ULONG Flags;
    ULONG RequestId;
} GAYM_PROTOCOL_HEADER, *PGAYM_PROTOCOL_HEADER;

GAYM_STATIC_ASSERT(sizeof(GAYM_PROTOCOL_HEADER) == 20, gaym_protocol_header_must_be_20_bytes);

#endif
