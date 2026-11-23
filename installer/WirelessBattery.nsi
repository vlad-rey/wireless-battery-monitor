; Wireless Battery Monitor: NSIS installer script.
; Output: dist\WirelessBattery-Setup-<version>.exe
; Per-user install, no administrator rights needed.

Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"

!define APP_NAME        "Wireless Battery Monitor"
!define APP_ID          "WirelessBatteryMonitor"
!define APP_VERSION     "1.0.1"
!define APP_EXE         "WirelessBattery.exe"
!define APP_PUBLISHER   "Wireless Battery Monitor contributors"
!define APP_URL         "https://github.com/vlad-rey/wireless-battery-monitor"
!define ROOT            ".."

Name "${APP_NAME}"
OutFile "${ROOT}\dist\WirelessBattery-Setup-${APP_VERSION}.exe"
InstallDir "$LOCALAPPDATA\Programs\${APP_ID}"
InstallDirRegKey HKCU "Software\${APP_ID}" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 32
BrandingText "${APP_NAME}"

VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName"     "${APP_NAME}"
VIAddVersionKey /LANG=1033 "FileDescription" "${APP_NAME} Setup"
VIAddVersionKey /LANG=1033 "FileVersion"     "${APP_VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion"  "${APP_VERSION}"
VIAddVersionKey /LANG=1033 "CompanyName"     "${APP_PUBLISHER}"
VIAddVersionKey /LANG=1033 "LegalCopyright"  "GPLv3+"

!define MUI_ICON          "${ROOT}\res\app.ico"
!define MUI_UNICON        "${ROOT}\res\app.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Welcome to ${APP_NAME} Setup"
!define MUI_WELCOMEPAGE_TEXT "${APP_NAME} shows the battery levels of your wireless mouse, headset, keyboard and controller on the Windows taskbar.$\r$\n$\r$\nIt installs for the current user only and needs no administrator rights.$\r$\n$\r$\nClick Next to continue."
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP_NAME} now"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.txt"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Show the README"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------- install

Function QuitRunningApp
  IfFileExists "$INSTDIR\${APP_EXE}" 0 done
  DetailPrint "Closing the running app..."
  ExecWait '"$INSTDIR\${APP_EXE}" --quit'
  Sleep 600
  done:
FunctionEnd

Section "Wireless Battery Monitor (required)" SecMain
  SectionIn RO
  Call QuitRunningApp
  SetOutPath "$INSTDIR"
  File "${ROOT}\WirelessBattery.exe"
  File "${ROOT}\third_party\headsetcontrol\headsetcontrol.exe"
  File /oname=LICENSE.txt "${ROOT}\LICENSE"
  File "${ROOT}\installer\THIRD_PARTY_NOTICES.txt"
  File "${ROOT}\installer\README.txt"

  CreateDirectory "$SMPROGRAMS"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"

  WriteRegStr   HKCU "Software\${APP_ID}" "InstallDir" "$INSTDIR"
  WriteRegStr   HKCU "Software\${APP_ID}" "Version" "${APP_VERSION}"

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegStr   HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_ID}" "DisplayName"     "${APP_NAME}"
  WriteRegStr   HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_ID}" "DisplayIcon"     "$INSTDIR\${APP_EXE}"
  WriteRegStr   HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_ID}" "DisplayVersion"  "${APP_VERSION}"
  WriteRegStr   HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_ID}" "Publisher"       "${APP_PUBLISHER}"
  WriteRegStr   HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_ID}" "HelpLink"        "${APP_URL}"
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_ID}" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_ID}" "NoRepair" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_ID}" "EstimatedSize" $0
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr   HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_ID}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
SectionEnd

; The same quoted value the app's own "Start with Windows" menu item writes, so the menu shows it as on.
Section /o "Start with Windows" SecAutostart
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "WirelessBattery" '"$INSTDIR\${APP_EXE}"'
  ; Mark the entry enabled: some Windows 11 builds skip Run entries without this flag.
  WriteRegBin HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" "WirelessBattery" 020000000000000000000000
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "The taskbar battery widget, and HeadsetControl for some headsets."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAutostart} "Start when you sign in to Windows (can also be changed later in the app's menu)."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---------------------------------------------------------------- uninstall

Section "Uninstall"
  ExecWait '"$INSTDIR\${APP_EXE}" --quit'
  Sleep 600

  Delete "$SMPROGRAMS\${APP_NAME}.lnk"
  Delete "$INSTDIR\${APP_EXE}"
  Delete "$INSTDIR\headsetcontrol.exe"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\THIRD_PARTY_NOTICES.txt"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir  "$INSTDIR"

  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "WirelessBattery"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" "WirelessBattery"
  DeleteRegKey  HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_ID}"
  DeleteRegKey  HKCU "Software\${APP_ID}"
  DeleteRegKey  HKCU "Software\WirelessBattery"
SectionEnd
