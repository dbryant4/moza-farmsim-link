; Installer for MOZA FS25 Link. Built by scripts/package-release.sh:
;   makensis -DVERSION=0.1.3-pre1 -DNUMVERSION=0.1.3 -DSOURCE=<folder with the files> -DOUTFILE=<setup.exe> installer/moza-farmsim-link.nsi
; VERSION is what people see; NUMVERSION is the same without any "-preN", for where Windows wants numbers only.
;
; Installs per user (no administrator prompt), so the program can update itself later.
; MOZA_SDK.dll is MOZA's to distribute and is not inside this installer: on a first install the
; user downloads MOZA's Flight SDK zip and points to it, and the one file needed is pulled out.
;
; Switches, for updates and scripted installs:
;   /S            silent
;   /SDK=<path>   MOZA's Flight SDK zip, or MOZA_SDK.dll itself (first install when silent)
;   /NOMOD        do not touch Farming Simulator's mods folder (also understood by the uninstaller)
;   /RESTART      start the program when a silent install has finished
;   /D=<folder>   install somewhere else (must be last)

Unicode true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

!define APP_NAME "MOZA FS25 Link"
!define APP_EXE "moza_fs_link.exe"
!define MOD_ZIP "FS25_MozaYokeLink.zip"
!define SDK_URL "https://mozaracing.com/pages/sdk"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\MozaFarmsimLink"
!define SETTINGS_KEY "Software\MozaFarmsimLink"

Name "${APP_NAME}"
OutFile "${OUTFILE}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\${APP_NAME}"
InstallDirRegKey HKCU "${SETTINGS_KEY}" "InstallDir"
VIProductVersion "${NUMVERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "FileDescription" "${APP_NAME} setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "MIT licence"

Var SdkPath
Var SdkField
Var NoMod
Var Restart

!define MUI_ICON "..\bridge\windows\app.ico"
!define MUI_UNICON "..\bridge\windows\app.ico"
!define MUI_ABORTWARNING
!define MUI_CUSTOMFUNCTION_GUIINIT RememberEarlierChoices

!define MUI_WELCOMEPAGE_TEXT "This installs ${APP_NAME} ${VERSION}: force feedback for Farming Simulator 25 on a MOZA flight yoke.$\r$\n$\r$\nIt installs for your Windows account only and needs no administrator rights. MOZA Cockpit must already be installed.$\r$\n$\r$\nIf ${APP_NAME} is running, it is asked to release the yoke and close first."
!insertmacro MUI_PAGE_WELCOME
Page custom SdkPageCreate SdkPageLeave
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP_NAME} now (it appears as an icon by the clock)"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------------------------
; MOZA's SDK

; Puts MOZA_SDK.dll from $SdkPath (a Flight SDK zip, or the dll) into $PLUGINSDIR.
; Leaves "ok" on the stack, or a sentence saying what was wrong.
Function ExtractSdk
  Delete "$PLUGINSDIR\MOZA_SDK.dll"
  ${IfNot} ${FileExists} "$SdkPath"
    Push "That file does not exist."
    Return
  ${EndIf}

  ${GetFileExt} "$SdkPath" $0
  ${If} $0 == "dll"
    CopyFiles /SILENT "$SdkPath" "$PLUGINSDIR\MOZA_SDK.dll"
  ${Else}
    ; The paths travel in environment variables so that no quoting can go wrong, whatever they contain.
    System::Call 'Kernel32::SetEnvironmentVariable(t "MOZA_SDK_SOURCE", t "$SdkPath")'
    System::Call 'Kernel32::SetEnvironmentVariable(t "MOZA_SDK_TARGET", t "$PLUGINSDIR\MOZA_SDK.dll")'
    ; Backticks delimit the NSIS string so that PowerShell can use both kinds of quote inside it.
    nsExec::ExecToStack `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "try { Add-Type -AssemblyName System.IO.Compression.FileSystem; $$zip = [IO.Compression.ZipFile]::OpenRead($$env:MOZA_SDK_SOURCE); $$entry = $$zip.Entries | Where-Object { $$_.FullName -notlike '__MACOSX*' -and $$_.FullName -like '*x64-release/bin/MOZA_SDK.dll' } | Select-Object -First 1; if (-not $$entry) { $$zip.Dispose(); exit 3 }; [IO.Compression.ZipFileExtensions]::ExtractToFile($$entry, $$env:MOZA_SDK_TARGET, $$true); $$zip.Dispose(); exit 0 } catch { exit 4 }"`
    Pop $0 ; exit code
    Pop $1 ; output, unused
    ${If} $0 == "3"
      Push "That zip does not contain x64-release\bin\MOZA_SDK.dll. Make sure it is the Flight SDK from MOZA's SDK page, not the Racing SDK or another download."
      Return
    ${ElseIf} $0 != "0"
      Push "That file could not be read as a zip. Download the Flight SDK again from MOZA's SDK page and point to the zip as it was downloaded."
      Return
    ${EndIf}
  ${EndIf}

  ${IfNot} ${FileExists} "$PLUGINSDIR\MOZA_SDK.dll"
    Push "MOZA_SDK.dll could not be copied out of that file."
    Return
  ${EndIf}
  ; The real file is about 14 MB; anything tiny is not it.
  FileOpen $0 "$PLUGINSDIR\MOZA_SDK.dll" r
  FileSeek $0 0 END $1
  FileClose $0
  ${If} $1 < 1000000
    Delete "$PLUGINSDIR\MOZA_SDK.dll"
    Push "That file is too small to be MOZA_SDK.dll."
    Return
  ${EndIf}
  Push "ok"
