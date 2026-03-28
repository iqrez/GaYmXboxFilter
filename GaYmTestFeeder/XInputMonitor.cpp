#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "xinput.lib")

namespace {

constexpr wchar_t kWindowClassName[] = L"GaYmXInputMonitorWindow";
constexpr UINT_PTR kPollTimerId = 1;
constexpr UINT kPollIntervalMs = 16;
constexpr int kWindowWidth = 760;
constexpr int kWindowHeight = 520;
constexpr int kMargin = 20;

struct MonitorState {
    bool Connected = false;
    DWORD PadIndex = 0;
    DWORD PacketNumber = 0;
    XINPUT_GAMEPAD Gamepad = {};
};

MonitorState g_State = {};

static bool TryGetPadState(MonitorState* state)
{
    for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
        XINPUT_STATE current = {};
        if (XInputGetState(index, &current) == ERROR_SUCCESS) {
            state->Connected = true;
            state->PadIndex = index;
            state->PacketNumber = current.dwPacketNumber;
            state->Gamepad = current.Gamepad;
            return true;
        }
    }

    state->Connected = false;
    state->PadIndex = 0;
    state->PacketNumber = 0;
    ZeroMemory(&state->Gamepad, sizeof(state->Gamepad));
    return false;
}

static int NormalizeAxis(SHORT value, int halfSpan)
{
    const double normalized = static_cast<double>(value) / 32767.0;
    return static_cast<int>(normalized * static_cast<double>(halfSpan));
}

static void DrawAxisBox(HDC dc, const RECT& rect, SHORT xValue, SHORT yValue, const wchar_t* label)
{
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);

    const int centerX = (rect.left + rect.right) / 2;
    const int centerY = (rect.top + rect.bottom) / 2;
    MoveToEx(dc, centerX, rect.top, NULL);
    LineTo(dc, centerX, rect.bottom);
    MoveToEx(dc, rect.left, centerY, NULL);
    LineTo(dc, rect.right, centerY);

    const int halfWidth = (rect.right - rect.left) / 2 - 8;
    const int halfHeight = (rect.bottom - rect.top) / 2 - 8;
    const int pointX = centerX + NormalizeAxis(xValue, halfWidth);
    const int pointY = centerY - NormalizeAxis(yValue, halfHeight);

    HBRUSH pointBrush = CreateSolidBrush(RGB(30, 144, 255));
    HGDIOBJ oldBrush = SelectObject(dc, pointBrush);
    Ellipse(dc, pointX - 8, pointY - 8, pointX + 8, pointY + 8);
    SelectObject(dc, oldBrush);
    DeleteObject(pointBrush);

    wchar_t text[128] = {};
    swprintf_s(text, L"%ls  X:%6d  Y:%6d", label, xValue, yValue);
    TextOutW(dc, rect.left, rect.bottom + 8, text, static_cast<int>(wcslen(text)));
}

static void DrawTriggerBar(HDC dc, const RECT& rect, BYTE value, const wchar_t* label)
{
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);

    RECT fill = rect;
    const int height = rect.bottom - rect.top;
    const int filledHeight = static_cast<int>((static_cast<double>(value) / 255.0) * height);
    fill.top = rect.bottom - filledHeight;

    HBRUSH fillBrush = CreateSolidBrush(RGB(46, 204, 113));
    FillRect(dc, &fill, fillBrush);
    DeleteObject(fillBrush);

    wchar_t text[64] = {};
    swprintf_s(text, L"%ls %3u", label, static_cast<unsigned>(value));
    TextOutW(dc, rect.left, rect.bottom + 8, text, static_cast<int>(wcslen(text)));
}

