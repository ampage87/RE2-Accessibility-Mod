# ============================================================
# Resident Evil 2 Accessibility Mod - Deploy
# Started by Deploy.vbs in the mod folder. Builds the mod (deploy.bat),
# then installs it into the Classic REbirth game folder:
#   RE2Access.asi        the mod (loaded by the ASI loader, dsound.dll)
#   RE2Access\*.txt      room names, file texts, menu labels
#   RE2Access\movies\    audio description of the movies (cues.txt + WAVs per movie)
#   RE2Access.ini        settings; log_copy_dir points at the mod folder's Logs
# A progress window reports each step; screen readers hear the changes.
# Run with -InstallOnly (used internally, elevated) to just copy files.
# ============================================================
param(
    [switch]$InstallOnly,
    [string]$GameDir = "",
    [string]$ResultFile = ""
)

$ErrorActionPreference = "Stop"
$srcDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$modDir   = Split-Path -Parent $srcDir                 # ...\Mod
$rootDir  = Split-Path -Parent $modDir                 # the mod folder (Deploy.vbs lives here)
$logsDir  = Join-Path $rootDir "Logs"
$batPath  = Join-Path $srcDir "deploy.bat"
$asiBuilt = Join-Path $modDir "build\RE2Access.asi"
$dataDir  = Join-Path $modDir "data"

function Install-Mod([string]$target) {
    # Copies the mod into the game folder and verifies every file. Returns a list of messages.
    $msgs = New-Object System.Collections.Generic.List[string]
    Copy-Item -LiteralPath $asiBuilt -Destination (Join-Path $target "RE2Access.asi") -Force
    $msgs.Add("Copied RE2Access.asi")
    $dataTarget = Join-Path $target "RE2Access"
    if (-not (Test-Path -LiteralPath $dataTarget)) { New-Item -ItemType Directory -Path $dataTarget | Out-Null }
    foreach ($f in Get-ChildItem -LiteralPath $dataDir -File) {
        Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $dataTarget $f.Name) -Force
        $msgs.Add("Copied RE2Access\$($f.Name)")
    }
    # Audio description of the movies: RE2Access\movies\<movie>\cues.txt and its WAVs
    $movieSrc = Join-Path $dataDir "movies"
    $movieFiles = @()
    if (Test-Path -LiteralPath $movieSrc) {
        $movieFiles = @(Get-ChildItem -LiteralPath $movieSrc -Recurse -File)
        foreach ($f in $movieFiles) {
            $rel = $f.FullName.Substring($dataDir.Length + 1)
            $to = Join-Path $dataTarget $rel
            $toDir = Split-Path -Parent $to
            if (-not (Test-Path -LiteralPath $toDir)) { New-Item -ItemType Directory -Path $toDir -Force | Out-Null }
            Copy-Item -LiteralPath $f.FullName -Destination $to -Force
        }
        $msgs.Add("Copied $($movieFiles.Count) movie audio description files")
    }
    # The screen reader library (Prism, MPL-2.0) and its licence, when it has been built
    $prismDll = Join-Path $modDir "third_party\prism\x86\prism.dll"
    if (Test-Path -LiteralPath $prismDll) {
        Copy-Item -LiteralPath $prismDll -Destination (Join-Path $target "prism.dll") -Force
        foreach ($lf in @("LICENSE-Prism.txt", "NOTICE-Prism.txt")) {
            $lp = Join-Path $modDir "third_party\prism\$lf"
            if (Test-Path -LiteralPath $lp) { Copy-Item -LiteralPath $lp -Destination (Join-Path $dataTarget $lf) -Force }
        }
        if ((Get-FileHash -LiteralPath $prismDll).Hash -ne (Get-FileHash -LiteralPath (Join-Path $target "prism.dll")).Hash) {
            throw "prism.dll in the game folder does not match."
        }
        $msgs.Add("Copied prism.dll (screen reader support)")
    } else {
        $msgs.Add("prism.dll not built; the mod will speak with SAPI only")
    }
    # Settings: keep what is there (voice, rate, volume), set the log copy folder.
    $ini = Join-Path $target "RE2Access.ini"
    $lines = New-Object System.Collections.Generic.List[string]
    if (Test-Path -LiteralPath $ini) { foreach ($l in [System.IO.File]::ReadAllLines($ini)) { $lines.Add($l) } }
    if (-not ($lines | Where-Object { $_.Trim() -ieq "[RE2Access]" })) { $lines.Insert(0, "[RE2Access]") }
    $setting = "log_copy_dir=$logsDir"
    $idx = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\s*log_copy_dir\s*=') { $idx = $i; break } }
    if ($idx -ge 0) { $lines[$idx] = $setting }
    else {
        $sec = 0
        for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim() -ieq "[RE2Access]") { $sec = $i; break } }
        $lines.Insert($sec + 1, $setting)
    }
    [System.IO.File]::WriteAllLines($ini, $lines, [System.Text.Encoding]::Default)
    $msgs.Add("Updated RE2Access.ini ($setting)")
    # Verify
    $a = (Get-FileHash -LiteralPath $asiBuilt).Hash
    $b = (Get-FileHash -LiteralPath (Join-Path $target "RE2Access.asi")).Hash
    if ($a -ne $b) { throw "RE2Access.asi in the game folder does not match the build." }
    foreach ($f in Get-ChildItem -LiteralPath $dataDir -File) {
        if ((Get-FileHash -LiteralPath $f.FullName).Hash -ne (Get-FileHash -LiteralPath (Join-Path $dataTarget $f.Name)).Hash) {
            throw "RE2Access\$($f.Name) in the game folder does not match."
        }
    }
    foreach ($f in $movieFiles) {
        $rel = $f.FullName.Substring($dataDir.Length + 1)
        if ((Get-FileHash -LiteralPath $f.FullName).Hash -ne (Get-FileHash -LiteralPath (Join-Path $dataTarget $rel)).Hash) {
            throw "RE2Access\$rel in the game folder does not match."
        }
    }
    $msgs.Add("Verified all files")
    return $msgs
}

