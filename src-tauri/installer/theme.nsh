; Native field-recorder visual treatment. No install policy belongs here.
!ifndef CLIPTURE_INSTALLER_THEME
!define CLIPTURE_INSTALLER_THEME

SetFont "Segoe UI" 9
!include "${__FILEDIR__}\bitmap-fit.nsh"
!define MUI_BGCOLOR "F0F0EB"
!define MUI_TEXTCOLOR "272824"
!define MUI_HEADERIMAGE_RIGHT
!define MUI_INSTFILESPAGE_COLORS "E8E9E2 1B1C19"
!define MUI_INSTFILESPAGE_PROGRESSBAR "smooth colored"
!define MUI_CUSTOMFUNCTION_GUIINIT CliptureThemeInit
!define MUI_CUSTOMFUNCTION_UNGUIINIT un.CliptureThemeInit
!define MUI_WELCOMEPAGE_TITLE "Clipture"
!define MUI_WELCOMEPAGE_TEXT "Capture. Replay. Keep it.$\r$\n$\r$\nSet up your native desktop recorder, choose an installation location, and select how Clipture starts with Windows.$\r$\n$\r$\nSelect Next to begin."
!define MUI_FINISHPAGE_TITLE "Ready to capture."
!define MUI_FINISHPAGE_TEXT "Clipture is installed.$\r$\n$\r$\nChoose whether to open it now, then select Finish to complete setup."

Var CliptureInstrumentFont
Var CliptureHeaderBitmap
Var CliptureSidebarFont

!macro CLIPTURE_SIDEBAR_LABELS
  Push $0
  ${NSD_CreateLabel} 14u 12u 83u 20u "CLIPTURE"
  Pop $0
  SetCtlColors $0 "F0F0EB" "1B1C19"
  SendMessage $0 ${WM_SETFONT} $CliptureSidebarFont 1
  ${NSD_CreateLabel} 14u 34u 83u 14u "Desktop recorder"
  Pop $0
  SetCtlColors $0 "C5C8BD" "1B1C19"
  ${NSD_CreateLabel} 14u 176u 83u 14u "Capture / Replay"
  Pop $0
  SetCtlColors $0 "F0F0EB" "1B1C19"
  Pop $0
!macroend

!macro CLIPTURE_THEME_FUNCTIONS PREFIX
  Function ${PREFIX}CliptureThemeInit
    Push $0
    SetCtlColors $HWNDPARENT "272824" "F0F0EB"
    CreateFont $CliptureInstrumentFont "Segoe UI" 10 700
    CreateFont $CliptureSidebarFont "Segoe UI" 12 700
    GetDlgItem $0 $HWNDPARENT 1037
    SendMessage $0 ${WM_SETFONT} $CliptureInstrumentFont 1
    GetDlgItem $0 $HWNDPARENT 1256
    SetCtlColors $0 "65675E" "F0F0EB"
    SendMessage $0 ${WM_SETTEXT} 0 "STR:CLIPTURE / DESKTOP RECORDER"
    GetDlgItem $0 $HWNDPARENT 1028
    SetCtlColors $0 "65675E" "F0F0EB"
    Pop $0
  FunctionEnd

  !if "${PREFIX}" == ""
  Function .onGUIEnd
  !else
  Function un.onGUIEnd
  !endif
    System::Call 'gdi32::DeleteObject(p $CliptureInstrumentFont)'
    System::Call 'gdi32::DeleteObject(p $CliptureHeaderBitmap)'
    System::Call 'gdi32::DeleteObject(p $CliptureSidebarFont)'
  FunctionEnd

  Function ${PREFIX}CliptureProgressStyle
    Push $0
    Push $1
    FindWindow $0 "#32770" "" $HWNDPARENT
    SetCtlColors $0 "272824" "F0F0EB"
    GetDlgItem $1 $0 1006
    SetCtlColors $1 "272824" "F0F0EB"
    GetDlgItem $1 $0 1004
    ; Disable visual styles for this control only so Windows doesn't substitute
    ; a green animation. PBM colors use COLORREF (BGR), not the MUI RGB palette.
    System::Call 'uxtheme::SetWindowTheme(p r1, w "", w "")'
    SendMessage $1 0x409 0 0x0065FF ; PBM_SETBARCOLOR: orange
    SendMessage $1 0x2001 0 0xD4D6CD ; PBM_SETBKCOLOR: silver-gray
    Pop $1
    Pop $0
  FunctionEnd
!macroend

!insertmacro CLIPTURE_THEME_FUNCTIONS ""
!insertmacro CLIPTURE_THEME_FUNCTIONS "un."

!macro CLIPTURE_PROGRESS_STYLE PREFIX
  !define MUI_PAGE_CUSTOMFUNCTION_SHOW ${PREFIX}CliptureProgressStyle
!macroend

!endif
