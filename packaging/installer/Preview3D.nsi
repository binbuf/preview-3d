Unicode true

!ifndef STAGE_DIR
  !error "STAGE_DIR must identify the validated installer payload."
!endif
!ifndef OUTPUT_FILE
  !error "OUTPUT_FILE must identify the setup executable to create."
!endif

!define PRODUCT_NAME "3D Preview"
!define PRODUCT_PUBLISHER "Binbuf"
!define PRODUCT_VERSION "0.1.0"
!define PRODUCT_EXE "Preview3D.exe"
!define PRODUCT_KEY "Software\Binbuf\Preview3D"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Binbuf.Preview3D"

!define PROGID_GLTF "Binbuf.Preview3D.glTF.1"
!define PROGID_STL "Binbuf.Preview3D.STL.1"
!define PROGID_PLY "Binbuf.Preview3D.PLY.1"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
!include "FileFunc.nsh"
!include "Integration.nsh"

Name "${PRODUCT_NAME}"
OutFile "${OUTPUT_FILE}"
InstallDir "$PROGRAMFILES64\Binbuf\3D Preview"
InstallDirRegKey HKLM "${PRODUCT_KEY}" "InstallLocation"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
SetCompressorDictSize 32
CRCCheck on
XPStyle on
ShowInstDetails show
ShowUninstDetails show
BrandingText "${PRODUCT_NAME} ${PRODUCT_VERSION}"

VIProductVersion "0.1.0.0"
VIAddVersionKey /LANG=1033 "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey /LANG=1033 "CompanyName" "${PRODUCT_PUBLISHER}"
VIAddVersionKey /LANG=1033 "FileDescription" "${PRODUCT_NAME} installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright (c) 2026 Binbuf"

!ifdef SIGNTOOL
  !uninstfinalize '"${SIGNTOOL}" sign /sha1 "${SIGN_THUMBPRINT}" /fd SHA256 /tr "${TIMESTAMP_URL}" /td SHA256 "%1"' = 0
!endif

!define MUI_ABORTWARNING
!define MUI_ICON "..\..\interactive-viewer\resources\App.ico"
!define MUI_UNICON "..\..\interactive-viewer\resources\App.ico"
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Review default apps for 3D Preview"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchDefaultApps

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "${PRODUCT_NAME} requires 64-bit Windows."
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin11}
    MessageBox MB_OK|MB_ICONSTOP "${PRODUCT_NAME} requires Windows 11 or later."
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
check_viewer_closed:
  FindWindow $0 "Preview3DWindow"
  StrCmp $0 0 viewer_closed
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Close 3D Preview before installing or upgrading, then choose Retry." IDRETRY check_viewer_closed
  Abort
viewer_closed:
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
check_viewer_closed:
  FindWindow $0 "Preview3DWindow"
  StrCmp $0 0 viewer_closed
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Close 3D Preview before uninstalling, then choose Retry." IDRETRY check_viewer_closed
  Abort
viewer_closed:
FunctionEnd

Function LaunchDefaultApps
  ExecShell "open" "ms-settings:defaultapps?registeredAppMachine=3D%20Preview"
FunctionEnd

!macro RegisterProgId PROGID FRIENDLY_NAME
  WriteRegStr HKLM "Software\Classes\${PROGID}" "" "${FRIENDLY_NAME}"
  WriteRegStr HKLM "Software\Classes\${PROGID}" "FriendlyTypeName" "${FRIENDLY_NAME}"
  WriteRegStr HKLM "Software\Classes\${PROGID}\DefaultIcon" "" '"$INSTDIR\${PRODUCT_EXE}",0'
  WriteRegStr HKLM "Software\Classes\${PROGID}\shell\open" "FriendlyAppName" "${PRODUCT_NAME}"
  WriteRegStr HKLM "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\${PRODUCT_EXE}" --open "%1"'
!macroend

!macro RegisterExtension EXT PROGID
  WriteRegNone HKLM "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}"
  WriteRegNone HKLM "Software\Classes\Applications\${PRODUCT_EXE}\SupportedTypes" "${EXT}"
  WriteRegStr HKLM "${PRODUCT_KEY}\Capabilities\FileAssociations" "${EXT}" "${PROGID}"
!macroend

