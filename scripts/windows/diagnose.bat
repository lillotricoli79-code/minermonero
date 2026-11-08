@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0diagnose.ps1" %*
exit /b %errorlevel%
