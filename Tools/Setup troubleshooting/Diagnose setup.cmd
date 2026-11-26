<# : batch launcher
@echo off
setlocal
title RE2 setup diagnostic
set "RE2DIAG_SCRIPT=%~f0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$s = [IO.File]::ReadAllText($env:RE2DIAG_SCRIPT); & ([ScriptBlock]::Create($s))"
endlocal
#>

# Read-only diagnostic for the Sourcenext setup. Changes nothing.
# Writes Logs\diagnostic.txt next to this script.
$ErrorActionPreference = 'Continue'
$dir = Split-Path -Parent $env:RE2DIAG_SCRIPT
$out = Join-Path $dir 'Logs\diagnostic.txt'
New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
$lines = New-Object System.Collections.Generic.List[string]
function W($m) { $lines.Add([string]$m) }

Write-Host 'Collecting information for about 30 seconds. Nothing is changed.'
W ("Diagnostic at " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))

$log = Get-ChildItem (Join-Path $dir 'Logs') -Filter 'sourcenext-setup-*.log' | Sort-Object LastWriteTime | Select-Object -Last 1
$game = $null
if ($log) {
    W "Latest setup log: $($log.Name)"
    $txt = Get-Content $log.FullName
    $m = $txt | Select-String 'Found the game at: (.+)$' | Select-Object -Last 1
    if ($m) { $game = $m.Matches[0].Groups[1].Value.Trim() }
    W '--- last 15 log lines'; $txt | Select-Object -Last 15 | ForEach-Object { W $_ }
}
W "Game folder: $game"

# Processes
$all = @(Get-CimInstance Win32_Process)
$pack = @($all | Where-Object { $_.Name -like '*enhancement-pack*' })
$ids = @{}; foreach ($p in $pack) { $ids[[int]$p.ProcessId] = $true }
$grew = $true
while ($grew) { $grew = $false; foreach ($p in $all) { if ($ids.ContainsKey([int]$p.ParentProcessId) -and -not $ids.ContainsKey([int]$p.ProcessId)) { $ids[[int]$p.ProcessId] = $true; $grew = $true } } }
W '--- pack process tree'
if ($ids.Count -eq 0) { W 'No enhancement pack process is running.' }

function Snap {
    $r = @{}
    foreach ($id in $ids.Keys) {
        $p = Get-CimInstance Win32_Process -Filter "ProcessId=$id"
        if ($p) { $r[$id] = $p }
    }
    return $r
}
function Net { $s = 0; foreach ($a in (Get-NetAdapterStatistics -ErrorAction SilentlyContinue)) { $s += $a.ReceivedBytes }; return $s }

$roots = @()
if ($game) { $roots += $game }
$roots += $env:TEMP
function Recent {
    $r = @{}
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        Get-ChildItem -LiteralPath $root -Recurse -File -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTime -gt (Get-Date).AddMinutes(-30) } |
            ForEach-Object { $r[$_.FullName] = $_.Length }
    }
    return $r
}

$p1 = Snap; $n1 = Net; $f1 = Recent
Start-Sleep -Seconds 20
$p2 = Snap; $n2 = Net; $f2 = Recent

foreach ($id in $p2.Keys) {
    $a = $p1[$id]; $b = $p2[$id]
    $cpu = 0; $rd = 0; $wr = 0; $ot = 0
    if ($a) {
        $cpu = (($b.KernelModeTime + $b.UserModeTime) - ($a.KernelModeTime + $a.UserModeTime)) / 1e7
        $rd = $b.ReadTransferCount - $a.ReadTransferCount
        $wr = $b.WriteTransferCount - $a.WriteTransferCount
        $ot = $b.OtherTransferCount - $a.OtherTransferCount
    }
    W ("{0} (pid {1}): CPU {2:N1}s in 20s, read {3:N1} MB, written {4:N1} MB, other {5:N1} MB. Command: {6}" -f $b.Name, $id, $cpu, ($rd/1MB), ($wr/1MB), ($ot/1MB), $b.CommandLine)
}
W ("Network received in 20s, all adapters: {0:N1} MB ({1:N2} MB per second)" -f (($n2 - $n1)/1MB), (($n2 - $n1)/1MB/20))

W '--- files changed in the last 30 minutes (game folder and TEMP), largest first, with growth over 20s'
$f2.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 25 | ForEach-Object {
    $before = if ($f1.ContainsKey($_.Key)) { $f1[$_.Key] } else { 0 }
    W ("{0:N1} MB (+{1:N1} MB)  {2}" -f ($_.Value/1MB), (($_.Value - $before)/1MB), $_.Key)
}

W '--- windows owned by the pack process tree'
try {
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $A = [System.Windows.Automation.AutomationElement]
    foreach ($id in $ids.Keys) {
        $wins = $A::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children, (New-Object System.Windows.Automation.PropertyCondition($A::ProcessIdProperty, [int]$id)))
        foreach ($w in $wins) {
            $t = $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition) | ForEach-Object { $_.Current.Name } | Where-Object { $_ }
            W ("pid {0} window '{1}' class {2}: {3}" -f $id, $w.Current.Name, $w.Current.ClassName, (($t | Select-Object -First 20) -join ' | '))
        }
    }
} catch { W "UI Automation failed: $($_.Exception.Message)" }

if ($game) {
    W '--- top of game folder'
    Get-ChildItem -LiteralPath $game -Force | ForEach-Object { W ("{0}  {1}" -f $_.Name, $(if ($_.PSIsContainer) { '<dir>' } else { '{0:N1} MB' -f ($_.Length/1MB) })) }
    $rb = Join-Path $game 'rebirth'
    if (Test-Path -LiteralPath $rb) { W '--- top of rebirth folder'; Get-ChildItem -LiteralPath $rb -Force | ForEach-Object { W ("{0}  {1}" -f $_.Name, $(if ($_.PSIsContainer) { '<dir>' } else { '{0:N1} MB' -f ($_.Length/1MB) })) } }
}

Set-Content -LiteralPath $out -Value $lines -Encoding UTF8
Write-Host "Done. Saved to $out"
Write-Host 'You can close this window, or press Enter.'
[void](Read-Host)
