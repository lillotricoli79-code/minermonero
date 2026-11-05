@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0download_xmrig.ps1"
if errorlevel 1 (
  echo Download/installazione XMRig non riuscita.
  pause
  exit /b 1
)
pause
