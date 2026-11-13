@echo off
rem Baut MiniDAW auf diesem Rechner. Voraussetzung: Visual Studio 2022 oder die
rem "Build Tools fuer Visual Studio 2022" mit "Desktopentwicklung mit C++".
rem Ergebnis: build\Release\minidaw.exe
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs

call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -no_logo
if errorlevel 1 goto failed

cmake -B build
if errorlevel 1 goto failed
cmake --build build --config Release
if errorlevel 1 goto failed
ctest --test-dir build -C Release --output-on-failure
if errorlevel 1 goto failed

echo.
echo Fertig: %CD%\build\Release\minidaw.exe
certutil -hashfile build\Release\minidaw.exe SHA256
exit /b 0

:novs
echo Visual Studio mit C++ wurde nicht gefunden.
echo Bitte die "Build Tools fuer Visual Studio 2022" installieren und dabei
echo "Desktopentwicklung mit C++" auswaehlen.
exit /b 1

:failed
echo.
echo Der Build ist fehlgeschlagen. Die Meldungen oben zeigen den Grund.
exit /b 1
