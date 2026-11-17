@echo off
setlocal
cd /d "%~dp0"
if not exist ".build\Release\quickDoc.exe" (
  echo Development executable missing. Run setup.bat first.
  pause
  exit /b 1
)
start "" ".build\Release\quickDoc.exe" %*
exit /b 0
