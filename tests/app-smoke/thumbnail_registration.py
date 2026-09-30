"""T41 shell-registration lifecycle contract.

Two independent checks:

  * static: the NSIS installer and the product registration scripts agree with
    the frozen identities in ``thumbnail-provider/FamilyRouting.h``, register
    all eight CLSIDs and every direct extension, never write an
    ``IPreviewHandler``/context-menu/property/icon-overlay handler or an
    isolation opt-out, and announce the change from a non-elevated action;
  * lifecycle: runs the exact product registration script against a sandboxed
    per-user key store (no elevation, no real association store touched) and
    verifies install, non-clobber conflict, same-version repair rerun and
    uninstall, including a pre-seeded third-party handler and a pre-seeded
    default-app association that must survive.

Exit 0 means every check passed.
"""
import re
import subprocess
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NSI = ROOT / 'packaging' / 'installer' / 'Preview3D.nsi'
REGISTER = ROOT / 'packaging' / 'installer' / 'Preview3DThumbnailRegistration.ps1'
NOTIFY = ROOT / 'packaging' / 'installer' / 'Notify-Preview3DShellChanged.ps1'
ROUTING = ROOT / 'thumbnail-provider' / 'FamilyRouting.h'

HANDLER_GUID = '{E357FCCD-A995-4576-B01F-234630154E96}'
FOREIGN_HANDLER = '{11111111-1111-1111-1111-111111111111}'
FOREIGN_PROGID = 'ForeignVendor.Model.1'
FAKE_DLL = r'C:\fake\Preview3DThumbnailProvider.dll'

EXPECTED = {
    '{A592F425-EA68-4C88-BB96-020805D4BE56}': ['.glb', '.gltf'],
    '{BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9}': ['.stl'],
    '{F4DC6119-E235-4BAC-8089-54EDD84F8492}': ['.ply'],
    '{D4722752-C480-4D9C-BEBE-1A9B514A8846}': ['.obj'],
    '{FBC218D4-FD2C-41DF-B168-7F3B9E53C84E}': ['.fbx'],
    '{D8389A63-8526-454A-9892-72F3149484B9}': ['.3mf'],
    '{E938BC70-4C08-4446-A15D-EE31576BFB48}': ['.usd', '.usda', '.usdc', '.usdz'],
    '{6EE961AC-AC3B-4958-A898-E30523FEE79D}': ['.step', '.stp'],
}
DIRECT_EXTENSIONS = [ext for exts in EXPECTED.values() for ext in exts]


def powershell():
    for name in ('powershell.exe', 'pwsh.exe', 'powershell', 'pwsh'):
        try:
            subprocess.run([name, '-NoProfile', '-Command', 'exit 0'],
                           check=True, capture_output=True, text=True)
            return name
        except (OSError, subprocess.CalledProcessError):
            continue
    raise RuntimeError('no PowerShell interpreter found')


PS = powershell()


def run_script(*arguments):
    command = [PS, '-NoLogo', '-NoProfile', '-NonInteractive',
               '-ExecutionPolicy', 'Bypass', '-File', str(REGISTER), *arguments]
    return subprocess.run(command, capture_output=True, text=True)


def check_static():
    nsi = NSI.read_text(encoding='utf-8-sig')
    register = REGISTER.read_text(encoding='utf-8-sig')
    notify = NOTIFY.read_text(encoding='utf-8-sig')
    routing = ROUTING.read_text(encoding='utf-8-sig')

    # Identity agreement with the frozen routing table.
    routing_clsids = set(re.findall(
        r'\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}', routing))
    routing_clsids.discard(HANDLER_GUID)
    assert routing_clsids == set(EXPECTED), f'FamilyRouting.h drift: {routing_clsids}'

    registered_clsids = set(re.findall(r"Clsid\s*=\s*'([^']+)'", register))
    assert registered_clsids == set(EXPECTED), f'registration table drift: {registered_clsids}'
    assert HANDLER_GUID in register, 'handler GUID missing'

    for clsid, extensions in EXPECTED.items():
        assert f"'{clsid}'" in register, clsid
        for extension in extensions:
            assert f"'{extension}'" in register, (clsid, extension)

    # Every direct extension is routed; .mtl is not.
    for extension in DIRECT_EXTENSIONS:
        assert f"'{extension}'" in register, extension
    assert "'.mtl'" not in register

    # No forbidden handler categories or isolation opt-out.
    for forbidden in ('IPreviewHandler', 'ContextMenuHandlers', 'PropertySheetHandlers', 'IconOverlay'):
        assert forbidden.lower() not in register.lower(), forbidden
    for forbidden in ('IPreviewHandler', 'ContextMenuHandlers', 'PropertySheetHandlers', 'IconOverlay',
                      'DllRegisterServer', 'DllUnregisterServer', 'UserChoice'):
        assert forbidden.lower() not in nsi.lower(), forbidden
    nsi_write_isolation = re.search(r'WriteReg\w+\s+[^\n]*DisableProcessIsolation', nsi)
    assert nsi_write_isolation is None, 'installer must never write DisableProcessIsolation'
    assert 'DisableProcessIsolation' not in notify

    # Installer wiring.
    assert '"Preview3DThumbnailRegistration.ps1"' in nsi
    assert 'Notify-Preview3DShellChanged.ps1' in nsi
    assert nsi.count('-Action Install -Scope HKLM') == 1
    assert nsi.count('-Action Uninstall -Scope HKLM') == 1
    assert 'Preview3DThumbnailProvider.dll' in nsi
    assert 'Call NotifyPreview3DShellChanged' in nsi
    assert 'Call un.NotifyPreview3DShellChanged' in nsi
    assert 'runas.exe" /trustlevel:0x20000' in nsi
    assert 'SHCNE_ASSOCCHANGED' in (notify + nsi)

    # The notification is the exact SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, null, null).
    assert '0x08000000' in notify
    assert '[Preview3DShellNotify]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)' in notify


