<# : batch launcher
@echo off
setlocal
title RE2 Sourcenext and Classic REbirth setup
set "RE2SETUP_SCRIPT=%~f0"
set "RE2SETUP_ARGS=%*"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$s = [IO.File]::ReadAllText($env:RE2SETUP_SCRIPT); & ([ScriptBlock]::Create($s))"
set "RC=%ERRORLEVEL%"
endlocal & exit /b %RC%
#>

# The lines above are a small Windows batch launcher. PowerShell treats them as a comment.

# ---------------------------------------------------------------------------------------
# Resident Evil 2 (1998, Steam app 4249110): convert to Sourcenext v1.1.0 + Classic REbirth
#
# What it does, start to finish, with no mouse or visual steps:
#   1. Finds the Steam install of Resident Evil 2 (1998).
#   2. Checks free disk space, write access, and that the game is closed.
#   3. Downloads the newest Mulderland "Resident Evil 2 Steam Enhancement Pack" from
#      GitHub (open-source NSIS installer, github.com/Mulderland/MulderLoad) and runs it
#      silently. That pack:
#        - copies the Japanese game files into a new "rebirth" folder
#        - updates them to the Sourcenext edition, v1.1.0
#        - installs Classic REbirth, the 4 GB memory patch and the save-crash fix
#        - installs the high-quality videos the Sourcenext executable needs
#        - installs the Ultimate ASI Loader (which our mod will use later), plus
#          HD graphics and audio packs
#        - downgrades the Steam executables to the GOG versions and adds MulderConfig
#      Every pop-up question from the pack is answered automatically (Yes, Retry or OK),
#      including hidden ones, so the player never has to find or answer a window.
#   4. Checks the result (Sourcenext bio2.exe, REbirth ddraw.dll, 4 GB patch, videos).
#   5. Makes it screen-reader friendly: turns off the Classic REbirth settings window at
#      start-up and makes the game start straight into English.
#   6. Adds "Resident Evil 2 Classic REbirth" shortcuts to the desktop and Start menu.
#   7. Writes "Game Install Info.txt" and a log next to this script.
#
# Options (add after the file name in a command prompt):
#   --check            only find the game and report what is installed; change nothing
#   --game "FOLDER"    use this game folder instead of searching Steam
#   --reinstall        run the pack again even if Classic REbirth is already installed
#   --no-shortcuts     do not create shortcuts
#   --no-pause         do not wait for Enter at the end
#
# Safe to run again. Steam's "Verify integrity of game files" restores the Steam
# executables; the "rebirth" folder is separate and is not touched by Steam.
# ---------------------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}

$AppId          = '4249110'
$SteamFolder    = '4249110_Biohazard2'
$PackAsset      = 'resident-evil-2-steam-enhancement-pack.exe'
$PackRepo       = 'Mulderland/MulderLoad'
$PackFallback   = 'https://github.com/Mulderland/MulderLoad/releases/download/26.10.16/resident-evil-2-steam-enhancement-pack.exe'
$PackFallbackSha = '19D8B25143E706C951596496384CC6F71CE7F81107BE59A93CB66D4C1B816597'
$NeedFreeGB     = 12
$GameProcesses  = @('bio2', '4249110_Launcher', 'RE2Launcher', 'BH2Launcher', 'LeonJ', 'ClaireJ', 'MulderConfig')

$ScriptPath = $env:RE2SETUP_SCRIPT
if (-not $ScriptPath) { $ScriptPath = $PSCommandPath }
$ScriptDir  = Split-Path -Parent $ScriptPath
$IsWin      = ($env:OS -eq 'Windows_NT')

# ---------- arguments ----------
$Opt = @{ check = $false; game = $null; reinstall = $false; noshortcuts = $false; nopause = $false; elevated = $false }
$argText = "$env:RE2SETUP_ARGS"
$tokens = @([regex]::Matches($argText, '"([^"]*)"|(\S+)') | ForEach-Object { if ($_.Groups[1].Success) { $_.Groups[1].Value } else { $_.Groups[2].Value } })
for ($i = 0; $i -lt $tokens.Count; $i++) {
    switch ($tokens[$i].ToLower()) {
        '--check'        { $Opt.check = $true }
        '--reinstall'    { $Opt.reinstall = $true }
        '--no-shortcuts' { $Opt.noshortcuts = $true }
        '--no-pause'     { $Opt.nopause = $true }
        '--elevated'     { $Opt.elevated = $true }
        '--game'         { $i++; if ($i -lt $tokens.Count) { $Opt.game = $tokens[$i] } }
        default {
            # A folder dropped onto the script arrives as a bare path.
            if (Test-Path -LiteralPath $tokens[$i] -PathType Container) { $Opt.game = $tokens[$i] }
            else { Write-Host "Ignoring unknown option: $($tokens[$i])" }
        }
    }
}

