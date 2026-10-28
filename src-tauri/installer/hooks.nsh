; Keep compatibility glue separate from the pinned upstream installer template.
!include "${__FILEDIR__}\theme.nsh"
!include "${__FILEDIR__}\icon-refresh.nsh"
Var CliptureRequestedDirectory
Var CliptureLegacyDirectory
Var CliptureLegacyScope
Var CliptureStartupCheckbox
Var CliptureStartupChoice
; 1 when a running Clipture moves itself from Program Files to a per-user
; install (ADR 0020). That Clipture keeps running from elsewhere, so the
; installer must not close it. Clipture moves the user's shortcuts itself.
Var CliptureMigrateMode

!macro CLIPTURE_STARTUP_PAGE
  Page custom CliptureStartupPage CliptureStartupLeave

  Function CliptureStartupPage
    ${If} ${Silent}
    ${OrIf} $PassiveMode = 1
    ${OrIf} $UpdateMode = 1
      Abort
    ${EndIf}
    !insertmacro MUI_HEADER_TEXT "Startup options" "Choose how Clipture starts with Windows."
    nsDialogs::Create 1018
    Pop $0
    ${If} $0 == error
      Abort
    ${EndIf}
    SetCtlColors $0 "272824" "F0F0EB"
    ${If} $CliptureStartupChoice == ""
      StrCpy $CliptureStartupChoice ${BST_CHECKED}
    ${EndIf}
    ${NSD_CreateCheckbox} 0 12u 100% 18u "Start Clipture with Windows"
    Pop $CliptureStartupCheckbox
    SetCtlColors $CliptureStartupCheckbox "272824" "F0F0EB"
    ${NSD_SetState} $CliptureStartupCheckbox $CliptureStartupChoice
    ${NSD_CreateLabel} 0 38u 100% 36u "Start quietly in the system tray when you sign in. You can change this later in Clipture settings."
    Pop $0
    SetCtlColors $0 "65675E" "F0F0EB"
    nsDialogs::Show
  FunctionEnd

  Function CliptureStartupLeave
    ${NSD_GetState} $CliptureStartupCheckbox $CliptureStartupChoice
  FunctionEnd
!macroend

!macro CLIPTURE_FINISH_SETUP
  !define MUI_PAGE_CUSTOMFUNCTION_LEAVE CliptureFinishLeave
!macroend

!macro CLIPTURE_FINISH_ACTION
  Function CliptureFinishLeave
    ; No choice was shown during silent/passive updates: keep user preferences.
    ${If} $CliptureStartupChoice == ""
      Return
    ${EndIf}
    StrCpy $0 "false"
    ${If} $CliptureStartupChoice == ${BST_CHECKED}
      StrCpy $0 "true"
    ${EndIf}
    StrCpy $1 "--installer-startup $0"
    ${NSD_GetState} $mui.FinishPage.Run $2
    ${If} $2 == ${BST_CHECKED}
      StrCpy $1 "$1 --launch"
    ${EndIf}
    ; One unelevated process saves the choice, registers startup, then optionally
    ; launches. This avoids racing a separate helper against the first app run.
    nsis_tauri_utils::RunAsUser "$INSTDIR\${MAINBINARYNAME}.exe" "$1"
    Pop $0
    ${If} $0 != 0
      MessageBox MB_ICONSTOP|MB_OK "Could not apply the startup option. Please try Finish again."
      Abort
    ${EndIf}
    ${NSD_Uncheck} $mui.FinishPage.Run
  FunctionEnd
!macroend

; A migrating Clipture runs from elsewhere and installs into an empty
; directory: closing Clipture by name would only stop its recording, and a
; desktop shortcut is created only where the user already had one.
!macro CLIPTURE_UNLESS_MIGRATING
  ${If} $CliptureMigrateMode <> 1
!macroend

!macro CLIPTURE_END_UNLESS
  ${EndIf}
!macroend

!macro CLIPTURE_INIT_BEGIN
  StrCpy $CliptureMigrateMode 0
  ClearErrors
  ${GetOptions} $CMDLINE "/MIGRATE" $8
  ${IfNot} ${Errors}
    StrCpy $CliptureMigrateMode 1
  ${EndIf}
  ClearErrors
  StrCpy $CliptureRequestedDirectory ""
  ${If} $INSTDIR != "${PLACEHOLDER_INSTALL_DIR}"
  ${AndIf} $INSTDIR != ""
    StrCpy $CliptureRequestedDirectory $INSTDIR
  ${EndIf}
!macroend

