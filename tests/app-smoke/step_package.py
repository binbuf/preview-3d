"""Static STEP distribution contract for built portable and NSIS payload stages.

This does not replace the clean-machine installer lifecycle in STEP-008.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OCCT_DLLS = (
    'TKBO.dll', 'TKBRep.dll', 'TKCAF.dll', 'TKCDF.dll', 'TKDE.dll', 'TKDESTEP.dll',
    'TKernel.dll', 'TKG2d.dll', 'TKG3d.dll', 'TKGeomAlgo.dll', 'TKGeomBase.dll',
    'TKHLR.dll', 'TKLCAF.dll', 'TKMath.dll', 'TKMesh.dll', 'TKPrim.dll',
    'TKService.dll', 'TKShHealing.dll', 'TKTopAlgo.dll', 'TKV3d.dll', 'TKVCAF.dll',
    'TKXCAF.dll', 'TKXSBase.dll',
)
CRT = ('concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
LICENSES = ('opencascade.txt',)
COMPONENTS = ('opencascade',)


def check_stage(stage):
    manifest = json.loads((stage / 'MANIFEST.json').read_text(encoding='utf-8-sig'))
    entries = {entry['path']: entry for entry in manifest['files']}
    for path, entry in entries.items():
        payload = stage / path
        assert payload.is_file(), f'manifest names a missing file: {payload}'
        assert payload.stat().st_size == entry['bytes'], path
        assert hashlib.sha256(payload.read_bytes()).hexdigest() == entry['sha256'], path
    assert 'StepHost/Preview3DStepHost.exe' in entries, f'{stage}: missing STEP host'
    for name in OCCT_DLLS:
        assert f'StepHost/{name}' in entries, f'{stage}: missing StepHost/{name}'
        assert name not in entries, f'{stage}: OCCT DLL escaped StepHost: {name}'
        assert f'worker/{name}' not in entries, f'{stage}: OCCT DLL entered worker: {name}'
        assert f'OpenUsdHost/{name}' not in entries, f'{stage}: OCCT DLL entered USD host: {name}'
    for name in CRT:
        assert f'StepHost/{name}' in entries, f'{stage}: missing StepHost CRT {name}'
    for name in LICENSES:
        assert f'licenses/{name}' in entries, f'{stage}: missing license {name}'
    sbom = json.loads((stage / 'SBOM.cdx.json').read_text(encoding='utf-8-sig'))
    components = {component['name'] for component in sbom['components']}
    assert set(COMPONENTS) <= components, f'{stage}: incomplete STEP SBOM'
    return len(entries)


def check_registration():
    nsi = (ROOT / 'packaging/installer/Preview3D.nsi').read_text(encoding='utf-8-sig')
    reset = (ROOT / 'packaging/installer/Reset-Preview3DTestAssociations.ps1').read_text(encoding='utf-8-sig')
    progid = 'Binbuf.Preview3D.STEP.1'
    assert f'!define PROGID_STEP "{progid}"' in nsi
    assert nsi.count('RegisterProgId "${PROGID_STEP}"') == 1
    for ext in ('.step', '.stp'):
        assert nsi.count(f'RegisterExtension "{ext}" "${{PROGID_STEP}}"') == 1, ext
        assert nsi.count(f'UnregisterExtension "{ext}" "${{PROGID_STEP}}"') == 1, ext
        assert f"'{ext}'" in reset, ext
    assert nsi.count('DeleteRegKey HKLM "Software\\Classes\\${PROGID_STEP}"') == 1
    assert 'UserChoice' not in nsi
    assert 'shellex' not in nsi.lower()
    assert f"'{progid}'" in reset
    assert 'File /r "${STAGE_DIR}\\StepHost"' in nsi
    assert 'RMDir /r "$INSTDIR\\StepHost"' in nsi
    for name in LICENSES:
        assert f'Delete "$INSTDIR\\licenses\\{name}"' in nsi


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stages', nargs='+', type=Path)
    args = parser.parse_args()
    check_registration()
    for stage in args.stages:
        print(f'{stage}: {check_stage(stage)} manifest entries verified')
    print('STEP package and symmetric registration contract passed')