# ---------- output and log ----------
$LogDir = Join-Path $ScriptDir 'Logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$LogFile = Join-Path $LogDir ("sourcenext-setup-{0}.log" -f (Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'))

function Log([string]$m) { Add-Content -LiteralPath $LogFile -Value ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m) }
function Say([string]$m) { Write-Host $m; Log $m }
function Step([int]$n, [string]$m) { Write-Host ''; Say "Step $n of 7. $m" }

function Finish([int]$code, [string]$m) {
    Write-Host ''
    Say $m
    Say "The log is in the Logs folder next to this script: $LogFile"
    if ($IsWin) { try { if ($code -eq 0) { [console]::Beep(880, 200); [console]::Beep(1175, 300) } else { [console]::Beep(330, 600) } } catch {} }
    if (-not $Opt.nopause) {
        # Discard stray key presses so the window waits for a real Enter.
        try { $Host.UI.RawUI.FlushInputBuffer() } catch {}
        Write-Host ''; [void](Read-Host 'Press Enter to close')
    }
    exit $code
}

function Size-GB([double]$bytes) { '{0:N1} GB' -f ($bytes / 1GB) }

function Folder-Bytes([string]$p) {
    if (-not (Test-Path -LiteralPath $p)) { return 0 }
    $s = (Get-ChildItem -LiteralPath $p -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum
    if ($s) { return [double]$s } else { return 0 }
}

# ---------- Steam discovery ----------
function Test-GameFolder([string]$p) {
    if (-not $p) { return $false }
    return (Test-Path -LiteralPath (Join-Path $p 'japanese\Pl0')) -and (Test-Path -LiteralPath (Join-Path $p 'japanese\Common'))
}

function Get-SteamLibraries {
    $roots = New-Object System.Collections.Generic.List[string]
    $regs = @(
        @{ Path = 'HKCU:\Software\Valve\Steam'; Name = 'SteamPath' },
        @{ Path = 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam'; Name = 'InstallPath' },
        @{ Path = 'HKLM:\SOFTWARE\Valve\Steam'; Name = 'InstallPath' }
    )
    foreach ($r in $regs) {
        try { $v = (Get-ItemProperty -Path $r.Path -Name $r.Name -ErrorAction Stop).($r.Name); if ($v) { $roots.Add(($v -replace '/', '\')) } } catch {}
    }
    foreach ($d in @("${env:ProgramFiles(x86)}\Steam", "$env:ProgramFiles\Steam")) { if ($d -and (Test-Path -LiteralPath $d)) { $roots.Add($d) } }
    if ($env:RE2SETUP_TEST_STEAM) { $roots.Add($env:RE2SETUP_TEST_STEAM) }

    $libs = New-Object System.Collections.Generic.List[string]
    foreach ($root in ($roots | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $libs.Add($root)
        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libs.Add(($m.Groups[1].Value -replace '\\\\', '\'))
            }
        }
    }
    return $libs | Select-Object -Unique
}

function Find-Game {
    if ($Opt.game) { return $Opt.game }
    foreach ($lib in Get-SteamLibraries) {
        $installdir = $SteamFolder
        $acf = Join-Path $lib "steamapps\appmanifest_$AppId.acf"
        if (Test-Path -LiteralPath $acf) {
            $m = [regex]::Match((Get-Content -LiteralPath $acf -Raw), '"installdir"\s+"([^"]+)"')
            if ($m.Success) { $installdir = $m.Groups[1].Value }
        }
        $p = Join-Path $lib "steamapps\common\$installdir"
        if (Test-GameFolder $p) { return $p }
    }
    try {
        $u = Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App $AppId" -ErrorAction Stop
        if (Test-GameFolder $u.InstallLocation) { return $u.InstallLocation }
    } catch {}
    foreach ($drive in [IO.DriveInfo]::GetDrives()) {
        if ($drive.DriveType -ne 'Fixed' -or -not $drive.IsReady) { continue }
        foreach ($sub in @('SteamLibrary', 'Steam', 'Games\Steam', 'Games\SteamLibrary', 'Program Files (x86)\Steam', 'Program Files\Steam')) {
            $p = Join-Path $drive.RootDirectory.FullName "$sub\steamapps\common\$SteamFolder"
            if (Test-GameFolder $p) { return $p }
        }
    }
    return $null
}

# ---------- install state ----------
function Get-State([string]$game) {
    $rb = Join-Path $game 'rebirth'
    $exe = Join-Path $rb 'bio2.exe'
    $st = [ordered]@{
        Rebirth   = $rb
        Exe       = $exe
        HasExe    = Test-Path -LiteralPath $exe
        HasDll    = Test-Path -LiteralPath (Join-Path $rb 'ddraw.dll')
        HasConfig = Test-Path -LiteralPath (Join-Path $rb 'config.ini')
        HasAsi    = Test-Path -LiteralPath (Join-Path $rb 'dsound.dll')
        HasData   = (Test-Path -LiteralPath (Join-Path $rb 'Pl0')) -and (Test-Path -LiteralPath (Join-Path $rb 'Common'))
        VideoGB   = 0
        LAA       = $null
        Version   = ''
    }
    $vb = 0
    foreach ($z in @('Pl0\Zmovie', 'Pl1\Zmovie', 'Zmovie')) { $vb += Folder-Bytes (Join-Path $rb $z) }
    $st.VideoGB = [math]::Round($vb / 1GB, 2)
    if ($st.HasExe) {
        try { $st.LAA = Test-LargeAddressAware $exe } catch { $st.LAA = $null }
        # The Sourcenext exe has no version resource, so identify it by size and SHA-256.
        try { $st.Version = ("{0} bytes, SHA-256 {1}" -f (Get-Item -LiteralPath $exe).Length, (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash) } catch {}
    }
    $st.Complete = $st.HasExe -and $st.HasDll -and $st.HasData
    return New-Object psobject -Property $st
}

# ---------- PE helpers (4 GB "large address aware" flag) ----------
function Get-PeOffset([byte[]]$b) {
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    if ($b[0] -ne 0x4D -or $b[1] -ne 0x5A -or $b[$pe] -ne 0x50 -or $b[$pe + 1] -ne 0x45) { throw 'Not a Windows executable' }
    return $pe
}
function Test-LargeAddressAware([string]$path) {
    $b = [IO.File]::ReadAllBytes($path); $pe = Get-PeOffset $b
    return (([BitConverter]::ToUInt16($b, $pe + 22)) -band 0x20) -ne 0
}
function Set-LargeAddressAware([string]$path) {
    $b = [IO.File]::ReadAllBytes($path); $pe = Get-PeOffset $b
    $ch = [BitConverter]::ToUInt16($b, $pe + 22) -bor 0x20
    $b[$pe + 22] = [byte]($ch -band 0xFF); $b[$pe + 23] = [byte](($ch -shr 8) -band 0xFF)
    # Recompute the PE checksum (field at optional header + 64).
    $ck = $pe + 24 + 64
    [uint64]$sum = 0
    for ($i = 0; $i -lt $b.Length; $i += 2) {
        if ($i -eq $ck -or $i -eq $ck + 2) { continue }
        $w = [uint64]$b[$i]; if ($i + 1 -lt $b.Length) { $w += [uint64]$b[$i + 1] * 256 }
        $sum += $w; $sum = ($sum -band 0xFFFF) + ($sum -shr 16)
    }
    $sum = ($sum -band 0xFFFF) + ($sum -shr 16)
    $sum += [uint64]$b.Length
    $cb = [BitConverter]::GetBytes([uint32]($sum -band 0xFFFFFFFF))
    for ($k = 0; $k -lt 4; $k++) { $b[$ck + $k] = $cb[$k] }
    Copy-Item -LiteralPath $path -Destination "$path.before-4gb-patch.bak" -Force
    [IO.File]::WriteAllBytes($path, $b)
}

# ---------- pack download ----------
function Get-PackInfo {
    $headers = @{ 'User-Agent' = 'RE2-Accessibility-Mod-Setup'; 'Accept' = 'application/vnd.github+json' }
    try {
        for ($page = 1; $page -le 3; $page++) {
            $rels = Invoke-RestMethod -UseBasicParsing -Headers $headers -Uri "https://api.github.com/repos/$PackRepo/releases?per_page=30&page=$page"
            foreach ($r in $rels) {
                if ($r.prerelease -or $r.draft) { continue }
                foreach ($a in $r.assets) {
                    if ($a.name -eq $PackAsset) {
                        $sha = $null; if ($a.digest -and "$($a.digest)" -like 'sha256:*') { $sha = "$($a.digest)".Substring(7) }
                        return @{ Url = $a.browser_download_url; Version = $r.tag_name; Sha256 = $sha; Size = $a.size }
                    }
                }
            }
            if (@($rels).Count -lt 30) { break }
        }
    } catch { Log "GitHub release lookup failed: $($_.Exception.Message)" }
    return @{ Url = $PackFallback; Version = '26.10.16 (built-in fallback)'; Sha256 = $PackFallbackSha; Size = 0 }
}

# ---------- dialog watcher (answers the pack's pop-ups automatically) ----------
# The pack runs silently, but a few of its message boxes still appear, sometimes hidden.
# Every question is answered here so the player never has to: Yes to yes/no questions,
# Retry to retry questions, OK to messages (errors are recorded and reported at the end).
# Win32 is used rather than UI Automation, because UI Automation could not see the buttons
# of the hidden message box.
$Script:Re2WinReady = $false
$Script:HandledDialogs = @{}
$Script:DialogAttempts = @{}
$Script:PackErrors = New-Object System.Collections.Generic.List[string]
function Init-Uia {
    if (-not $IsWin) { return }
    try {
        Add-Type -TypeDefinition @'
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class Re2Win {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr d, int id);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    static string Cls(IntPtr h) { var c = new StringBuilder(64); GetClassName(h, c, 64); return c.ToString(); }
    public static string Title(IntPtr h) { var t = new StringBuilder(512); GetWindowText(h, t, 512); return t.ToString(); }
    public static IntPtr[] Dialogs(int[] pids) {
        var set = new List<uint>(); foreach (var p in pids) set.Add((uint)p);   // List, not HashSet: PowerShell 5.1 does not reference System.Core
        var r = new List<IntPtr>();
        EnumWindows((h, l) => { uint pid; GetWindowThreadProcessId(h, out pid); if (set.Contains(pid) && Cls(h) == "#32770") r.Add(h); return true; }, IntPtr.Zero);
        return r.ToArray();
    }
    public static string StaticText(IntPtr dlg) {
        var all = new StringBuilder();
        EnumChildWindows(dlg, (h, l) => {
            if (Cls(h) == "Static") { var t = new StringBuilder(2048); GetWindowText(h, t, 2048); if (t.Length > 0) all.Append(t.ToString()).Append(' '); }
            return true; }, IntPtr.Zero);
        return all.ToString().Trim();
    }
    public static bool Has(IntPtr dlg, int id) { return GetDlgItem(dlg, id) != IntPtr.Zero; }
    public static void Command(IntPtr dlg, int id) { ShowWindow(dlg, 5); PostMessage(dlg, 0x0111, (IntPtr)id, GetDlgItem(dlg, id)); }
    public static void Click(IntPtr dlg, int id) { ShowWindow(dlg, 5); IntPtr b = GetDlgItem(dlg, id); if (b != IntPtr.Zero) SendMessage(b, 0x00F5, IntPtr.Zero, IntPtr.Zero); }
    // Brings the dialog to the front. Returns true only if it really is the foreground window,
    // so a key press can never land in another program.
    public static bool Front(IntPtr dlg) {
        ShowWindow(dlg, 9);
        uint dummy; uint fg = GetWindowThreadProcessId(GetForegroundWindow(), out dummy);
        uint me = GetCurrentThreadId();
        AttachThreadInput(me, fg, true);
        keybd_event(0x12, 0, 0, UIntPtr.Zero); keybd_event(0x12, 0, 2, UIntPtr.Zero);   // Alt tap unlocks SetForegroundWindow
        SetForegroundWindow(dlg);
        AttachThreadInput(me, fg, false);
        System.Threading.Thread.Sleep(300);
        return GetForegroundWindow() == dlg;
    }
}
'@
        $Script:Re2WinReady = $true
    } catch { Log "Win32 dialog helper not available: $($_.Exception.Message)" }
}
function Get-ProcessTree([int]$rootPid) {
    $all = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Select-Object ProcessId, ParentProcessId)
    $set = @{ $rootPid = $true }; $grew = $true
    while ($grew) {
        $grew = $false
        foreach ($p in $all) { if ($set.ContainsKey([int]$p.ParentProcessId) -and -not $set.ContainsKey([int]$p.ProcessId)) { $set[[int]$p.ProcessId] = $true; $grew = $true } }
    }
    return @($set.Keys)
}
function Wait-Closed([IntPtr]$h, [int]$seconds) {
    for ($i = 0; $i -lt ($seconds * 4); $i++) { if (-not [Re2Win]::IsWindow($h)) { return $true }; Start-Sleep -Milliseconds 250 }
    return $false
}
# Presses a button: window command, then button click, then (only if the dialog is
# verifiably in front) the button's keyboard shortcut. Checks after each that the window closed.
function Answer-Dialog([IntPtr]$h, [int]$id, [string]$key) {
    [Re2Win]::Command($h, $id); if (Wait-Closed $h 4) { return 'window command' }
    [Re2Win]::Click($h, $id);   if (Wait-Closed $h 4) { return 'button click' }
    if ([Re2Win]::Front($h)) {
        (New-Object -ComObject WScript.Shell).SendKeys($key)
        if (Wait-Closed $h 6) { return 'key press' }
    }
    return $null
}
function Watch-Dialogs([int[]]$pids) {
    if (-not $Script:Re2WinReady) { return }
    foreach ($h in [Re2Win]::Dialogs($pids)) {
        try {
            $k = [int64]$h
            if ($Script:HandledDialogs.ContainsKey($k)) { continue }
            $msg = [Re2Win]::StaticText($h)
            $title = [Re2Win]::Title($h)
            $hasYes = [Re2Win]::Has($h, 6); $hasRetry = [Re2Win]::Has($h, 4); $hasOk = [Re2Win]::Has($h, 1); $hasCancel = [Re2Win]::Has($h, 2)
            if (-not $msg -and -not ($hasYes -or $hasRetry -or $hasOk -or $hasCancel)) { continue }   # not a message box
            if (-not $Script:DialogAttempts.ContainsKey($k)) {
                $Script:DialogAttempts[$k] = 0
                Log "Dialog '$title' (yes=$hasYes retry=$hasRetry ok=$hasOk cancel=$hasCancel): $msg"
            }
            $Script:DialogAttempts[$k]++
            if ($Script:DialogAttempts[$k] -gt 5) { continue }

            if ($hasYes)       { $id = 6; $key = 'y'; $answer = 'Yes' }
            elseif ($hasRetry) { $id = 4; $key = 'r'; $answer = 'Retry' }
            elseif ($hasOk)    { $id = 1; $key = '~'; $answer = 'OK' }
            else               { $id = 2; $key = '{ESC}'; $answer = 'Close' }
            $isError = $msg -match 'fail|error|incorrect|abort|cannot|unable'
            if ($isError) { $Script:PackErrors.Add($msg); Say "The pack reported a problem: $msg" }

            $how = Answer-Dialog $h $id $key
            if ($how) {
                $Script:HandledDialogs[$k] = $true
                if ($msg -match "Replace 'BioHazard 2'") { Say 'Answered the pack''s question about the video titles: Yes, use Resident Evil 2.' }
                elseif (-not $isError) { Say "Answered a question from the pack automatically: $answer. It asked: $msg" }
                Log "Answered '$answer' by $how."
            } elseif ($Script:DialogAttempts[$k] -eq 5) {
                Say "Could not answer a question from the pack automatically. It asked: $msg. The setup keeps waiting; answering it with Alt+Tab lets it continue."
                if ($IsWin) { try { [console]::Beep(660, 300); [console]::Beep(660, 300) } catch {} }
            }
        } catch { Log "Dialog handling error: $($_.Exception.Message)" }
    }
}

# ---------- accessibility settings ----------
function Set-IniValue([string]$file, [string]$key, [string]$value) {
    $lines = @(Get-Content -LiteralPath $file)
    $found = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match ('^\s*' + [regex]::Escape($key) + '\s*=')) { $lines[$i] = "$key = $value"; $found = $true }
    }
    if (-not $found) {
        $out = New-Object System.Collections.Generic.List[string]; $added = $false
        foreach ($l in $lines) { $out.Add($l); if (-not $added -and $l -match '^\s*\[DLL\]') { $out.Add("$key = $value"); $added = $true } }
        if (-not $added) { $out.Add('[DLL]'); $out.Add("$key = $value") }
        $lines = $out.ToArray()
    }
    Set-Content -LiteralPath $file -Value $lines -Encoding Ascii
}

function New-Shortcut([string]$lnk, [string]$target, [string]$workdir, [string]$desc) {
    $sh = New-Object -ComObject WScript.Shell
    $s = $sh.CreateShortcut($lnk); $s.TargetPath = $target; $s.WorkingDirectory = $workdir; $s.Description = $desc; $s.IconLocation = "$target,0"; $s.Save()
}

# =======================================================================================
Write-Host 'Resident Evil 2 setup: Sourcenext edition and Classic REbirth.'
Log "Script: $ScriptPath"
Log "Arguments: $argText"
Log ("PowerShell {0}, OS {1}" -f $PSVersionTable.PSVersion, [Environment]::OSVersion.VersionString)

# Step 1: find the game
Step 1 'Finding Resident Evil 2 in your Steam library.'
$Game = Find-Game
if (-not $Game -and -not $Opt.check -and -not $Opt.nopause) {
    Say 'I could not find the game automatically.'
    $typed = Read-Host 'Type or paste the game folder (the one that contains the japanese and english folders), then press Enter. Leave it empty to cancel'
    if ($typed) { $Game = $typed.Trim('"', ' ') }
}
if (-not (Test-GameFolder $Game)) {
    Finish 2 "Stopped: the Steam copy of Resident Evil 2 (1998) was not found, or its japanese folder is missing. Install it from Steam (app $AppId) and run this again. You can also give the folder with --game."
}
$Game = (Resolve-Path -LiteralPath $Game).Path
Say "Found the game at: $Game"

$State = Get-State $Game
if ($State.Complete) { Say "Classic REbirth is already installed in the rebirth folder." }
elseif ((Test-Path -LiteralPath $State.Rebirth)) { Say 'A rebirth folder exists, but the installation in it is incomplete.' }
else { Say 'Classic REbirth is not installed yet.' }

if ($Opt.check) {
    Say ("Sourcenext bio2.exe: {0}. Classic REbirth ddraw.dll: {1}. ASI loader: {2}. 4 GB patch: {3}. Videos: {4} GB." -f `
        ($(if ($State.HasExe) { 'yes' } else { 'no' })), ($(if ($State.HasDll) { 'yes' } else { 'no' })), ($(if ($State.HasAsi) { 'yes' } else { 'no' })),
        ($(if ($State.LAA) { 'yes' } elseif ($State.HasExe) { 'no' } else { 'not applicable' })), $State.VideoGB)
    Finish 0 'Check finished. Nothing was changed.'
}

# Step 2: pre-flight checks
Step 2 'Checking that the game is closed, there is enough disk space, and the folder can be written to.'
$warned = $false
while ($true) {
    $running = @(Get-Process -Name $GameProcesses -ErrorAction SilentlyContinue)
    if ($running.Count -eq 0) { break }
    if (-not $warned) { Say ("Please close the game first. Still running: {0}. Waiting for it to close." -f (($running | ForEach-Object { $_.ProcessName } | Select-Object -Unique) -join ', ')); $warned = $true }
    Start-Sleep -Seconds 3
}
if ($warned) { Say 'The game has closed. Continuing.' }

$needInstall = $Opt.reinstall -or -not $State.Complete
if ($needInstall) {
    $drive = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($Game))
    $freeGB = [math]::Round($drive.AvailableFreeSpace / 1GB, 1)
    Say "Free space on drive $($drive.Name): $freeGB GB."
    if ($freeGB -lt $NeedFreeGB) { Finish 3 "Stopped: at least $NeedFreeGB GB of free space is needed on that drive (the pack downloads about 6 GB). Free some space and run this again." }
}

$probe = Join-Path $Game ('.write-test-' + [guid]::NewGuid().ToString('N'))
$canWrite = $true
try { Set-Content -LiteralPath $probe -Value 'x'; Remove-Item -LiteralPath $probe -Force } catch { $canWrite = $false }
if (-not $canWrite) {
    if ($IsWin -and -not $Opt.elevated) {
        Say 'Windows needs administrator permission to change the game folder. A permission prompt will appear; choose Yes. Setup then continues in a new window.'
        $relaunch = @('--elevated', '--game', "`"$Game`"")
        if ($Opt.reinstall) { $relaunch += '--reinstall' }
        if ($Opt.noshortcuts) { $relaunch += '--no-shortcuts' }
        try { Start-Process -FilePath $ScriptPath -ArgumentList ($relaunch -join ' ') -Verb RunAs; $Opt.nopause = $true; Finish 0 'Continuing in the administrator window.' }
        catch { Finish 4 'Stopped: administrator permission was not given, so the game folder cannot be changed.' }
    }
    Finish 4 "Stopped: this script cannot write to $Game."
}
Say 'All checks passed.'

# Step 3: download and run the pack
if ($needInstall) {
    Step 3 'Downloading the Mulderland Resident Evil 2 Steam Enhancement Pack.'
    if ($State.HasExe -or (Test-Path -LiteralPath $State.Rebirth)) {
        $aside = "$($State.Rebirth).old-" + (Get-Date -Format 'yyyyMMdd-HHmmss')
        Rename-Item -LiteralPath $State.Rebirth -NewName (Split-Path -Leaf $aside)
        Say "Moved the previous rebirth folder aside to: $aside"
    }
    $work = Join-Path ([IO.Path]::GetTempPath()) 'RE2SourcenextSetup'
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $packExe = Join-Path $work $PackAsset

    if ($env:RE2SETUP_TEST_FAKE_PACK) {
        $info = @{ Version = 'test'; Url = 'test'; Sha256 = $null }
    } else {
        $info = Get-PackInfo
        Say "Pack version $($info.Version)."
        Log "Download URL: $($info.Url)"
        try { (New-Object Net.WebClient).DownloadFile($info.Url, $packExe) }
        catch { Finish 5 "Stopped: the pack could not be downloaded. Check the internet connection and try again. Details: $($_.Exception.Message)" }
        if ($info.Sha256) {
            $h = (Get-FileHash -LiteralPath $packExe -Algorithm SHA256).Hash
            if ($h -ne $info.Sha256.ToUpper()) { Remove-Item -LiteralPath $packExe -Force; Finish 5 'Stopped: the downloaded pack did not match its published checksum, so it was deleted. Try again later.' }
            Say 'Download verified against the checksum published on GitHub.'
        }
        try {
            $sig = Get-AuthenticodeSignature -LiteralPath $packExe
            Log "Signature: $($sig.Status) $($sig.SignerCertificate.Subject)"
        } catch {}
        try { Unblock-File -LiteralPath $packExe } catch {}
    }

    Write-Host ''
    Say 'Step 3 continued. Running the pack silently. It downloads about 6 GB, so this can take from 15 minutes to over an hour. There is nothing to do while it runs: any questions the pack asks are answered automatically, and progress is read out every 2 minutes.'
    Init-Uia
    $baseline = Folder-Bytes $Game
    $start = Get-Date
    if ($env:RE2SETUP_TEST_FAKE_PACK) {
        $proc = Start-Process -FilePath 'pwsh' -ArgumentList @('-NoProfile', '-File', "`"$env:RE2SETUP_TEST_FAKE_PACK`"", "`"$Game`"") -PassThru
    } else {
        # Windows treats the pack as an installer and would silently run it as administrator.
        # The setup itself is not elevated, and Windows blocks a normal program from answering an
        # administrator's windows, so the pack's questions could not be answered. RunAsInvoker
        # starts the pack at this script's own level; the game folder was checked as writable
        # (or this script was already relaunched as administrator), so nothing needs elevation.
        $oldCompat = $env:__COMPAT_LAYER
        $env:__COMPAT_LAYER = 'RunAsInvoker'
        try { $proc = Start-Process -FilePath $packExe -ArgumentList "/S /D=$Game" -PassThru }
        finally { $env:__COMPAT_LAYER = $oldCompat }
    }
    Log "Pack process id: $($proc.Id)"
    $Script:WrittenByPid = @{}
    $rbDir = Join-Path $Game 'rebirth'
    $stages = @(
        @{ Test = { Test-Path -LiteralPath (Join-Path $rbDir 'bio2.exe') };  Say = 'Progress: the Sourcenext update is installed.' },
        @{ Test = { Test-Path -LiteralPath (Join-Path $rbDir 'ddraw.dll') }; Say = 'Progress: Classic REbirth is installed.' },
        @{ Test = { Test-Path -LiteralPath (Join-Path $rbDir 'hires') };     Say = 'Progress: installing the HD graphics.' },
        @{ Test = { @(Get-ChildItem -LiteralPath $rbDir -Filter 'RE-ENHANCE_RE2_FMV-Pack*.zip' -ErrorAction SilentlyContinue).Count -gt 0 }; Say = 'Progress: downloading the high-quality videos, about 3.4 GB. This is the longest step, and the progress number may stay still while the file downloads.' },
        @{ Test = { @(Get-ChildItem -LiteralPath $rbDir -Filter '*HQ Music*' -ErrorAction SilentlyContinue).Count -gt 0 }; Say = 'Progress: the videos are installed. Downloading the high-quality audio, about 1.2 GB.' },
        @{ Test = { Test-Path -LiteralPath (Join-Path $Game 'MulderConfig.exe') }; Say = 'Progress: final step, setting up the MulderConfig launcher.' }
    )
    $stageDone = @{}; $lastStage = Get-Date
    $lastReport = Get-Date; $lastGrowth = Get-Date; $lastBytes = -1; $tree = @($proc.Id); $lastTree = Get-Date
    while (-not $proc.HasExited) {
        Start-Sleep -Seconds 2
        if ($IsWin -and ((Get-Date) - $lastTree).TotalSeconds -ge 10) { $tree = Get-ProcessTree $proc.Id; $lastTree = Get-Date }
        Watch-Dialogs $tree
        if (((Get-Date) - $lastStage).TotalSeconds -ge 10) {
            for ($si = 0; $si -lt $stages.Count; $si++) {
                if (-not $stageDone.ContainsKey($si)) { try { if (& $stages[$si].Test) { $stageDone[$si] = $true; Say $stages[$si].Say } } catch {} }
            }
            $lastStage = Get-Date
        }
        if (((Get-Date) - $lastReport).TotalSeconds -ge 120) {
            # Count what the pack has written. Folder sizes are not used, because Windows does not
            # update the listed size of a file that is still being downloaded.
            if ($IsWin) {
                foreach ($id in $tree) {
                    $cp = Get-CimInstance Win32_Process -Filter "ProcessId=$id" -ErrorAction SilentlyContinue
                    if ($cp) { $Script:WrittenByPid[[int]$id] = [double]$cp.WriteTransferCount }
                }
                $now = 0; foreach ($v in $Script:WrittenByPid.Values) { $now += $v }
                $what = 'The pack has downloaded or written {1} so far.'
            } else {
                $now = (Folder-Bytes $Game) - $baseline
                $what = 'The game folder has grown by {1}.'
            }
            $mins = [int]((Get-Date) - $start).TotalMinutes
            Say (("Still working, {0} minutes so far. " + $what) -f $mins, (Size-GB $now))
            if ($now -ne $lastBytes) { $lastGrowth = Get-Date; $lastBytes = $now }
            elseif (((Get-Date) - $lastGrowth).TotalMinutes -ge 20) { Say 'Nothing new has been written for 20 minutes. The download may be stalled. The script keeps waiting; you can close this window to stop.' ; $lastGrowth = Get-Date }
            $lastReport = Get-Date
        }
    }
    $proc.WaitForExit()
    $exit = $proc.ExitCode
    Say ("The pack finished after {0} minutes with exit code {1}." -f [int]((Get-Date) - $start).TotalMinutes, $exit)
    if ($Script:PackErrors.Count -gt 0 -and $exit -ne 0) { Finish 6 ("Stopped: the pack reported: " + ($Script:PackErrors -join ' / ') + ' Run this script again to retry.') }
    if ($Script:PackErrors.Count -gt 0) { Say ('The pack reported problems but finished; checking the result: ' + ($Script:PackErrors -join ' / ')) }
    if ($exit -ne 0) { Log "Non-zero exit code $exit; checking the result anyway." }
} else {
    Step 3 'Skipping the download, because Classic REbirth is already installed. Use --reinstall to run the pack again.'
}

# Step 4: verify
Step 4 'Checking the result.'
$State = Get-State $Game
$problems = New-Object System.Collections.Generic.List[string]
if (-not $State.HasExe)  { $problems.Add('the Sourcenext bio2.exe is missing') }
if (-not $State.HasDll)  { $problems.Add('the Classic REbirth ddraw.dll is missing') }
if (-not $State.HasData) { $problems.Add('the copied game data (Pl0 and Common) is missing') }
if ($problems.Count -gt 0) { Finish 7 ('Stopped: the installation is not complete: ' + ($problems -join '; ') + '. Run this script again with --reinstall.') }
Say 'Sourcenext bio2.exe is installed.'
Say 'Classic REbirth is installed.'
if ($State.LAA -eq $false) {
    Set-LargeAddressAware $State.Exe
    Say 'The 4 GB memory patch was missing, so it was applied now. A backup of the original exe was kept.'
} else { Say 'The 4 GB memory patch is applied.' }
if ($State.VideoGB -lt 0.3) { Say "Warning: only $($State.VideoGB) GB of videos were found. The Sourcenext game may crash at videos without the high-quality video pack. Run this again with --reinstall if it does." }
else { Say "High-quality videos are installed, $($State.VideoGB) GB." }
if ($State.HasAsi) { Say 'The Ultimate ASI Loader is installed, so the accessibility mod will be able to load as a plugin.' }
else { Say 'Note: the ASI loader (dsound.dll) was not found. The accessibility mod installer will add it later.' }

# Step 5: accessibility settings
Step 5 'Making the game screen-reader friendly.'
$ini = Join-Path $State.Rebirth 'config.ini'
if (-not (Test-Path -LiteralPath $ini)) { Set-Content -LiteralPath $ini -Value @('[DLL]') -Encoding Ascii }
Copy-Item -LiteralPath $ini -Destination "$ini.before-accessibility.bak" -Force
Set-IniValue $ini 'BootConfig' '0'
Set-IniValue $ini 'JapaneseEnable' '0'
Say 'Turned off the Classic REbirth settings window at start-up, so the game opens straight away, in English.'
Say 'To change Classic REbirth settings later, set BootConfig = 1 in rebirth\config.ini.'

# Step 6: shortcuts
if ($Opt.noshortcuts -or -not $IsWin) {
    Step 6 'Skipping shortcuts.'
} else {
    Step 6 'Adding shortcuts.'
    $name = 'Resident Evil 2 Classic REbirth.lnk'
    $desc = 'Resident Evil 2 (1998), Sourcenext edition with Classic REbirth'
    try {
        New-Shortcut (Join-Path ([Environment]::GetFolderPath('Desktop')) $name) $State.Exe $State.Rebirth $desc
        New-Shortcut (Join-Path ([Environment]::GetFolderPath('Programs')) $name) $State.Exe $State.Rebirth $desc
        Say 'Added "Resident Evil 2 Classic REbirth" to the desktop and the Start menu.'
    } catch { Say "Could not create shortcuts: $($_.Exception.Message)" }
}

# Step 7: record what was installed
Step 7 'Writing Game Install Info.txt next to this script.'
$State = Get-State $Game
$infoLines = @(
    'Resident Evil 2 (1998) - Sourcenext + Classic REbirth install',
    ('Updated: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm')),
    "Steam game folder: $Game",
    "Classic REbirth folder: $($State.Rebirth)",
    "Game executable: $($State.Exe)",
    "bio2.exe: $($State.Version)",
    ('Large address aware (4 GB patch): ' + $State.LAA),
    ('ASI loader (dsound.dll): ' + $State.HasAsi),
    "Videos: $($State.VideoGB) GB",
    'Settings: rebirth\config.ini (BootConfig = 0, JapaneseEnable = 0)',
    'Steam Play button: opens MulderConfig. The desktop shortcut starts bio2.exe directly.'
)
Set-Content -LiteralPath (Join-Path $ScriptDir 'Game Install Info.txt') -Value $infoLines -Encoding UTF8
Log ($infoLines -join ' | ')

Finish 0 'Done. Resident Evil 2 is now the Sourcenext edition with Classic REbirth. Start it from the "Resident Evil 2 Classic REbirth" shortcut on the desktop or in the Start menu.'
