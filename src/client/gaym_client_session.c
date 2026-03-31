#define INITGUID

#include "gaym_client_internal.h"

#include <setupapi.h>

#include <strsafe.h>

static HANDLE gaym_client_open_path_handle(LPCWSTR path)
{
    return CreateFileW(
        path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);
}

static HANDLE gaym_client_open_upper_control_handle(void)
{
    return gaym_client_open_path_handle(GAYM_CONTROL_DEVICE_UPPER_W);
}

HANDLE gaym_client_open_diagnostic_control_handle(void)
{
    return gaym_client_open_path_handle(GAYM_CONTROL_DEVICE_DIAGNOSTIC_W);
}

static BOOL gaym_client_try_append_path(
    PGAYM_CLIENT_DEVICE_PATH devices,
    DWORD capacity,
    DWORD index,
    LPCWSTR path,
    INT deviceIndex)
{
    if (devices == NULL || index >= capacity) {
        return FALSE;
    }

    devices[index].Path[0] = L'\0';
    devices[index].Index = deviceIndex;
    return SUCCEEDED(StringCchCopyW(devices[index].Path, GAYM_CLIENT_MAX_DEVICE_PATH_CHARS, path));
}

static DWORD gaym_client_append_preferred_control_paths(
    PGAYM_CLIENT_DEVICE_PATH devices,
    DWORD capacity)
{
    HANDLE handle;

    handle = gaym_client_open_path_handle(GAYM_CONTROL_DEVICE_UPPER_W);
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        if (devices == NULL || capacity == 0) {
            return 1;
        }
        if (!gaym_client_try_append_path(devices, capacity, 0, GAYM_CONTROL_DEVICE_UPPER_W, 0)) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        return 1;
    }

    handle = gaym_client_open_path_handle(GAYM_CONTROL_DEVICE_DIAGNOSTIC_W);
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        if (devices == NULL || capacity == 0) {
            return 1;
        }
        if (!gaym_client_try_append_path(devices, capacity, 0, GAYM_CONTROL_DEVICE_DIAGNOSTIC_W, 0)) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        return 1;
    }

    return 0;
}

void gaym_client_initialize_session(PGAYM_CLIENT_SESSION session)
{
    if (session == NULL) {
        return;
    }

    ZeroMemory(session, sizeof(*session));
    session->AdapterHandle = INVALID_HANDLE_VALUE;
    session->DriverAbiMajor = (USHORT)GAYM_ABI_MAJOR;
    session->DriverAbiMinor = (USHORT)GAYM_ABI_MINOR;
}

void gaym_client_close_session(PGAYM_CLIENT_SESSION session)
{
    if (session == NULL) {
        return;
    }

    if ((session->SessionFlags & GAYM_CLIENT_SESSION_FLAG_WRITER_HELD) != 0 &&
        session->AdapterHandle != NULL &&
        session->AdapterHandle != INVALID_HANDLE_VALUE) {
        gaym_client_send_ioctl(
            session->AdapterHandle,
            (DWORD)IOCTL_GAYM_RELEASE_WRITER_SESSION,
            NULL,
            0,
            NULL,
            0,
            NULL);
    }

    if (session->AdapterHandle != NULL && session->AdapterHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(session->AdapterHandle);
    }

    gaym_client_initialize_session(session);
}

