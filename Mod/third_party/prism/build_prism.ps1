# ============================================================
# Builds the 32-bit prism.dll the mod uses to talk to screen readers.
# Prism (https://github.com/ethindp/prism, MPL-2.0) does not publish a 32-bit Windows build,
# and Resident Evil 2 is a 32-bit game, so it is built here from the release source with the
# same Visual Studio tools that build the mod. Called by deploy.bat when x86\prism.dll is
# missing; run it by hand (powershell -ExecutionPolicy Bypass -File build_prism.ps1) to rebuild.
#
# Steps: download the release source (third-party code is bundled in it), apply two local
# changes (below), configure with CMake for Win32, Release, static C runtime (no VC++
# redistributable needed), optional parts off, build, copy prism.dll and its licences here.
#
# Local changes to Prism's source:
#   1. source\backend.h: every backend's initialize() runs inside a structured-exception guard,
#      so a screen reader's DLL that faults while starting is reported as unavailable instead
#      of closing the game (the same change the KOTOR mod's Prism build carries).
#   2. cmake\PrismOptions.cmake: C standard 17 instead of 23 (some MSVC/CMake pairs reject C23;
#      Prism's C code needs only C17).
#   3. source\atlbase.h: a small stand-in for ATL's header (atl_shim\atlbase.h here), so the
#      build works with Visual Studio Build Tools that have no ATL component installed.
# Lines starting "PRISM:" are shown in Deploy's progress window.
# ============================================================
param([string]$Version = "0.18.4")
$ErrorActionPreference = "Stop"
$here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir  = Join-Path $here "x86"
$work    = Join-Path $env:LOCALAPPDATA "RE2Access\prism-build"
$srcZip  = Join-Path $work "prism-$Version.zip"
$srcDir  = Join-Path $work "prism-$Version"
$bldDir  = Join-Path $work "build-x86"

function Say($t) { Write-Output "PRISM: $t" }
function Fail($t) { Write-Output "WARNING: $t The mod will use SAPI speech only."; exit 1 }

