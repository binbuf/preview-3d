"""Environment, version and reference-condition capture."""
from __future__ import annotations

import json
import os
import platform
import subprocess
from pathlib import Path

from common import ROOT, sha256_file


def _powershell(script: str):
    try:
        completed = subprocess.run(
            ['powershell', '-NoProfile', '-NonInteractive', '-Command', script],
            capture_output=True, text=True, timeout=60,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        if completed.returncode != 0:
            return None
        text = completed.stdout.strip()
        return json.loads(text) if text else None
    except Exception:
        return None


def gpu_adapters():
    value = _powershell(
        "Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,DriverDate "
        "| ConvertTo-Json -Compress")
    if not value:
        return []
    if isinstance(value, dict):
        value = [value]
    return value


def operating_system():
    return {
        'platform': platform.platform(),
        'system': platform.system(),
        'release': platform.release(),
        'version': platform.version(),
        'machine': platform.machine(),
    }


def tool_version(executable: Path, *args):
    executable = Path(executable)
    if not executable.exists():
        return None
    try:
        completed = subprocess.run([str(executable), *args], capture_output=True,
                                   text=True, timeout=30,
                                   creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        text = (completed.stdout or '') + (completed.stderr or '')
        return text.strip().splitlines()
    except Exception as error:
        return [f'version probe failed: {error}']


def source_identity():
    """Report the working-tree source without invoking git from verification."""
    return {
        'repository': str(ROOT),
        'viewerSource': 'interactive-viewer/src/app/Preview3D.cpp',
        'cameraSource': 'interactive-viewer/src/render/Renderer.cpp',
        'runnerSource': 'tests/render-quality/verify.py',
        'worktreeCleanNote': 'Hashes of capture-relevant sources are recorded below; '
                             'the harness does not mutate the tree.',
        'hashes': {
            'interactive-viewer/src/app/Preview3D.cpp':
                sha256_file(ROOT / 'interactive-viewer/src/app/Preview3D.cpp'),
            'interactive-viewer/src/render/Renderer.cpp':
                sha256_file(ROOT / 'interactive-viewer/src/render/Renderer.cpp'),
            'interactive-viewer/src/render/Renderer.h':
                sha256_file(ROOT / 'interactive-viewer/src/render/Renderer.h'),
            'interactive-viewer/src/graphics/D3D11On12Overlay.cpp':
                sha256_file(ROOT / 'interactive-viewer/src/graphics/D3D11On12Overlay.cpp'),
            'interactive-viewer/src/graphics/D3D11On12Overlay.h':
                sha256_file(ROOT / 'interactive-viewer/src/graphics/D3D11On12Overlay.h'),
            'tests/render-quality/verify.py':
                sha256_file(ROOT / 'tests/render-quality/verify.py'),
            'tests/render-quality/profiles/T20.json':
                sha256_file(ROOT / 'tests/render-quality/profiles/T20.json'),
        },
    }


def f3d_reference():
    exe = Path('C:/Program Files/F3D/bin/f3d.exe')
    preset = Path('C:/Program Files/F3D/share/f3d/configs/config.d/05_all.json')
    local_source = Path('D:/repos/github/f3d')
    version = tool_version(exe, '--version') if exe.exists() else None
    commit = None
    describe = None
    if local_source.exists():
        try:
            commit = subprocess.run(['git', '-C', str(local_source), 'rev-parse', 'HEAD'],
                                    capture_output=True, text=True, timeout=20).stdout.strip()
            describe = subprocess.run(['git', '-C', str(local_source), 'describe', '--tags'],
                                      capture_output=True, text=True, timeout=20).stdout.strip()
        except Exception:
            pass
    local_exe = None
    if local_source.exists():
        candidates = list(local_source.rglob('f3d.exe'))
        local_exe = str(candidates[0]) if candidates else None
    return {
        'installed': {
            'binary': str(exe),
            'available': exe.exists(),
            'versionOutput': version,
            'version': '3.5.0' if version and any('3.5.0' in line for line in version) else None,
            'vtk': next((line for line in (version or []) if 'VTK version' in line), None),
            'shippedPreset': str(preset),
            'shippedPresetSha256': sha256_file(preset) if preset.exists() else None,
            'shippedPresetAvailable': preset.exists(),
        },
        'localSource': {
            'source': str(local_source),
            'commit': commit,
            'describe': describe,
            'declaredVersion': '4.0.0 source tree',
            'renderableBinary': local_exe,
            'rendered': False,
            'note': 'No local 4.0.0 build exists on this workstation; installed 3.5.0/VTK '
                    'images must not be attributed to 4.0.0 source behavior.',
        },
        'khronosReference': {
            'route': 'Khronos glTF Sample Viewer / Sample Renderer with the generated '
                     'glass-reference.glb and KHR_materials_transmission plus volume.',
            'sampleViewerBinary': None,
            'rendered': False,
            'note': 'No Khronos sample viewer is installed locally. The route is recorded so '
                    'a later review can render the same fixture; F3D gray Camera lens is not '
                    'the accepted glass target.',
        },
    }


def environment(executable: Path):
    return {
        'os': operating_system(),
        'gpu': gpu_adapters(),
        'python': platform.python_version(),
        'viewerBinary': str(executable),
        'viewerSha256': sha256_file(executable) if executable.exists() else None,
        'viewerMtime': executable.stat().st_mtime if executable.exists() else None,
        'f3d': f3d_reference(),
        'source': source_identity(),
    }
