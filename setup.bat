@echo off
setlocal EnableExtensions
call :run_setup
set "setup_result=%errorlevel%"
if not "%setup_result%"=="0" (
  echo.
  echo =============================================
  echo SETUP FAILED - Exit code %setup_result%
  echo The window will stay open so you can read the error.
  echo =============================================
  pause
)
exit /b %setup_result%

:run_setup
setlocal EnableExtensions
cd /d "%~dp0"
where cmake >nul 2>nul || (echo ERROR: CMake is required. Install Visual Studio 2022 with Desktop development with C++ and CMake tools. & exit /b 1)
if not exist ".deps\pdfium\bin\pdfium.dll" (
  echo Downloading PDFium once into .deps...
  powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $url='https://github.com/bblanchon/pdfium-binaries/releases/latest/download/pdfium-win-x64.tgz'; New-Item -ItemType Directory -Force '.deps\pdfium' | Out-Null; Invoke-WebRequest $url -OutFile '.deps\pdfium.tgz'; tar -xzf '.deps\pdfium.tgz' -C '.deps\pdfium'; if($LASTEXITCODE -ne 0){throw 'PDFium extraction failed'}; Move-Item '.deps\pdfium\include' '.deps\pdfium\public'; Remove-Item '.deps\pdfium.tgz'"
  if errorlevel 1 (echo ERROR: PDFium download failed. & exit /b 1)
)
if not exist ".deps\pdfium\lib\pdfium.dll.lib" (echo ERROR: PDFium import library not found. & exit /b 1)
echo Configuring development build...
cmake -S . -B .build -G "Visual Studio 17 2022" -A x64 -DEMBED_PDFIUM=OFF
if errorlevel 1 exit /b 1
cmake --build .build --config Release --parallel
if errorlevel 1 exit /b 1
echo.
echo Setup complete. Use launch.bat to run without compiling.
exit /b 0
