@echo off
rem Builds WirelessBattery.exe with Visual Studio 2022 (Build Tools are enough) instead of MinGW.
rem Every .cpp file in src\ and src\drivers\ is compiled, so a new driver needs no change here.
setlocal enabledelayedexpansion
cd /d "%~dp0"
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (echo vswhere.exe not found: install Visual Studio 2022 or its Build Tools & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo No Visual Studio with the C++ x64 tools was found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build\msvc mkdir build\msvc
rem Every lang\*.txt is a built-in UI language: joined into one resource, each after an "@file <name>" line.
(for %%f in (lang\*.txt) do (echo @file %%~nf& type "%%f"& echo.)) > build\languages.txt
rc /nologo /i res /i build /fo build\msvc\app.res res\app.rc || exit /b 1
set SOURCES=
for %%f in (src\*.cpp src\drivers\*.cpp) do set SOURCES=!SOURCES! %%f
cl /nologo /std:c++17 /O1 /EHsc /MT /utf-8 /W1 /MP /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DNOMINMAX ^
   /Fobuild\msvc\ !SOURCES! build\msvc\app.res /Fe:WirelessBattery.exe ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup hid.lib setupapi.lib cfgmgr32.lib ole32.lib oleaut32.lib comctl32.lib ^
   gdi32.lib user32.lib shell32.lib advapi32.lib runtimeobject.lib ws2_32.lib bthprops.lib dwmapi.lib uxtheme.lib || exit /b 1
echo Built WirelessBattery.exe
