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
set "OBJ_OUT=%OUT%\obj"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvarsall.bat failed. Is VS Build Tools installed?
    exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ_OUT%" mkdir "%OBJ_OUT%"
if not exist "%CLIENT_OUT%\gaym_client.lib" (
    call "%~dp0build_client.bat" %CONFIGURATION%
    if errorlevel 1 exit /b 1
)

for %%F in (QuickVerify.exe QuickVerifyHid.exe XInputAutoVerify.exe) do (
    if exist "%OUT%\%%F" del /q "%OUT%\%%F"
)
for %%F in (QuickVerify.obj QuickVerifyHid.obj XInputAutoVerify.obj) do (
    if exist "%OUT%\%%F" del /q "%OUT%\%%F"
)
del /q "%OBJ_OUT%\*.obj" 2>nul

echo.
echo === Compiler Info ===
cl 2>&1 | findstr /i "Compiler Version"
echo.

pushd "%OUT%"

if /I "%CONFIGURATION%"=="Release" (
    set "TOOL_FLAGS=/O2 /DNDEBUG /WX"
) else (
    set "TOOL_FLAGS=/Od /DDEBUG /WX"
)

echo === Building GaYmCLI.exe (%CONFIGURATION%) ===
cl.exe /nologo /c /EHsc /std:c++17 /W4 %TOOL_FLAGS% /I"%SRC%" /I"%SHARED_INC%" /I"%CLIENT_SRC%" /Fo"%OBJ_OUT%\\" "%SRC%\GaYmCLI.cpp"
if errorlevel 1 (
    echo.
    echo FAILED: GaYmCLI.cpp compile
    popd
    exit /b 1
)
link.exe /nologo /OUT:GaYmCLI.exe "%OBJ_OUT%\GaYmCLI.obj" "%CLIENT_OUT%\gaym_client.lib" setupapi.lib
if errorlevel 1 (
    echo.
    echo FAILED: GaYmCLI.exe
    popd
    exit /b 1
)
echo OK: GaYmCLI.exe
echo.

echo === Building GaYmFeeder.exe (%CONFIGURATION%) ===
cl.exe /nologo /c /EHsc /std:c++17 /W4 %TOOL_FLAGS% /I"%SRC%" /I"%SHARED_INC%" /I"%CLIENT_SRC%" /Fo"%OBJ_OUT%\\" ^
    "%SRC%\main.cpp" ^
    "%SRC%\Config.cpp" ^
    "%SRC%\KeyboardProvider.cpp" ^
    "%SRC%\MouseProvider.cpp" ^
    "%SRC%\NetworkProvider.cpp" ^
    "%SRC%\MacroProvider.cpp"
if errorlevel 1 (
    echo.
    echo FAILED: GaYmFeeder compile
    popd
    exit /b 1
)
link.exe /nologo /OUT:GaYmFeeder.exe ^
    "%OBJ_OUT%\main.obj" ^
    "%OBJ_OUT%\Config.obj" ^
    "%OBJ_OUT%\KeyboardProvider.obj" ^
    "%OBJ_OUT%\MouseProvider.obj" ^
    "%OBJ_OUT%\NetworkProvider.obj" ^
    "%OBJ_OUT%\MacroProvider.obj" ^
    "%CLIENT_OUT%\gaym_client.lib" setupapi.lib ws2_32.lib user32.lib
if errorlevel 1 (
    echo.
    echo FAILED: GaYmFeeder.exe
    popd
    exit /b 1
)
echo OK: GaYmFeeder.exe
echo.

echo === Building MinimalTestFeeder.exe (%CONFIGURATION%) ===
cl.exe /nologo /c /EHsc /std:c++17 /W4 %TOOL_FLAGS% /I"%SRC%" /I"%SHARED_INC%" /I"%CLIENT_SRC%" /Fo"%OBJ_OUT%\\" ^
    "%SRC%\MinimalTestFeeder.cpp" ^
    "%SRC%\KeyboardProvider.cpp"
if errorlevel 1 (
    echo.
    echo FAILED: MinimalTestFeeder compile
    popd
    exit /b 1
)
link.exe /nologo /OUT:MinimalTestFeeder.exe ^
    "%OBJ_OUT%\MinimalTestFeeder.obj" ^
    "%OBJ_OUT%\KeyboardProvider.obj" ^
    "%CLIENT_OUT%\gaym_client.lib" setupapi.lib user32.lib
if errorlevel 1 (
    echo.
    echo FAILED: MinimalTestFeeder.exe
    popd
    exit /b 1
)
echo OK: MinimalTestFeeder.exe
echo.

echo === Building AutoVerify.exe (%CONFIGURATION%) ===
cl.exe /nologo /c /EHsc /std:c++17 /W4 %TOOL_FLAGS% /I"%SRC%" /I"%SHARED_INC%" /I"%CLIENT_SRC%" /Fo"%OBJ_OUT%\\" "%SRC%\AutoVerify.cpp"
if errorlevel 1 (
    echo.
    echo FAILED: AutoVerify.cpp compile
    popd
    exit /b 1
)
link.exe /nologo /OUT:AutoVerify.exe "%OBJ_OUT%\AutoVerify.obj" "%CLIENT_OUT%\gaym_client.lib" setupapi.lib xinput.lib hid.lib
if errorlevel 1 (
    echo.
    echo FAILED: AutoVerify.exe
    popd
    exit /b 1
)
echo OK: AutoVerify.exe
echo.

echo === Building XInputCheck.exe (%CONFIGURATION%) ===
cl.exe /nologo /c /EHsc /std:c++17 /W4 %TOOL_FLAGS% /I"%SRC%" /Fo"%OBJ_OUT%\\" "%SRC%\XInputCheck.cpp"
if errorlevel 1 (
    echo.
    echo FAILED: XInputCheck.cpp compile
    popd
    exit /b 1
)
link.exe /nologo /OUT:XInputCheck.exe "%OBJ_OUT%\XInputCheck.obj" "%CLIENT_OUT%\gaym_client.lib" xinput.lib
if errorlevel 1 (
    echo.
    echo FAILED: XInputCheck.exe
    popd
    exit /b 1
)
echo OK: XInputCheck.exe
echo.

echo === Build complete ===
del /q "%OBJ_OUT%\*.obj" 2>nul
dir /b *.exe 2>nul
echo.

echo === Quick smoke test ===
GaYmCLI.exe status
popd
endlocal
