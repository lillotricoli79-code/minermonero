@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_release.ps1"
if errorlevel 1 (
  echo Creazione pacchetto non riuscita.
  pause
  exit /b 1
)
echo Pacchetto sorgenti creato nella cartella dist del repository.
pause