static void DrawButton(HDC dc, const RECT& rect, bool pressed, const wchar_t* label)
{
    HBRUSH brush = CreateSolidBrush(pressed ? RGB(231, 76, 60) : RGB(210, 210, 210));
    FillRect(dc, &rect, brush);
    DeleteObject(brush);

    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    DrawTextW(dc, label, -1, const_cast<RECT*>(&rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DrawDPad(HDC dc, int originX, int originY, WORD buttons)
{
    const int size = 32;
    RECT up = { originX + size, originY, originX + (size * 2), originY + size };
    RECT left = { originX, originY + size, originX + size, originY + (size * 2) };
    RECT right = { originX + (size * 2), originY + size, originX + (size * 3), originY + (size * 2) };
    RECT down = { originX + size, originY + (size * 2), originX + (size * 2), originY + (size * 3) };

    DrawButton(dc, up, (buttons & XINPUT_GAMEPAD_DPAD_UP) != 0, L"U");
    DrawButton(dc, left, (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0, L"L");
    DrawButton(dc, right, (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0, L"R");
    DrawButton(dc, down, (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0, L"D");
    TextOutW(dc, originX, originY + (size * 3) + 10, L"D-Pad", 5);
}

static void DrawButtons(HDC dc, int originX, int originY, WORD buttons)
{
    const int size = 36;
    RECT yButton = { originX + size, originY, originX + (size * 2), originY + size };
    RECT xButton = { originX, originY + size, originX + size, originY + (size * 2) };
    RECT bButton = { originX + (size * 2), originY + size, originX + (size * 3), originY + (size * 2) };
    RECT aButton = { originX + size, originY + (size * 2), originX + (size * 2), originY + (size * 3) };

    DrawButton(dc, yButton, (buttons & XINPUT_GAMEPAD_Y) != 0, L"Y");
    DrawButton(dc, xButton, (buttons & XINPUT_GAMEPAD_X) != 0, L"X");
    DrawButton(dc, bButton, (buttons & XINPUT_GAMEPAD_B) != 0, L"B");
    DrawButton(dc, aButton, (buttons & XINPUT_GAMEPAD_A) != 0, L"A");
    TextOutW(dc, originX, originY + (size * 3) + 10, L"Face Buttons", 12);
}

static void DrawShoulders(HDC dc, int originX, int originY, WORD buttons)
{
    RECT leftBumper = { originX, originY, originX + 120, originY + 36 };
    RECT rightBumper = { originX + 140, originY, originX + 260, originY + 36 };
    RECT backButton = { originX + 40, originY + 56, originX + 100, originY + 92 };
    RECT startButton = { originX + 160, originY + 56, originX + 220, originY + 92 };

    DrawButton(dc, leftBumper, (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0, L"LB");
    DrawButton(dc, rightBumper, (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0, L"RB");
    DrawButton(dc, backButton, (buttons & XINPUT_GAMEPAD_BACK) != 0, L"Back");
    DrawButton(dc, startButton, (buttons & XINPUT_GAMEPAD_START) != 0, L"Start");
}

static void PaintWindow(HWND windowHandle, HDC dc)
{
    RECT clientRect = {};
    GetClientRect(windowHandle, &clientRect);

    HBRUSH backgroundBrush = CreateSolidBrush(RGB(248, 248, 248));
    FillRect(dc, &clientRect, backgroundBrush);
    DeleteObject(backgroundBrush);

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));

    if (!g_State.Connected) {
        const wchar_t* message = L"No XInput controller detected.";
        DrawTextW(dc, message, -1, &clientRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    wchar_t header[160] = {};
    swprintf_s(
        header,
        L"GaYm XInput Monitor   Pad %lu   Packet %lu",
        g_State.PadIndex,
        g_State.PacketNumber);
    TextOutW(dc, kMargin, kMargin, header, static_cast<int>(wcslen(header)));

    RECT leftStickRect = { kMargin, 60, 250, 260 };
    RECT rightStickRect = { 280, 60, 510, 260 };
    DrawAxisBox(dc, leftStickRect, g_State.Gamepad.sThumbLX, g_State.Gamepad.sThumbLY, L"Left Stick");
    DrawAxisBox(dc, rightStickRect, g_State.Gamepad.sThumbRX, g_State.Gamepad.sThumbRY, L"Right Stick");

    RECT leftTriggerRect = { 560, 70, 620, 250 };
    RECT rightTriggerRect = { 650, 70, 710, 250 };
    DrawTriggerBar(dc, leftTriggerRect, g_State.Gamepad.bLeftTrigger, L"LT");
    DrawTriggerBar(dc, rightTriggerRect, g_State.Gamepad.bRightTrigger, L"RT");

    DrawShoulders(dc, 20, 320, g_State.Gamepad.wButtons);
    DrawDPad(dc, 40, 390, g_State.Gamepad.wButtons);
    DrawButtons(dc, 520, 360, g_State.Gamepad.wButtons);

    wchar_t footer[256] = {};
    swprintf_s(
        footer,
        L"Use with GaYmFeeder.exe -p keyboard or the auto verifiers. This window reflects XInputGetState().");
    TextOutW(dc, kMargin, clientRect.bottom - 30, footer, static_cast<int>(wcslen(footer)));
}

static LRESULT CALLBACK WindowProc(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        SetTimer(windowHandle, kPollTimerId, kPollIntervalMs, NULL);
        return 0;

    case WM_TIMER:
        if (wParam == kPollTimerId) {
            MonitorState nextState = {};
            TryGetPadState(&nextState);
            g_State = nextState;
            InvalidateRect(windowHandle, NULL, FALSE);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT paintStruct = {};
        HDC dc = BeginPaint(windowHandle, &paintStruct);
        PaintWindow(windowHandle, dc);
        EndPaint(windowHandle, &paintStruct);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(windowHandle, kPollTimerId);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(windowHandle, message, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instanceHandle, HINSTANCE, PWSTR, int showCommand)
{
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instanceHandle;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hCursor = LoadCursorW(NULL, reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(32512)));
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassW(&windowClass)) {
        return 1;
    }

    HWND windowHandle = CreateWindowExW(
        0,
        kWindowClassName,
        L"GaYm XInput Monitor",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        kWindowWidth,
        kWindowHeight,
        NULL,
        NULL,
        instanceHandle,
        NULL);

    if (!windowHandle) {
        return 1;
    }

    ShowWindow(windowHandle, showCommand == 0 ? SW_SHOWNORMAL : showCommand);
    UpdateWindow(windowHandle);

    MSG message = {};
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
