@echo off
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0CampusLogin.ps1"
echo Exit code: %errorlevel%
echo 0=Success  1=Failed; see local log  2=Configuration cancelled  6=Already running
pause
