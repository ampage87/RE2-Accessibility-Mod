' ============================================================
' Resident Evil 2 Accessibility Mod - Deploy
' Double-click to build the mod and install it into the game.
' A small progress window reports each step, then a message
' box says whether it worked. No console window appears.
' ============================================================
Set fso = CreateObject("Scripting.FileSystemObject")
Set WshShell = CreateObject("WScript.Shell")
scriptDir = fso.GetParentFolderName(WScript.ScriptFullName)
ps1Path = fso.BuildPath(scriptDir, "Mod\src\deploy.ps1")
WshShell.CurrentDirectory = scriptDir
WshShell.Run "powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & ps1Path & """", 0, True
