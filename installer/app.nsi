; Per-app installer for the Afterburner suite.
; build.sh compiles this once per app with:
;   -DAPPKEY=<exe base name> -DAPPNAME=<display name> -DAPPGUID=<unique GUID>
;   -DVERSION=x.y.z -DSRCDIR=<dir holding the exe> -DOUTFILE=<setup.exe>
; Each app gets its own install folder, uninstall registry key ({GUID}),
; Start-menu shortcut and uninstaller, so every app can be installed,
; updated and removed side by side without touching the others.

Target amd64-unicode   ; native 64-bit installer (the apps are 64-bit only)
SetCompressor /SOLID lzma
RequestExecutionLevel admin
!include "MUI2.nsh"
!include "LogicLib.nsh"

Name "${APPNAME}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\Afterburner\${APPNAME}"
InstallDirRegKey HKLM "Software\CoolCupMan\Afterburner\${APPKEY}" "InstallDir"
BrandingText "Afterburner Suite"

!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\{${APPGUID}}"
!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APPKEY}.exe"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "FileDescription" "${APPNAME} Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "CoolCupMan"
VIAddVersionKey "LegalCopyright" "CoolCupMan"

Function .onInit
  SetRegView 64
  SetShellVarContext all   ; per-machine install -> shortcuts for all users
  ; a running copy of THIS app only (other suite apps keep running)
  FindWindow $0 "Afterburner.${APPKEY}.Wnd"
  ${If} $0 != 0
    MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION "${APPNAME} is running. Close it and click OK." /SD IDOK IDOK +2
    Abort
    SendMessage $0 0x0010 0 0 /TIMEOUT=5000
  ${EndIf}
FunctionEnd

Section "${APPNAME} (required)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "${SRCDIR}\${APPKEY}.exe"
  ; marker: keep settings/logs in %LOCALAPPDATA%\Afterburner\${APPKEY}
  FileOpen $0 "$INSTDIR\installed.txt" w
  FileWrite $0 "Installed by setup. Data folder: %LOCALAPPDATA%\Afterburner\${APPKEY}$\r$\n"
  FileClose $0
  WriteUninstaller "$INSTDIR\Uninstall ${APPKEY}.exe"

  CreateDirectory "$SMPROGRAMS\Afterburner"
  CreateShortcut "$SMPROGRAMS\Afterburner\${APPNAME}.lnk" "$INSTDIR\${APPKEY}.exe"
  ExecWait '"$INSTDIR\${APPKEY}.exe" --set-shortcut-appid "$SMPROGRAMS\Afterburner\${APPNAME}.lnk"'

  WriteRegStr HKLM "Software\CoolCupMan\Afterburner\${APPKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "CoolCupMan"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\${APPKEY}.exe"
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall ${APPKEY}.exe"'
  WriteRegStr HKLM "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall ${APPKEY}.exe" /S'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "EstimatedSize" 600
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${APPKEY}.exe"
  ExecWait '"$INSTDIR\${APPKEY}.exe" --set-shortcut-appid "$DESKTOP\${APPNAME}.lnk"'
SectionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Section "Uninstall"
  FindWindow $0 "Afterburner.${APPKEY}.Wnd"
  ${If} $0 != 0
    SendMessage $0 0x0010 0 0 /TIMEOUT=5000
    Sleep 500
  ${EndIf}
  Delete "$INSTDIR\${APPKEY}.exe"
  Delete "$INSTDIR\installed.txt"
  Delete "$INSTDIR\Uninstall ${APPKEY}.exe"
  RMDir "$INSTDIR"
  RMDir "$INSTDIR\.."
  Delete "$SMPROGRAMS\Afterburner\${APPNAME}.lnk"
  RMDir "$SMPROGRAMS\Afterburner"
  Delete "$DESKTOP\${APPNAME}.lnk"
  DeleteRegKey HKLM "${UNINSTKEY}"
  DeleteRegKey HKLM "Software\CoolCupMan\Afterburner\${APPKEY}"
  DeleteRegKey /ifempty HKLM "Software\CoolCupMan\Afterburner"
  ; user data in %LOCALAPPDATA%\Afterburner\${APPKEY} is kept on purpose
SectionEnd
