/*
 * SecurityAutoVerify - Negative tests for the hardened GaYm control surface.
 *
 * Verifies:
 *   - restricted non-admin impersonation cannot open the control device
 *   - read-only handles can query but cannot mutate driver state
 *   - malformed payloads fail closed
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdio>
#include <vector>

#include "DeviceHelper.h"

#pragma comment(lib, "advapi32.lib")

static void PrintFailure(const char* label, DWORD error)
{
    std::fprintf(stderr, "%s failed (error %lu).\n", label, error);
}

static bool ExpectFailure(const char* label, BOOL success, DWORD error)
{
    if (success) {
        std::fprintf(stderr, "%s unexpectedly succeeded.\n", label);
        return false;
    }

    std::printf("%s: PASS (error %lu)\n", label, error);
    return true;
}

static bool TestRestrictedTokenOpen()
{
    HANDLE processToken = NULL;
    HANDLE restrictedPrimaryToken = NULL;
    HANDLE restrictedImpersonationToken = NULL;
    HANDLE device = INVALID_HANDLE_VALUE;
    BYTE adminSidBuffer[SECURITY_MAX_SID_SIZE] = {};
    DWORD adminSidSize = sizeof(adminSidBuffer);
    SID_AND_ATTRIBUTES disabledSid = {};
    bool passed = false;

    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_DUPLICATE | TOKEN_IMPERSONATE | TOKEN_QUERY,
            &processToken)) {
        PrintFailure("OpenProcessToken", GetLastError());
        return false;
    }

    if (!CreateWellKnownSid(
            WinBuiltinAdministratorsSid,
            nullptr,
            adminSidBuffer,
            &adminSidSize)) {
        PrintFailure("CreateWellKnownSid(Administrators)", GetLastError());
        CloseHandle(processToken);
        return false;
    }

    disabledSid.Sid = adminSidBuffer;
    disabledSid.Attributes = 0;

    if (!CreateRestrictedToken(
            processToken,
            DISABLE_MAX_PRIVILEGE,
            1,
            &disabledSid,
            0,
            nullptr,
            0,
            nullptr,
            &restrictedPrimaryToken)) {
        PrintFailure("CreateRestrictedToken", GetLastError());
        CloseHandle(processToken);
        return false;
    }

    if (!DuplicateTokenEx(
            restrictedPrimaryToken,
            TOKEN_IMPERSONATE | TOKEN_QUERY,
            nullptr,
            SecurityImpersonation,
            TokenImpersonation,
            &restrictedImpersonationToken)) {
        PrintFailure("DuplicateTokenEx(restricted)", GetLastError());
        CloseHandle(restrictedPrimaryToken);
        CloseHandle(processToken);
        return false;
    }

    if (!SetThreadToken(nullptr, restrictedImpersonationToken)) {
        PrintFailure("SetThreadToken(restricted)", GetLastError());
        CloseHandle(restrictedImpersonationToken);
        CloseHandle(restrictedPrimaryToken);
        CloseHandle(processToken);
        return false;
    }

    device = OpenGaYmControlDevicePathStrictWithAccess(
        GaYmPrimaryControlDevicePath(),
        GENERIC_READ | GENERIC_WRITE);
    if (device == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
        std::printf("Restricted token open: PASS (error %lu)\n", GetLastError());
        passed = true;
    } else if (device != INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "Restricted token open unexpectedly succeeded.\n");
        CloseHandle(device);
    } else {
        PrintFailure("Restricted token open", GetLastError());
    }

    RevertToSelf();
    CloseHandle(restrictedImpersonationToken);
    CloseHandle(restrictedPrimaryToken);
    CloseHandle(processToken);
    return passed;
}

static bool TestReadOnlyHandlePolicy()
{
    HANDLE device = OpenGaYmDeviceForTargetWithAccess(
        GaYmControlTarget::Upper,
        GENERIC_READ);
    GAYM_QUERY_DEVICE_SUMMARY_RESPONSE summary = {};
    DWORD bytesReturned = 0;
    DWORD lastError;
    BOOL success;

    if (device == INVALID_HANDLE_VALUE) {
        PrintFailure("Open read-only upper control handle", GetLastError());
        return false;
    }

    if (!QueryDeviceSummary(device, &summary, &bytesReturned)) {
        lastError = GetLastError();
        CloseHandle(device);
        PrintFailure("QueryDeviceSummary(read-only)", lastError);
        return false;
    }

    std::printf("Read-only query: PASS (%lu bytes)\n", bytesReturned);

    success = SendIoctlRaw(device, IOCTL_GAYM_OVERRIDE_ON, nullptr, 0, nullptr, 0);
    lastError = GetLastError();
    if (!ExpectFailure("Read-only override on", success, lastError)) {
        CloseHandle(device);
        return false;
    }

    if (lastError != ERROR_ACCESS_DENIED) {
        std::fprintf(stderr, "Read-only override on returned %lu instead of access denied.\n", lastError);
        CloseHandle(device);
        return false;
    }

    CloseHandle(device);
    return true;
}

static bool TestMalformedPayloads()
{
    HANDLE device = OpenGaYmDeviceForTargetWithAccess(
        GaYmControlTarget::Upper,
        GENERIC_READ | GENERIC_WRITE);
    DWORD bytesReturned = 0;
    UCHAR oneByte = 0;
    GAYM_REPORT report = {};
    GAYM_OUTPUT_STATE outputState = {};
    GAYM_JITTER_CONFIG jitter = {};
    GAYM_QUERY_DEVICE_SUMMARY_RESPONSE summaryResponse = {};
    GAYM_QUERY_SNAPSHOT_REQUEST snapshotRequest = {};
    std::vector<UCHAR> oversizeReport(sizeof(GAYM_REPORT) + 1, 0);
    BOOL success = FALSE;
    DWORD lastError = ERROR_SUCCESS;

    if (device == INVALID_HANDLE_VALUE) {
        PrintFailure("Open read-write upper control handle", GetLastError());
        return false;
    }

    success = SendIoctlRaw(device, IOCTL_GAYM_OVERRIDE_ON, &oneByte, sizeof(oneByte), nullptr, 0);
    lastError = GetLastError();
    if (!ExpectFailure("Override on with unexpected input", success, lastError)) {
        CloseHandle(device);
        return false;
    }

    success = SendIoctlRaw(
        device,
        IOCTL_GAYM_QUERY_DEVICE,
        &oneByte,
        sizeof(oneByte),
        &summaryResponse,
        sizeof(summaryResponse),
        &bytesReturned);
    lastError = GetLastError();
    if (!ExpectFailure("Legacy query with unexpected input", success, lastError)) {
        CloseHandle(device);
        return false;
    }

    InitializeGaYmProtocolHeader(
        &snapshotRequest.Header,
        sizeof(snapshotRequest) - sizeof(snapshotRequest.Header));
    snapshotRequest.SnapshotKind = GAYM_SNAPSHOT_DEVICE_SUMMARY;
    snapshotRequest.Header.Magic = 0;

    success = SendIoctlRaw(
        device,
        IOCTL_GAYM_QUERY_SNAPSHOT,
        &snapshotRequest,
        sizeof(snapshotRequest),
        &summaryResponse,
        sizeof(summaryResponse),
        &bytesReturned);
    lastError = GetLastError();
    if (!ExpectFailure("Protocol query with invalid magic", success, lastError)) {
        CloseHandle(device);
        return false;
    }

    success = SendIoctlRaw(
        device,
        IOCTL_GAYM_INJECT_REPORT,
        &report,
        sizeof(report) - 1,
        nullptr,
        0);
    lastError = GetLastError();
    if (!ExpectFailure("Inject short report", success, lastError)) {
        CloseHandle(device);
        return false;
    }

    success = SendIoctlRaw(
        device,
        IOCTL_GAYM_INJECT_REPORT,
        oversizeReport.data(),
        static_cast<DWORD>(oversizeReport.size()),
        nullptr,
        0);
    lastError = GetLastError();
    if (!ExpectFailure("Inject oversized report", success, lastError)) {
        CloseHandle(device);
        return false;
    }

    success = SendIoctlRaw(
        device,
        IOCTL_GAYM_APPLY_OUTPUT,
        &outputState,
        sizeof(outputState) - 1,
        nullptr,
        0);
    lastError = GetLastError();
    if (!ExpectFailure("ApplyOutput short payload", success, lastError)) {
        CloseHandle(device);
        return false;
    }

    jitter.Enabled = TRUE;
    jitter.MinDelayUs = 100;
    jitter.MaxDelayUs = 1;
    if (SendIoctlRaw(
            device,
            IOCTL_GAYM_SET_JITTER,
            &jitter,
            sizeof(jitter),
            nullptr,
            0)) {
        std::fprintf(stderr, "Invalid jitter range unexpectedly succeeded.\n");
        CloseHandle(device);
        return false;
    }

    if (GetLastError() != ERROR_INVALID_PARAMETER &&
        GetLastError() != ERROR_NOT_SUPPORTED) {
        std::fprintf(stderr, "Invalid jitter range returned unexpected error %lu.\n", GetLastError());
        CloseHandle(device);
        return false;
    }

    std::printf("Invalid jitter range: PASS (error %lu)\n", GetLastError());
    CloseHandle(device);
    return true;
}

int main()
{
    std::printf("GaYm security verifier\n\n");

    if (!TestRestrictedTokenOpen()) {
        return 1;
    }

    if (!TestReadOnlyHandlePolicy()) {
        return 1;
    }

    if (!TestMalformedPayloads()) {
        return 1;
    }

    std::printf("\nResult: PASS\n");
    return 0;
}
