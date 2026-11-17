@echo off
setlocal EnableExtensions
call :build_release
set "build_result=%errorlevel%"
if not "%build_result%"=="0" (
  echo.
  echo =============================================
  echo BUILD FAILED - Exit code %build_result%
  echo The window will stay open so you can read the error.
  echo =============================================
  pause
)
exit /b %build_result%

:build_release
setlocal EnableExtensions
cd /d "%~dp0"
if not exist ".deps\pdfium\bin\pdfium.dll" (echo ERROR: Run setup.bat first. & exit /b 1)
echo Creating single-file portable release...
cmake -S . -B .release -G "Visual Studio 17 2022" -A x64 -DEMBED_PDFIUM=ON
if errorlevel 1 exit /b 1
cmake --build .release --config Release --parallel
if errorlevel 1 exit /b 1
if not exist dist mkdir dist
copy /y ".release\Release\quickDoc.exe" "dist\quickDoc.exe" >nul
if errorlevel 1 exit /b 1
echo.
echo Release: dist\quickDoc.exe
 echo NOTE: Single-file distribution. PDFium is extracted to LocalAppData cache on first launch.
exit /b 0
