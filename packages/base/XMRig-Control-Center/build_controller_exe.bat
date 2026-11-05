@echo off
setlocal
cd /d "%~dp0"
where py >nul 2>&1
if errorlevel 1 (
  echo Python Launcher non trovato. Installa Python 3.10 o superiore.
  pause
  exit /b 1
)
py -3 -c "import PyInstaller" >nul 2>&1
if errorlevel 1 (
  echo Installazione PyInstaller...
  py -3 -m pip install --user pyinstaller
  if errorlevel 1 exit /b 1
)
set "AI_DIR=%~dp0..\..\aiqween\XMRig-Control-Center-AIQween-module"
if exist "%AI_DIR%\ai_qween" (
  py -3 -m PyInstaller --noconfirm --clean --windowed --name XMRigControlCenter --paths "%AI_DIR%" app.py
) else (
  py -3 -m PyInstaller --noconfirm --clean --windowed --name XMRigControlCenter app.py
)
if errorlevel 1 (
  echo Build EXE non riuscita.
  pause
  exit /b 1
)
echo Build completata. Controlla la cartella dist.
pause
