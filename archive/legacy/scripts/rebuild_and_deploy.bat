@echo off
setlocal
cd /d "%~dp0"

set "WDK_BIN=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0"
set "INF2CAT=%WDK_BIN%\x86\Inf2Cat.exe"
set "SIGNTOOL=%WDK_BIN%\x64\signtool.exe"
set "DRIVER_DIR=%~dp0build\driver"
set "DRIVER_INF=%DRIVER_DIR%\GaYmFilter.inf"
set "DRIVER_SYS=%DRIVER_DIR%\GaYmFilter.sys"
set "DRIVER_CAT=%DRIVER_DIR%\GaYmFilter.cat"
set "ATTACH_PS1=%~dp0attach_filter.ps1"

echo ============================================
echo  GaYmFilter - Full Rebuild + Deploy
echo ============================================
echo.

net session >nul 2>&1
if errorlevel 1 (
    echo ERROR: Run this script from an elevated Administrator prompt.
    exit /b 1
)

if not exist "%INF2CAT%" (
    echo ERROR: Inf2Cat.exe not found at "%INF2CAT%"
    exit /b 1
)

if not exist "%SIGNTOOL%" (
    echo ERROR: signtool.exe not found at "%SIGNTOOL%"
    exit /b 1
)

if not exist "%ATTACH_PS1%" (
    echo ERROR: attach_filter.ps1 not found at "%ATTACH_PS1%"
    exit /b 1
)

echo [1/6] Building driver...
call "%~dp0build_driver.bat"
if errorlevel 1 (
    echo FAILED: Driver build failed.
    exit /b 1
)
echo.

echo [2/6] Generating catalog...
"%INF2CAT%" /driver:"%DRIVER_DIR%" /os:10_X64
if errorlevel 1 (
    echo FAILED: Inf2Cat failed.
    exit /b 1
)
echo.

echo [3/6] Signing GaYmFilter.sys...
"%SIGNTOOL%" sign /a /s PrivateCertStore /n "GaYmFilter Test" /fd sha256 /t http://timestamp.digicert.com "%DRIVER_SYS%"
if errorlevel 1 (
    echo FAILED: signtool sign .sys failed.
    exit /b 1
)
echo.

echo [4/6] Signing GaYmFilter.cat...
"%SIGNTOOL%" sign /a /s PrivateCertStore /n "GaYmFilter Test" /fd sha256 /t http://timestamp.digicert.com "%DRIVER_CAT%"
if errorlevel 1 (
    echo FAILED: signtool sign .cat failed.
    exit /b 1
)
echo.

echo [5/6] Installing and verifying filter attachment...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ATTACH_PS1%" -DriverInf "%DRIVER_INF%"
if errorlevel 1 (
    echo FAILED: attach_filter.ps1 reported an installation or stack verification error.
    exit /b 1
)
echo.

echo [6/6] Building user-mode tools and checking status...
call "%~dp0build_usermode.bat"
if errorlevel 1 (
    echo FAILED: User-mode build failed.
    exit /b 1
)
echo.

echo ============================================
echo  Deploy complete.
echo  Driver package: %DRIVER_INF%
echo  Verify runtime with:
echo    %~dp0GaYmTestFeeder\GaYmCLI.exe status
echo    %~dp0GaYmTestFeeder\MinimalTestFeeder.exe --scripted
echo ============================================
echo.
exit /b 0
