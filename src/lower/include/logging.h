#pragma once
/*
 * GaYmFilter - Kernel logging infrastructure
 * Uses DbgPrint with level-based filtering.
 * In production, replace with WPP tracing for zero-overhead release builds.
 */

#include <ntddk.h>

#define GAYM_TAG  'mYaG'   /* Pool tag (little-endian: GaYm) */

/* Log levels */
#define GAYM_LOG_LEVEL_ERROR    0
#define GAYM_LOG_LEVEL_WARN     1
#define GAYM_LOG_LEVEL_INFO     2
#define GAYM_LOG_LEVEL_DEBUG    3

/* Default: show info and above in debug, errors only in release */
#if DBG
#define GAYM_CURRENT_LOG_LEVEL  GAYM_LOG_LEVEL_DEBUG
#else
#define GAYM_CURRENT_LOG_LEVEL  GAYM_LOG_LEVEL_ERROR
#endif

static __forceinline ULONG GaYmCurrentLogLevel(VOID)
{
    return GAYM_CURRENT_LOG_LEVEL;
}

static __forceinline BOOLEAN GaYmShouldLog(_In_ ULONG Level)
{
    return Level <= GaYmCurrentLogLevel();
}

#define GAYM_LOG(level, prefix, fmt, ...)                              \
    do {                                                               \
        if (GaYmShouldLog((ULONG)(level))) {                           \
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,         \
                "[GaYmFilter][" prefix "] " fmt "\n", ##__VA_ARGS__); \
        }                                                              \
    } while (0)

#define GAYM_LOG_ERROR(fmt, ...)  GAYM_LOG(GAYM_LOG_LEVEL_ERROR, "ERR",  fmt, ##__VA_ARGS__)
#define GAYM_LOG_WARN(fmt, ...)   GAYM_LOG(GAYM_LOG_LEVEL_WARN,  "WRN",  fmt, ##__VA_ARGS__)
#define GAYM_LOG_INFO(fmt, ...)   GAYM_LOG(GAYM_LOG_LEVEL_INFO,  "INF",  fmt, ##__VA_ARGS__)
#define GAYM_LOG_DEBUG(fmt, ...)  GAYM_LOG(GAYM_LOG_LEVEL_DEBUG, "DBG",  fmt, ##__VA_ARGS__)
