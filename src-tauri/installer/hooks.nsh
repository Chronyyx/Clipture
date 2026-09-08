; Keep compatibility glue separate from the pinned upstream installer template.
Var CliptureRequestedDirectory
Var CliptureLegacyDirectory
Var CliptureLegacyScope

!macro CLIPTURE_INIT_BEGIN
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

!macro NSIS_HOOK_POSTINSTALL
  ; Electron's updater uses --force-run; Tauri's installer normally uses /R.
  ${If} ${Silent}
    ClearErrors
    ${GetOptions} $CMDLINE "--force-run" $8
    ${IfNot} ${Errors}
      nsis_tauri_utils::RunAsUser "$INSTDIR\${MAINBINARYNAME}.exe" ""
    ${EndIf}
  ${EndIf}
!macroend
