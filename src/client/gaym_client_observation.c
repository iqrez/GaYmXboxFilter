#include "gaym_client_internal.h"

#include <string.h>

static HANDLE gaym_client_open_upper_observation_handle(void)
{
    return CreateFileW(
        GAYM_CONTROL_DEVICE_UPPER_W,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);
}

BOOL gaym_client_query_semantic_observation_handle(HANDLE device, PGAYM_OBSERVATION_V1 observation)
{
    GAYM_DEVICE_INFO info;
    HANDLE upperHandle;

    if (observation == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    memset(observation, 0, sizeof(*observation));

    upperHandle = gaym_client_open_upper_observation_handle();
    if (upperHandle != INVALID_HANDLE_VALUE) {
        if (gaym_client_send_ioctl(
                upperHandle,
                (DWORD)IOCTL_GAYM_QUERY_OBSERVATION,
                NULL,
                0,
                observation,
                sizeof(*observation),
                NULL)) {
            CloseHandle(upperHandle);
            return TRUE;
        }

        CloseHandle(upperHandle);
    }

    if (gaym_client_send_ioctl(
            device,
            (DWORD)IOCTL_GAYM_QUERY_OBSERVATION,
            NULL,
            0,
            observation,
            sizeof(*observation),
            NULL)) {
        return TRUE;
    }

    if (!gaym_client_query_device_info_handle(device, &info)) {
        return FALSE;
    }

    gaym_client_populate_protocol_header(&observation->Header, (ULONG)sizeof(*observation));
    observation->AdapterFamily = gaym_client_adapter_family_from_device_type(info.DeviceType);
    observation->CapabilityFlags =
        GAYM_CAPABILITY_SEMANTIC_CONTROL |
        GAYM_CAPABILITY_SEMANTIC_OBSERVATION |
        GAYM_CAPABILITY_NATIVE_DIAGNOSTICS;
    observation->StatusFlags = GAYM_STATUS_DEVICE_PRESENT | GAYM_STATUS_OBSERVATION_SYNTHETIC;
    if (info.OverrideActive) {
        observation->StatusFlags |= GAYM_STATUS_OVERRIDE_ACTIVE;
    }

    observation->LastObservedSequence = info.ReadRequestsSeen;
    observation->LastInjectedSequence = info.ReportsSent;
    observation->TimestampQpc = gaym_client_query_qpc();
    observation->BatteryPercent = 0xFF;
    return TRUE;
}

BOOL gaym_client_query_semantic_observation_session(const GAYM_CLIENT_SESSION* session, PGAYM_OBSERVATION_V1 observation)
{
    if (session == NULL || session->AdapterHandle == NULL || session->AdapterHandle == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    return gaym_client_query_semantic_observation_handle(session->AdapterHandle, observation);
}
