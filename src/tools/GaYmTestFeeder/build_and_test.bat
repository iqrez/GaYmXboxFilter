@echo off
setlocal
title GaYm Xbox 02FF Build + Test
set "SRC=%~dp0"
set "SHARED_INC=%~dp0..\..\shared"
set "CLIENT_OUT=%~dp0..\..\..\out\dev\client"
set "OUT=%~dp0..\..\..\out\dev\tools"

if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%"

echo ============================================
echo  GaYm Xbox 02FF Test Feeder - Build ^& Test
echo ============================================
echo.

REM --- Build MinimalTestFeeder ---
echo [1/3] Building MinimalTestFeeder.exe ...
cl /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /I"%SRC%" /I"%SHARED_INC%" /Fe:MinimalTestFeeder.exe "%SRC%\MinimalTestFeeder.cpp" /link user32.lib setupapi.lib >nul 2>&1
if %errorlevel% neq 0 (
    echo  FAILED. Retrying with full output:
    cl /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /I"%SRC%" /I"%SHARED_INC%" /Fe:MinimalTestFeeder.exe "%SRC%\MinimalTestFeeder.cpp" /link user32.lib setupapi.lib
    goto :fail
)
echo  OK

REM --- Build XInputCheck ---
echo [2/3] Building XInputCheck.exe ...
if not exist "%CLIENT_OUT%\gaym_client.lib" (
    call "%~dp0..\..\..\build_client.bat" Debug
    if %errorlevel% neq 0 goto :fail
)
cl /EHsc /D_CRT_SECURE_NO_WARNINGS /I"%SRC%" /Fe:XInputCheck.exe "%SRC%\XInputCheck.cpp" /link "%CLIENT_OUT%\gaym_client.lib" xinput.lib >nul 2>&1
if %errorlevel% neq 0 (
    echo  FAILED. Retrying with full output:
    cl /EHsc /D_CRT_SECURE_NO_WARNINGS /I"%SRC%" /Fe:XInputCheck.exe "%SRC%\XInputCheck.cpp" /link "%CLIENT_OUT%\gaym_client.lib" xinput.lib
    goto :fail
)
echo  OK

REM --- Build AutoVerify ---
echo [3/3] Building AutoVerify.exe ...
cl /EHsc /D_CRT_SECURE_NO_WARNINGS /I"%SRC%" /I"%SHARED_INC%" /Fe:AutoVerify.exe "%SRC%\AutoVerify.cpp" /link xinput.lib setupapi.lib hid.lib >nul 2>&1
if %errorlevel% neq 0 (
    echo  FAILED. Retrying with full output:
    cl /EHsc /D_CRT_SECURE_NO_WARNINGS /I"%SRC%" /I"%SHARED_INC%" /Fe:AutoVerify.exe "%SRC%\AutoVerify.cpp" /link xinput.lib setupapi.lib hid.lib
    goto :fail
)
echo  OK
echo.

echo ============================================
echo  Build complete. Choose test mode:
echo ============================================
echo.
echo   1  Scripted test  (auto 30s, no keyboard needed)
echo   2  Keyboard mode  (WASD/arrows/keys = gamepad)
echo   3  XInput monitor (just watch XInput, no injection)
echo   4  Scripted + XInput side-by-side (two windows)
echo   5  Automated verify (single process)
echo   6  Exit
echo.
set /p choice="Pick [1-6]: "

if "%choice%"=="1" goto :scripted
if "%choice%"=="2" goto :keyboard
if "%choice%"=="3" goto :xinput
if "%choice%"=="4" goto :sidebyside
if "%choice%"=="5" goto :autoverify
if "%choice%"=="6" goto :done

echo Invalid choice.
goto :done

:scripted
echo.
echo Starting scripted test (30 seconds)...
echo Watch joy.cpl or XInputCheck in another window.
echo.
MinimalTestFeeder.exe --scripted
goto :done

:keyboard
echo.
echo Starting keyboard mode (Ctrl+C to stop)...
echo.
MinimalTestFeeder.exe
goto :done

:xinput
echo.
echo Polling XInput continuously until any key is pressed...
echo.
XInputCheck.exe --continuous
goto :done

:sidebyside
echo.
echo Launching XInputCheck in a new window...
start "XInput Monitor" cmd /c "XInputCheck.exe --continuous"
timeout /t 2 /nobreak >nul
echo Launching MinimalTestFeeder --scripted ...
echo.
MinimalTestFeeder.exe --scripted
goto :done

:autoverify
echo.
echo Running automated XInput verification...
echo.
AutoVerify.exe
goto :done

:fail
echo.
echo *** BUILD FAILED ***
echo Make sure you're in a VS Developer Command Prompt.
echo.
pause
exit /b 1

:done
echo.
echo Done.
pause
