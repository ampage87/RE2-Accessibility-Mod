@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM Resident Evil 2 Accessibility Mod - build step
REM Called by deploy.ps1 (started from Deploy.vbs in the mod folder).
REM Builds Mod\build\RE2Access.asi with Visual Studio (x86).
REM If Visual Studio is missing or the build fails, the prebuilt
REM Mod\build\prebuilt\RE2Access.asi is used instead.
REM ============================================================

for %%I in ("%~dp0.") do set "SRC_DIR=%%~fI"
for %%I in ("%SRC_DIR%\..") do set "MOD_DIR=%%~fI"
set "BUILD_DIR=%MOD_DIR%\build"
set "OBJ_DIR=%BUILD_DIR%\obj"
set "OUT=%BUILD_DIR%\RE2Access.asi"
set "PREBUILT=%BUILD_DIR%\prebuilt\RE2Access.asi"

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%OBJ_DIR%" mkdir "%OBJ_DIR%"
del /q "%OBJ_DIR%\*.obj" 2>nul
del /q "%OUT%" 2>nul

echo Locating Visual Studio x86 build tools...
where /q cl.exe
if %errorlevel%==0 goto :build

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%P in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%P\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%%P\VC\Auxiliary\Build\vcvars32.bat"
    )
)
if not defined VCVARS (
    for %%E in (BuildTools Community Professional Enterprise) do (
        for %%Y in (2022 2019 18) do (
            for %%P in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
                if not defined VCVARS if exist "%%~P\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%%~P\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars32.bat"
            )
        )
    )
)
if not defined VCVARS (
    echo WARNING: Visual Studio C++ tools not found. Using the prebuilt mod.
    goto :prebuilt
)
echo Found: %VCVARS%
call "%VCVARS%" >nul 2>&1

:build
echo Building RE2Access.asi...
cl /nologo /utf-8 /std:c++17 /W3 /EHsc /O2 /MT /LD /DNDEBUG ^
    /I"%SRC_DIR%" ^
    /Fo"%OBJ_DIR%\\" ^
    /Fe"%OUT%" ^
    "%SRC_DIR%\win_main.cpp" ^
    "%SRC_DIR%\mod.cpp" ^
    "%SRC_DIR%\state.cpp" ^
    "%SRC_DIR%\room.cpp" ^
    "%SRC_DIR%\map.cpp" ^
    /link /NOLOGO ole32.lib oleaut32.lib user32.lib gdi32.lib winmm.lib
if errorlevel 1 (
    echo WARNING: The build failed. Using the prebuilt mod. The compiler messages are above.
    goto :prebuilt
)
if not exist "%OUT%" (
    echo WARNING: The build produced no file. Using the prebuilt mod.
    goto :prebuilt
)
del /q "%BUILD_DIR%\RE2Access.lib" "%BUILD_DIR%\RE2Access.exp" 2>nul
echo Build successful.
call :prism
exit /b 0

:prebuilt
if not exist "%PREBUILT%" (
    echo ERROR: No prebuilt mod at "%PREBUILT%".
    exit /b 1
)
copy /Y "%PREBUILT%" "%OUT%" >nul
if errorlevel 1 (
    echo ERROR: Could not copy the prebuilt mod.
    exit /b 1
)
echo Using prebuilt RE2Access.asi.
call :prism
exit /b 0

REM The screen reader library (Prism, 32-bit). Built once from source by
REM third_party\prism\build_prism.ps1; kept in third_party\prism\x86 after that. A failure
REM is only a warning: the mod then speaks with SAPI.
:prism
if exist "%MOD_DIR%\third_party\prism\x86\prism.dll" exit /b 0
powershell -NoProfile -ExecutionPolicy Bypass -File "%MOD_DIR%\third_party\prism\build_prism.ps1"
exit /b 0
