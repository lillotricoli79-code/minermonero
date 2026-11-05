@echo off
setlocal
cd /d "%~dp0"
where py >nul 2>&1
if not errorlevel 1 (
  py -3 app.py
) else (
  where python >nul 2>&1
  if errorlevel 1 (
    echo Python 3 non trovato. Installa Python 3.10 o superiore.
    pause
    exit /b 1
  )
  python app.py
)
if errorlevel 1 pause
