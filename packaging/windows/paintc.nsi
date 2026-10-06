; paintc.nsi - the paint.c installer for Windows x64 (lane I, NSIS 3).
;
; Build from an install tree (cmake --install build --prefix stage):
;   makensis -DVERSION=0.1.2 -DSTAGE=<absolute stage path> ^
;            -DOUTFILE=<absolute path>\paintc-0.1.2-windows-x64-setup.exe ^
;            packaging\windows\paintc.nsi
; (makensis works in this script's folder, so pass absolute paths; ICON
; defaults to assets/icons/paintc.ico)
;
; Installs paintc.exe, the licenses and the README to Program Files, adds a
; Start menu shortcut and an uninstaller, and registers paint.c for every
; image type it opens: an "Open with" entry for each extension, the
; RegisteredApplications capabilities (Default apps settings), and .pdn as
; the default when no other program claims it. Existing defaults of the
; other types are left alone (Windows asks the user).

Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

!ifndef VERSION
  !define VERSION "0.1.2"
!endif
!ifndef STAGE
  !define STAGE "stage"
!endif
!ifndef ICON
  !define ICON "../../assets/icons/paintc.ico"
!endif
!ifndef OUTFILE
  !define OUTFILE "paintc-${VERSION}-windows-x64-setup.exe"
!endif

!define APPNAME   "paint.c"
!define PROGID    "paintc.Image"
!define PROGIDPDN "paintc.pdn"
!define CAPS      "Software\paint.c\Capabilities"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\paint.c"

Name "${APPNAME}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\paint.c"
InstallDirRegKey HKLM "Software\paint.c" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "${APPNAME} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "CompanyName" "The paint.c authors"
VIAddVersionKey "LegalCopyright" "MIT License"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\paintc.exe"
!insertmacro MUI_PAGE_LICENSE "${STAGE}\licenses\LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; One extension: "Open with" entry, capability, supported type.
!macro Assoc EXT
  WriteRegStr HKLM "Software\Classes\.${EXT}\OpenWithProgids" "${PROGID}" ""
  WriteRegStr HKLM "${CAPS}\FileAssociations" ".${EXT}" "${PROGID}"
  WriteRegStr HKLM "Software\Classes\Applications\paintc.exe\SupportedTypes" ".${EXT}" ""
!macroend
!macro UnAssoc EXT
  DeleteRegValue HKLM "Software\Classes\.${EXT}\OpenWithProgids" "${PROGID}"
!macroend

; Every extension paint.c opens (tests/app/test_i_packaging.c keeps this
; list equal to the codec registry).
!macro AllAssoc MACRO
  !insertmacro ${MACRO} png
  !insertmacro ${MACRO} jpg
  !insertmacro ${MACRO} jpeg
  !insertmacro ${MACRO} jpe
  !insertmacro ${MACRO} jfif
  !insertmacro ${MACRO} exif
  !insertmacro ${MACRO} bmp
  !insertmacro ${MACRO} dib
  !insertmacro ${MACRO} rle
  !insertmacro ${MACRO} gif
  !insertmacro ${MACRO} tga
  !insertmacro ${MACRO} dds
  !insertmacro ${MACRO} tif
  !insertmacro ${MACRO} tiff
  !insertmacro ${MACRO} webp
  ; lane AVIFJXL (ADR-019): AVIF and JPEG XL
  !insertmacro ${MACRO} avif
  !insertmacro ${MACRO} jxl
  !insertmacro ${MACRO} ora
