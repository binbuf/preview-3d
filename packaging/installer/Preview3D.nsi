Unicode true

!ifndef STAGE_DIR
  !error "STAGE_DIR must identify the validated installer payload."
!endif
!ifndef OUTPUT_FILE
  !error "OUTPUT_FILE must identify the setup executable to create."
!endif
!ifndef PRODUCT_VERSION
  !define PRODUCT_VERSION "0.1.0"
!endif
!ifndef PRODUCT_FILE_VERSION
  !define PRODUCT_FILE_VERSION "0.1.0.0"
!endif

!define PRODUCT_NAME "Preview 3D"
!define PRODUCT_PUBLISHER "Binbuf"
!define PRODUCT_EXE "Preview3D.exe"
!define PRODUCT_KEY "Software\Binbuf\Preview3D"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Binbuf.Preview3D"

!define PROGID_GLTF "Binbuf.Preview3D.glTF.1"
!define PROGID_STL "Binbuf.Preview3D.STL.1"
!define PROGID_PLY "Binbuf.Preview3D.PLY.1"
!define PROGID_OBJ "Binbuf.Preview3D.OBJ.1"
!define PROGID_FBX "Binbuf.Preview3D.FBX.1"
!define PROGID_3MF "Binbuf.Preview3D.ThreeMF.1"
!define PROGID_USD "Binbuf.Preview3D.USD.1"
!define PROGID_STEP "Binbuf.Preview3D.STEP.1"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
!include "FileFunc.nsh"
!include "Integration.nsh"

Name "${PRODUCT_NAME}"
OutFile "${OUTPUT_FILE}"
InstallDir "$PROGRAMFILES64\Binbuf\Preview 3D"
InstallDirRegKey HKLM "${PRODUCT_KEY}" "InstallLocation"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
SetCompressorDictSize 32
CRCCheck on
XPStyle on
ShowInstDetails show
ShowUninstDetails show
BrandingText "${PRODUCT_NAME} ${PRODUCT_VERSION}"

VIProductVersion "${PRODUCT_FILE_VERSION}"
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
!define MUI_FINISHPAGE_RUN_TEXT "Review default apps for Preview 3D"
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
  ; This engineering build is unsigned, and Smart App Control has no per-app
  ; exception. It evaluates every executable image separately, so an unsigned
  ; payload can install cleanly and then fail at run time when the worker loads
  ; a bundled DLL (a Bad Image error naming, for example, worker\zstd.dll with
  ; status 0xC0E90002). Surface that here, before install, instead of after.
  ; VerifiedAndReputablePolicyState is 0 when Smart App Control is off; any
  ; other value means it is active (enforcing or evaluating).
  StrCpy $1 0
  ReadRegDWORD $1 HKLM "SYSTEM\CurrentControlSet\Control\CI\Policy" "VerifiedAndReputablePolicyState"
  StrCmp $1 0 sac_policy_ok
  MessageBox MB_YESNO|MB_ICONEXCLAMATION \
    "This Preview 3D build is unsigned, and Smart App Control is active on this PC.$\r$\n$\r$\nSmart App Control has no per-app exception, so Windows can block the app or one of the DLLs bundled with it after installation (for example worker\zstd.dll, error status 0xC0E90002).$\r$\n$\r$\nTo run this build, turn off Smart App Control under Windows Security > App & browser control > Smart App Control, then run setup again.$\r$\n$\r$\nInstall anyway?" \
    IDYES sac_policy_ok IDNO sac_policy_abort
sac_policy_abort:
  Abort
sac_policy_ok:
check_viewer_closed:
  FindWindow $0 "Preview3DWindow"
  StrCmp $0 0 viewer_closed
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Close Preview 3D before installing or upgrading, then choose Retry." IDRETRY check_viewer_closed
  Abort
viewer_closed:
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
check_viewer_closed:
  FindWindow $0 "Preview3DWindow"
  StrCmp $0 0 viewer_closed
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Close Preview 3D before uninstalling, then choose Retry." IDRETRY check_viewer_closed
  Abort
