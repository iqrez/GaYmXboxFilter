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
cl.exe /nologo /EHsc /std:c++17 /W3 /I. GaYmCLI.cpp /Fe:GaYmCLI.exe /link setupapi.lib
if errorlevel 1 (
    echo.
    echo FAILED: GaYmCLI.exe
    popd
    exit /b 1
)
echo OK: GaYmCLI.exe
echo.

echo === Building GaYmFeeder.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. main.cpp Config.cpp KeyboardProvider.cpp MouseProvider.cpp NetworkProvider.cpp MacroProvider.cpp /Fe:GaYmFeeder.exe /link setupapi.lib ws2_32.lib user32.lib
if errorlevel 1 (
    echo.
    echo FAILED: GaYmFeeder.exe
    popd
    exit /b 1
)
echo OK: GaYmFeeder.exe
echo.

echo === Building MinimalTestFeeder.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I..\GaYmFilter MinimalTestFeeder.cpp KeyboardProvider.cpp /Fe:MinimalTestFeeder.exe /link setupapi.lib user32.lib
if errorlevel 1 (
    echo.
    echo FAILED: MinimalTestFeeder.exe
    popd
    exit /b 1
)
echo OK: MinimalTestFeeder.exe
echo.

echo === Building AutoVerify.exe ===
cl.exe /nologo /EHsc /std:c++17 /W3 /I. /I..\GaYmFilter AutoVerify.cpp /Fe:AutoVerify.exe /link setupapi.lib xinput.lib hid.lib
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