!macroend

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "paint.c needs 64-bit Windows 10 or later."
    Abort
  ${EndIf}
  SetRegView 64
  ; per-machine install: the Start menu shortcut goes to All Users, not to
  ; the profile of the administrator account that UAC elevated to
  SetShellVarContext all
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Section "paint.c" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "${STAGE}\paintc.exe"
  File "${STAGE}\README.txt"
  SetOutPath "$INSTDIR\licenses"
  File "${STAGE}\licenses\*.*"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\paint.c" "InstallDir" "$INSTDIR"

  ; program ids
  WriteRegStr HKLM "Software\Classes\${PROGID}" "" "Image"
  WriteRegStr HKLM "Software\Classes\${PROGID}\DefaultIcon" "" "$INSTDIR\paintc.exe,0"
  WriteRegStr HKLM "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\paintc.exe" "%1"'
  WriteRegStr HKLM "Software\Classes\${PROGIDPDN}" "" "Paint.NET image"
  WriteRegStr HKLM "Software\Classes\${PROGIDPDN}\DefaultIcon" "" "$INSTDIR\paintc.exe,0"
  WriteRegStr HKLM "Software\Classes\${PROGIDPDN}\shell\open\command" "" '"$INSTDIR\paintc.exe" "%1"'
  WriteRegStr HKLM "Software\Classes\Applications\paintc.exe" "FriendlyAppName" "${APPNAME}"
  WriteRegStr HKLM "Software\Classes\Applications\paintc.exe\shell\open\command" "" '"$INSTDIR\paintc.exe" "%1"'

  ; file types
  !insertmacro AllAssoc Assoc
  WriteRegStr HKLM "Software\Classes\.pdn\OpenWithProgids" "${PROGIDPDN}" ""
  WriteRegStr HKLM "${CAPS}\FileAssociations" ".pdn" "${PROGIDPDN}"
  WriteRegStr HKLM "Software\Classes\Applications\paintc.exe\SupportedTypes" ".pdn" ""
  ReadRegStr $0 HKLM "Software\Classes\.pdn" ""
  ${If} $0 == ""
    WriteRegStr HKLM "Software\Classes\.pdn" "" "${PROGIDPDN}"
    WriteRegStr HKLM "Software\paint.c" "OwnsPdn" "1"
  ${EndIf}
  WriteRegStr HKLM "${CAPS}" "ApplicationName" "${APPNAME}"
  WriteRegStr HKLM "${CAPS}" "ApplicationDescription" "Image editor with layers, effects and adjustments"
  WriteRegStr HKLM "${CAPS}" "ApplicationIcon" "$INSTDIR\paintc.exe,0"
  WriteRegStr HKLM "Software\RegisteredApplications" "${APPNAME}" "${CAPS}"

  ; Start menu and Apps & features
  CreateShortCut "$SMPROGRAMS\paint.c.lnk" "$INSTDIR\paintc.exe" "" "$INSTDIR\paintc.exe" 0
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "The paint.c authors"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\paintc.exe,0"
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1

  ; tell Explorer about the new associations
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Section "Uninstall"
  !insertmacro AllAssoc UnAssoc
  DeleteRegValue HKLM "Software\Classes\.pdn\OpenWithProgids" "${PROGIDPDN}"
  ReadRegStr $0 HKLM "Software\paint.c" "OwnsPdn"
  ReadRegStr $1 HKLM "Software\Classes\.pdn" ""
  ${If} $0 == "1"
  ${AndIf} $1 == "${PROGIDPDN}"
    DeleteRegValue HKLM "Software\Classes\.pdn" ""
  ${EndIf}
  DeleteRegKey HKLM "Software\Classes\${PROGID}"
  DeleteRegKey HKLM "Software\Classes\${PROGIDPDN}"
  DeleteRegKey HKLM "Software\Classes\Applications\paintc.exe"
  DeleteRegValue HKLM "Software\RegisteredApplications" "${APPNAME}"
  DeleteRegKey HKLM "Software\paint.c"
  DeleteRegKey HKLM "${UNINSTKEY}"
  Delete "$SMPROGRAMS\paint.c.lnk"
  Delete "$INSTDIR\paintc.exe"
  Delete "$INSTDIR\README.txt"
  RMDir /r "$INSTDIR\licenses"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd
