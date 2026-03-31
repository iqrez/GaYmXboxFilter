@echo off
setlocal
set "CONFIGURATION=%~1"
if "%CONFIGURATION%"=="" set "CONFIGURATION=Debug"
set "SRC=%~dp0src\client"
set "SHARED_INC=%~dp0src\shared"
if /I "%CONFIGURATION%"=="Release" (
    set "OUT=%~dp0out\release\client"
) else (
    set "OUT=%~dp0out\dev\client"
)
set "OBJ=%OUT%\obj"

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvarsall.bat failed. Is VS Build Tools installed?
    exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"

pushd "%OUT%"

if /I "%CONFIGURATION%"=="Release" (
    set "CLIENT_FLAGS=/O2 /DNDEBUG"
) else (
    set "CLIENT_FLAGS=/Od /DDEBUG"
)

echo === Building gaym_client.lib (%CONFIGURATION%) ===
cl.exe /nologo /c /TC /W4 %CLIENT_FLAGS% /I"%SRC%" /I"%SHARED_INC%" /Fo"%OBJ%\\" ^
    "%SRC%\gaym_client.c" ^
    "%SRC%\gaym_client_session.c" ^
    "%SRC%\gaym_client_observation.c" ^
    "%SRC%\gaym_client_diag.c"
if errorlevel 1 (
    echo.
    echo FAILED: gaym_client.obj build
    popd
    exit /b 1
)

lib.exe /nologo /OUT:gaym_client.lib "%OBJ%\gaym_client.obj" "%OBJ%\gaym_client_session.obj" "%OBJ%\gaym_client_observation.obj" "%OBJ%\gaym_client_diag.obj"
if errorlevel 1 (
    echo.
    echo FAILED: gaym_client.lib
    popd
    exit /b 1
)

echo OK: gaym_client.lib
dir /b gaym_client.lib
popd
endlocal
