@echo off
setlocal

set "CONFIGURATION=%~1"
if "%CONFIGURATION%"=="" set "CONFIGURATION=Debug"
set "VS_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
set "WDK=C:\Program Files (x86)\Windows Kits\10"
set "WDKVER=10.0.26100.0"
set "KMDFVER=1.33"
set "UPPER_BATCH=%~dp0build_upper.bat"
set "SRC=%~dp0src\lower"
if /I "%CONFIGURATION%"=="Release" (
    set "OUT=%~dp0out\release\driver"
    set "LEGACY_OUT=%~dp0out\release\lower-driver"
    set "DRIVER_FLAGS=/O2 /DNDEBUG /WX"
) else (
    set "OUT=%~dp0out\dev\driver"
    set "LEGACY_OUT=%~dp0out\dev\lower-driver"
    set "DRIVER_FLAGS=/Od /DDEBUG /WX"
)
set "OBJ_OUT=%OUT%\obj"
set "KM_INC=%WDK%\Include\%WDKVER%\km"
set "SHARED_INC=%WDK%\Include\%WDKVER%\shared"
set "WDF_INC=%WDK%\Include\wdf\kmdf\%KMDFVER%"
set "SRC_INC=%SRC%\include"
set "LOCAL_SHARED=%~dp0src\shared"
set "EXTERNAL_FLAGS=/experimental:external /external:anglebrackets /external:W0"
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
if not exist "%OBJ_OUT%" mkdir "%OBJ_OUT%"
if exist "%OUT%\gaymfilter.cat" del /q "%OUT%\gaymfilter.cat"
if exist "%LEGACY_OUT%" rmdir /s /q "%LEGACY_OUT%"
for %%F in (GaYmFilter.pdb GaYmFilter.map) do (
    if exist "%OUT%\%%F" del /q "%OUT%\%%F"
)

echo.
echo === Building GaYmFilter.sys (KMDF x64 %CONFIGURATION%) ===
echo    WDK: %WDKVER%   KMDF: %KMDFVER%
echo.

echo [1/4] Compiling driver.c ...
cl.exe /nologo /c /Zi /W4 %DRIVER_FLAGS% ^
    /D _WIN64 /D _AMD64_ /D AMD64 ^
    /D NTDDI_VERSION=0x0A000000 /D _WIN32_WINNT=0x0A00 ^
    /D DEPRECATE_DDK_FUNCTIONS=1 ^
    /D KMDF_VERSION_MAJOR=1 /D KMDF_VERSION_MINOR=33 ^
    /Zp8 /kernel /GS- /Gy /Fd"%OBJ_OUT%\GaYmFilter.pdb" ^
    %EXTERNAL_FLAGS% ^
    /I"%KM_INC%" /I"%SHARED_INC%" /I"%WDF_INC%" /I"%SRC_INC%" /I"%LOCAL_SHARED%" ^
    /Fo"%OBJ_OUT%\driver.obj" "%SRC%\driver.c"
if errorlevel 1 goto :fail

echo [2/4] Compiling device.c ...
cl.exe /nologo /c /Zi /W4 %DRIVER_FLAGS% ^
    /D _WIN64 /D _AMD64_ /D AMD64 ^
    /D NTDDI_VERSION=0x0A000000 /D _WIN32_WINNT=0x0A00 ^
    /D DEPRECATE_DDK_FUNCTIONS=1 ^
    /D KMDF_VERSION_MAJOR=1 /D KMDF_VERSION_MINOR=33 ^
    /Zp8 /kernel /GS- /Gy /Fd"%OBJ_OUT%\GaYmFilter.pdb" ^
    %EXTERNAL_FLAGS% ^
    /I"%KM_INC%" /I"%SHARED_INC%" /I"%WDF_INC%" /I"%SRC_INC%" /I"%LOCAL_SHARED%" ^
    /Fo"%OBJ_OUT%\device.obj" "%SRC%\device.c"
if errorlevel 1 goto :fail

echo [3/4] Compiling devices.c ...
cl.exe /nologo /c /Zi /W4 %DRIVER_FLAGS% ^
    /D _WIN64 /D _AMD64_ /D AMD64 ^
    /D NTDDI_VERSION=0x0A000000 /D _WIN32_WINNT=0x0A00 ^
    /D DEPRECATE_DDK_FUNCTIONS=1 ^
    /D KMDF_VERSION_MAJOR=1 /D KMDF_VERSION_MINOR=33 ^
    /Zp8 /kernel /GS- /Gy /Fd"%OBJ_OUT%\GaYmFilter.pdb" ^
    %EXTERNAL_FLAGS% ^
    /I"%KM_INC%" /I"%SHARED_INC%" /I"%WDF_INC%" /I"%SRC_INC%" /I"%LOCAL_SHARED%" ^
    /Fo"%OBJ_OUT%\devices.obj" "%SRC%\devices.c"
if errorlevel 1 goto :fail

echo [4/4] Linking GaYmFilter.sys ...
link.exe /nologo ^
    /DRIVER:WDM /SUBSYSTEM:NATIVE,10.00 /ENTRY:FxDriverEntry ^
    /MACHINE:X64 /DEBUG /NODEFAULTLIB ^
    /OUT:"%OUT%\GaYmFilter.sys" ^
    /PDB:"%OBJ_OUT%\GaYmFilter.pdb" ^
    /MAP:"%OBJ_OUT%\GaYmFilter.map" ^
    "%OBJ_OUT%\driver.obj" "%OBJ_OUT%\device.obj" "%OBJ_OUT%\devices.obj" ^
    "%KM_LIB%\ntoskrnl.lib" ^
    "%KM_LIB%\hal.lib" ^
    "%KM_LIB%\ntstrsafe.lib" ^
    "%KM_LIB%\bufferoverflowfastfailk.lib" ^
    "%WDF_LIB%\wdfldr.lib" ^
    "%WDF_LIB%\wdfdriverentry.lib"
if errorlevel 1 goto :fail

copy /Y "%SRC%\GaYmFilter.inf" "%OUT%\GaYmFilter.inf" >nul
if errorlevel 1 goto :fail

if exist "%UPPER_BATCH%" (
    echo.
    echo === Building Upper Driver Package ===
    call "%UPPER_BATCH%" "%CONFIGURATION%"
    if errorlevel 1 goto :fail
)

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
