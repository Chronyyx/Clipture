; Refresh only this installation's shell items, once after files are replaced.
; Never clear the global icon cache, restart Explorer, or recreate taskbar pins.
!macro CLIPTURE_NOTIFY_ICON PATH
  ; SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT. Explorer owns repaint timing.
  System::Call 'shell32::SHChangeNotify(i 0x2000, i 0x3005, w "${PATH}", p 0)'
!macroend

!macro CLIPTURE_REFRESH_SHORTCUT PATH
  ${If} ${FileExists} "${PATH}"
    ; Use Tauri's existing ShellLink target check, not the shortcut's display name.
    !insertmacro IsShortcutTarget "${PATH}" "$INSTDIR\${MAINBINARYNAME}.exe"
    Pop $3
    ${If} $3 == 1
      !insertmacro CLIPTURE_NOTIFY_ICON "${PATH}"
    ${EndIf}
  ${EndIf}
!macroend

!macro CLIPTURE_REFRESH_ICONS
  Push $0
  Push $1
  Push $2
  Push $3
  Push $R7
  Push $R8
  Push $R9
  !insertmacro CLIPTURE_NOTIFY_ICON "$INSTDIR\${MAINBINARYNAME}.exe"
  !insertmacro CLIPTURE_REFRESH_SHORTCUT "$DESKTOP\${PRODUCTNAME}.lnk"
  !insertmacro CLIPTURE_REFRESH_SHORTCUT "$SMPROGRAMS\${PRODUCTNAME}.lnk"
  ${If} $AppStartMenuFolder != ""
    !insertmacro CLIPTURE_REFRESH_SHORTCUT "$SMPROGRAMS\$AppStartMenuFolder\${PRODUCTNAME}.lnk"
  ${EndIf}

  ; CSIDL_APPDATA is per-user even when NSIS's shell context is all-users.
  ; Enumerate existing pins only: no pinning, unpinning or shortcut rewrites.
  System::Call 'shell32::SHGetFolderPathW(p 0, i 0x1a, p 0, i 0, w .r0) i .r3'
  ${If} $3 == 0
    StrCpy $R7 "$0\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar"
    FindFirst $R8 $R9 "$R7\*.lnk"
    ${If} $R8 != ""
      ${DoWhile} $R9 != ""
        !insertmacro CLIPTURE_REFRESH_SHORTCUT "$R7\$R9"
        FindNext $R8 $R9
      ${Loop}
      FindClose $R8
    ${EndIf}
  ${EndIf}
  Pop $R9
  Pop $R8
  Pop $R7
  Pop $3
  Pop $2
  Pop $1
  Pop $0
!macroend