# --- sandboxed registry lifecycle -------------------------------------------------

import winreg  # noqa: E402  (Windows-only import after static checks)

HKCU = winreg.HKEY_CURRENT_USER
SANDBOX_PARENT = r'Software\Preview3DThumbnailRegistrationTest'
SANDBOX = SANDBOX_PARENT + '\\' + str(uuid.uuid4())
CLASSES = SANDBOX + r'\Classes'
STATE = SANDBOX + r'\State'


def open_key(path, create=False):
    if create:
        return winreg.CreateKeyEx(HKCU, path, 0, winreg.KEY_READ | winreg.KEY_WRITE)
    return winreg.OpenKey(HKCU, path, 0, winreg.KEY_READ)


def key_exists(path):
    try:
        key = open_key(path)
        key.Close()
        return True
    except FileNotFoundError:
        return False


def default_value(path):
    try:
        key = open_key(path)
    except FileNotFoundError:
        return None
    try:
        try:
            return winreg.QueryValueEx(key, '')[0]
        except FileNotFoundError:
            return None
    finally:
        key.Close()


def named_value(path, name):
    try:
        key = open_key(path)
    except FileNotFoundError:
        return None
    try:
        try:
            return winreg.QueryValueEx(key, name)[0]
        except FileNotFoundError:
            return None
    finally:
        key.Close()


def set_default(path, value):
    key = open_key(path, create=True)
    try:
        winreg.SetValueEx(key, '', 0, winreg.REG_SZ, value)
    finally:
        key.Close()


def set_named(path, name, value):
    key = open_key(path, create=True)
    try:
        winreg.SetValueEx(key, name, 0, winreg.REG_SZ, value)
    finally:
        key.Close()


def delete_tree(path):
    if not key_exists(path):
        return
    _delete_children(path)
    winreg.DeleteKey(HKCU, path)


def _delete_children(path):
    key = open_key(path)
    try:
        names = []
        index = 0
        while True:
            try:
                names.append(winreg.EnumKey(key, index))
                index += 1
            except OSError:
                break
    finally:
        key.Close()
    for name in names:
        child = path + '\\' + name
        _delete_children(child)
        winreg.DeleteKey(HKCU, child)


def reseed_foreign():
    """Re-create the pre-existing third-party state that must survive."""
    set_default(rf'{CLASSES}\.stl\shellex\{HANDLER_GUID}', FOREIGN_HANDLER)
    set_default(rf'{CLASSES}\.obj\shellex\{HANDLER_GUID}', FOREIGN_HANDLER)
    # A pre-existing default-app association on .stl (as an unrelated vendor's ProgID).
    set_default(rf'{CLASSES}\.stl', FOREIGN_PROGID)
    set_named(rf'{CLASSES}\.stl\OpenWithProgids', FOREIGN_PROGID, '')


def check_install():
    for clsid, extensions in EXPECTED.items():
        clsid_key = rf'{CLASSES}\CLSID\{clsid}'
        assert key_exists(clsid_key), f'CLSID not registered: {clsid}'
        assert default_value(clsid_key) == f'Preview 3D {_family(clsid)} Thumbnail Provider'
        assert named_value(clsid_key, 'ThreadingModel') is None
        assert named_value(clsid_key, 'DisableProcessIsolation') is None
        assert default_value(clsid_key + r'\InprocServer32') == FAKE_DLL, clsid
        assert named_value(clsid_key + r'\InprocServer32', 'ThreadingModel') == 'Apartment'
        appid = named_value(clsid_key, 'AppID')
        assert appid, f'AppID missing for {clsid}'
        assert named_value(rf'{CLASSES}\AppID\{appid}', 'DllSurrogate') == ''
        assert named_value(rf'{CLASSES}\AppID\{appid}', 'DisableProcessIsolation') is None
        for extension in extensions:
            handler_key = rf'{CLASSES}\{extension}\shellex\{HANDLER_GUID}'
            if extension in ('.stl', '.obj'):
                assert default_value(handler_key) == FOREIGN_HANDLER, extension
            else:
                assert default_value(handler_key) == clsid, extension

    # Default-app association and unrelated handlers survive.
    assert default_value(rf'{CLASSES}\.stl') == FOREIGN_PROGID
    assert named_value(rf'{CLASSES}\.stl\OpenWithProgids', FOREIGN_PROGID) is not None

    # Conflicts are recorded; owned extensions are tracked.
    assert named_value(rf'{STATE}\Conflicts', '.stl') == FOREIGN_HANDLER
    assert named_value(rf'{STATE}\Conflicts', '.obj') == FOREIGN_HANDLER
    assert named_value(rf'{STATE}\OwnedShellEx', '.stl') is None
    assert named_value(rf'{STATE}\OwnedShellEx', '.ply') == '{F4DC6119-E235-4BAC-8089-54EDD84F8492}'


