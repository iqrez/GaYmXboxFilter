#pragma once

#ifndef GAYM_CLIENT_H
#define GAYM_CLIENT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winioctl.h>

#include "../shared/ioctl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAYM_CLIENT_MAX_DEVICE_PATH_CHARS 512
#define GAYM_CLIENT_SESSION_FLAG_WRITER_HELD 0x00000001UL

typedef struct _GAYM_CLIENT_DEVICE_PATH {
    WCHAR Path[GAYM_CLIENT_MAX_DEVICE_PATH_CHARS];
    INT Index;
} GAYM_CLIENT_DEVICE_PATH, *PGAYM_CLIENT_DEVICE_PATH;

typedef struct _GAYM_CLIENT_SESSION {
    HANDLE AdapterHandle;
    USHORT DriverAbiMajor;
    USHORT DriverAbiMinor;
    ULONG SessionFlags;
} GAYM_CLIENT_SESSION, *PGAYM_CLIENT_SESSION;

void gaym_client_initialize_session(PGAYM_CLIENT_SESSION session);
void gaym_client_close_session(PGAYM_CLIENT_SESSION session);

BOOL gaym_client_has_control_device(void);
DWORD gaym_client_enumerate_supported_adapters(
    PGAYM_CLIENT_DEVICE_PATH devices,
    DWORD capacity,
    DWORD* totalCount);
HANDLE gaym_client_open_supported_adapter_handle(int index);
BOOL gaym_client_open_supported_adapter_session(int index, PGAYM_CLIENT_SESSION session);

BOOL gaym_client_query_device_info_handle(HANDLE device, PGAYM_DEVICE_INFO info);
BOOL gaym_client_acquire_writer_handle(HANDLE device);
BOOL gaym_client_release_writer_handle(HANDLE device);
BOOL gaym_client_set_override_handle(HANDLE device, BOOL enabled);
BOOL gaym_client_inject_legacy_report_handle(HANDLE device, const GAYM_REPORT* report);
BOOL gaym_client_configure_jitter_handle(HANDLE device, const GAYM_JITTER_CONFIG* config);
BOOL gaym_client_query_semantic_observation_handle(HANDLE device, PGAYM_OBSERVATION_V1 observation);
BOOL gaym_client_query_diagnostic_snapshot_handle(HANDLE device, void* buffer, DWORD bufferSize, DWORD* bytesReturned);
BOOL gaym_client_capture_native_observation_handle(HANDLE device, void* buffer, DWORD bufferSize, DWORD* bytesReturned);

BOOL gaym_client_query_device_info_session(const GAYM_CLIENT_SESSION* session, PGAYM_DEVICE_INFO info);
BOOL gaym_client_acquire_writer_session(PGAYM_CLIENT_SESSION session);
BOOL gaym_client_release_writer_session(PGAYM_CLIENT_SESSION session);
BOOL gaym_client_enable_override_session(const GAYM_CLIENT_SESSION* session);
BOOL gaym_client_disable_override_session(const GAYM_CLIENT_SESSION* session);
BOOL gaym_client_inject_legacy_report_session(const GAYM_CLIENT_SESSION* session, const GAYM_REPORT* report);
BOOL gaym_client_query_semantic_observation_session(const GAYM_CLIENT_SESSION* session, PGAYM_OBSERVATION_V1 observation);

const char* gaym_client_device_type_name(GAYM_DEVICE_TYPE type);
ULONG gaym_client_adapter_family_from_device_type(GAYM_DEVICE_TYPE type);

#ifdef __cplusplus
}

#include <string>
#include <vector>

namespace gaym {
namespace client {

struct DevicePath {
    std::wstring path;
    int index;
};

inline bool HasControlDevice()
{
    return gaym_client_has_control_device() != FALSE;
}

inline std::vector<DevicePath> EnumerateSupportedAdapters()
{
    DWORD total = 0;
    if (!gaym_client_enumerate_supported_adapters(NULL, 0, &total) && total == 0) {
        return {};
    }

    std::vector<GAYM_CLIENT_DEVICE_PATH> raw(total);
    if (total == 0 || !gaym_client_enumerate_supported_adapters(raw.data(), total, &total)) {
        return {};
    }

    std::vector<DevicePath> result;
    result.reserve(total);
    for (DWORD index = 0; index < total; ++index) {
        DevicePath devicePath;
        devicePath.path = raw[index].Path;
        devicePath.index = raw[index].Index;
        result.push_back(devicePath);
    }

    return result;
}

inline HANDLE OpenSupportedAdapter(int index = 0)
{
    return gaym_client_open_supported_adapter_handle(index);
}

inline bool QueryAdapterInfo(HANDLE device, GAYM_DEVICE_INFO* info)
{
    return gaym_client_query_device_info_handle(device, info) != FALSE;
}

inline bool AcquireWriterSession(HANDLE device)
{
    return gaym_client_acquire_writer_handle(device) != FALSE;
}

inline bool ReleaseWriterSession(HANDLE device)
{
    return gaym_client_release_writer_handle(device) != FALSE;
}

inline bool SetOverrideEnabled(HANDLE device, bool enabled)
{
    return gaym_client_set_override_handle(device, enabled ? TRUE : FALSE) != FALSE;
}

inline bool EnableOverride(HANDLE device)
{
    return gaym_client_set_override_handle(device, TRUE) != FALSE;
}

inline bool DisableOverride(HANDLE device)
{
    return gaym_client_set_override_handle(device, FALSE) != FALSE;
}

inline bool InjectReport(HANDLE device, const GAYM_REPORT* report)
{
    return gaym_client_inject_legacy_report_handle(device, report) != FALSE;
}

inline bool ConfigureJitter(HANDLE device, const GAYM_JITTER_CONFIG* config)
{
    return gaym_client_configure_jitter_handle(device, config) != FALSE;
}

inline bool QueryObservation(HANDLE device, GAYM_OBSERVATION_V1* observation)
{
    return gaym_client_query_semantic_observation_handle(device, observation) != FALSE;
}

inline const char* DeviceTypeName(GAYM_DEVICE_TYPE type)
{
    return gaym_client_device_type_name(type);
}

} // namespace client
} // namespace gaym
#endif

#endif
