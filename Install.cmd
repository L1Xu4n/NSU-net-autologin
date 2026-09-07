@echo off
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0CampusLogin.ps1" -Install
pause
