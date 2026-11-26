<# : batch launcher
@echo off
setlocal
title Copy RE2 game files for the simulator
set "RE2COPY_SCRIPT=%~f0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$s = [IO.File]::ReadAllText($env:RE2COPY_SCRIPT); & ([ScriptBlock]::Create($s))"
endlocal
#>

# Copies the installed Classic REbirth game (the Steam "rebirth" folder) into
# "Game Files\rebirth" in the mod folder, for the simulator and for reverse engineering.
# It only READS the Steam folder; nothing there is changed.
# Left out: videos (Zmovie), HD textures (hires), saves, and leftover archives, which the
# simulator does not need and which would fill OneDrive.
$ErrorActionPreference = 'Stop'
$toolDir = Split-Path -Parent $env:RE2COPY_SCRIPT
$modDir = $toolDir
while ($modDir -and -not (Test-Path -LiteralPath (Join-Path $modDir 'Game Files'))) { $modDir = Split-Path -Parent $modDir }
if (-not $modDir) { Write-Host 'Could not find the mod folder (the one containing Game Files).'; [void](Read-Host 'Press Enter to close'); exit 1 }

# Where the game is: from Game Install Info.txt written by the setup script.
$info = Join-Path $modDir 'Game Install Info.txt'
$src = $null
if (Test-Path -LiteralPath $info) {
    $m = Select-String -LiteralPath $info -Pattern '^Classic REbirth folder: (.+)$' | Select-Object -First 1
    if ($m) { $src = $m.Matches[0].Groups[1].Value.Trim() }
}
if (-not $src -or -not (Test-Path -LiteralPath (Join-Path $src 'bio2.exe'))) {
    Write-Host 'Could not find the Classic REbirth folder. Run "Install Sourcenext and Classic REbirth.cmd" first.'
    [void](Read-Host 'Press Enter to close'); exit 1
}
$dst = Join-Path $modDir 'Game Files\rebirth'
Write-Host "Copying from: $src"
Write-Host "Copying to: $dst"
Write-Host 'Leaving out videos, HD textures, saves and archives. This takes a minute or two.'

$log = Join-Path $modDir 'Logs\copy-game-files.log'
New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
# /MIR keeps the copy identical to the source (files removed from the game are removed from the copy).
& robocopy.exe $src $dst /MIR /XD Zmovie hires savedata .MulderConfig /XF *.zip *.7z *.7z.0* *.bak *.Backup /R:1 /W:1 /NP /NFL /NDL "/LOG:$log" | Out-Null
$rc = $LASTEXITCODE
if ($rc -ge 8) {
    Write-Host "The copy failed (robocopy code $rc). Details are in $log"
    [void](Read-Host 'Press Enter to close'); exit 1
}

$files = @(Get-ChildItem -LiteralPath $dst -Recurse -File)
$size = ($files | Measure-Object Length -Sum).Sum
$rdt = @($files | Where-Object { $_.Extension -ieq '.rdt' }).Count
$lines = @(
    'Snapshot of the installed Classic REbirth game, for the simulator and reverse engineering.',
    ('Copied: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm')),
    "Source: $src",
    ("Files: {0}, size {1:N0} MB, room files (.rdt): {2}" -f $files.Count, ($size / 1MB), $rdt),
    'Not copied: Zmovie (videos), hires (HD textures), savedata, archives.',
    ('bio2.exe SHA-256: ' + (Get-FileHash -LiteralPath (Join-Path $dst 'bio2.exe') -Algorithm SHA256).Hash)
)
Set-Content -LiteralPath (Join-Path $dst '_snapshot-info.txt') -Value $lines -Encoding UTF8
Write-Host ''
$lines | Select-Object -Skip 3 | ForEach-Object { Write-Host $_ }
Write-Host 'Done. The Steam folder was not changed.'
try { [console]::Beep(880, 200); [console]::Beep(1175, 300) } catch {}
try { $Host.UI.RawUI.FlushInputBuffer() } catch {}
[void](Read-Host 'Press Enter to close')
