; Uniformly contain artwork in its actual control, then HALFTONE downsample.
; MUI's FitControl distorts bitmaps when dialog-unit/font proportions change.
!include Util.nsh

!macro CLIPTURE_FIT_BITMAP CONTROL PATH BACKGROUND RESULT
  Push "${PATH}"
  Push ${CONTROL}
  Push ${BACKGROUND}
  ${CallArtificialFunction} CliptureFitBitmap
  Pop ${RESULT}
!macroend

!macro CliptureFitBitmap
  System::Store "S"
  Pop $9 ; COLORREF background
  Pop $0 ; destination control
  Pop $1 ; source path
  System::Call 'user32::GetClientRect(p r0, @r2)'
  System::Call '*$2(i, i, i.r3, i.r4)' ; destination width / height
  System::Call 'user32::LoadImageW(p 0, w r1, i 0, i 0, i 0, i 0x10) p.r5'
  StrCpy $R9 0
  ${If} $5 != 0
  ${AndIf} $3 > 0
  ${AndIf} $4 > 0
    System::Call 'gdi32::GetObjectW(p r5, i 32, @r2)'
    System::Call '*$2(i, i.r6, i.r7)' ; source width / height
    StrCpy $R0 $3
    IntOp $R1 $7 * $3
    IntOp $R1 $R1 / $6
    ${If} $R1 > $4
      StrCpy $R1 $4
      IntOp $R0 $6 * $4
      IntOp $R0 $R0 / $7
    ${EndIf}
    IntOp $R2 $3 - $R0
    IntOp $R2 $R2 / 2
    IntOp $R3 $4 - $R1
    IntOp $R3 $R3 / 2
    System::Call 'user32::GetDC(p r0) p.r8'
    System::Call 'gdi32::CreateCompatibleDC(p r8) p.r1'
    System::Call 'gdi32::CreateCompatibleDC(p r8) p.r2'
    System::Call 'gdi32::SelectObject(p r1, p r5) p.R4'
    System::Call 'gdi32::CreateCompatibleBitmap(p r8, i r3, i r4) p.R9'
    System::Call 'gdi32::SelectObject(p r2, p R9) p.R5'
    System::Call 'gdi32::CreateSolidBrush(i r9) p.R6'
    System::Call 'gdi32::SelectObject(p r2, p R6) p.R7'
    System::Call 'gdi32::PatBlt(p r2, i 0, i 0, i r3, i r4, i 0xF00021)'
    System::Call 'gdi32::SetStretchBltMode(p r2, i 4)'
    System::Call 'gdi32::SetBrushOrgEx(p r2, i 0, i 0, p 0)'
    System::Call 'gdi32::StretchBlt(p r2, i R2, i R3, i R0, i R1, p r1, i 0, i 0, i r6, i r7, i 0xCC0020)'
    System::Call 'gdi32::SelectObject(p r2, p R7)'
    System::Call 'gdi32::DeleteObject(p R6)'
    System::Call 'gdi32::SelectObject(p r2, p R5)'
    System::Call 'gdi32::SelectObject(p r1, p R4)'
    System::Call 'gdi32::DeleteDC(p r1)'
    System::Call 'gdi32::DeleteDC(p r2)'
    System::Call 'user32::ReleaseDC(p r0, p r8)'
    SendMessage $0 ${STM_SETIMAGE} ${IMAGE_BITMAP} $R9
  ${EndIf}
  System::Call 'gdi32::DeleteObject(p r5)'
  Push $R9 ; caller owns the fitted bitmap
  System::Store "L"
!macroend

; Narrow overrides of MUI's image loading, not its pages or install behavior.
!macroundef MUI_INTERNAL_FULLWINDOW_LOADWIZARDIMAGE
!macro MUI_INTERNAL_FULLWINDOW_LOADWIZARDIMAGE UN CONTROL PATH RESULT
  !insertmacro CLIPTURE_FIT_BITMAP ${CONTROL} "${PATH}" 0x191C1B ${RESULT}
  !insertmacro CLIPTURE_SIDEBAR_LABELS
!macroend

!macroundef MUI_HEADERIMAGE_INITHELPER_LOADIMAGE
!macro MUI_HEADERIMAGE_INITHELPER_LOADIMAGE UN RTL ID PATH
  GetDlgItem $mui.Header.Image $HWNDPARENT ${ID}
  !insertmacro CLIPTURE_FIT_BITMAP $mui.Header.Image "${PATH}" 0xEBF0F0 $CliptureHeaderBitmap
!macroend