viewer_closed:
FunctionEnd

Function LaunchDefaultApps
  ExecShell "open" "ms-settings:defaultapps?registeredAppMachine=Preview%203D"
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

Section "Preview 3D" SEC_MAIN
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
  File "${STAGE_DIR}\LICENSE"
  File "${STAGE_DIR}\NOTICE"
  File "${STAGE_DIR}\THIRD-PARTY-NOTICES.txt"
  File "${STAGE_DIR}\SBOM.cdx.json"
  File "${STAGE_DIR}\MANIFEST.json"
  File "${STAGE_DIR}\Remove-Preview3DProfile.ps1"
  File "${STAGE_DIR}\Provision-Preview3DWorkerAcl.ps1"
  File /r "${STAGE_DIR}\licenses"
  File /r "${STAGE_DIR}\worker"
  File /r "${STAGE_DIR}\OpenUsdHost"
  File /r "${STAGE_DIR}\StepHost"

  DetailPrint "Provisioning the isolated importer payload ACLs..."
  nsExec::ExecToStack '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\Provision-Preview3DWorkerAcl.ps1" -WorkerDirectory "$INSTDIR\worker" -OpenUsdHostDirectory "$INSTDIR\OpenUsdHost" -StepHostDirectory "$INSTDIR\StepHost"'
  Pop $0
  Pop $1
  ${If} $0 != 0
    DetailPrint "$1"
    MessageBox MB_OK|MB_ICONSTOP "The importer sandboxes could not be provisioned. Setup cannot continue."
    Abort
  ${EndIf}

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\Preview 3D"
  CreateShortcut "$SMPROGRAMS\Preview 3D\Preview 3D.lnk" "$INSTDIR\${PRODUCT_EXE}"
  CreateShortcut "$SMPROGRAMS\Preview 3D\Uninstall Preview 3D.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "${PRODUCT_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${PRODUCT_KEY}" "Version" "${PRODUCT_VERSION}"

  !insertmacro RegisterProgId "${PROGID_GLTF}" "3D model (glTF)"
  !insertmacro RegisterProgId "${PROGID_STL}" "3D model (STL)"
  !insertmacro RegisterProgId "${PROGID_PLY}" "3D model (PLY)"
  !insertmacro RegisterProgId "${PROGID_OBJ}" "3D model (Wavefront OBJ)"
  !insertmacro RegisterProgId "${PROGID_FBX}" "3D model (FBX)"
  !insertmacro RegisterProgId "${PROGID_3MF}" "3D model (3MF)"
  !insertmacro RegisterProgId "${PROGID_USD}" "3D model (Universal Scene Description)"
  !insertmacro RegisterProgId "${PROGID_STEP}" "3D model (STEP)"

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
  !insertmacro RegisterExtension ".obj" "${PROGID_OBJ}"
  !insertmacro RegisterExtension ".fbx" "${PROGID_FBX}"
  !insertmacro RegisterExtension ".3mf" "${PROGID_3MF}"
  !insertmacro RegisterExtension ".usd" "${PROGID_USD}"
  !insertmacro RegisterExtension ".usda" "${PROGID_USD}"
  !insertmacro RegisterExtension ".usdc" "${PROGID_USD}"
  !insertmacro RegisterExtension ".usdz" "${PROGID_USD}"
  !insertmacro RegisterExtension ".step" "${PROGID_STEP}"
  !insertmacro RegisterExtension ".stp" "${PROGID_STEP}"
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
  !insertmacro UnregisterExtension ".obj" "${PROGID_OBJ}"
  !insertmacro UnregisterExtension ".fbx" "${PROGID_FBX}"
  !insertmacro UnregisterExtension ".3mf" "${PROGID_3MF}"
  !insertmacro UnregisterExtension ".usd" "${PROGID_USD}"
  !insertmacro UnregisterExtension ".usda" "${PROGID_USD}"
  !insertmacro UnregisterExtension ".usdc" "${PROGID_USD}"
  !insertmacro UnregisterExtension ".usdz" "${PROGID_USD}"
  !insertmacro UnregisterExtension ".step" "${PROGID_STEP}"
  !insertmacro UnregisterExtension ".stp" "${PROGID_STEP}"
  DeleteRegKey HKLM "Software\Classes\${PROGID_GLTF}"
  DeleteRegKey HKLM "Software\Classes\${PROGID_STL}"
  DeleteRegKey HKLM "Software\Classes\${PROGID_PLY}"
  DeleteRegKey HKLM "Software\Classes\${PROGID_OBJ}"
  DeleteRegKey HKLM "Software\Classes\${PROGID_FBX}"
  DeleteRegKey HKLM "Software\Classes\${PROGID_3MF}"
  DeleteRegKey HKLM "Software\Classes\${PROGID_USD}"
  DeleteRegKey HKLM "Software\Classes\${PROGID_STEP}"
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  DeleteRegKey HKLM "${PRODUCT_KEY}"
  DeleteRegKey /IfEmpty HKLM "Software\Binbuf"

  Delete "$SMPROGRAMS\Preview 3D\Preview 3D.lnk"
  Delete "$SMPROGRAMS\Preview 3D\Uninstall Preview 3D.lnk"
  RMDir "$SMPROGRAMS\Preview 3D"

  Delete "$INSTDIR\worker\Preview3DImportWorker.exe"
  Delete "$INSTDIR\worker\bz2.dll"
  Delete "$INSTDIR\worker\draco.dll"
  Delete "$INSTDIR\worker\fastgltf.dll"
  Delete "$INSTDIR\worker\ktx.dll"
  Delete "$INSTDIR\worker\lib3mf.dll"
  Delete "$INSTDIR\worker\libsharpyuv.dll"
  Delete "$INSTDIR\worker\libwebp.dll"
  Delete "$INSTDIR\worker\meshoptimizer.dll"
  Delete "$INSTDIR\worker\simdjson.dll"
  Delete "$INSTDIR\worker\z.dll"
  Delete "$INSTDIR\worker\zip.dll"
  Delete "$INSTDIR\worker\zstd.dll"
  Delete "$INSTDIR\worker\concrt140.dll"
  Delete "$INSTDIR\worker\msvcp140.dll"
  Delete "$INSTDIR\worker\msvcp140_1.dll"
  Delete "$INSTDIR\worker\vcruntime140.dll"
  Delete "$INSTDIR\worker\vcruntime140_1.dll"
  RMDir "$INSTDIR\worker"

  RMDir /r "$INSTDIR\OpenUsdHost"
  RMDir /r "$INSTDIR\StepHost"

  Delete "$INSTDIR\licenses\basisu.txt"
  Delete "$INSTDIR\licenses\bzip2.txt"
  Delete "$INSTDIR\licenses\draco.txt"
  Delete "$INSTDIR\licenses\fastgltf.txt"
  Delete "$INSTDIR\licenses\ktx.txt"
  Delete "$INSTDIR\licenses\lib3mf.txt"
  Delete "$INSTDIR\licenses\libwebp.txt"
  Delete "$INSTDIR\licenses\libzip.txt"
  Delete "$INSTDIR\licenses\meshoptimizer.txt"
  Delete "$INSTDIR\licenses\opencascade.txt"
  Delete "$INSTDIR\licenses\openusd.txt"
  Delete "$INSTDIR\licenses\simdjson.txt"
  Delete "$INSTDIR\licenses\tbb.txt"
  Delete "$INSTDIR\licenses\tinyusdz.txt"
  Delete "$INSTDIR\licenses\ufbx.txt"
  Delete "$INSTDIR\licenses\zlib.txt"
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
  Delete "$INSTDIR\NOTICE"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\SBOM.cdx.json"
  Delete "$INSTDIR\MANIFEST.json"
  Delete "$INSTDIR\Remove-Preview3DProfile.ps1"
  Delete "$INSTDIR\Provision-Preview3DWorkerAcl.ps1"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  RMDir "$PROGRAMFILES64\Binbuf"

  ${NotifyShell_AssocChanged}
SectionEnd
