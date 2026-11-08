@echo off
setlocal

rem MSR requires an elevated process on Windows. Relaunch this BAT through UAC.
fltmc >nul 2>&1
if not "%errorlevel%"=="0" (
    echo Richiesta autorizzazione amministratore per attivare MSR...
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" %*
exit /b %errorlevel%
