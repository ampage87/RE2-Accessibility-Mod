<# : batch launcher
@echo off
setlocal
title Copy RE2 HD text images
set "RE2COPY_SCRIPT=%~f0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$s = [IO.File]::ReadAllText($env:RE2COPY_SCRIPT); & ([ScriptBlock]::Create($s))"
endlocal
#>

# Copies the HD project's replacement images for Common\data (title cards, story texts,
# menus) from the installed game's "hires" folder into "Game Files\rebirth-hires" in the mod
# folder, so the mod can speak text that the game only shows as pictures.
# It only READS the Steam folder; nothing there is changed.
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
$hires = Join-Path $src 'hires'
if (-not (Test-Path -LiteralPath $hires)) {
    Write-Host 'The game has no hires folder.'
    [void](Read-Host 'Press Enter to close'); exit 1
}
$dst = Join-Path $modDir 'Game Files\rebirth-hires'
Write-Host "Copying from: $hires"
Write-Host "Copying to: $dst"

# The folder list, so it is clear what the HD project contains.
$dirs = @(Get-ChildItem -LiteralPath $hires -Recurse -Directory | ForEach-Object { $_.FullName.Substring($hires.Length).TrimStart('\') })
New-Item -ItemType Directory -Force -Path $dst | Out-Null
$all = @(Get-ChildItem -LiteralPath $hires -Recurse -File)
$sum = @('HD project folders and file counts (hires):')
foreach ($d in (@('') + $dirs)) {
    $full = if ($d) { Join-Path $hires $d } else { $hires }
    $n = @(Get-ChildItem -LiteralPath $full -File).Count
    if ($n) { $sum += ("{0}  {1} files" -f ($(if ($d) { $d } else { '(top)' })), $n) }
}
Set-Content -LiteralPath (Join-Path $dst '_hires-folders.txt') -Value $sum -Encoding UTF8

# Copy every file whose path mentions common\data (any depth), plus any file named like the
# story and opening cards (open??, st?_*, title*, type*, warning*, select*, result*).
$want = $all | Where-Object {
    $rel = $_.FullName.Substring($hires.Length).TrimStart('\')
    ($rel -match '(?i)common\\data\\') -or ($_.Name -match '(?i)^(open\d\d|st\d_|title|type\d|warning|gwarning|select|result|res_moji|moji|clear|end\d\d)')
}
$size = 0
foreach ($f in $want) {
    $rel = $f.FullName.Substring($hires.Length).TrimStart('\')
    $to = Join-Path $dst $rel
    $toDir = Split-Path -Parent $to
    if (-not (Test-Path -LiteralPath $toDir)) { New-Item -ItemType Directory -Force -Path $toDir | Out-Null }
    Copy-Item -LiteralPath $f.FullName -Destination $to -Force
    $size += $f.Length
}
Write-Host ''
Write-Host ("Copied {0} files, {1:N0} MB. The folder list is in _hires-folders.txt." -f @($want).Count, ($size / 1MB))
Write-Host 'Done. The Steam folder was not changed.'
try { [console]::Beep(880, 200); [console]::Beep(1175, 300) } catch {}
try { $Host.UI.RawUI.FlushInputBuffer() } catch {}
[void](Read-Host 'Press Enter to close')