BOOL gaym_client_has_control_device(void)
{
    HANDLE handle = gaym_client_open_path_handle(GAYM_CONTROL_DEVICE_UPPER_W);
    if (handle == INVALID_HANDLE_VALUE) {
        handle = gaym_client_open_path_handle(GAYM_CONTROL_DEVICE_DIAGNOSTIC_W);
    }

    if (handle == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    CloseHandle(handle);
    return TRUE;
}

DWORD gaym_client_enumerate_supported_adapters(
    PGAYM_CLIENT_DEVICE_PATH devices,
    DWORD capacity,
    DWORD* totalCount)
{
    HDEVINFO devInfo;
    SP_DEVICE_INTERFACE_DATA ifData;
    DWORD foundCount = 0;
    DWORD storedCount = 0;

    foundCount = gaym_client_append_preferred_control_paths(devices, capacity);
    storedCount = foundCount;
    if (foundCount != 0) {
        if (totalCount != NULL) {
            *totalCount = foundCount;
        }
        return foundCount;
    }

    devInfo = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_GAYM_FILTER,
        NULL,
        NULL,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (devInfo != INVALID_HANDLE_VALUE) {
        ZeroMemory(&ifData, sizeof(ifData));
        ifData.cbSize = sizeof(ifData);

        for (DWORD idx = 0;
             SetupDiEnumDeviceInterfaces(devInfo, NULL, &GUID_DEVINTERFACE_GAYM_FILTER, idx, &ifData);
             ++idx) {
            DWORD needed = 0;
            PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = NULL;
            BOOL copied = FALSE;

            SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &needed, NULL);
            if (needed == 0) {
                continue;
            }

            detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
            if (detail == NULL) {
                SetupDiDestroyDeviceInfoList(devInfo);
                SetLastError(ERROR_OUTOFMEMORY);
                return 0;
            }

            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, needed, NULL, NULL)) {
                copied = gaym_client_try_append_path(devices, capacity, storedCount, detail->DevicePath, (INT)idx);
                ++foundCount;
                if (copied) {
                    ++storedCount;
                }
            }

            HeapFree(GetProcessHeap(), 0, detail);
        }

        SetupDiDestroyDeviceInfoList(devInfo);
    }

    if (totalCount != NULL) {
        *totalCount = foundCount;
    }

    if (foundCount == 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return 0;
    }

    if (devices != NULL && storedCount < foundCount) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return storedCount;
    }

    return foundCount;
}

