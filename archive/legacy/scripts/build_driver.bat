@echo off
setlocal

set "VS_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
set "WDK=C:\Program Files (x86)\Windows Kits\10"
set "WDKVER=10.0.26100.0"
set "KMDFVER=1.33"
set "SRC=%~dp0GaYmFilter"
set "OUT=%~dp0build\driver"
set "KM_INC=%WDK%\Include\%WDKVER%\km"
set "SHARED_INC=%WDK%\Include\%WDKVER%\shared"
set "WDF_INC=%WDK%\Include\wdf\kmdf\%KMDFVER%"
set "SRC_INC=%SRC%"
set "KM_LIB=%WDK%\lib\%WDKVER%\km\x64"
set "WDF_LIB=%WDK%\lib\wdf\kmdf\x64\%KMDFVER%"

if not exist "%VS_VCVARS%" (
    echo ERROR: vcvarsall.bat not found at "%VS_VCVARS%"
    exit /b 1
)

call "%VS_VCVARS%" x64 >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvarsall.bat failed. Is Visual Studio Build Tools installed correctly?
    exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\gaymfilter.cat" del /q "%OUT%\gaymfilter.cat"

echo.
echo === Building GaYmFilter.sys (KMDF x64 Debug) ===
echo    WDK: %WDKVER%   KMDF: %KMDFVER%
echo.

echo [1/4] Compiling driver.c ...
cl.exe /nologo /c /Zi /W4 /WX- /Od ^
    /D _WIN64 /D _AMD64_ /D AMD64 ^
    /D NTDDI_VERSION=0x0A000000 /D _WIN32_WINNT=0x0A00 ^
    /D DEPRECATE_DDK_FUNCTIONS=1 ^
    /D KMDF_VERSION_MAJOR=1 /D KMDF_VERSION_MINOR=33 ^
    /Zp8 /kernel /GS- /Gy ^
    /I"%KM_INC%" /I"%SHARED_INC%" /I"%WDF_INC%" /I"%SRC_INC%" ^
    /Fo"%OUT%\driver.obj" "%SRC%\driver.c"
if errorlevel 1 goto :fail

echo [2/4] Compiling device.c ...
cl.exe /nologo /c /Zi /W4 /WX- /Od ^
    /D _WIN64 /D _AMD64_ /D AMD64 ^
    /D NTDDI_VERSION=0x0A000000 /D _WIN32_WINNT=0x0A00 ^
    /D DEPRECATE_DDK_FUNCTIONS=1 ^
    /D KMDF_VERSION_MAJOR=1 /D KMDF_VERSION_MINOR=33 ^
    /Zp8 /kernel /GS- /Gy ^
    /I"%KM_INC%" /I"%SHARED_INC%" /I"%WDF_INC%" /I"%SRC_INC%" ^
    /Fo"%OUT%\device.obj" "%SRC%\device.c"
if errorlevel 1 goto :fail

echo [3/4] Compiling devices.c ...
cl.exe /nologo /c /Zi /W4 /WX- /Od ^
    /D _WIN64 /D _AMD64_ /D AMD64 ^
    /D NTDDI_VERSION=0x0A000000 /D _WIN32_WINNT=0x0A00 ^
    /D DEPRECATE_DDK_FUNCTIONS=1 ^
    /D KMDF_VERSION_MAJOR=1 /D KMDF_VERSION_MINOR=33 ^
    /Zp8 /kernel /GS- /Gy ^
    /I"%KM_INC%" /I"%SHARED_INC%" /I"%WDF_INC%" /I"%SRC_INC%" ^
    /Fo"%OUT%\devices.obj" "%SRC%\devices.c"
if errorlevel 1 goto :fail

echo [4/4] Linking GaYmFilter.sys ...
link.exe /nologo ^
    /DRIVER:WDM /SUBSYSTEM:NATIVE,10.00 /ENTRY:FxDriverEntry ^
    /MACHINE:X64 /DEBUG /NODEFAULTLIB ^
    /OUT:"%OUT%\GaYmFilter.sys" ^
    /MAP:"%OUT%\GaYmFilter.map" ^
    "%OUT%\driver.obj" "%OUT%\device.obj" "%OUT%\devices.obj" ^
    "%KM_LIB%\ntoskrnl.lib" ^
    "%KM_LIB%\hal.lib" ^
    "%KM_LIB%\ntstrsafe.lib" ^
    "%KM_LIB%\bufferoverflowfastfailk.lib" ^
    "%WDF_LIB%\wdfldr.lib" ^
    "%WDF_LIB%\wdfdriverentry.lib"
if errorlevel 1 goto :fail

copy /Y "%SRC%\GaYmFilter.inf" "%OUT%\GaYmFilter.inf" >nul
if errorlevel 1 goto :fail

echo.
echo === BUILD SUCCEEDED ===
echo Output: %OUT%\GaYmFilter.sys
dir "%OUT%\GaYmFilter.sys"
echo INF copied to: %OUT%\GaYmFilter.inf
echo NOTE: Regenerate and re-sign GaYmFilter.cat after each rebuild.
echo.
goto :end

:fail
echo.
echo === BUILD FAILED ===
exit /b 1

:end
endlocal
