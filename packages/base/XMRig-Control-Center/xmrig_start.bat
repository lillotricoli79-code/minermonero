@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0xmrig-shell.ps1" -Action Start
exit /b %errorlevel%
