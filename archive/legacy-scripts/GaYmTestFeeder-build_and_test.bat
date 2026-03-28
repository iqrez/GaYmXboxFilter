@echo off
setlocal
title GaYmFilter Build + Test
cd /d "%~dp0"

echo ============================================
echo  GaYmFilter Test Feeder - Build ^& Test
echo ============================================
echo.

REM --- Build MinimalTestFeeder ---
echo [1/4] Building MinimalTestFeeder.exe ...
cl /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /Fe:MinimalTestFeeder.exe MinimalTestFeeder.cpp /I..\GaYmFilter /link user32.lib setupapi.lib >nul 2>&1
if %errorlevel% neq 0 (
    echo  FAILED. Retrying with full output:
    cl /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /Fe:MinimalTestFeeder.exe MinimalTestFeeder.cpp /I..\GaYmFilter /link user32.lib setupapi.lib
    goto :fail
)
echo  OK

REM --- Build XInputCheck ---
echo [2/4] Building XInputCheck.exe ...
cl /EHsc /D_CRT_SECURE_NO_WARNINGS /Fe:XInputCheck.exe XInputCheck.cpp /link xinput.lib >nul 2>&1
if %errorlevel% neq 0 (
    echo  FAILED. Retrying with full output:
    cl /EHsc /D_CRT_SECURE_NO_WARNINGS /Fe:XInputCheck.exe XInputCheck.cpp /link xinput.lib
    goto :fail
)
echo  OK

REM --- Build AutoVerify ---
echo [3/4] Building AutoVerify.exe ...
cl /EHsc /D_CRT_SECURE_NO_WARNINGS /Fe:AutoVerify.exe AutoVerify.cpp /I..\GaYmFilter /link xinput.lib setupapi.lib hid.lib >nul 2>&1
if %errorlevel% neq 0 (
    echo  FAILED. Retrying with full output:
    cl /EHsc /D_CRT_SECURE_NO_WARNINGS /Fe:AutoVerify.exe AutoVerify.cpp /I..\GaYmFilter /link xinput.lib setupapi.lib hid.lib
    goto :fail
)
echo  OK

REM --- Build GaYmCLI ---
echo [4/4] Building GaYmCLI.exe ...
cl /nologo /EHsc /std:c++17 /W3 /I. GaYmCLI.cpp GuidDefinitions.cpp /Fe:GaYmCLI.exe /link setupapi.lib >nul 2>&1
if %errorlevel% neq 0 (
    echo  FAILED. Retrying with full output:
    cl /nologo /EHsc /std:c++17 /W3 /I. GaYmCLI.cpp GuidDefinitions.cpp /Fe:GaYmCLI.exe /link setupapi.lib
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
echo   6  Wake regression (sleep/resume workflow)
echo   7  Exit
echo.
set /p choice="Pick [1-7]: "

if "%choice%"=="1" goto :scripted
if "%choice%"=="2" goto :keyboard
if "%choice%"=="3" goto :xinput
if "%choice%"=="4" goto :sidebyside
if "%choice%"=="5" goto :autoverify
if "%choice%"=="6" goto :wakeregression
if "%choice%"=="7" goto :done

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
echo Polling XInput for 30 seconds...
echo.
XInputCheck.exe
goto :done

:sidebyside
echo.
echo Launching XInputCheck in a new window...
start "XInput Monitor" cmd /c "XInputCheck.exe & pause"
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

:wakeregression
echo.
echo Running wake regression workflow...
echo Sleep the machine when prompted, then return here after resume.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0RunWakeRegression.ps1"
if %errorlevel% neq 0 goto :fail
goto :done

:fail
echo.
echo *** BUILD OR TEST FAILED ***
echo Make sure you're in a VS Developer Command Prompt for builds.
echo Check the command output above for the exact failing step.
echo.
pause
exit /b 1

:done
echo.
echo Done.
pause
