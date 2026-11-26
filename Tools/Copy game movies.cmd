<# : batch launcher
@echo off
setlocal
title Copy RE2 game movies
set "RE2COPY_SCRIPT=%~f0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$s = [IO.File]::ReadAllText($env:RE2COPY_SCRIPT); & ([ScriptBlock]::Create($s))"
endlocal
#>

# Copies the movie folders (Zmovie) of the installed Classic REbirth game into
# "Game Files\rebirth-movies" in the mod folder, for the audio descriptions.
# It only READS the Steam folder; nothing there is changed.
# Kept apart from "Game Files\rebirth" because "Copy game files for simulator" mirrors that
# folder and would remove anything it did not copy itself.
$ErrorActionPreference = 'Stop'
$toolDir = Split-Path -Parent $env:RE2COPY_SCRIPT
$modDir = $toolDir
while ($modDir -and -not (Test-Path -LiteralPath (Join-Path $modDir 'Game Files'))) { $modDir = Split-Path -Parent $modDir }
if (-not $modDir) { Write-Host 'Could not find the mod folder (the one containing Game Files).'; [void](Read-Host 'Press Enter to close'); exit 1 }

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
$dst = Join-Path $modDir 'Game Files\rebirth-movies'
Write-Host "Copying movies from: $src"
Write-Host "Copying to: $dst"

$log = Join-Path $modDir 'Logs\copy-game-movies.log'
New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
Set-Content -LiteralPath $log -Value ("Copy game movies " + (Get-Date -Format 'yyyy-MM-dd HH:mm')) -Encoding UTF8

$dirs = @(Get-ChildItem -LiteralPath $src -Recurse -Directory | Where-Object { $_.Name -ieq 'Zmovie' -or $_.Name -ieq 'movie' })
if ($dirs.Count -eq 0) {
    Write-Host 'No movie folders (Zmovie) were found in the game folder.'
    Add-Content -LiteralPath $log -Value 'No Zmovie folders found.'
    [void](Read-Host 'Press Enter to close'); exit 1
}
foreach ($d in $dirs) {
    $rel = $d.FullName.Substring($src.TrimEnd('\').Length).TrimStart('\')
    $to = Join-Path $dst $rel
    Write-Host "  $rel"
    & robocopy.exe $d.FullName $to /MIR /R:1 /W:1 /NP /NDL "/LOG+:$log" | Out-Null
    if ($LASTEXITCODE -ge 8) {
        Write-Host "The copy failed (robocopy code $LASTEXITCODE). Details are in $log"
        [void](Read-Host 'Press Enter to close'); exit 1
    }
}

$files = @(Get-ChildItem -LiteralPath $dst -Recurse -File | Where-Object { $_.Name -ne '_movies-info.txt' })
$size = ($files | Measure-Object Length -Sum).Sum
$lines = @('Movies copied from the installed Classic REbirth game, for the audio descriptions.',
    ('Copied: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm')), "Source: $src",
    ("Files: {0}, size {1:N0} MB" -f $files.Count, ($size / 1MB)), '')
$lines += $files | ForEach-Object { "{0}  {1:N0} bytes" -f $_.FullName.Substring($dst.Length + 1), $_.Length }
Set-Content -LiteralPath (Join-Path $dst '_movies-info.txt') -Value $lines -Encoding UTF8
Write-Host ''
Write-Host ("Copied {0} movie files, {1:N0} MB." -f $files.Count, ($size / 1MB))
Write-Host 'Done. The Steam folder was not changed.'
try { [console]::Beep(880, 200); [console]::Beep(1175, 300) } catch {}
try { $Host.UI.RawUI.FlushInputBuffer() } catch {}
[void](Read-Host 'Press Enter to close')
