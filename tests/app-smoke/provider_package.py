"""T42 provider payload-closure contract for the installed NSIS distribution.

Two independent checks:

  * static: the NSIS installer stages the provider DLL and its resolved
    app-local CRT closure at the stable install path, the packaging allowlist
    ships the provider only in the installed distribution, the provider's
    dumpbin closure is a closed allowlist, the provider-only static closure must
    not leak as standalone DLLs, and notices/SBOM name the provider;
  * manifest/SBOM: for each built installed stage path, every MANIFEST.json entry
    hashes correctly, the provider DLL and msvcp140_1.dll are present at the root
    (never under another payload directory), and SBOM.cdx.json declares the
    provider component.

Exit 0 means every check passed. This does not replace the clean-machine install
lifecycle (T44).
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NSI = ROOT / 'packaging' / 'installer' / 'Preview3D.nsi'
PORTABLE = ROOT / 'packaging' / 'portable' / 'Create-PortableRelease.ps1'
NOTICES = ROOT / 'packaging' / 'portable' / 'THIRD-PARTY-NOTICES.txt'
CREATE_INSTALLER = ROOT / 'packaging' / 'CreateInstaller.proj'

PROVIDER = 'Preview3DThumbnailProvider.dll'
PROVIDER_CRT = ('msvcp140.dll', 'msvcp140_1.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
PROVIDER_ONLY_MODULES = (
    'fastgltf.dll', 'draco.dll', 'ktx.dll', 'libwebp.dll', 'libsharpyuv.dll', 'basisu.dll',
    'meshoptimizer.dll', 'ufbx.dll', 'lib3mf.dll', 'libzip.dll', 'zip.dll', 'z.dll', 'bz2.dll',
    'tinyusdz.dll', 'simdjson.dll', 'zstd.dll',
)


def check_static():
    nsi = NSI.read_text(encoding='utf-8-sig')
    portable = PORTABLE.read_text(encoding='utf-8-sig')
    notices = NOTICES.read_text(encoding='utf-8-sig')
    create_installer = CREATE_INSTALLER.read_text(encoding='utf-8-sig')

    # Installer stages the provider and its extra resolved CRT module at root.
    assert f'File "${{STAGE_DIR}}\\${{THUMBNAIL_PROVIDER_DLL}}"' in nsi, 'installer does not stage the provider'
    assert '!define THUMBNAIL_PROVIDER_DLL "Preview3DThumbnailProvider.dll"' in nsi
    assert 'File "${STAGE_DIR}\\msvcp140_1.dll"' in nsi, 'installer does not stage the provider CRT'
    assert 'Delete "$INSTDIR\\Preview3DThumbnailProvider.dll"' in nsi, 'uninstaller does not remove the provider'
    assert 'Delete "$INSTDIR\\msvcp140_1.dll"' in nsi, 'uninstaller does not remove the provider CRT'
    # The registration points InprocServer32 at the same stable install path.
    assert 'Preview3DThumbnailRegistration.ps1' in nsi

    # Packaging: provider ships only for the installed distribution.
    assert "Copy-RequiredFile (Join-Path $buildOutput $providerDllName) (Join-Path $stage $providerDllName)" in portable
    assert "$providerDllName = 'Preview3DThumbnailProvider.dll'" in portable
    assert "if ($Distribution -eq 'Installer')" in portable
    assert "$forbiddenNames += 'Preview3DThumbnailProvider.dll'" in portable, 'portable must stay provider-free'
    # Closed dumpbin allowlist and the one-way boundary proof exist.
    assert 'providerAllowedImports' in portable and 'providerCrtImports' in portable
    assert 'The thumbnail provider imports unexpected non-system module' in portable
    assert 'Provider-only static closure leaked' in portable
    assert 'provider boundary is one-way' in portable

    # Notices and SBOM name the provider.
    assert 'Preview3DThumbnailProvider.dll' in notices
    assert 'statically linked' in notices
    assert "name = 'Preview3DThumbnailProvider'" in portable, 'SBOM component missing'

    # The `msbuild /t:CreateInstaller` path used by the release workflow invokes
    # Create-Installer.ps1 with -SkipBuild, so the installer target itself must
    # build the provider or the payload stage fails on the missing DLL.
    assert 'thumbnail-provider\\Preview3DThumbnailProvider.vcxproj' in create_installer, \
        'CreateInstaller.proj does not build the thumbnail provider'


def check_stage(stage):
    manifest = json.loads((stage / 'MANIFEST.json').read_text(encoding='utf-8-sig'))
    entries = {entry['path']: entry for entry in manifest['files']}
    for path, entry in entries.items():
        payload = stage / path
        assert payload.is_file(), f'manifest names a missing file: {payload}'
        assert payload.stat().st_size == entry['bytes'], path
        assert hashlib.sha256(payload.read_bytes()).hexdigest() == entry['sha256'], path

    assert PROVIDER in entries, f'{stage}: missing provider DLL at the install root'
    for name in PROVIDER_CRT:
        assert name in entries, f'{stage}: missing provider runtime {name}'
    for name in PROVIDER_ONLY_MODULES:
        for prefix in ('', 'worker/', 'OpenUsdHost/', 'StepHost/'):
            assert f'{prefix}{name}' not in entries, f'{stage}: provider-only module leaked: {prefix}{name}'
    for name in entries:
        leaf = Path(name).name
        if leaf.startswith('TK') and leaf.endswith('.dll'):
            raise AssertionError(f'{stage}: OCCT toolkit escaped into the standalone payload: {name}')

    sbom = json.loads((stage / 'SBOM.cdx.json').read_text(encoding='utf-8-sig'))
    components = {component['name'] for component in sbom['components']}
    assert 'Preview3DThumbnailProvider' in components, f'{stage}: SBOM omits the provider component'
    assert 'opencascade' in components, f'{stage}: SBOM omits the statically linked OCCT closure'
    return len(entries)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stages', nargs='*', type=Path)
    args = parser.parse_args()
    check_static()
    for stage in args.stages:
        print(f'{stage}: {check_stage(stage)} manifest entries verified')
    print('provider payload-closure contract passed')