# ---------------------------------------------------------------- elevated copy only
if ($InstallOnly) {
    try {
        $m = Install-Mod $GameDir
        [System.IO.File]::WriteAllText($ResultFile, "OK`r`n" + ($m -join "`r`n"))
        exit 0
    } catch {
        [System.IO.File]::WriteAllText($ResultFile, "ERROR`r`n" + $_.Exception.Message)
        exit 1
    }
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
if (-not (Test-Path $logsDir)) { New-Item -ItemType Directory -Path $logsDir | Out-Null }
$logPath = Join-Path $logsDir "deploy_latest.log"

$modVersion = "unknown"
$hdr = Join-Path $srcDir "mod.h"
if (Test-Path $hdr) {
    $m = Select-String -Path $hdr -Pattern '^#define RE2ACCESS_VERSION "([^"]+)"' | Select-Object -First 1
    if ($m) { $modVersion = $m.Matches[0].Groups[1].Value }
}
$title = "Deploying Resident Evil 2 Accessibility Mod $modVersion"

$shared = @{
    Log           = New-Object System.Text.StringBuilder
    LinesRead     = 0
    UiError       = $null
    Status        = "Building Resident Evil 2 Accessibility Mod $modVersion"
    StatusChanged = $true
    Warnings      = New-Object System.Collections.Generic.List[string]
}
function Add-Log([string]$t) { [void]$shared.Log.AppendLine($t) }
function Set-Status([string]$t) { $shared.Status = $t; $shared.StatusChanged = $true; Add-Log $t }
Add-Log "$title - started $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"

function Show-Result([bool]$ok, [string]$text) {
    [System.IO.File]::WriteAllText($logPath, $shared.Log.ToString())
    if ($ok) {
        [System.Windows.Forms.MessageBox]::Show($text, $title,
            [System.Windows.Forms.MessageBoxButtons]::OK,
            [System.Windows.Forms.MessageBoxIcon]::Information) | Out-Null
        return
    }
    $r = [System.Windows.Forms.MessageBox]::Show("$text`r`n`r`nOpen the deploy log?", "$title - Error",
        [System.Windows.Forms.MessageBoxButtons]::YesNo,
        [System.Windows.Forms.MessageBoxIcon]::Error)
    if ($r -eq [System.Windows.Forms.DialogResult]::Yes) { Start-Process "notepad.exe" -ArgumentList "`"$logPath`"" }
}

# Any error not handled below is reported in a message box and the log, never as a crash.
trap {
    Add-Log "ERROR: $($_.Exception.Message)"
    Add-Log "$($_.InvocationInfo.PositionMessage)"
    Show-Result $false "Deploy stopped with an error: $($_.Exception.Message)"
    exit 1
}

# ---------------------------------------------------------------- find the game
$gameDir = ""
$info = Join-Path $rootDir "Game Install Info.txt"
if (Test-Path -LiteralPath $info) {
    foreach ($l in Get-Content -LiteralPath $info) {
        if ($l -match '^Classic REbirth folder:\s*(.+)$') { $gameDir = $Matches[1].Trim() }
    }
}
if (-not $gameDir) { $gameDir = Join-Path ${env:ProgramFiles(x86)} "Steam\steamapps\common\4249110_Biohazard2\rebirth" }
Add-Log "Game folder: $gameDir"
if (-not (Test-Path -LiteralPath (Join-Path $gameDir "bio2.exe"))) {
    Show-Result $false "The game was not found in $gameDir. Run Install Sourcenext and Classic REbirth.cmd first."
    exit 1
}
if (-not (Test-Path -LiteralPath (Join-Path $gameDir "dsound.dll"))) {
    $shared.Warnings.Add("The ASI loader (dsound.dll) is missing from the game folder, so the mod will not load. Run Install Sourcenext and Classic REbirth.cmd again.")
}
if (Get-Process -Name "bio2" -ErrorAction SilentlyContinue) {
    Show-Result $false "Resident Evil 2 is running. Close the game, then run Deploy again."
    exit 1
}

# ---------------------------------------------------------------- progress window
# The build runs in cmd.exe with its output going to a file; a timer on the window reads
# new lines from that file. (No PowerShell event handlers: they cannot run while the
# window is open, and any error inside a window event shows "unhandled exception".)
$buildLog = Join-Path $logsDir "build_latest.log"
Remove-Item -LiteralPath $buildLog -ErrorAction SilentlyContinue

$notify = [System.Windows.Forms.Control].GetMethod("AccessibilityNotifyClients",
    [System.Reflection.BindingFlags]"Instance,NonPublic", $null,
    [type[]]@([System.Windows.Forms.AccessibleEvents], [int]), $null)

$form = New-Object System.Windows.Forms.Form
$form.Text            = $title
$form.StartPosition   = "CenterScreen"
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox     = $false
$form.MinimizeBox     = $false
$form.ControlBox      = $false
$form.TopMost         = $true
$form.Width           = 460
$form.Height          = 160

$statusBox = New-Object System.Windows.Forms.TextBox
$statusBox.Text           = $shared.Status
$statusBox.ReadOnly       = $true
$statusBox.Multiline      = $true
$statusBox.WordWrap       = $true
$statusBox.BorderStyle    = "None"
$statusBox.BackColor      = $form.BackColor
$statusBox.TabStop        = $true
$statusBox.Width          = 420
$statusBox.Height         = 80
$statusBox.Location       = New-Object System.Drawing.Point(14, 16)
$statusBox.AccessibleName = "Status"
$form.Controls.Add($statusBox)
$form.Add_Shown({
    try { $form.Activate(); $statusBox.Focus(); $statusBox.SelectionLength = 0 } catch {}
})

$process = New-Object System.Diagnostics.Process
$process.StartInfo.FileName         = "cmd.exe"
$process.StartInfo.Arguments        = "/c `"`"$batPath`" > `"$buildLog`" 2>&1`""
$process.StartInfo.WorkingDirectory = $modDir
$process.StartInfo.UseShellExecute  = $false
$process.StartInfo.CreateNoWindow   = $true
$process.Start() | Out-Null

$shared.LinesRead = 0
function Read-BuildLog {
    if (-not (Test-Path -LiteralPath $buildLog)) { return }
    $all = @()
    try {
        $fs = New-Object System.IO.FileStream($buildLog, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $sr = New-Object System.IO.StreamReader($fs)
        $all = $sr.ReadToEnd() -split "`r?`n"
        $sr.Close()
    } catch { return }
    # the last element may be a partial line until the build finishes
    $complete = if ($process.HasExited) { $all.Count } else { $all.Count - 1 }
    for ($i = $shared.LinesRead; $i -lt $complete; $i++) {
        $line = $all[$i]
        $new = $null
        if     ($line -match "Locating Visual Studio") { $new = "Looking for Visual Studio build tools..." }
        elseif ($line -match "^Building RE2Access")    { $new = "Building the mod. This takes about a minute." }
        elseif ($line -match "^Build successful")      { $new = "Build successful. Installing into the game folder..." }
        elseif ($line -match "^Using prebuilt")        { $new = "Using the prebuilt mod. Installing into the game folder..." }
        elseif ($line -match "^PRISM: (.*)")           { $new = $Matches[1] }
        elseif ($line -match "^WARNING: (.*)")         { $new = $Matches[1]; $shared.Warnings.Add($Matches[1]) }
        elseif ($line -match "^ERROR:")                { $new = $line }
        if ($null -ne $new) { $shared.Status = $new; $shared.StatusChanged = $true }
    }
    if ($complete -gt $shared.LinesRead) { $shared.LinesRead = $complete }
}

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 250
$timer.Add_Tick({
    try {
        Read-BuildLog
        if ($shared.StatusChanged) {
            $shared.StatusChanged = $false
            $statusBox.Text = $shared.Status
            $statusBox.SelectionLength = 0
            if ($notify) {
                try {
                    [void]$notify.Invoke($statusBox, @([System.Windows.Forms.AccessibleEvents]::NameChange, 0))
                    [void]$notify.Invoke($statusBox, @([System.Windows.Forms.AccessibleEvents]::ValueChange, 0))
                } catch {}
            }
        }
        if ($process.HasExited) { $timer.Stop(); $form.Close() }
    } catch {
        $timer.Stop()
        $shared.UiError = $_.Exception.Message
        try { $form.Close() } catch {}
    }
})
$timer.Start()
$form.ShowDialog() | Out-Null
$timer.Dispose()
$process.WaitForExit()
$buildExit = $process.ExitCode
Read-BuildLog
if (Test-Path -LiteralPath $buildLog) { Add-Log ([System.IO.File]::ReadAllText($buildLog)) }
if ($shared.UiError) { Add-Log "Progress window error (ignored): $($shared.UiError)" }

if ($buildExit -ne 0 -or -not (Test-Path -LiteralPath $asiBuilt)) {
    Show-Result $false "The mod could not be built, and no prebuilt copy was available."
    exit 1
}

# ---------------------------------------------------------------- install
Add-Log "Installing into $gameDir"
$installed = $false
try {
    $msgs = Install-Mod $gameDir
    foreach ($m in $msgs) { Add-Log $m }
    $installed = $true
} catch [System.UnauthorizedAccessException] {
    Add-Log "No write access to the game folder; asking Windows for permission (User Account Control)."
} catch {
    if ($_.Exception.Message -match "denied|unauthori") {
        Add-Log "No write access to the game folder; asking Windows for permission (User Account Control)."
    } else {
        Add-Log "ERROR: $($_.Exception.Message)"
        Show-Result $false "Installing the mod failed: $($_.Exception.Message)"
        exit 1
    }
}
if (-not $installed) {
    $result = Join-Path $env:TEMP "RE2Access_deploy_result.txt"
    Remove-Item -LiteralPath $result -ErrorAction SilentlyContinue
    $psArgs = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$($MyInvocation.MyCommand.Path)`" -InstallOnly -GameDir `"$gameDir`" -ResultFile `"$result`""
    try {
        Start-Process -FilePath "powershell.exe" -ArgumentList $psArgs -Verb RunAs -WindowStyle Hidden -Wait
    } catch {
        Show-Result $false "Windows permission was declined, so the mod could not be copied into the game folder."
        exit 1
    }
    if (-not (Test-Path -LiteralPath $result)) {
        Show-Result $false "The elevated copy did not report a result."
        exit 1
    }
    $r = [System.IO.File]::ReadAllLines($result)
    foreach ($l in $r) { Add-Log $l }
    if ($r[0] -ne "OK") {
        Show-Result $false "Installing the mod failed: $($r[1..($r.Count-1)] -join ' ')"
        exit 1
    }
}

$text = "Resident Evil 2 Accessibility Mod $modVersion is installed. Start the game to test it. " +
        "You should hear: Resident Evil 2 accessibility mod $modVersion loaded. " +
        "Game logs are written to the Logs folder in the mod folder."
if ($shared.Warnings.Count -gt 0) { $text += "`r`n`r`nNotes: " + ($shared.Warnings -join " ") }
Add-Log "Done $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Show-Result $true $text
exit 0