def check_cleanup():
    for clsid in EXPECTED:
        assert not key_exists(rf'{CLASSES}\CLSID\{clsid}'), f'CLSID survived uninstall: {clsid}'
    for extension in DIRECT_EXTENSIONS:
        handler = rf'{CLASSES}\{extension}\shellex\{HANDLER_GUID}'
        if extension in ('.stl', '.obj'):
            assert default_value(handler) == FOREIGN_HANDLER, extension
        else:
            assert not key_exists(handler), f'handler survived uninstall: {extension}'
    assert default_value(rf'{CLASSES}\.stl') == FOREIGN_PROGID
    assert named_value(rf'{CLASSES}\.stl\OpenWithProgids', FOREIGN_PROGID) is not None
    assert not key_exists(STATE), 'product state key survived uninstall'


def _family(clsid):
    return {
        '{A592F425-EA68-4C88-BB96-020805D4BE56}': 'glTF',
        '{BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9}': 'STL',
        '{F4DC6119-E235-4BAC-8089-54EDD84F8492}': 'PLY',
        '{D4722752-C480-4D9C-BEBE-1A9B514A8846}': 'OBJ',
        '{FBC218D4-FD2C-41DF-B168-7F3B9E53C84E}': 'FBX',
        '{D8389A63-8526-454A-9892-72F3149484B9}': '3MF',
        '{E938BC70-4C08-4446-A15D-EE31576BFB48}': 'USD',
        '{6EE961AC-AC3B-4958-A898-E30523FEE79D}': 'STEP',
    }[clsid]


def check_lifecycle():
    delete_tree(SANDBOX)
    try:
        reseed_foreign()

        install = run_script('-Action', 'Install', '-Scope', 'HKCU',
                             '-ClassesRoot', f'HKCU:\\{CLASSES}',
                             '-StateRoot', f'HKCU:\\{STATE}',
                             '-DllPath', FAKE_DLL)
        assert install.returncode == 0, install.stderr
        assert 'interactive Open With support installed' in install.stdout
        check_install()

        # Same-version repair rerun: remove a product key + handler, rerun install.
        winreg.DeleteKey(HKCU, rf'{CLASSES}\CLSID\{{A592F425-EA68-4C88-BB96-020805D4BE56}}\InprocServer32')
        winreg.DeleteKey(HKCU, rf'{CLASSES}\CLSID\{{A592F425-EA68-4C88-BB96-020805D4BE56}}')
        winreg.DeleteKey(HKCU, rf'{CLASSES}\.ply\shellex\{HANDLER_GUID}')
        repair = run_script('-Action', 'Install', '-Scope', 'HKCU',
                            '-ClassesRoot', f'HKCU:\\{CLASSES}',
                            '-StateRoot', f'HKCU:\\{STATE}',
                            '-DllPath', FAKE_DLL)
        assert repair.returncode == 0, repair.stderr
        assert key_exists(rf'{CLASSES}\CLSID\{{A592F425-EA68-4C88-BB96-020805D4BE56}}')
        assert default_value(rf'{CLASSES}\.ply\shellex\{HANDLER_GUID}') == '{F4DC6119-E235-4BAC-8089-54EDD84F8492}'
        # Non-clobber held through repair.
        assert default_value(rf'{CLASSES}\.stl\shellex\{HANDLER_GUID}') == FOREIGN_HANDLER

        uninstall = run_script('-Action', 'Uninstall', '-Scope', 'HKCU',
                               '-ClassesRoot', f'HKCU:\\{CLASSES}',
                               '-StateRoot', f'HKCU:\\{STATE}',
                               '-DllPath', FAKE_DLL)
        assert uninstall.returncode == 0, uninstall.stderr
        check_cleanup()
    finally:
        delete_tree(SANDBOX)
        try:
            winreg.DeleteKey(HKCU, SANDBOX_PARENT)
        except OSError:
            pass  # a concurrent run may still own a sibling sandbox


if __name__ == '__main__':
    check_static()
    checks = ['static contract']
    if hasattr(winreg, 'HKEY_CURRENT_USER'):
        check_lifecycle()
        checks.append('sandboxed install/conflict/repair/uninstall lifecycle')
    print('thumbnail registration checks passed: ' + ', '.join(checks))