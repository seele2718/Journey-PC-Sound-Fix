@echo off
setlocal
if "%~1"=="" (
  echo Drag your original Journey.exe onto this file.
  exit /b 2
)
py -3 -c "import sys" >nul 2>&1
if not errorlevel 1 (
  py -3 "%~dp0install.py" "%~1"
) else (
  python "%~dp0install.py" "%~1"
)
set "rc=%errorlevel%"
if not "%rc%"=="0" (
  echo Journey.sound-fix.exe was not created.
) else (
  echo Journey.sound-fix.exe was created beside your original executable.
)
pause
exit /b %rc%