!macro UnregisterExtension EXT PROGID
  DeleteRegValue HKLM "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}"
  DeleteRegKey /IfEmpty HKLM "Software\Classes\${EXT}\OpenWithProgids"
  DeleteRegKey /IfEmpty HKLM "Software\Classes\${EXT}"
!macroend

Section "3D Preview" SEC_MAIN
  SectionIn RO
  SetRegView 64
  SetShellVarContext all
  SetOutPath "$INSTDIR"
  SetOverwrite on

  File "${STAGE_DIR}\Preview3D.exe"
  File "${STAGE_DIR}\concrt140.dll"
  File "${STAGE_DIR}\msvcp140.dll"
  File "${STAGE_DIR}\msvcp140_atomic_wait.dll"
  File "${STAGE_DIR}\vcruntime140.dll"
  File "${STAGE_DIR}\vcruntime140_1.dll"
  File "${STAGE_DIR}\README.txt"
  File "${STAGE_DIR}\THIRD-PARTY-NOTICES.txt"
  File "${STAGE_DIR}\SBOM.cdx.json"
  File "${STAGE_DIR}\MANIFEST.json"
  File "${STAGE_DIR}\Remove-Preview3DProfile.ps1"
  File "${STAGE_DIR}\Provision-Preview3DWorkerAcl.ps1"
  File /r "${STAGE_DIR}\licenses"
  File /r "${STAGE_DIR}\worker"

  DetailPrint "Provisioning the import-worker sandbox ACL..."
  nsExec::ExecToStack '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\Provision-Preview3DWorkerAcl.ps1" -WorkerDirectory "$INSTDIR\worker"'
  Pop $0
  Pop $1
  ${If} $0 != 0
    DetailPrint "$1"
    MessageBox MB_OK|MB_ICONSTOP "The import-worker sandbox could not be provisioned. Setup cannot continue."
    Abort
  ${EndIf}

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\3D Preview"
  CreateShortcut "$SMPROGRAMS\3D Preview\3D Preview.lnk" "$INSTDIR\${PRODUCT_EXE}"
  CreateShortcut "$SMPROGRAMS\3D Preview\Uninstall 3D Preview.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "${PRODUCT_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${PRODUCT_KEY}" "Version" "${PRODUCT_VERSION}"

  !insertmacro RegisterProgId "${PROGID_GLTF}" "3D model (glTF)"
  !insertmacro RegisterProgId "${PROGID_STL}" "3D model (STL)"
  !insertmacro RegisterProgId "${PROGID_PLY}" "3D model (PLY)"

  WriteRegStr HKLM "Software\Classes\Applications\${PRODUCT_EXE}" "FriendlyAppName" "${PRODUCT_NAME}"
  WriteRegStr HKLM "Software\Classes\Applications\${PRODUCT_EXE}" "ApplicationCompany" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKLM "Software\Classes\Applications\${PRODUCT_EXE}\DefaultIcon" "" '"$INSTDIR\${PRODUCT_EXE}",0'
  WriteRegStr HKLM "Software\Classes\Applications\${PRODUCT_EXE}\shell\open\command" "" '"$INSTDIR\${PRODUCT_EXE}" --open "%1"'

  WriteRegStr HKLM "${PRODUCT_KEY}\Capabilities" "ApplicationName" "${PRODUCT_NAME}"
  WriteRegStr HKLM "${PRODUCT_KEY}\Capabilities" "ApplicationDescription" "Fast, read-only viewing for supported local 3D models."
  WriteRegStr HKLM "${PRODUCT_KEY}\Capabilities" "ApplicationIcon" '"$INSTDIR\${PRODUCT_EXE}",0'
  !insertmacro RegisterExtension ".glb" "${PROGID_GLTF}"
  !insertmacro RegisterExtension ".gltf" "${PROGID_GLTF}"
  !insertmacro RegisterExtension ".stl" "${PROGID_STL}"
  !insertmacro RegisterExtension ".ply" "${PROGID_PLY}"
  WriteRegStr HKLM "Software\RegisteredApplications" "${PRODUCT_NAME}" "${PRODUCT_KEY}\Capabilities"

  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\${PRODUCT_EXE}" "" "$INSTDIR\${PRODUCT_EXE}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\${PRODUCT_EXE}" "Path" "$INSTDIR"

  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\${PRODUCT_EXE},0"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "EstimatedSize" $0

  ${NotifyShell_AssocChanged}
SectionEnd

Section "Uninstall"
  SetRegView 64
  SetShellVarContext all

  ; Best-effort cleanup for the uninstalling user's AppContainer profile and
  ; its worker-directory ACE. Other users' now-inert profiles cannot access a
  ; payload after the files below are removed.
  IfFileExists "$INSTDIR\Remove-Preview3DProfile.ps1" 0 profile_cleanup_done
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\Remove-Preview3DProfile.ps1"'
profile_cleanup_done:

  DeleteRegValue HKLM "Software\RegisteredApplications" "${PRODUCT_NAME}"
  DeleteRegKey HKLM "${PRODUCT_KEY}\Capabilities"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\${PRODUCT_EXE}"
  DeleteRegKey HKLM "Software\Classes\Applications\${PRODUCT_EXE}"
  !insertmacro UnregisterExtension ".glb" "${PROGID_GLTF}"
  !insertmacro UnregisterExtension ".gltf" "${PROGID_GLTF}"
  !insertmacro UnregisterExtension ".stl" "${PROGID_STL}"
  !insertmacro UnregisterExtension ".ply" "${PROGID_PLY}"
  DeleteRegKey HKLM "Software\Classes\${PROGID_GLTF}"
  DeleteRegKey HKLM "Software\Classes\${PROGID_STL}"
  DeleteRegKey HKLM "Software\Classes\${PROGID_PLY}"
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  DeleteRegKey HKLM "${PRODUCT_KEY}"
  DeleteRegKey /IfEmpty HKLM "Software\Binbuf"

  Delete "$SMPROGRAMS\3D Preview\3D Preview.lnk"
  Delete "$SMPROGRAMS\3D Preview\Uninstall 3D Preview.lnk"
  RMDir "$SMPROGRAMS\3D Preview"

  Delete "$INSTDIR\worker\Preview3DImportWorker.exe"
  Delete "$INSTDIR\worker\draco.dll"
  Delete "$INSTDIR\worker\fastgltf.dll"
  Delete "$INSTDIR\worker\ktx.dll"
  Delete "$INSTDIR\worker\libsharpyuv.dll"
  Delete "$INSTDIR\worker\libwebp.dll"
  Delete "$INSTDIR\worker\meshoptimizer.dll"
  Delete "$INSTDIR\worker\simdjson.dll"
  Delete "$INSTDIR\worker\zstd.dll"
  Delete "$INSTDIR\worker\concrt140.dll"
  Delete "$INSTDIR\worker\msvcp140.dll"
  Delete "$INSTDIR\worker\msvcp140_1.dll"
  Delete "$INSTDIR\worker\vcruntime140.dll"
  Delete "$INSTDIR\worker\vcruntime140_1.dll"
  RMDir "$INSTDIR\worker"

  Delete "$INSTDIR\licenses\basisu.txt"
  Delete "$INSTDIR\licenses\draco.txt"
  Delete "$INSTDIR\licenses\fastgltf.txt"
  Delete "$INSTDIR\licenses\ktx.txt"
  Delete "$INSTDIR\licenses\libwebp.txt"
  Delete "$INSTDIR\licenses\meshoptimizer.txt"
  Delete "$INSTDIR\licenses\simdjson.txt"
  Delete "$INSTDIR\licenses\zstd.txt"
  RMDir "$INSTDIR\licenses"

  Delete "$INSTDIR\Preview3D.exe"
  Delete "$INSTDIR\concrt140.dll"
  Delete "$INSTDIR\msvcp140.dll"
  Delete "$INSTDIR\msvcp140_atomic_wait.dll"
  Delete "$INSTDIR\vcruntime140.dll"
  Delete "$INSTDIR\vcruntime140_1.dll"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\THIRD-PARTY-NOTICES.txt"
  Delete "$INSTDIR\SBOM.cdx.json"
  Delete "$INSTDIR\MANIFEST.json"
  Delete "$INSTDIR\Remove-Preview3DProfile.ps1"
  Delete "$INSTDIR\Provision-Preview3DWorkerAcl.ps1"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  RMDir "$PROGRAMFILES64\Binbuf"

  ${NotifyShell_AssocChanged}
SectionEnd
