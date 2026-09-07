@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0CampusLogin.ps1" -Uninstall
pause
