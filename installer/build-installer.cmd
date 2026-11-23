@echo off
setlocal enabledelayedexpansion
rem Builds the installer.
rem Needs: NSIS (makensis on PATH) and a previously built WirelessBattery.exe (see ..\build.bat).

cd /d "%~dp0.."

if not exist "WirelessBattery.exe" (
    echo [error] WirelessBattery.exe not found. Build the application first with build.bat.
    exit /b 1
)

where makensis >nul 2>&1
if errorlevel 1 (
    echo [error] makensis not found. Install NSIS 3.x ^(winget install NSIS.NSIS^) and try again.
    exit /b 1
)

set HC=third_party\headsetcontrol\headsetcontrol.exe
set HCSHA=e371da043710ddb8fc70af77f2ccad1cd03f8c6f49ba423e08764ad7a94a3c40

if not exist "%HC%" (
    echo [1/2] Downloading HeadsetControl 4.3.0 ...
    if not exist "third_party\headsetcontrol" mkdir "third_party\headsetcontrol"
    curl -L -o "%HC%" "https://github.com/Sapd/HeadsetControl/releases/download/4.3.0/headsetcontrol-windows-x86_64.exe"
    if errorlevel 1 (
        echo [error] Download failed. Put headsetcontrol.exe ^(4.3.0, win64^) into third_party\headsetcontrol\ manually.
        exit /b 1
    )
)

for /f "skip=1 delims=" %%H in ('certutil -hashfile "%HC%" SHA256') do (
    set ACT=%%H
    goto :havehash
)
:havehash
set ACT=%ACT: =%
if /i not "%ACT%"=="%HCSHA%" (
    echo [error] headsetcontrol.exe SHA-256 mismatch:
    echo         expected %HCSHA%
    echo         actual   %ACT%
    exit /b 1
)

if not exist "dist" mkdir "dist"
echo [2/2] Running makensis ...
makensis "installer\WirelessBattery.nsi"
if errorlevel 1 exit /b 1

echo.
echo Done: dist\WirelessBattery-Setup-1.0.0.exe
