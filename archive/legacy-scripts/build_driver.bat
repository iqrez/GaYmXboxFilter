@echo off
setlocal
cd /d "%~dp0"

set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
set "PROJECT=%~dp0GaYmFilter.vcxproj"
set "CONFIG=Debug"
set "PLATFORM=x64"
set "OUT_BIN_DIR=%~dp0x64\%CONFIG%\GaYmFilter"
set "PACKAGE_DIR=%OUT_BIN_DIR%\GaYmFilter"
set "OUT_DIR=%~dp0build\driver"

if not exist "%MSBUILD%" (
    echo ERROR: MSBuild not found at "%MSBUILD%"
    exit /b 1
)

if not exist "%PROJECT%" (
    echo ERROR: Project not found at "%PROJECT%"
    exit /b 1
)

echo.
echo === Building GaYmFilter.sys (KMDF %PLATFORM% %CONFIG%) ===
echo.

"%MSBUILD%" "%PROJECT%" /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /t:Rebuild
if errorlevel 1 (
    echo.
    echo === BUILD FAILED ===
    exit /b 1
)

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

copy /Y "%OUT_BIN_DIR%\GaYmFilter.sys" "%OUT_DIR%\GaYmFilter.sys" >nul
copy /Y "%OUT_BIN_DIR%\GaYmFilter.inf" "%OUT_DIR%\GaYmFilter.inf" >nul
copy /Y "%PACKAGE_DIR%\gaymfilter.cat" "%OUT_DIR%\GaYmFilter.cat" >nul
copy /Y "%OUT_BIN_DIR%\GaYmFilter.cer" "%OUT_DIR%\GaYmFilter.cer" >nul

echo.
echo === BUILD SUCCEEDED ===
echo Driver:  %OUT_DIR%\GaYmFilter.sys
echo Package: %OUT_DIR%\GaYmFilter.inf
echo Catalog: %OUT_DIR%\GaYmFilter.cat
echo.
exit /b 0