FunctionEnd

Function OpenSdkPage
  ExecShell "open" "${SDK_URL}"
FunctionEnd

Function BrowseForSdk
  nsDialogs::SelectFileDialog open "$SdkPath" "MOZA Flight SDK (*.zip;MOZA_SDK.dll)|*.zip;MOZA_SDK.dll|All files|*.*"
  Pop $0
  ${If} $0 != ""
    StrCpy $SdkPath $0
    ${NSD_SetText} $SdkField $SdkPath
  ${EndIf}
FunctionEnd

Function SdkPageCreate
  ; An update: the file from the first install is still there.
  ${If} ${FileExists} "$INSTDIR\MOZA_SDK.dll"
    Abort
  ${EndIf}

  !insertmacro MUI_HEADER_TEXT "One file from MOZA" "${APP_NAME} talks to the yoke through MOZA's SDK, which only MOZA hands out."
  nsDialogs::Create 1018
  Pop $0

  ${NSD_CreateLabel} 0 0 100% 56u "1. Open MOZA's SDK page and download the Flight SDK. The page also offers a Racing SDK; that is not the one.$\r$\n$\r$\n2. Point to the zip you downloaded. There is no need to unpack it: the one file needed, MOZA_SDK.dll, is copied out of it.$\r$\n$\r$\nThis is only asked once. Updates keep the file."
  Pop $0
  ${NSD_CreateButton} 0 60u 110u 15u "Open MOZA's SDK page"
  Pop $0
  ${NSD_OnClick} $0 OpenSdkPage

  ${NSD_CreateLabel} 0 88u 100% 10u "The downloaded Flight SDK zip:"
  Pop $0
  ${NSD_CreateFileRequest} 0 100u 236u 13u "$SdkPath"
  Pop $SdkField
  ${NSD_CreateBrowseButton} 242u 100u 58u 13u "Browse..."
  Pop $0
  ${NSD_OnClick} $0 BrowseForSdk

  nsDialogs::Show
FunctionEnd

Function SdkPageLeave
  ${NSD_GetText} $SdkField $SdkPath
  ${If} $SdkPath == ""
    MessageBox MB_OK|MB_ICONEXCLAMATION "Point to the Flight SDK zip you downloaded from MOZA's SDK page."
    Abort
  ${EndIf}
  Call ExtractSdk
  Pop $0
  ${If} $0 != "ok"
    MessageBox MB_OK|MB_ICONEXCLAMATION "$0"
    Abort
  ${EndIf}
FunctionEnd

; ---------------------------------------------------------------------------------------------
; Helpers

; Asks a running copy to release the yoke and exit, and waits until its file can be replaced.
!macro CloseRunningCopy
  ${If} ${FileExists} "$INSTDIR\${APP_EXE}"
    nsExec::Exec '"$INSTDIR\${APP_EXE}" --quit'
    Pop $0
    StrCpy $1 0
    ${Do}
      ClearErrors
      FileOpen $0 "$INSTDIR\${APP_EXE}" a
      ${IfNot} ${Errors}
        FileClose $0
        ${ExitDo}
      ${EndIf}
      Sleep 500
      IntOp $1 $1 + 1
    ${LoopUntil} $1 >= 40
  ${EndIf}
!macroend

; ---------------------------------------------------------------------------------------------
; Install

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "${APP_NAME} needs 64-bit Windows."
    Abort
  ${EndIf}
  InitPluginsDir

  StrCpy $NoMod 0
  StrCpy $Restart 0
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/SDK=" $SdkPath
  ClearErrors
  ${GetOptions} $R0 "/NOMOD" $R1
  ${IfNot} ${Errors}
    StrCpy $NoMod 1
  ${EndIf}
  ClearErrors
  ${GetOptions} $R0 "/RESTART" $R1
  ${IfNot} ${Errors}
    StrCpy $Restart 1
  ${EndIf}

  ; Without the wizard there is no page to ask on, so the switch has to carry the SDK.
  ${If} ${Silent}
  ${AndIfNot} ${FileExists} "$INSTDIR\MOZA_SDK.dll"
    ${If} $SdkPath == ""
      SetErrorLevel 2
      Abort
    ${EndIf}
    Call ExtractSdk
    Pop $0
    ${If} $0 != "ok"
      SetErrorLevel 3
      Abort
    ${EndIf}
  ${EndIf}
FunctionEnd

Section "${APP_NAME}" SecApp
  SectionIn RO
  !insertmacro CloseRunningCopy

  SetOutPath "$INSTDIR"
  File "${SOURCE}\moza_fs_link.exe"
  File "${SOURCE}\moza_fs_bridge.exe"
  File "${SOURCE}\${MOD_ZIP}"
  File "${SOURCE}\INSTALL.md"
  File "${SOURCE}\LICENSE.txt"
  ${If} ${FileExists} "$PLUGINSDIR\MOZA_SDK.dll"
    CopyFiles /SILENT "$PLUGINSDIR\MOZA_SDK.dll" "$INSTDIR\MOZA_SDK.dll"
  ${EndIf}

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKCU "${SETTINGS_KEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "moza-farmsim-link"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout" "https://github.com/dbryant4/moza-farmsim-link"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "EstimatedSize" "$0"

  CreateShortcut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
SectionEnd

Section "Farming Simulator 25 mod" SecMod
  ${If} $NoMod == 1
    Return
  ${EndIf}
  ; The program also does this itself whenever it starts and finds the installed mod out of date,
  ; which is what covers the case where the game is running now and has the file locked.
  ${If} ${FileExists} "$DOCUMENTS\My Games\FarmingSimulator2025\*.*"
    CreateDirectory "$DOCUMENTS\My Games\FarmingSimulator2025\mods"
    ClearErrors
    CopyFiles /SILENT "$INSTDIR\${MOD_ZIP}" "$DOCUMENTS\My Games\FarmingSimulator2025\mods\${MOD_ZIP}"
    ${If} ${Errors}
      DetailPrint "The mod could not be replaced (is Farming Simulator running?). ${APP_NAME} will do it when it next starts with the game closed."
    ${Else}
      DetailPrint "Mod installed in Farming Simulator 25's mods folder."
    ${EndIf}
  ${Else}
    DetailPrint "Farming Simulator 25's folder was not found; the mod is in $INSTDIR\${MOD_ZIP}."
  ${EndIf}
SectionEnd

Section /o "Start with Windows" SecStartup
  CreateShortcut "$SMSTARTUP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecApp} "The program itself: it sits as an icon by the clock, and a click on the icon opens its settings."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMod} "The FS25 mod that tells the program what the vehicle is doing. Enable it for your savegame in the game's mod list."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStartup} "Start ${APP_NAME} whenever you sign in to Windows."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A shortcut on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; An update keeps what the user chose before.
Function RememberEarlierChoices
  ${If} ${FileExists} "$SMSTARTUP\${APP_NAME}.lnk"
    SectionGetFlags ${SecStartup} $0
    IntOp $0 $0 | ${SF_SELECTED}
    SectionSetFlags ${SecStartup} $0
  ${EndIf}
  ${If} ${FileExists} "$DESKTOP\${APP_NAME}.lnk"
    SectionGetFlags ${SecDesktop} $0
    IntOp $0 $0 | ${SF_SELECTED}
    SectionSetFlags ${SecDesktop} $0
  ${EndIf}
FunctionEnd

Function .onInstSuccess
  ${If} ${Silent}
  ${AndIf} $Restart == 1
    Exec '"$INSTDIR\${APP_EXE}"'
  ${EndIf}
FunctionEnd

; ---------------------------------------------------------------------------------------------
; Uninstall

Function un.onInit
  StrCpy $NoMod 0
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/NOMOD" $R1
  ${IfNot} ${Errors}
    StrCpy $NoMod 1
  ${EndIf}
FunctionEnd

Section "Uninstall"
  !insertmacro CloseRunningCopy

  Delete "$SMPROGRAMS\${APP_NAME}.lnk"
  Delete "$SMSTARTUP\${APP_NAME}.lnk"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  ${If} $NoMod != 1
    ; The game may be running with the mod open; then it stays and can be deleted by hand.
    Delete "$DOCUMENTS\My Games\FarmingSimulator2025\mods\${MOD_ZIP}"
  ${EndIf}

  Delete "$INSTDIR\moza_fs_link.exe"
  Delete "$INSTDIR\moza_fs_bridge.exe"
  Delete "$INSTDIR\MOZA_SDK.dll"
  Delete "$INSTDIR\${MOD_ZIP}"
  Delete "$INSTDIR\INSTALL.md"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\moza_fs_bridge.log"
  Delete "$INSTDIR\devsdk.log"
  Delete "$INSTDIR\yoke_lights_original.txt"
  Delete "$INSTDIR\moza_fs_link_stuck.dmp"
  Delete "$INSTDIR\uninstall.exe"

  ${IfNot} ${Silent}
    MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Also remove your saved settings (moza_fs_bridge.ini)?" IDNO keepSettings
    Delete "$INSTDIR\moza_fs_bridge.ini"
    keepSettings:
  ${EndIf}
  RMDir "$INSTDIR"

  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  DeleteRegKey HKCU "${SETTINGS_KEY}"
SectionEnd
