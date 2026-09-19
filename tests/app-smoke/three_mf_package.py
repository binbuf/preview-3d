"""Static 3MF distribution contract for built portable and NSIS payload stages.

This does not replace the clean-machine installer lifecycle in 3MF-007.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNTIMES = ('lib3mf.dll', 'zip.dll', 'z.dll', 'bz2.dll')
LICENSES = ('lib3mf.txt', 'libzip.txt', 'zlib.txt', 'bzip2.txt')
COMPONENTS = ('lib3mf', 'libzip', 'zlib', 'bzip2')


def check_stage(stage):
    manifest = json.loads((stage / 'MANIFEST.json').read_text(encoding='utf-8-sig'))
    entries = {entry['path']: entry for entry in manifest['files']}
    for path, entry in entries.items():
        payload = stage / path
        assert payload.is_file(), f'manifest names a missing file: {payload}'
        assert payload.stat().st_size == entry['bytes'], path
        assert hashlib.sha256(payload.read_bytes()).hexdigest() == entry['sha256'], path
    for name in RUNTIMES:
        assert f'worker/{name}' in entries, f'{stage}: missing worker {name}'
        assert name not in entries, f'{stage}: parser DLL escaped worker payload: {name}'
        assert f'OpenUsdHost/{name}' not in entries, f'{stage}: parser DLL entered USD host: {name}'
    for name in LICENSES:
        assert f'licenses/{name}' in entries, f'{stage}: missing license {name}'
    sbom = json.loads((stage / 'SBOM.cdx.json').read_text(encoding='utf-8-sig'))
    components = {component['name'] for component in sbom['components']}
    assert set(COMPONENTS) <= components, f'{stage}: incomplete 3MF SBOM'
    return len(entries)


def check_registration():
    nsi = (ROOT / 'packaging/installer/Preview3D.nsi').read_text(encoding='utf-8-sig')
    reset = (ROOT / 'packaging/installer/Reset-Preview3DTestAssociations.ps1').read_text(encoding='utf-8-sig')
    progid = 'Binbuf.Preview3D.ThreeMF.1'
    assert f'!define PROGID_3MF "{progid}"' in nsi
    assert nsi.count('RegisterProgId "${PROGID_3MF}"') == 1
    assert nsi.count('RegisterExtension ".3mf" "${PROGID_3MF}"') == 1
    assert nsi.count('UnregisterExtension ".3mf" "${PROGID_3MF}"') == 1
    assert nsi.count('DeleteRegKey HKLM "Software\\Classes\\${PROGID_3MF}"') == 1
    assert 'UserChoice' not in nsi
    assert 'shellex' not in nsi.lower()
    assert "'.3mf'" in reset and f"'{progid}'" in reset
    for name in RUNTIMES:
        assert f'Delete "$INSTDIR\\worker\\{name}"' in nsi
    for name in LICENSES:
        assert f'Delete "$INSTDIR\\licenses\\{name}"' in nsi


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stages', nargs='+', type=Path)
    args = parser.parse_args()
    check_registration()
    for stage in args.stages:
        print(f'{stage}: {check_stage(stage)} manifest entries verified')
    print('3MF package and symmetric registration contract passed')
