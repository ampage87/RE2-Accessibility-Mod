<# : batch launcher
@echo off
setlocal
title Answer the RE2 pack question
set "RE2ANS_SCRIPT=%~f0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$s = [IO.File]::ReadAllText($env:RE2ANS_SCRIPT); & ([ScriptBlock]::Create($s))"
endlocal
#>

# Finds the Mulderland pack's question window, says what it asks, and answers Yes.
# Only answers the video-title question automatically; anything else is reported, not answered.
$ErrorActionPreference = 'Continue'
$log = Join-Path (Split-Path -Parent $env:RE2ANS_SCRIPT) 'Logs\answer-question.txt'
function Say($m) { Write-Host $m; Add-Content -LiteralPath $log -Value ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m) }

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class Re2Ans {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr d, int id);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static bool HasButton(IntPtr dlg, int id) { return GetDlgItem(dlg, id) != IntPtr.Zero; }
    public static void Answer(IntPtr dlg, int id) { PostMessage(dlg, 0x0111, (IntPtr)id, GetDlgItem(dlg, id)); }
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    public static void ClickButton(IntPtr dlg, int id) { IntPtr b = GetDlgItem(dlg, id); if (b != IntPtr.Zero) SendMessage(b, 0x00F5, IntPtr.Zero, IntPtr.Zero); }
    public static string StaticText(IntPtr dlg) {
        var all = new StringBuilder();
        EnumChildWindows(dlg, (h, l) => {
            var c = new StringBuilder(64); GetClassName(h, c, 64);
            if (c.ToString() == "Static") { var t = new StringBuilder(2048); GetWindowText(h, t, 2048); if (t.Length > 0) all.Append(t.ToString()).Append(' '); }
            return true; }, IntPtr.Zero);
        return all.ToString().Trim();
    }
}
'@

$A = [System.Windows.Automation.AutomationElement]
$found = $false
foreach ($p in @(Get-Process | Where-Object { $_.ProcessName -like '*enhancement-pack*' })) {
    $wins = $A::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children,
        (New-Object System.Windows.Automation.PropertyCondition($A::ProcessIdProperty, [int]$p.Id)))
    foreach ($w in $wins) {
        if ($w.Current.ClassName -ne '#32770') { continue }
        $found = $true
        $msg = [Re2Ans]::StaticText([IntPtr]$w.Current.NativeWindowHandle)
        $btns = $w.FindAll([System.Windows.Automation.TreeScope]::Descendants,
            (New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)))
        $names = @($btns | ForEach-Object { $_.Current.Name -replace '&', '' })
        Say "Window: $($w.Current.Name)"
        Say "Question: $msg"
        Say "Buttons: $($names -join ', ')"
        $h = [IntPtr]$w.Current.NativeWindowHandle
        if ($msg -match "Replace 'BioHazard 2' with 'Resident Evil 2'") {
            $closed = $false
            $tries = @(
                @{ Name = 'Yes command';   Do = { [Re2Ans]::Answer($h, 6) } },
                @{ Name = 'Yes click';     Do = { [Re2Ans]::ClickButton($h, 6) } },
                @{ Name = 'Enter key';     Do = { [void][Re2Ans]::ShowWindow($h, 9); [void][Re2Ans]::SetForegroundWindow($h); Start-Sleep -Milliseconds 400; (New-Object -ComObject WScript.Shell).SendKeys('y') } }
            )
            foreach ($t in $tries) {
                Say "Trying: $($t.Name)."
                try { & $t.Do } catch { Say "  failed: $($_.Exception.Message)" }
                Start-Sleep -Seconds 2
                if (-not [Re2Ans]::IsWindow($h)) { $closed = $true; break }
            }
            if ($closed) { Say 'Answered Yes and the question window has closed. The setup should continue now.' }
            else { Say 'The question window is still open. Press Alt+Tab to the window titled Resident Evil 2 Steam Enhancement Pack Setup and press Y.' }
        } else {
            Say 'This is not the expected question, so it was not answered. Tell Claude what it says.'
        }
    }
}
if (-not $found) { Say 'No question window from the pack is open. The setup is not waiting for an answer.' }
Write-Host ''
[void](Read-Host 'Press Enter to close')
