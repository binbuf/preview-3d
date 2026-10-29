"""Upgrade contract for the NSIS installer's previous-version removal.

The user-visible rule is that a setup run never leaves a second copy behind:
an existing installation is located, shown to the user, and removed by its
own uninstaller before the new payload is written.

Checked here (static; this does not replace the clean-VM upgrade run):

  * .onInit detects the previous installation from its own registry state and
    keeps the values in registers that no nsDialogs/FileFunc macro reuses;
  * a previous-version page exists between the directory page and the install
    page, is shown only when something is detected, is read-only, and reports
    the location ($PREV_LOCATION) that will be removed;
  * the install section removes the previous version before writing payload,
    re-creates the product key the old uninstaller deletes, and handles the
    same-directory case without letting an uninstaller remove the new copy;
  * the previous-version uninstaller is invoked the way an NSIS uninstaller
    must be invoked for an unattended run (/S and _?=), and the profile script
    it calls accepts the switch setup passes;
  * the normally silent removal reports a real failure instead of exiting 0;
  * `/S` does not block on interactive dialogs, and the installed update helper
    is staged and removed with the rest of the payload.

Exit 0 means every check passed.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NSI = ROOT / 'packaging' / 'installer' / 'Preview3D.nsi'
PROFILE = ROOT / 'packaging' / 'portable' / 'Remove-Preview3DProfile.ps1'
PRODUCT_KEY = r'Software\Binbuf\Preview3D'
UNINSTALL_KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\Binbuf.Preview3D'


def section(nsi, start, end=None):
    begin = nsi.index(start)
    stop = nsi.index(end) if end else len(nsi)
    return nsi[begin:stop]


def check_detection(nsi):
    init = section(nsi, 'Function .onInit', 'Function un.onInit')
    for value in ('UninstallString', 'QuietUninstallString', 'InstallLocation', 'Version'):
        assert value in init, f'.onInit does not read {value}'
    assert '!define PRODUCT_KEY "Software\\Binbuf\\Preview3D"' in nsi
    assert '!define UNINSTALL_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Binbuf.Preview3D"' in nsi
    assert 'HKLM "${UNINSTALL_KEY}"' in init and 'HKLM "${PRODUCT_KEY}"' in init
    # The location is taken from the existing installation, never from the
    # requested $INSTDIR.
    assert 'StrCpy ${PREV_LOCATION} "$INSTDIR"' not in init, \
        'detection must not assume the requested install directory'
    assert '${GetParent} "${PREV_LOCATION}" ${PREV_LOCATION}' in init
    # Registers that survives every macro between detection and removal.
    for register in ('$4', '$5'):
        assert not re.search(rf'\$R[0-9]', init), \
            'detection must not keep state in $R0-$R9 (clobbered by nsDialogs/FileFunc)'
    assert '!define PREV_VERSION $4' in nsi
    assert '!define PREV_LOCATION $5' in nsi
    assert '!define PREV_UNINSTALLSTR $6' in nsi
    # A version entry without a working uninstaller is not reported as one.
    assert '${IfNot} ${FileExists} "${PREV_LOCATION}\\Uninstall.exe"' in init
    assert 'StrCpy ${PREV_VERSION} "an earlier version"' in init


def check_page(nsi):
    pages = [
        '!insertmacro MUI_PAGE_WELCOME',
        '!insertmacro MUI_PAGE_LICENSE',
        '!insertmacro MUI_PAGE_DIRECTORY',
        'Page custom PreviousVersionPageCreate PreviousVersionPageLeave',
        '!insertmacro MUI_PAGE_INSTFILES',
    ]
    positions = [nsi.index(page) for page in pages]
    assert positions == sorted(positions), 'previous-version page is out of order'

    create = section(nsi, 'Function PreviousVersionPageCreate', 'Function PreviousVersionPageLeave')
    # Hidden for a clean install: no empty page for the frequent update case.
    assert re.search(r'\$\{If\} \$\{PREV_LOCATION\} == ""\s*\n\s*Abort', create), \
        'the page must not appear when nothing is installed'
    assert 'nsDialogs::Create' in create and 'nsDialogs::Show' in create
    assert '${NSD_CreateDirRequest} 24u 45u -24u 12u "${PREV_LOCATION}"' in create, \
        'the page must show the location that will be removed'
    assert 'SendMessage $PrevPageLocation ${EM_SETREADONLY} 1 0' in create, \
        'the shown location is not editable'
    # The page must not offer a choice that would contradict the removal.
    for forbidden in ('NSD_CreateRadioButton', 'NSD_CreateCheckBox', 'NSD_OnClick',
                      'NSD_OnChange', 'EnableWindow', 'GetDlgItem'):
        assert forbidden not in create, f'previous-version page must stay informational: {forbidden}'
    assert '$INSTDIR' not in create, 'the page must report the previous location, not the new one'


def check_removal(nsi):
    main = section(nsi, 'Section "Preview 3D" SEC_MAIN', 'Section "Uninstall"')
    assert '!insertmacro UpdateExistingInstallation' in main
    payload = main.index('File "${STAGE_DIR}\\Preview3D.exe"')
    assert main.index('!insertmacro UpdateExistingInstallation') < payload, \
        'the previous version must be removed before the new payload is written'

    macro = nsi[nsi.index('!macro UpdateExistingInstallation'):
                nsi.index('Section "Preview 3D" SEC_MAIN')]
    macro = macro[:macro.index('\n!macroend')]
    # The existing uninstaller is the authority for that version's payload.
    assert re.search(r'ExecWait \'"\$\{PREV_LOCATION\}\\Uninstall\.exe" /S _\?=\$\{PREV_LOCATION\}\' \$2', macro), \
        'the previous uninstaller must run unattended and in place'
    assert 'TaskKill' not in macro and 'nsExec' not in macro
    # Same directory: only the stale uninstaller/state may go.
    assert '${If} ${PREV_LOCATION} == $INSTDIR' in macro
    in_place = section(macro, '${If} ${PREV_LOCATION} == $INSTDIR', '${Else}')
    assert 'ExecWait' not in in_place, 'an in-place upgrade must not run the old uninstaller'
    assert 'Delete "$INSTDIR\\Uninstall.exe"' in in_place
    # The previous uninstaller deletes the product root; the new section needs it.
    assert macro.rindex('WriteRegStr HKLM "${PRODUCT_KEY}" "InstallLocation" "$INSTDIR"') > \
        macro.index('ExecWait'), 'the product key must be re-created after the old uninstaller'
    # Lifecycle failures are reported, never swallowed, and never abort.
    assert 'DetailPrint "The previous version did not uninstall (exit code $2)' in macro
    assert 'MessageBox' not in macro and 'Abort' not in macro


def check_unattended(nsi):
    init = section(nsi, 'Function .onInit', 'Function un.onInit')
    uninit = section(nsi, 'Function un.onInit', 'Function LaunchDefaultApps')
    main = section(nsi, 'Section "Preview 3D" SEC_MAIN', 'Section "Uninstall"')
    uninstall = section(nsi, 'Section "Uninstall"')

    # /S may not stop on the Smart App Control warning or the "close the
    # viewer" retry dialog; a still-running window is a bounded timeout.
    assert 'IfSilent sac_policy_ok' in init
    assert 'IfSilent silent_wait_for_viewer' in init and 'IfSilent silent_wait_for_viewer' in uninit
    assert 'silent_viewer_timeout:' in init and 'SetErrorLevel 2' in init
    assert 'silent_viewer_timeout:' in uninit and 'SetErrorLevel 2' in uninit

    # A provisioning failure must not show a blocking dialog under /S.
    assert 'IfSilent acl_failed_silent' in main
    assert re.search(r'acl_failed_silent:\s*\n\s*SetErrorLevel 3\s*\n\s*Abort', main)

    # The viewer's updater needs its helper in the installed payload, and the
    # uninstaller must remove it with the rest of the product scripts.
    assert 'File "${STAGE_DIR}\\${APPLY_UPDATE_SCRIPT}"' in main
    assert 'Delete "$INSTDIR\\${APPLY_UPDATE_SCRIPT}"' in uninstall
    assert '!define APPLY_UPDATE_SCRIPT "Apply-Preview3DUpdate.ps1"' in nsi


def check_uninstaller(nsi, profile):
    uninstall = section(nsi, 'Section "Uninstall"')
    assert '-KeepDirectoryAcls' in uninstall, \
        'the upgrade removal must pass -KeepDirectoryAcls'
    # The script's exit code is captured (not just logged) and, only for a
    # silent run, turned into the uninstaller's exit code: an NSIS uninstaller
    # otherwise exits 0 after a failure, which is what upgrade detection reads.
    assert re.search(r"IfSilent profile_cleanup_silent\s*\n\s*nsExec::ExecToStack '\$\{PSCALL\} \"\$INSTDIR\\Remove-Preview3DProfile\.ps1\"", uninstall)
    assert re.search(r"profile_cleanup_silent:\s*\n\s*nsExec::ExecToStack '.*-KeepDirectoryAcls'", uninstall), \
        'the silent upgrade removal must pass -KeepDirectoryAcls'
    assert re.search(r"StrCpy \$ProfileCleanupExitCode \$0", uninstall), \
        'the cleanup exit code must survive the notification helper'
    assert re.search(r"IfSilent uninstall_silent_result\s*\n\s*Goto uninstall_done\s*\nuninstall_silent_result:\s*\n\s*\$\{If\} \$ProfileCleanupExitCode != \"0\"\s*\n\s*SetErrorLevel 1", uninstall), \
        'a silent uninstall must report a failed profile cleanup'
    assert 'nsExec::ExecToError' not in uninstall
    assert re.search(r'\[switch\]\$KeepDirectoryAcls', profile), \
        'Remove-Preview3DProfile.ps1 does not accept -KeepDirectoryAcls'
    assert 'if (-not $KeepDirectoryAcls)' in profile, \
        '-KeepDirectoryAcls must skip the directory ACE edits'
    # Deleting the profiles stays unconditional.
    assert profile.count("Remove-Profile 'Binbuf.Preview3D.") == 3


if __name__ == '__main__':
    nsi = NSI.read_text(encoding='utf-8-sig')
    profile = PROFILE.read_text(encoding='utf-8-sig')
    check_detection(nsi)
    check_page(nsi)
    check_removal(nsi)
    check_unattended(nsi)
    check_uninstaller(nsi, profile)
    print('installer upgrade contract passed: detection, read-only previous-version page, '
          'silent in-place previous uninstaller, key re-creation, unattended /S path')
