' Starts the kiosk watchdog (watchdog.ps1 beside this file) with no visible window.
'
' Why a script host and not powershell.exe or conhost.exe directly: on Windows 11 the default-terminal setting
' hands every new console to Windows Terminal, which ignores -WindowStyle Hidden and leaves an empty terminal
' window over the XMB. WScript.Shell.Run with window style 0 starts PowerShell hidden without that hand-off.
' conhost.exe --headless also hid the window, but the XMB it started then drew at about 0.5 fps instead of 60
' (measured on the HTPC 2026-11-16); this launcher measured 0 terminal windows and 60 fps.
'
' Usage: wscript.exe //B //Nologo kiosk-launch.vbs [display]
Option Explicit
Dim fso, folder, display, command
Set fso = CreateObject("Scripting.FileSystemObject")
folder = fso.GetParentFolderName(WScript.ScriptFullName)
display = "0"
If WScript.Arguments.Count > 0 Then display = WScript.Arguments(0)
command = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & folder & "\watchdog.ps1"" -Display " & display
CreateObject("WScript.Shell").Run command, 0, False
