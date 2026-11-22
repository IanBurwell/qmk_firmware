' Launch sofle_sync.py silently (no console window) on Windows
Set objShell = CreateObject("WScript.Shell")
Set objFSO = CreateObject("Scripting.FileSystemObject")
strDir = objFSO.GetParentFolderName(WScript.ScriptFullName)
objShell.Run "pythonw """ & strDir & "\sofle_sync.py""", 0, False