!macro CLIPTURE_INIT_END
  ; Electron's updater supplies no scope flag. Infer the existing installation
  ; instead of letting an elevated installer default to a second machine install.
  !if "${INSTALLMODE}" == "both"
    ClearErrors
    ${GetOptions} $CMDLINE "/currentuser" $8
    ${If} ${Errors}
      ClearErrors
      ${GetOptions} $CMDLINE "/allusers" $8
      ${If} ${Errors}
        ReadRegStr $8 HKCU "Software\7c2d96be-745e-5837-8fec-244a9a96ab0c" "InstallLocation"
        ${If} $8 != ""
          Call MultiUser.InstallMode.CurrentUser
        ${Else}
          ReadRegStr $8 HKLM "Software\7c2d96be-745e-5837-8fec-244a9a96ab0c" "InstallLocation"
          ${If} $8 != ""
            Call MultiUser.InstallMode.AllUsers
          ${EndIf}
        ${EndIf}
      ${EndIf}
    ${EndIf}
  !endif
  !insertmacro CLIPTURE_RESOLVE_LEGACY
  ${If} $CliptureRequestedDirectory != ""
    StrCpy $INSTDIR $CliptureRequestedDirectory
  ${ElseIf} $CliptureLegacyDirectory != ""
    ReadRegStr $8 SHCTX "${UNINSTKEY}" "InstallLocation"
    ${If} $8 == ""
      StrCpy $INSTDIR $CliptureLegacyDirectory
    ${EndIf}
  !if "${INSTALLMODE}" == "currentUser"
  ${ElseIf} $INSTDIR == "$LOCALAPPDATA\${PRODUCTNAME}"
    ; Upstream's per-user default is %LOCALAPPDATA%\Clipture, which also holds
    ; the replay cache and runtime store. New installs use the Windows
    ; per-user program location; earlier per-user installs stay where they are.
    ReadRegStr $8 HKCU "${MANUPRODUCTKEY}" ""
    ${If} $8 == ""
      StrCpy $INSTDIR "$LOCALAPPDATA\Programs\${PRODUCTNAME}"
    ${EndIf}
  !endif
  ${EndIf}
!macroend

!macro CLIPTURE_RESOLVE_LEGACY
  ReadRegStr $CliptureLegacyDirectory SHCTX "Software\7c2d96be-745e-5837-8fec-244a9a96ab0c" "InstallLocation"
  StrCpy $CliptureLegacyScope "/currentuser"
  !if "${INSTALLMODE}" == "both"
    ${If} $MultiUser.InstallMode == "AllUsers"
      StrCpy $CliptureLegacyScope "/allusers"
    ${EndIf}
  !else if "${INSTALLMODE}" == "perMachine"
    StrCpy $CliptureLegacyScope "/allusers"
  !endif
!macroend

!macro NSIS_HOOK_PREINSTALL
  ; The user may have changed installation scope on the interactive wizard.
  !insertmacro CLIPTURE_RESOLVE_LEGACY
  ${If} $CliptureLegacyDirectory != ""
    InitPluginsDir
    SetOutPath $TEMP
    File "/oname=$PLUGINSDIR\clipture-preflight.exe" "${MAINBINARYSRCPATH}"
    nsExec::ExecToStack '"$PLUGINSDIR\clipture-preflight.exe" --verify-legacy-upgrade "$CliptureLegacyDirectory"'
    Pop $8
    Pop $9
    ${If} $8 != 0
      MessageBox MB_ICONSTOP|MB_OK "Cannot safely upgrade the old Clipture installation. $9" /SD IDOK
      Abort
    ${EndIf}
    ClearErrors
    CopyFiles /SILENT "$CliptureLegacyDirectory\Uninstall Clipture.exe" "$PLUGINSDIR\clipture-legacy-uninstall.exe"
    ${If} ${Errors}
      Abort "Could not stage the legacy Clipture uninstaller."
    ${EndIf}
    ExecWait '"$PLUGINSDIR\clipture-legacy-uninstall.exe" /S /KEEP_APP_DATA --updated $CliptureLegacyScope _?=$CliptureLegacyDirectory' $8
    ${If} $8 != 0
    ${OrIf} ${FileExists} "$CliptureLegacyDirectory\resources\app.asar"
      Abort "Legacy Clipture could not be removed. Installation stopped."
    ${EndIf}
    SetOutPath $INSTDIR
  ${EndIf}
!macroend

!macro NSIS_HOOK_POSTUNINSTALL
  ; Staged or backed-up update files are part of the installation.
  RMDir /r "$INSTDIR\.update"
  RMDir "$INSTDIR"
!macroend

!macro NSIS_HOOK_POSTINSTALL
  !insertmacro CLIPTURE_REFRESH_ICONS
  ; A full install is authoritative. A leftover in-place transaction
  ; (ADR 0020) could otherwise restore its backups over these files.
  RMDir /r "$INSTDIR\.update"
  ; Electron's updater uses --force-run; Tauri's installer normally uses /R.
  ${If} ${Silent}
    ClearErrors
    ${GetOptions} $CMDLINE "--force-run" $8
    ${IfNot} ${Errors}
      nsis_tauri_utils::RunAsUser "$INSTDIR\${MAINBINARYNAME}.exe" ""
    ${EndIf}
  ${EndIf}
!macroend
