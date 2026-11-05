@echo off
setlocal
set "AI_DIR=%~dp0..\..\aiqween\XMRig-Control-Center-AIQween-module"
if not exist "%AI_DIR%\tests\test_engine.py" (
  echo Modulo AI Qween o test non trovati.
  echo Mantieni insieme le cartelle packages\base e packages\aiqween.
  pause
  exit /b 1
)
pushd "%AI_DIR%"
py -3 -m unittest discover -s tests -v
set "RC=%ERRORLEVEL%"
popd
if not "%RC%"=="0" pause
exit /b %RC%
