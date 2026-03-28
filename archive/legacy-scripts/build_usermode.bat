@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvarsall.bat failed. Is VS Build Tools installed?
    exit /b 1
)

echo.
echo === Compiler Info ===
cl 2>&1 | findstr /i "Compiler Version"
echo.

pushd "%~dp0GaYmTestFeeder"

echo === Building GaYmCLI.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. GaYmCLI.cpp GuidDefinitions.cpp /Fe:GaYmCLI.exe /link setupapi.lib
if errorlevel 1 (
    echo.
    echo FAILED: GaYmCLI.exe
    popd
    exit /b 1
)
echo OK: GaYmCLI.exe
echo.

echo === Building GaYmFeeder.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. main.cpp GuidDefinitions.cpp Config.cpp KeyboardProvider.cpp MouseProvider.cpp NetworkProvider.cpp MacroProvider.cpp /Fe:GaYmFeeder.exe /link setupapi.lib ws2_32.lib user32.lib
if errorlevel 1 (
    echo.
    echo FAILED: GaYmFeeder.exe
    popd
    exit /b 1
)
echo OK: GaYmFeeder.exe
echo.

echo === Building MinimalTestFeeder.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I.. MinimalTestFeeder.cpp KeyboardProvider.cpp /Fe:MinimalTestFeeder.exe /link setupapi.lib user32.lib
if errorlevel 1 (
    echo.
    echo FAILED: MinimalTestFeeder.exe
    popd
    exit /b 1
)
echo OK: MinimalTestFeeder.exe
echo.

echo === Building AutoVerify.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I.. AutoVerify.cpp GuidDefinitions.cpp /Fe:AutoVerify.exe /link setupapi.lib xinput.lib hid.lib
if errorlevel 1 (
    echo.
    echo FAILED: AutoVerify.exe
    popd
    exit /b 1
)
echo OK: AutoVerify.exe
echo.

echo === Building FeederAutoVerify.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I.. FeederAutoVerify.cpp GuidDefinitions.cpp /Fe:FeederAutoVerify.exe /link setupapi.lib xinput.lib
if errorlevel 1 (
    echo.
    echo FAILED: FeederAutoVerify.exe
    popd
    exit /b 1
)
echo OK: FeederAutoVerify.exe
echo.

echo === Building KeyboardFeederAutoVerify.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I.. KeyboardFeederAutoVerify.cpp GuidDefinitions.cpp /Fe:KeyboardFeederAutoVerify.exe /link setupapi.lib xinput.lib user32.lib
if errorlevel 1 (
    echo.
    echo FAILED: KeyboardFeederAutoVerify.exe
    popd
    exit /b 1
)
echo OK: KeyboardFeederAutoVerify.exe
echo.

echo === Building JoyAutoVerify.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I.. JoyAutoVerify.cpp GuidDefinitions.cpp /Fe:JoyAutoVerify.exe /link setupapi.lib winmm.lib
if errorlevel 1 (
    echo.
    echo FAILED: JoyAutoVerify.exe
    popd
    exit /b 1
)
echo OK: JoyAutoVerify.exe
echo.

echo === Building HybridAutoVerify.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I.. HybridAutoVerify.cpp GuidDefinitions.cpp /Fe:HybridAutoVerify.exe /link setupapi.lib winmm.lib xinput.lib
if errorlevel 1 (
    echo.
    echo FAILED: HybridAutoVerify.exe
    popd
    exit /b 1
)
echo OK: HybridAutoVerify.exe
echo.

echo === Building QuickVerify.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I.. QuickVerify.cpp GuidDefinitions.cpp /Fe:QuickVerify.exe /link setupapi.lib xinput.lib
if errorlevel 1 (
    echo.
    echo FAILED: QuickVerify.exe
    popd
    exit /b 1
)
echo OK: QuickVerify.exe
echo.

echo === Building QuickVerifyHid.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I.. QuickVerifyHid.cpp GuidDefinitions.cpp /Fe:QuickVerifyHid.exe /link setupapi.lib hid.lib
if errorlevel 1 (
    echo.
    echo FAILED: QuickVerifyHid.exe
    popd
    exit /b 1
)
echo OK: QuickVerifyHid.exe
echo.

echo === Building JoySniffer.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I.. JoySniffer.cpp /Fe:JoySniffer.exe /link winmm.lib
if errorlevel 1 (
    echo.
    echo FAILED: JoySniffer.exe
    popd
    exit /b 1
)
echo OK: JoySniffer.exe
echo.

echo === Building DirectInputAutoVerify.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I.. DirectInputAutoVerify.cpp GuidDefinitions.cpp /Fe:DirectInputAutoVerify.exe /link setupapi.lib dinput8.lib dxguid.lib ole32.lib user32.lib
if errorlevel 1 (
    echo.
    echo FAILED: DirectInputAutoVerify.exe
    popd
    exit /b 1
)
echo OK: DirectInputAutoVerify.exe
echo.

echo === Building DirectInputSniffer.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I.. DirectInputSniffer.cpp /Fe:DirectInputSniffer.exe /link dinput8.lib dxguid.lib ole32.lib user32.lib
if errorlevel 1 (
    echo.
    echo FAILED: DirectInputSniffer.exe
    popd
    exit /b 1
)
echo OK: DirectInputSniffer.exe
echo.

echo === Building TraceSniffer.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I.. TraceSniffer.cpp GuidDefinitions.cpp /Fe:TraceSniffer.exe /link setupapi.lib
if errorlevel 1 (
    echo.
    echo FAILED: TraceSniffer.exe
    popd
    exit /b 1
)
echo OK: TraceSniffer.exe
echo.

echo === Building SemanticSniffer.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I.. SemanticSniffer.cpp GuidDefinitions.cpp /Fe:SemanticSniffer.exe /link setupapi.lib
if errorlevel 1 (
    echo.
    echo FAILED: SemanticSniffer.exe
    popd
    exit /b 1
)
echo OK: SemanticSniffer.exe
echo.

echo === Building RawHidSniffer.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I.. RawHidSniffer.cpp /Fe:RawHidSniffer.exe /link setupapi.lib hid.lib
if errorlevel 1 (
    echo.
    echo FAILED: RawHidSniffer.exe
    popd
    exit /b 1
)
echo OK: RawHidSniffer.exe
echo.

echo === Building XInputMonitor.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I.. XInputMonitor.cpp /Fe:XInputMonitor.exe /link user32.lib gdi32.lib xinput.lib /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo.
    echo FAILED: XInputMonitor.exe
    popd
    exit /b 1
)
echo OK: XInputMonitor.exe
echo.

echo === Build complete ===
dir /b *.exe 2>nul
echo.

echo === Quick smoke test ===
GaYmCLI.exe status
popd
endlocal