try {
    New-Item -ItemType Directory -Force -Path $work, $outDir | Out-Null
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

    # ---- source
    if (-not (Test-Path -LiteralPath (Join-Path $srcDir "CMakeLists.txt"))) {
        Say "Downloading the Prism $Version source..."
        Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/ethindp/prism/archive/refs/tags/v$Version.zip" -OutFile $srcZip
        if (Test-Path -LiteralPath $srcDir) { Remove-Item -Recurse -Force -LiteralPath $srcDir }
        Expand-Archive -LiteralPath $srcZip -DestinationPath $work -Force
        $unpacked = Join-Path $work "prism-$Version"
        if (-not (Test-Path -LiteralPath (Join-Path $unpacked "CMakeLists.txt"))) { Fail "The Prism source archive was not as expected." }
    }

    # ---- local changes
    $opt = Join-Path $srcDir "cmake\PrismOptions.cmake"
    $t = [IO.File]::ReadAllText($opt)
    if ($t -match 'set\(CMAKE_C_STANDARD 23\)') {
        [IO.File]::WriteAllText($opt, $t.Replace('set(CMAKE_C_STANDARD 23)', "# RE2 mod: C17, see build_prism.ps1`r`nset(CMAKE_C_STANDARD 17)"))
    }
    $bh = Join-Path $srcDir "source\backend.h"
    $t = [IO.File]::ReadAllText($bh)
    if ($t -notmatch 'guarded_initialize') {
        $anchor = 'static constexpr bool requires_explicit_selection = false;'
        if ($t.IndexOf($anchor) -lt 0 -or $t.IndexOf('const auto result = initialize();') -lt 0) { Fail "Prism's backend.h has changed; the crash guard could not be added." }
        $guard = @'
static constexpr bool requires_explicit_selection = false;
#ifdef _WIN32
  // RE2 accessibility mod, local change: a screen reader's DLL that faults inside
  // initialize() is reported as unavailable instead of taking the game down. Kept free of
  // objects that need unwinding (MSVC C2712); BackendResult<> is trivially destructible.
  static bool seh_initialize(TextToSpeechBackend *self, BackendResult<> *out) {
    __try {
      *out = self->initialize();
      return true;
    } __except (1) {
      return false;
    }
  }
  BackendResult<> guarded_initialize() {
    BackendResult<> r = std::unexpected(BackendError::BackendNotAvailable);
    if (!seh_initialize(this, &r))
      return std::unexpected(BackendError::BackendNotAvailable);
    return r;
  }
#else
  BackendResult<> guarded_initialize() { return initialize(); }
#endif
'@
        $t = $t.Replace($anchor, $guard).Replace('const auto result = initialize();', 'const auto result = guarded_initialize();')
        [IO.File]::WriteAllText($bh, $t)
    }

    Copy-Item -LiteralPath (Join-Path $here "atl_shim\atlbase.h") -Destination (Join-Path $srcDir "source\atlbase.h") -Force

    # ---- tools: Visual Studio (x86 C++), CMake, Ninja
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = ""
    if (Test-Path -LiteralPath $vswhere) {
        $vs = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    }
    if (-not $vs) { Fail "Visual Studio C++ build tools were not found." }
    $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars32.bat"
    $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    $ninja = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    if (-not (Test-Path -LiteralPath $cmake)) {
        $c = Get-Command cmake.exe -ErrorAction SilentlyContinue
        if ($c) { $cmake = $c.Source }
        else {
            Say "Downloading CMake (Visual Studio's CMake component is not installed)..."
            $cv = "3.31.6"
            $cz = Join-Path $work "cmake-$cv.zip"
            Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/Kitware/CMake/releases/download/v$cv/cmake-$cv-windows-x86_64.zip" -OutFile $cz
            Expand-Archive -LiteralPath $cz -DestinationPath $work -Force
            $cmake = Join-Path $work "cmake-$cv-windows-x86_64\bin\cmake.exe"
        }
    }
    $gen = @()
    if (Test-Path -LiteralPath $ninja) { $gen = @("-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=$ninja") }
    else {
        $major = (& $vswhere -latest -products * -property installationVersion | Select-Object -First 1).Split('.')[0]
        $names = @{ "16" = "Visual Studio 16 2019"; "17" = "Visual Studio 17 2022"; "18" = "Visual Studio 18 2026" }
        $gen = @("-G", $names[$major], "-A", "Win32")
    }

    # ---- configure and build (inside the x86 build environment)
    Say "Building the screen reader library (first time only; this takes several minutes)..."
    if (Test-Path -LiteralPath $bldDir) { Remove-Item -Recurse -Force -LiteralPath $bldDir }
    $flags = @(
        "-S", "`"$srcDir`"", "-B", "`"$bldDir`"") + ($gen | ForEach-Object { "`"$_`"" }) + @(
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
        "-DPRISM_ENABLE_TESTS=OFF", "-DPRISM_ENABLE_DEMOS=OFF", "-DPRISM_ENABLE_GDEXTENSION=OFF",
        "-DPRISM_ENABLE_SHIMS=OFF", "-DPRISM_ENABLE_LEGACY_BACKENDS=OFF", "-DPRISM_ENABLE_LINTING=OFF",
        "-DPRISM_ENABLE_GDK_SPEECH_BACKEND=OFF", "-DPRISM_DEPENDENCY_PROVIDER=BUNDLED")
    $cmd = Join-Path $work "build.cmd"
    $lines = @(
        "@echo off",
        "call `"$vcvars`" >nul",
        "`"$cmake`" $($flags -join ' ')",
        "if errorlevel 1 exit /b 1",
        "`"$cmake`" --build `"$bldDir`" --config Release",
        "if errorlevel 1 exit /b 1")
    [IO.File]::WriteAllLines($cmd, $lines)
    & cmd.exe /c "`"$cmd`"" 2>&1 | ForEach-Object { Write-Output $_ }
    if ($LASTEXITCODE -ne 0) { Fail "Building the screen reader library failed (compiler messages above)." }

    $dll = Get-ChildItem -LiteralPath $bldDir -Recurse -Filter "prism.dll" | Select-Object -First 1
    if (-not $dll) { Fail "The build produced no prism.dll." }
    Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $outDir "prism.dll") -Force
    Copy-Item -LiteralPath (Join-Path $srcDir "LICENSE") -Destination (Join-Path $here "LICENSE-Prism.txt") -Force
    if (Test-Path -LiteralPath (Join-Path $srcDir "NOTICE")) { Copy-Item -LiteralPath (Join-Path $srcDir "NOTICE") -Destination (Join-Path $here "NOTICE-Prism.txt") -Force }
    [IO.File]::WriteAllText((Join-Path $outDir "VERSION.txt"), "Prism $Version, 32-bit, built $(Get-Date -Format 'yyyy-MM-dd') by build_prism.ps1`r`n")

    # ---- check: a 32-bit DLL with no dependency on the VC++ runtime
    $dumpbin = Get-ChildItem -LiteralPath (Join-Path $vs "VC\Tools\MSVC") -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'Hostx86\\x86' } | Select-Object -First 1
    if ($dumpbin) {
        $deps = & $dumpbin.FullName /nologo /dependents (Join-Path $outDir "prism.dll") | Out-String
        if ($deps -match 'vcruntime|msvcp') { Write-Output "WARNING: prism.dll needs the Visual C++ runtime; players may need the redistributable." }
        $hdr = & $dumpbin.FullName /nologo /headers (Join-Path $outDir "prism.dll") | Out-String
        if ($hdr -notmatch '14C machine \(x86\)') { Fail "prism.dll is not a 32-bit build." }
    }
    Say "Screen reader library built."
    exit 0
} catch {
    Fail "Building the screen reader library stopped: $($_.Exception.Message)"
}