HANDLE gaym_client_open_supported_adapter_handle(int index)
{
    DWORD total = 0;
    PGAYM_CLIENT_DEVICE_PATH devices = NULL;
    HANDLE handle = INVALID_HANDLE_VALUE;

    if (index < 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    if (gaym_client_enumerate_supported_adapters(NULL, 0, &total) == 0 && total == 0) {
        return INVALID_HANDLE_VALUE;
    }

    devices = (PGAYM_CLIENT_DEVICE_PATH)HeapAlloc(
        GetProcessHeap(),
        HEAP_ZERO_MEMORY,
        sizeof(GAYM_CLIENT_DEVICE_PATH) * total);
    if (devices == NULL) {
        SetLastError(ERROR_OUTOFMEMORY);
        return INVALID_HANDLE_VALUE;
    }

    if (gaym_client_enumerate_supported_adapters(devices, total, &total) == 0) {
        HeapFree(GetProcessHeap(), 0, devices);
        return INVALID_HANDLE_VALUE;
    }

    if ((DWORD)index >= total) {
        HeapFree(GetProcessHeap(), 0, devices);
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    handle = gaym_client_open_path_handle(devices[index].Path);
    HeapFree(GetProcessHeap(), 0, devices);
    return handle;
}

BOOL gaym_client_open_supported_adapter_session(int index, PGAYM_CLIENT_SESSION session)
{
    HANDLE handle;

    if (session == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    gaym_client_close_session(session);

    handle = gaym_client_open_supported_adapter_handle(index);
    if (handle == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    session->AdapterHandle = handle;
    session->DriverAbiMajor = (USHORT)GAYM_ABI_MAJOR;
    session->DriverAbiMinor = (USHORT)GAYM_ABI_MINOR;
    session->SessionFlags = 0;
    return TRUE;
}

BOOL gaym_client_query_device_info_handle(HANDLE device, PGAYM_DEVICE_INFO info)
{
    HANDLE upperHandle;
    HANDLE fallbackHandle;

    if (info == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    upperHandle = gaym_client_open_upper_control_handle();
    if (upperHandle != INVALID_HANDLE_VALUE) {
        if (gaym_client_send_ioctl(
                upperHandle,
                (DWORD)IOCTL_GAYM_QUERY_DEVICE,
                NULL,
                0,
                info,
                sizeof(*info),
                NULL)) {
            CloseHandle(upperHandle);
            return TRUE;
        }

        CloseHandle(upperHandle);
    }

    if (gaym_client_send_ioctl(
        device,
        (DWORD)IOCTL_GAYM_QUERY_DEVICE,
        NULL,
        0,
        info,
        sizeof(*info),
        NULL)) {
        return TRUE;
    }

    fallbackHandle = gaym_client_open_diagnostic_control_handle();
    if (fallbackHandle == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    if (gaym_client_send_ioctl(
            fallbackHandle,
            (DWORD)IOCTL_GAYM_QUERY_DEVICE,
            NULL,
            0,
            info,
            sizeof(*info),
            NULL)) {
        CloseHandle(fallbackHandle);
        return TRUE;
    }

    CloseHandle(fallbackHandle);
    return FALSE;
}

BOOL gaym_client_acquire_writer_handle(HANDLE device)
{
    if (gaym_client_send_ioctl(
            device,
            (DWORD)IOCTL_GAYM_ACQUIRE_WRITER_SESSION,
            NULL,
            0,
            NULL,
            0,
            NULL)) {
        return TRUE;
    }

    if (GetLastError() != ERROR_BUSY) {
        return FALSE;
    }

    (void)gaym_client_set_override_handle(device, FALSE);
    (void)gaym_client_release_writer_handle(device);

    return gaym_client_send_ioctl(
        device,
        (DWORD)IOCTL_GAYM_ACQUIRE_WRITER_SESSION,
        NULL,
        0,
        NULL,
        0,
        NULL);
}

BOOL gaym_client_release_writer_handle(HANDLE device)
{
    return gaym_client_send_ioctl(
        device,
        (DWORD)IOCTL_GAYM_RELEASE_WRITER_SESSION,
        NULL,
        0,
        NULL,
        0,
        NULL);
}

BOOL gaym_client_set_override_handle(HANDLE device, BOOL enabled)
{
    return gaym_client_send_ioctl(
        device,
        (DWORD)(enabled ? IOCTL_GAYM_OVERRIDE_ON : IOCTL_GAYM_OVERRIDE_OFF),
        NULL,
        0,
        NULL,
        0,
        NULL);
}

BOOL gaym_client_inject_legacy_report_handle(HANDLE device, const GAYM_REPORT* report)
{
    if (report == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    return gaym_client_send_ioctl(
        device,
        (DWORD)IOCTL_GAYM_INJECT_REPORT,
        (LPVOID)report,
        sizeof(*report),
        NULL,
        0,
        NULL);
}

BOOL gaym_client_configure_jitter_handle(HANDLE device, const GAYM_JITTER_CONFIG* config)
{
    if (config == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    return gaym_client_send_ioctl_with_diagnostic_fallback(
        device,
        (DWORD)IOCTL_GAYM_SET_JITTER,
        (LPVOID)config,
        sizeof(*config),
        NULL,
        0,
        NULL);
}

BOOL gaym_client_query_device_info_session(const GAYM_CLIENT_SESSION* session, PGAYM_DEVICE_INFO info)
{
    if (session == NULL || session->AdapterHandle == NULL || session->AdapterHandle == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    return gaym_client_query_device_info_handle(session->AdapterHandle, info);
}

BOOL gaym_client_acquire_writer_session(PGAYM_CLIENT_SESSION session)
{
    if (session == NULL || session->AdapterHandle == NULL || session->AdapterHandle == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (!gaym_client_acquire_writer_handle(session->AdapterHandle)) {
        return FALSE;
    }

    session->SessionFlags |= GAYM_CLIENT_SESSION_FLAG_WRITER_HELD;
    return TRUE;
}

BOOL gaym_client_release_writer_session(PGAYM_CLIENT_SESSION session)
{
    if (session == NULL || session->AdapterHandle == NULL || session->AdapterHandle == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (!gaym_client_release_writer_handle(session->AdapterHandle)) {
        return FALSE;
    }

    session->SessionFlags &= ~GAYM_CLIENT_SESSION_FLAG_WRITER_HELD;
    return TRUE;
}

BOOL gaym_client_enable_override_session(const GAYM_CLIENT_SESSION* session)
{
    if (session == NULL || session->AdapterHandle == NULL || session->AdapterHandle == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    return gaym_client_set_override_handle(session->AdapterHandle, TRUE);
}

BOOL gaym_client_disable_override_session(const GAYM_CLIENT_SESSION* session)
{
    if (session == NULL || session->AdapterHandle == NULL || session->AdapterHandle == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    return gaym_client_set_override_handle(session->AdapterHandle, FALSE);
}

BOOL gaym_client_inject_legacy_report_session(const GAYM_CLIENT_SESSION* session, const GAYM_REPORT* report)
{
    if (session == NULL || session->AdapterHandle == NULL || session->AdapterHandle == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    return gaym_client_inject_legacy_report_handle(session->AdapterHandle, report);
}
