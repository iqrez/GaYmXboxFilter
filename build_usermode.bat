@echo off
setlocal
set "CONFIGURATION=%~1"
if "%CONFIGURATION%"=="" set "CONFIGURATION=Debug"
set "SRC=%~dp0src\tools\GaYmTestFeeder"
set "SHARED_INC=%~dp0src\shared"
set "CLIENT_SRC=%~dp0src\client"
if /I "%CONFIGURATION%"=="Release" (
    set "CLIENT_OUT=%~dp0out\release\client"
    set "OUT=%~dp0out\release\tools"
) else (
    set "CLIENT_OUT=%~dp0out\dev\client"
    set "OUT=%~dp0out\dev\tools"
)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvarsall.bat failed. Is VS Build Tools installed?
    exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%CLIENT_OUT%\gaym_client.lib" (
    call "%~dp0build_client.bat" %CONFIGURATION%
    if errorlevel 1 exit /b 1
)

echo.
echo === Compiler Info ===
cl 2>&1 | findstr /i "Compiler Version"
echo.

pushd "%OUT%"

if /I "%CONFIGURATION%"=="Release" (
    set "TOOL_FLAGS=/O2 /DNDEBUG"
) else (
    set "TOOL_FLAGS=/Od /DDEBUG"
)

echo === Building GaYmCLI.exe (%CONFIGURATION%) ===
cl.exe /nologo /EHsc /std:c++17 /W3 %TOOL_FLAGS% /I"%SRC%" /I"%SHARED_INC%" /I"%CLIENT_SRC%" "%SRC%\GaYmCLI.cpp" /Fe:GaYmCLI.exe /link "%CLIENT_OUT%\gaym_client.lib" setupapi.lib
if errorlevel 1 (
    echo.
    echo FAILED: GaYmCLI.exe
    popd
    exit /b 1
)
echo OK: GaYmCLI.exe
echo.

echo === Building GaYmFeeder.exe (%CONFIGURATION%) ===
cl.exe /nologo /EHsc /std:c++17 /W3 %TOOL_FLAGS% /I"%SRC%" /I"%SHARED_INC%" /I"%CLIENT_SRC%" ^
    "%SRC%\main.cpp" ^
    "%SRC%\Config.cpp" ^
    "%SRC%\KeyboardProvider.cpp" ^
    "%SRC%\MouseProvider.cpp" ^
    "%SRC%\NetworkProvider.cpp" ^
    "%SRC%\MacroProvider.cpp" ^
    /Fe:GaYmFeeder.exe /link "%CLIENT_OUT%\gaym_client.lib" setupapi.lib ws2_32.lib user32.lib
if errorlevel 1 (
    echo.
    echo FAILED: GaYmFeeder.exe
    popd
    exit /b 1
)
echo OK: GaYmFeeder.exe
echo.

echo === Building MinimalTestFeeder.exe (%CONFIGURATION%) ===
cl.exe /nologo /EHsc /std:c++17 /W3 %TOOL_FLAGS% /I"%SRC%" /I"%SHARED_INC%" /I"%CLIENT_SRC%" ^
    "%SRC%\MinimalTestFeeder.cpp" ^
    "%SRC%\KeyboardProvider.cpp" ^
    /Fe:MinimalTestFeeder.exe /link "%CLIENT_OUT%\gaym_client.lib" setupapi.lib user32.lib
if errorlevel 1 (
    echo.
    echo FAILED: MinimalTestFeeder.exe
    popd
    exit /b 1
)
echo OK: MinimalTestFeeder.exe
echo.

echo === Building AutoVerify.exe (%CONFIGURATION%) ===
cl.exe /nologo /EHsc /std:c++17 /W3 %TOOL_FLAGS% /I"%SRC%" /I"%SHARED_INC%" /I"%CLIENT_SRC%" "%SRC%\AutoVerify.cpp" /Fe:AutoVerify.exe /link "%CLIENT_OUT%\gaym_client.lib" setupapi.lib xinput.lib hid.lib
if errorlevel 1 (
    echo.
    echo FAILED: AutoVerify.exe
    popd
    exit /b 1
)
echo OK: AutoVerify.exe
echo.

echo === Build complete ===
dir /b *.exe 2>nul
echo.

echo === Quick smoke test ===
GaYmCLI.exe status
popd
endlocal
