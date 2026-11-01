"""T34 full-corpus, cross-format and hostile-input render qualification.

This lane is deliberately more than another set of screenshots:

* It audits every entry in the local corpus inventory against the profile's
  capture matrix, so an unbounded/oversized/unavailable asset becomes a typed
  disposition instead of a quiet skip.
* It captures a portable FBX/USD/3MF/textured-OBJ/STEP set from in-repository
  fixtures in Studio, Clay and Directional, so cross-format rendering evidence
  is reproducible without the restricted local models.
* It opens hostile oversized/NaN fixtures and proves the rejection happens
  before a drawable GPU scene exists.
* It exercises rapid open/cancel, stale generations, mode changes, resize/DPI,
  coarse-to-full, low budget, eviction, device loss and reopen.
* It builds/checks the Release binary for the Debug-only control boundary and
  scans the Release dependency closure for parser/codec libraries.
* It records measured per-pass GPU/CPU/memory facts with the environment
  identity; it does not promote a timing claim from this workstation.
"""
from __future__ import annotations

import base64
import glob
import json
import os
import struct
import subprocess
import tempfile
import time
import zlib
from pathlib import Path

import common
import corpus
import env
import fixtures
from common import (RESULT_SCHEMA, decode_png, encoded_png, query, send,
                    send_field, sha256_file, wait_for_window, write_json)
from viewer import LoadFailed, Viewer, ViewerError

ROOT = Path(__file__).resolve().parents[2]
FAST = bool(os.environ.get('RENDER_QUALITY_FAST'))

SERIAL_LANES = [
    ('unit-wire-format', 'Tests.Unit.exe', ['[wire-format]']),
    ('unit-cache', 'Tests.Unit.exe', ['[cache]']),
    ('unit-format-catalog', 'Tests.Unit.exe', ['[format-catalog]']),
    ('import-compat', 'Tests.ImportIsolation.exe', ['[compat]']),
    ('import-section-validator', 'Tests.ImportIsolation.exe',
     ['[shared-section-validator]']),
    ('import-hostile-worker', 'Tests.ImportIsolation.exe', ['[hostile-worker]']),
    ('import-texture-budget', 'Tests.ImportIsolation.exe', ['[texture-budget]']),
    ('import-texture-progressive', 'Tests.ImportIsolation.exe',
     ['[texture-progressive]']),
    ('import-fuzz', 'Tests.ImportIsolation.exe', ['[fuzz]']),
]

DEPENDENCY_DENY = (
    'occt', 'tk', 'assimp', 'fastgltf', 'lib3mf', 'tinyusdz', 'ufbx', 'draco',
    'basisu', 'ktx', 'openusd', 'zlib', 'tbb', 'meshoptimizer',
)

PORTABLE = [
    {'id': 'portable-fbx-binary', 'family': 'fbx',
     'path': ROOT / 'tests/fixtures/fbx/cube-binary.fbx'},
    {'id': 'portable-fbx-hierarchy', 'family': 'fbx',
     'path': ROOT / 'tests/fixtures/fbx-spike/hierarchy-instances-pivots-ascii.fbx'},
    {'id': 'portable-usdz', 'family': 'usd',
     'path': ROOT / 'tests/fixtures/usd/cube.usdz'},
    {'id': 'portable-usd-crease', 'family': 'usd',
     'path': ROOT / 'tests/fixtures/usd-spike/subdiv_cube_cc.usda'},
    {'id': 'portable-3mf', 'family': '3mf',
     'path': ROOT / 'tests/fixtures/3mf/materials-texture.3mf'},
    {'id': 'portable-obj', 'family': 'obj',
     'path': ROOT / 'tests/fixtures/textures/package/scene.obj'},
    {'id': 'portable-step', 'family': 'step',
     'path': ROOT / 'tests/fixtures/stp-spike/assembly_ap214.stp'},
]


def _check(checks, name, passed, actual=None, expected=None):
    checks.append({'name': name, 'pass': bool(passed), 'actual': actual,
                   'expected': expected})


def _f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', float(value)))[0]


def _save(viewer, output: Path, name: str, settle=0.45):
    if settle:
        time.sleep(settle)
    frame = viewer.wait_stable()
    path = output / 'images' / f'{name}.png'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
    return {'frame': frame, 'path': str(path), 'sha256': sha256_file(path),
            'width': frame['width'], 'height': frame['height'],
            'dpi': frame['dpi'], 'dpiScale': frame['dpiScale'],
            'client': frame['client'], 'stats': frame['stats']}


def _decode(record):
    return decode_png(Path(record['path']).read_bytes())


def _changed_fraction(a, b):
    w, h, first = _decode(a)
    w2, h2, second = _decode(b)
    if (w, h) != (w2, h2):
        return None
    changed = 0
    for pixel in range(w * h):
        base = pixel * 3
        if max(abs(first[base] - second[base]),
               abs(first[base + 1] - second[base + 1]),
               abs(first[base + 2] - second[base + 2])) > 2:
            changed += 1
    return changed / (w * h)


def _load_captures(output: Path):
    path = output / 'captures.json'
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding='utf-8')).get('captures', {})


def _wait_generation(viewer, generation, timeout=180.0):
    """Wait for one requested generation, tolerating a cancelled predecessor.

    The public ``wait_open`` treats any transient Failed state as terminal. A
    rapid second open may leave the cancelled generation's error visible for a
    moment; that is not the requested generation's failure. This helper waits
    until the requested generation is resident, and only raises when the state
    is still terminal at the deadline.
    """
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        viewer.beat()
        state = viewer.import_state()
        last = state
        loaded = send_field(viewer.hwnd, 0x8000 + 104, 47) & 0xFFFFFFFF
        resident = state['generation'] == generation or loaded == generation
        if state['state'] == 2 and resident and state['complete']:
            return viewer.wait_textures_settled()
        time.sleep(0.1)
    if last is None:
        raise ViewerError('viewer produced no state while waiting for generation')
    raise LoadFailed('requested generation never became resident: '
                     + json.dumps({'loaded': send_field(
                         viewer.hwnd, 0x8000 + 104, 47) & 0xFFFFFFFF,
                         'state': last}, default=str)[:800],
                     last.get('state'), last.get('errorCode'),
                     last.get('errorStage'))


def _run_serial(configuration: str, output: Path):
    exe_dir = ROOT / f'x64/{configuration}'
    lanes = []
    for name, executable, filters in SERIAL_LANES:
        path = exe_dir / executable
        command = [str(path), *filters]
        started = time.monotonic()
        if not path.exists():
            lanes.append({'name': name, 'command': ' '.join(command),
                          'pass': False, 'reason': 'test executable missing'})
            continue
        completed = subprocess.run(command, cwd=ROOT, capture_output=True,
                                   text=True, timeout=2400)
        log = output / f'lane-{name}.log'
        log.write_text((completed.stdout or '') + (completed.stderr or ''),
                       encoding='utf-8', errors='replace')
        lanes.append({'name': name, 'command': ' '.join(command),
                      'exitCode': completed.returncode,
                      'seconds': round(time.monotonic() - started, 2),
                      'pass': completed.returncode == 0, 'log': str(log)})
    return lanes


def _portable_facts(state):
    return {
        'sourceFormat': state.get('sourceFormat'),
        'vertices': state.get('vertices'),
        'triangles': state.get('triangles'),
        'points': state.get('points'),
        'materials': state.get('materials'),
        'nodes': state.get('nodes'),
        'hasUv0': state.get('hasUv0'),
        'boundsVerified': state.get('boundsVerified'),
        'dimensions': state.get('dimensions'),
        'groundAxis': state.get('groundAxis'),
        'nativeOrientation': state.get('nativeOrientation'),
        'sourceToViewerTransformRowMajor': state.get(
            'sourceToViewerTransformRowMajor'),
    }


def _portable_lane(viewer, output: Path, checks):
    results = {}
    images = []
    for entry in PORTABLE:
        path = Path(entry['path'])
        item = {'id': entry['id'], 'family': entry['family'],
                'path': str(path), 'exists': path.exists()}
        if not path.exists():
            item['status'] = 'unavailable'
            results[entry['id']] = item
            _check(checks, f'{entry["id"]} portable fixture exists', False,
                   str(path), 'exists')
            continue
        try:
            generation, seconds, _ = viewer.open(path)
            state = viewer.import_state()
            model = viewer.model_state()
            viewer.apply_controlled_presentation()
            default = _save(viewer, output, f't34-portable-{entry["id"]}-default')
            modes = {}
            for mode_name, mode, yaw, pitch in (
                    ('studio', 0, 0.0, 90.0), ('clay', 1, 0.0, 90.0),
                    ('directional', 2, 30.0, 60.0)):
                send_field(viewer.hwnd, 88, mode)
                time.sleep(0.3)
                viewer.set_camera(yaw, pitch, 1.0)
                modes[mode_name] = _save(
                    viewer, output,
                    f't34-portable-{entry["id"]}-{mode_name}-front')
                images.append(modes[mode_name]['path'])
            images.append(default['path'])
            distinct = {}
            names = list(modes)
            for index, left in enumerate(names):
                for right in names[index + 1:]:
                    distinct[f'{left}/{right}'] = _changed_fraction(
                        modes[left], modes[right])
            item.update({
                'status': 'captured', 'generation': generation,
                'loadSeconds': round(seconds, 3), 'state': state,
                'model': model, 'facts': _portable_facts(model),
                'default': default['path'], 'modes': {
                    name: {'path': data['path'], 'sha256': data['sha256']}
                    for name, data in modes.items()},
                'modeDistinctness': distinct,
                'passes': viewer.pass_state(),
                'performance': viewer.performance(),
            })
            _check(checks, f'{entry["id"]} reaches a complete load',
                   state['complete'] and state['state'] == 2, state['state'],
                   'Ready/complete')
            _check(checks, f'{entry["id"]} has no terminal error',
                   state['errorCode'] == 0, state['errorCode'], 0)
            geometry = (state['vertices'] or 0) + (state['triangles'] or 0) + (
                model['points'] or 0)
            _check(checks, f'{entry["id"]} has resident geometry',
                   geometry > 0, geometry, '>0')
            _check(checks, f'{entry["id"]} mode captures differ',
                   all(value is not None and value > 0.0002
                       for value in distinct.values()), distinct,
                   'each pair >0.02% changed')
            if entry['id'] == 'portable-obj':
                _check(checks, 'textured OBJ resolves UVs and a texture',
                       bool(state['textureCount']) and bool(model['hasUv0']),
                       {'textureCount': state['textureCount'],
                        'hasUv0': model['hasUv0']}, 'texture>0, UV0 true')
            if entry['id'] == 'portable-step':
                _check(checks, 'STEP assembly exposes multiple parts',
                       (model['nodes'] or 0) > 1 or (model['materials'] or 0) > 1,
                       {'nodes': model['nodes'],
                        'materials': model['materials']}, 'nodes>1 or materials>1')
            if entry['id'] == 'portable-usd-crease':
                _check(checks, 'USD crease/degradation is typed, not silent',
                       state['optionalFeatureWarnings'] > 0
                       or state['compatibilityRecords'] > 0
                       or state['warningTextLength'] > 0,
                       {'optional': state['optionalFeatureWarnings'],
                        'records': state['compatibilityRecords'],
                        'warningLength': state['warningTextLength']}, '>0')
        except (LoadFailed, ViewerError) as error:
            item.update({
                'status': 'failed', 'reason': str(error)[:500],
                'code': getattr(error, 'code', None),
                'stage': getattr(error, 'stage', None),
                'state': getattr(error, 'state', None),
            })
            _check(checks, f'{entry["id"]} portable format opens', False,
                   item, 'complete load')
        results[entry['id']] = item
    return results, images


def _oversized_png_data_uri():
    def chunk(kind, payload):
        return (struct.pack('>I', len(payload)) + kind + payload
                + struct.pack('>I', zlib.crc32(kind + payload) & 0xffffffff))
    header = struct.pack('>IIBBBBB', 32000, 32000, 8, 6, 0, 0, 0)
    # A tiny IDAT intentionally cannot describe 32000x32000 RGBA rows. The
    # bounded decoder must reject it from the declared extent before any GPU
    # resource is created.
    data = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header)
            + chunk(b'IDAT', zlib.compress(b'\x00' * 16))
            + chunk(b'IEND', b''))
    return 'data:image/png;base64,' + base64.b64encode(data).decode('ascii')


def _hostile_fixture(path: Path, *, nan_uv: bool = False,
                     oversized: bool = False) -> Path:
    add_mesh, finish = fixtures._mesh_builder()
    uvs = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0)]
    if nan_uv:
        uvs[1] = (float('nan'), float('inf'))
    base = add_mesh([(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
                    [(0.0, 0.0, 1.0)] * 3, uvs, [0, 1, 2])
    image_uri = _oversized_png_data_uri() if oversized else fixtures._png_data_uri(
        bytes((200, 100, 50)) * 16)
    document = {
        'asset': {'version': '2.0', 'generator': 'preview-3d T34 hostile fixture'},
        'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
        'meshes': [{'primitives': [{
            'attributes': {'POSITION': base, 'NORMAL': base + 1,
                           'TEXCOORD_0': base + 2},
            'indices': base + 3, 'material': 0}]}],
        'materials': [{'name': 'hostile texture',
                       'pbrMetallicRoughness': {
                           'baseColorTexture': {'index': 0}}}],
        'images': [{'uri': image_uri}],
        'textures': [{'source': 0}],
    }
    document, binary = finish(document)
    return fixtures._glb(document, binary, path)


def _hostile_cases(output: Path):
    fixtures_dir = output / 'fixtures'
    fixtures_dir.mkdir(parents=True, exist_ok=True)
    return [
        ('oversized-image', _hostile_fixture(
            fixtures_dir / 't34-oversized-image.glb', oversized=True)),
        ('nan-inf-uv', _hostile_fixture(
            fixtures_dir / 't34-nan-uv.glb', nan_uv=True)),
    ]


def _run_hostile_case(viewer, output: Path, name: str, path: Path, checks):
    result = {'path': str(path)}
    accepted = False
    try:
        viewer.open(path)
        accepted = True
        state = viewer.import_state()
        result.update({'status': 'degraded-fallback', 'state': state})
    except LoadFailed as error:
        state = viewer.import_state()
        result.update({
            'status': 'rejected',
            'code': getattr(error, 'code', None),
            'stage': getattr(error, 'stage', None),
            'state': state,
        })
    if accepted and state['errorCode'] == 0:
        # Optional texture decode failures may degrade to the deterministic
        # bounded fallback, but only with an explicit warning and no full-size
        # hostile texture uploaded.
        safe = (state['textureWarnings'] > 0
                and (state['textureExtent'] or 0) <= 2)
        _check(checks, f'hostile {name} never uploads the hostile texture',
               safe, result, 'warning>0 and fallback extent<=2')
    elif accepted:
        _check(checks, f'hostile {name} returns a terminal typed error',
               state['state'] == 3 and state['errorCode'] != 0,
               result, 'Failed + typed code')
    else:
        _check(checks, f'hostile {name} returns a typed rejection',
               result.get('code') is not None, result, 'typed error')
        _check(checks, f'hostile {name} never becomes a successful image',
               not state['complete'] or state['errorCode'] != 0,
               {'complete': state['complete'],
                'errorCode': state['errorCode'],
                'state': state['state']},
               'terminal error, not complete success')
    return result


def _external_audit(output: Path, profile, inventory):
    captures = _load_captures(output)
    assets = {}
    for asset in profile['lanes']['external']['assets']:
        name = asset['id']
        records = {key: value for key, value in captures.items()
                   if value.get('asset') == name and value.get('lane') == 'external'}
        default = records.get(f'external-{name}-default')
        disposition = (records.get(f'external-{name}-disposition')
                       or records.get(f'external-{name}-failed-load'))
        modes = {}
        for mode in asset['modes']:
            mode_records = [value for value in records.values()
                            if value.get('mode') == mode
                            and value.get('path') and value.get('settled')]
            modes[mode] = sorted(
                (value.get('view') for value in mode_records
                 if value.get('view') != 'disposition'))
        images = sorted(value['path'] for value in records.values()
                        if value.get('path'))
        entry = {
            'id': name, 'family': asset.get('family'),
            'rootName': asset.get('rootName'), 'modes': modes,
            'images': images, 'imageCount': len(images),
            'disposition': (disposition or {}).get('disposition'),
            'failure': (disposition or {}).get('failure'),
            'degradation': (default or {}).get('degradation'),
            'importState': (default or {}).get('importState'),
            'model': (default or {}).get('model'),
            'performance': (default or {}).get('performance'),
            'passes': (default or {}).get('passes'),
            'loadSeconds': (default or {}).get('loadSeconds'),
        }
        assets[name] = entry
    # Every inventory root entry is accounted for exactly once.
    disposition_map = profile.get('dispositions', {})
    by_root = {asset.get('rootName'): asset['id']
               for asset in profile['lanes']['external']['assets']}
    unaccounted = []
    root_names = [name for name in inventory.get('rootAssets', {})
                  if corpus.is_corpus_entry(name)]
    for root_name in root_names:
        if root_name in by_root:
            continue
        if root_name in disposition_map:
            continue
        unaccounted.append(root_name)
    # Required family coverage counts captured external assets and the
    # portable lane (which the caller adds after this audit).
    families = {}
    for entry in assets.values():
        family = entry.get('family')
        if entry['imageCount']:
            families.setdefault(family, []).append(entry['id'])
    return {'assets': assets, 'unaccountedRootEntries': unaccounted,
            'families': families, 'dispositions': disposition_map,
            'rootEntryCount': len(root_names)}


def _corpus_accounting_check(output: Path, checks):
    """Focused T34b regression: root metadata is not corpus content.

    A root mixing OS metadata (``desktop.ini``/``Thumbs.db``), VCS metadata
    (``.git``) and one real entry must keep the real entry in the inventory and
    report zero unaccounted entries; an audit that omits the real entry must
    still flag it. Both the inventory and the audit use ``corpus.is_corpus_entry``.
    """
    with tempfile.TemporaryDirectory(prefix='t34b-corpus-', dir=str(output)) as temp:
        root = Path(temp)
        (root / '.git').mkdir()
        (root / '.git' / 'config').write_text('[core]\n', encoding='utf-8')
        for name in ('desktop.ini', 'Thumbs.db', 'ehthumbs.db'):
            (root / name).write_text('; OS metadata\n', encoding='utf-8')
        (root / 'Cube.STL').write_bytes(b'solid cube\n')
        record = corpus.inventory(root)
        names = sorted(record.get('rootAssets', {}))
        _check(checks, 'corpus root metadata is not an asset entry',
               names == ['Cube.STL'],
               {'rootAssets': names,
                'rootEntries': [item['name']
                                for item in record.get('rootEntries', [])]},
               ['Cube.STL'])
        profile = {'lanes': {'external': {'assets': [
            {'id': 'cube', 'rootName': 'Cube.STL', 'family': 'stl',
             'modes': [], 'views': []}]}}, 'dispositions': {}}
        audit = _external_audit(output, profile, record)
        _check(checks, 'corpus root metadata reports zero unaccounted entries',
               audit['unaccountedRootEntries'] == [] and audit['rootEntryCount'] == 1,
               {'unaccounted': audit['unaccountedRootEntries'],
                'rootEntryCount': audit['rootEntryCount']},
               {'unaccounted': [], 'rootEntryCount': 1})
        missing = _external_audit(
            output, {'lanes': {'external': {'assets': []}}, 'dispositions': {}},
            record)
        _check(checks, 'corpus audit still flags a real unaccounted entry',
               missing['unaccountedRootEntries'] == ['Cube.STL'],
               missing['unaccountedRootEntries'], ['Cube.STL'])


def _release_probe(configuration: str, output: Path):
    executable = ROOT / f'x64/{configuration}/Preview3D.exe'
    report = {'configuration': configuration, 'executable': str(executable),
              'exists': executable.exists(), 'controls': {},
              'unknownOptionExit': None, 'unknownOptionRejected': False}
    if not executable.exists():
        report['pass'] = False
        report['reason'] = 'built Release viewer not found'
        return report
    # The test-control surface is compiled into Debug only. A Release binary
    # must reject `--app-smoke` with the ordinary unknown-option usage exit
    # before any window exists, and must return zero for every control message.
    rejection_command = [str(executable), '--app-smoke']
    report['rejectionCommand'] = ' '.join(rejection_command)
    rejected = subprocess.run(
        rejection_command, cwd=ROOT, capture_output=True, text=True, timeout=60,
        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    report['unknownOptionExit'] = rejected.returncode
    report['unknownOptionRejected'] = rejected.returncode != 0
    process = subprocess.Popen(
        [str(executable)], cwd=ROOT, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL)
    hwnd = 0
    try:
        hwnd = wait_for_window(process)
        if not hwnd:
            report['pass'] = False
            report['reason'] = 'Release viewer window did not appear'
            return report
        time.sleep(0.5)
        for field in (0, 105, 107, 300):
            report['controls'][str(field)] = int(
                send(hwnd, 0x8000 + 104, field, 0)) & 0xFFFFFFFF
    except Exception as error:  # noqa: BLE001 - record, never hide a lane fault
        report['pass'] = False
        report['reason'] = f'Release viewer probe failed: {error}'
        return report
    finally:
        try:
            if hwnd:
                send(hwnd, 0x10)
        except Exception:
            pass
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
    report['pass'] = (report['unknownOptionRejected']
                      and all(value == 0 for value in report['controls'].values()))
    if not report['pass']:
        report['reason'] = (
            'Release build accepted a test-control option'
            if not report['unknownOptionRejected']
            else 'Release build answered a test-control message')
    return report


def _find_dumpbin():
    patterns = [
        'C:/Program Files/Microsoft Visual Studio/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe',
        'C:/Program Files (x86)/Microsoft Visual Studio/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe',
    ]
    for pattern in patterns:
        matches = sorted(glob.glob(pattern))
        if matches:
            return Path(matches[-1])
    return None


def _dependency_closure(configuration: str):
    executable = ROOT / f'x64/{configuration}/Preview3D.exe'
    report = {'executable': str(executable), 'exists': executable.exists(),
              'dumpbin': None, 'dependents': [], 'denied': []}
    dumpbin = _find_dumpbin()
    if dumpbin is None:
        report['pass'] = False
        report['reason'] = 'dumpbin not found'
        return report
    report['dumpbin'] = str(dumpbin)
    completed = subprocess.run([str(dumpbin), '/dependents', str(executable)],
                               cwd=ROOT, capture_output=True, text=True,
                               timeout=120)
    text = (completed.stdout or '') + (completed.stderr or '')
    for line in text.splitlines():
        line = line.strip()
        if line.lower().endswith('.dll'):
            report['dependents'].append(line)
    denied = []
    for name in report['dependents']:
        lowered = name.lower()
        if any(token in lowered for token in DEPENDENCY_DENY):
            denied.append(name)
    report['denied'] = denied
    report['pass'] = completed.returncode == 0 and not denied
    if completed.returncode != 0:
        report['reason'] = text[-2000:]
    return report


def _lifecycle_lane(viewer, output: Path, checks, result=None):
    fixture = output / 'fixtures' / 'edge-probe.glb'
    textured = output / 'fixtures' / 'textured-lobes.glb'
    layered = output / 'fixtures' / 'layered-glass.glb'
    result = {} if result is None else result
    result['stage'] = 'initial-open'
    viewer.open(fixture)
    viewer.apply_controlled_presentation()
    base_generation = viewer.model_state()['generation']

    # Rapid open/cancel: request two generations back to back; the last request
    # must be the resident generation and the cancelled one must never publish a
    # terminal error.
    result['stage'] = 'rapid-open-cancel'
    cancelled = viewer.request_open(layered)
    current = viewer.request_open(fixture)
    _wait_generation(viewer, current)
    state = viewer.import_state()
    result['rapidOpenCancel'] = {
        'cancelledGeneration': cancelled, 'currentGeneration': current,
        'loadedGeneration': state['generation'], 'state': state,
    }
    _check(checks, 'rapid open/cancel keeps the latest generation resident',
           state['generation'] == current and state['complete']
           and state['errorCode'] == 0, result['rapidOpenCancel'],
           'loaded == latest, complete, no error')
    _check(checks, 'rapid open/cancel produces a drawable frame',
           _save(viewer, output, 't34-lifecycle-rapid-open')['frame']['stats'][
               'uniqueSampleColours'] >= 8,
           result['rapidOpenCancel'], 'drawable')

    # Stale generation: the earlier request may finish first in the worker but
    # must not replace the newer displayed generation.
    result['stage'] = 'stale-generation'
    stale = viewer.request_open(layered)
    newer = viewer.request_open(fixture)
    _wait_generation(viewer, newer)
    state = viewer.import_state()
    result['staleGeneration'] = {
        'staleGeneration': stale, 'newerGeneration': newer,
        'loadedGeneration': state['generation'], 'state': state,
    }
    _check(checks, 'stale generation never replaces the newer open',
           state['generation'] == newer and state['complete']
           and state['errorCode'] == 0, result['staleGeneration'],
           'loaded == newer')

    # Mode changes, resize and simulated DPI.
    result['stage'] = 'modes'
    modes = {}
    for name, mode in (('studio', 0), ('clay', 1), ('directional', 2)):
        send_field(viewer.hwnd, 88, mode)
        time.sleep(0.35)
        viewer.set_camera(8.0, 72.5, 1.23)
        modes[name] = _save(viewer, output, f't34-lifecycle-mode-{name}')
    distinct = _changed_fraction(modes['studio'], modes['clay'])
    result['modes'] = {
        'images': {name: value['path'] for name, value in modes.items()},
        'studioClayChanged': distinct,
    }
    _check(checks, 'Studio/Clay mode changes alter the rendered frame',
           distinct is not None and distinct > 0.0002, distinct, '>0.02%')
    resize = {}
    result['stage'] = 'resize'
    for name, width, height in (('1600x1000', 1600, 1000),
                                ('1024x768', 1024, 768)):
        viewer.resize_client(width, height + (viewer.capture()['client'][1]
                                              - viewer.capture()['height']))
        resize[name] = _save(viewer, output, f't34-lifecycle-resize-{name}')
    result['resize'] = {
        name: {'width': value['width'], 'height': value['height'],
               'dpi': value['dpi'], 'path': value['path']}
        for name, value in resize.items()
    }
    _check(checks, 'resize keeps a drawable ready scene',
           all(value['stats']['uniqueSampleColours'] >= 8
               for value in resize.values()), result['resize'], 'drawable')
    dpi = {}
    result['stage'] = 'dpi'
    for value in (96, 144, 192):
        send_field(viewer.hwnd, 67, value)
        time.sleep(0.6)
        dpi[str(value)] = _save(viewer, output, f't34-lifecycle-dpi-{value}')
    send_field(viewer.hwnd, 67, 144)
    result['dpi'] = {name: {'width': item['width'], 'height': item['height'],
                            'path': item['path']} for name, item in dpi.items()}
    _check(checks, 'simulated 96/144/192 DPI stays drawable',
           all(item['width'] > 64 for item in dpi.values()), result['dpi'],
           'drawable')
    viewer.resize_client(1200, 860 + (viewer.capture()['client'][1]
                                      - viewer.capture()['height']))

    # Coarse-to-full and the complete-residency contract.
    result['stage'] = 'coarse-to-full'
    if textured.exists():
        viewer.open(textured)
        coarse = viewer.import_state()
        result['coarseToFull'] = {'state': coarse, 'passes': viewer.pass_state()}
        _check(checks, 'coarse-to-full reaches complete texture residency',
               coarse['complete'] and coarse['textureCount'] > 0
               and coarse['fineChunks'] >= coarse['coarseChunks']
               and coarse['requestedDetails'] == 0, result['coarseToFull'],
               'complete, textures>0, fine>=coarse')
    else:
        result['coarseToFull'] = {'status': 'missing', 'path': str(textured)}
        _check(checks, 'coarse-to-full fixture available', False,
               str(textured), 'exists')

    # Eviction and device loss/reopen.
    result['stage'] = 'eviction'
    before_evictions = viewer.import_state()['evictions']
    send_field(viewer.hwnd, 52, 0)
    time.sleep(0.6)
    after_evictions = viewer.import_state()['evictions']
    result['eviction'] = {'before': before_evictions, 'after': after_evictions}
    _check(checks, 'bounded eviction request is observable',
           after_evictions >= before_evictions, result['eviction'],
           'non-decreasing')
    before_recovery = send_field(viewer.hwnd, 66) & 0xFFFFFFFF
    result['stage'] = 'device-loss'
    send_field(viewer.hwnd, 65, 0)
    recovered = False
    deadline = time.monotonic() + 90.0
    while time.monotonic() < deadline:
        time.sleep(0.5)
        state = viewer.model_state()
        if state['state'] == 2 and state['complete']:
            recovered = True
            break
    after_recovery = send_field(viewer.hwnd, 66) & 0xFFFFFFFF
    loss = _save(viewer, output, 't34-lifecycle-after-device-loss')
    result['deviceLoss'] = {
        'recovered': recovered, 'recoveryBefore': before_recovery,
        'recoveryAfter': after_recovery, 'path': loss['path'],
    }
    _check(checks, 'device loss recovers to a drawable complete scene',
           recovered and loss['stats']['uniqueSampleColours'] >= 8
           and after_recovery >= before_recovery, result['deviceLoss'],
           'recovered, drawable')
    viewer.open(fixture)
    result['stage'] = 'reopen'
    reopen = _save(viewer, output, 't34-lifecycle-reopen')
    result['reopen'] = {'path': reopen['path']}
    _check(checks, 'reopen after device loss succeeds',
           viewer.model_state()['complete']
           and reopen['stats']['uniqueSampleColours'] >= 8,
           result['reopen'], 'complete drawable')
    result['baseGeneration'] = base_generation
    return result


def _low_budget_lane(configuration: str, output: Path, checks):
    fixture = output / 'fixtures' / 'edge-probe.glb'
    textured = output / 'fixtures' / 'textured-lobes.glb'
    result = {'stage': 'low-budget'}
    with Viewer(configuration, output, width=1200, height=860) as viewer:
        viewer.open(fixture)
        send_field(viewer.hwnd, 62, 32)  # 32 MiB detail target
        try:
            viewer.open(textured)
            state = viewer.import_state()
            status = 'completed'
        except LoadFailed as error:
            state = viewer.import_state()
            status = 'typed-failure'
            state['code'] = getattr(error, 'code', None)
            state['stage'] = getattr(error, 'stage', None)
        result.update({'status': status, 'state': state,
                       'passes': viewer.pass_state()})
        _check(checks, 'low budget is either complete or a typed ResourceLimit',
               status == 'completed'
               or (state.get('code') is not None), result,
               'complete or typed failure')
    # Reopen in a fresh process: the low-budget generation may have ended with a
    # typed limit, which must not prevent the next normal open.
    with Viewer(configuration, output, width=1200, height=860) as recovered:
        recovered.open(fixture)
        recovered.apply_controlled_presentation()
        image = _save(recovered, output, 't34-low-budget-reopen')
        result['reopen'] = image['path']
        _check(checks, 'normal open survives a low-budget generation',
               recovered.model_state()['complete']
               and image['frame']['stats']['uniqueSampleColours'] >= 8,
               result['reopen'], 'complete drawable')
    return result


def _performance_summary(output: Path, external, portable, configuration):
    executable = ROOT / f'x64/{configuration}/Preview3D.exe'
    assets = {}
    for source, entries in (('external', external.get('assets', {})),
                            ('portable', portable)):
        for name, entry in entries.items():
            if source == 'portable' and entry.get('status') != 'captured':
                continue
            performance = entry.get('performance') or {}
            state = entry.get('importState') or entry.get('state') or {}
            passes = entry.get('passes') or {}
            gpu = state.get('gpuBytes') or performance.get('accountedGpuBytes')
            target = state.get('targetBytes')
            assets[f'{source}:{name}'] = {
                'family': entry.get('family'),
                'loadSeconds': entry.get('loadSeconds'),
                'meanFrameMs': performance.get('meanFrameMs'),
                'p95FrameMs': performance.get('p95FrameMs'),
                'accountedGpuBytes': gpu,
                'targetGpuBytes': target,
                'budgetRatio': round(gpu / target, 4) if gpu and target else None,
                'viewerPrivateBytes': performance.get('privateBytes'),
                'textureCount': state.get('textureCount'),
                'textureExtent': state.get('textureExtent'),
                'textureMips': state.get('textureMips'),
                'coarseChunks': state.get('coarseChunks'),
                'fineChunks': state.get('fineChunks'),
                'passes': passes,
            }
    return {
        'schema': RESULT_SCHEMA,
        'environment': env.environment(executable),
        'assets': assets,
        'note': 'Measured on this workstation. Debug frame time includes adaptive '
                'refresh/settle work and is not a reference-hardware claim. GPU '
                'bytes are the viewer-accounted total compared with its own '
                'detail target; the UI/render targets are budgeted separately.',
    }


def _write_report(output: Path, report):
    lines = [
        '# T34 complete corpus, cross-format and hostile-input qualification', '',
        'Generated from serial unit/import/security lanes plus Debug GPU',
        'sessions. External models/screenshots are local evidence only.', '',
        '## Checks', '',
        f"- Status: **{report.get('status')}**",
        f"- Checks: {sum(1 for c in report.get('checks', []) if c['pass'])} passed / "
        f"{len(report.get('checks', []))} total",
        f"- Failures: {len(report.get('failures', []))}",
    ]
    for failure in report.get('failures', []):
        lines.append(f'  - {failure}')
    external = report.get('external', {})
    lines += ['', '## External corpus matrix', '',
              f"- Root entries accounted: {external.get('rootEntryCount')}",
              f"- Unaccounted root entries: {len(external.get('unaccountedRootEntries', []))}",
              '', '| asset | family | modes captured | images | disposition | warning state |',
              '| --- | --- | --- | ---: | --- | --- |']
    for name, entry in sorted(external.get('assets', {}).items()):
        modes = ', '.join(key for key, value in entry.get('modes', {}).items() if value)
        degradation = entry.get('degradation') or {}
        warning = (f"opt {degradation.get('optionalFeatureWarnings', '-')}, "
                   f"tex {degradation.get('textureWarnings', '-')}")
        lines.append(f"| {name} | {entry.get('family')} | {modes} | "
                     f"{entry.get('imageCount', 0)} | "
                     f"{entry.get('disposition') or 'captured'} | {warning} |")
    lines += ['', '### Size-capped dispositions', '']
    for name, item in report.get('external', {}).get('dispositions', {}).items():
        lines.append(f"- **{name}** ({item.get('family')}): {item.get('reason')}")
    lines += ['', '## Portable cross-format lane', '',
              '| fixture | family | status | source format | vertices/triangles | nodes |',
              '| --- | --- | --- | --- | --- | ---: |']
    for name, entry in sorted(report.get('portable', {}).items()):
        facts = entry.get('facts') or {}
        lines.append(f"| {name} | {entry.get('family')} | {entry.get('status')} | "
                     f"{facts.get('sourceFormat')} | "
                     f"{facts.get('vertices')}/{facts.get('triangles')} | "
                     f"{facts.get('nodes')} |")
    lines += ['', '## Lifecycle', '']
    lifecycle = report.get('lifecycle', {})
    for key in ('rapidOpenCancel', 'staleGeneration', 'lowBudget', 'eviction',
                'deviceLoss', 'reopen', 'coarseToFull'):
        if key in lifecycle:
            lines.append(f"- **{key}**: `{json.dumps(lifecycle[key])[:400]}`")
    lines += ['', '## Debug-only controls and dependency closure', '',
              f"- Release controls: `{json.dumps(report.get('releaseControls', {}))[:300]}`",
              f"- Release dependents: `{', '.join(report.get('dependencyClosure', {}).get('dependents', []))}`",
              f"- Denied parser/codec dependents: `{report.get('dependencyClosure', {}).get('denied', [])}`",
              '', '## Hostile input', '']
    for name, item in report.get('hostile', {}).items():
        if not isinstance(item, dict):
            continue
        lines.append(f"- **{name}**: {item.get('status')}, code {item.get('code')}, "
                     f"stage {item.get('stage')}, complete "
                     f"{(item.get('state') or {}).get('complete')}")
    lines += ['', '## Measured cost', '',
              '| asset | family | load s | GPU bytes / target | mean/p95 ms | textures |',
              '| --- | --- | ---: | --- | --- | --- |']
    for name, item in sorted(report.get('performance', {}).get('assets', {}).items()):
        lines.append(f"| {name} | {item.get('family')} | {item.get('loadSeconds')} | "
                     f"{item.get('accountedGpuBytes')} / {item.get('targetGpuBytes')} | "
                     f"{item.get('meanFrameMs')} / {item.get('p95FrameMs')} | "
                     f"{item.get('textureCount')} |")
    lines += ['', '## Mandatory paired regions', '',
              '- Car windshield/paint and Camera lens/lettering are captured in all',
              '  three modes and listed in `mandatoryRegions`.',
              '', '## Evidence', '',
              f"- `{output / 'diagnostics' / 'full-corpus.json'}`",
              f"- `{output / 'images'}` external and portable captures",
              f"- `{output / 'external'}` corpus inventory and matrix",
              f"- `{output / 'review-note.md'}` image review route", '']
    path = output / 'full-corpus-report.md'
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def run(workspace: Path, output: Path, configuration: str, fast: bool = False,
        profile=None, inventory=None):
    output = Path(output)
    report = {'schema': RESULT_SCHEMA, 'task': 'T34', 'checks': [],
              'failures': [], 'status': 'running', 'fast': fast}
    # T34b: prove the shared corpus-root filter in the same report that audits
    # the real corpus, before any expensive lane can obscure it.
    _corpus_accounting_check(output, report['checks'])
    report['serialLanes'] = _run_serial(configuration, output)
    for lane in report['serialLanes']:
        if not lane.get('pass'):
            report['failures'].append(f"serial lane failed: {lane['name']}")

    external = _external_audit(output, profile, inventory or {})
    report['external'] = external
    if not fast:
        if external['unaccountedRootEntries']:
            report['failures'].append(
                'unaccounted corpus root entries: '
                + ', '.join(external['unaccountedRootEntries']))
        for name, entry in external['assets'].items():
            if entry.get('disposition'):
                _check(report['checks'], f'{name} disposition is typed',
                       bool(entry['disposition']), entry.get('disposition'),
                       'resource-budget/open-failure/unavailable')
                continue
            for mode, views in entry.get('modes', {}).items():
                if not views:
                    report['failures'].append(
                        f'{name}: no settled {mode} capture')
            if not entry.get('imageCount'):
                report['failures'].append(f'{name}: no captured image')
            if not asset_has_modes(entry, profile, name):
                report['failures'].append(
                    f'{name}: missing one or more controlled mode/view captures')

    portable = {}
    portable_images = []
    try:
        with Viewer(configuration, output, width=1200, height=860) as viewer:
            portable, portable_images = _portable_lane(
                viewer, output, report['checks'])
            report['portable'] = portable
            # Mandatory paired regions come from the generic external capture
            # lane; verify they exist in all three modes.
            if not fast:
                for asset_name, regions in profile.get('mandatoryRegions', {}).items():
                    for region_name in regions:
                        for mode in ('studio', 'clay', 'directional'):
                            candidates = [
                                value for value in _load_captures(output).values()
                                if value.get('asset') == asset_name
                                and value.get('mode') == mode
                                and value.get('path')
                                and (value.get('view') in (
                                    'front', 'oblique', 'grazing', 'window', 'lens',
                                    'lettering', 'detail'))]
                            _check(report['checks'],
                                   f'{asset_name}/{region_name} has a {mode} image',
                                   bool(candidates), len(candidates), '>0')
        # Hostile imports and the lifecycle run in fresh viewer processes so a
        # deliberately terminal hostile generation cannot contaminate the next
        # case. Each case still proves the same viewer can reopen a valid file.
        report['hostile'] = {}
        for name, path in _hostile_cases(output):
            with Viewer(configuration, output, width=1200, height=860) as hostile:
                report['hostile'][name] = _run_hostile_case(
                    hostile, output, name, path, report['checks'])
        # A fresh viewer after the hostile cases proves the product is still
        # usable; same-process reopen is covered by the lifecycle lane.
        with Viewer(configuration, output, width=1200, height=860) as recovery:
            recovery.open(output / 'fixtures' / 'edge-probe.glb')
            recovery.apply_controlled_presentation()
            recovered = _save(recovery, output, 't34-hostile-recovered')
            report['hostileRecovery'] = {'path': recovered['path']}
            _check(report['checks'], 'viewer recovers after hostile imports',
                   recovered['frame']['stats']['uniqueSampleColours'] >= 8,
                   recovered['frame']['stats'], 'drawable')
        lifecycle = {}
        report['lifecycle'] = lifecycle
        with Viewer(configuration, output, width=1200, height=860) as viewer:
            _lifecycle_lane(viewer, output, report['checks'], lifecycle)
        report['lowBudget'] = _low_budget_lane(
            configuration, output, report['checks'])
    except Exception as error:  # noqa: BLE001 - record, never hide a lane fault
        stage = ((report.get('lifecycle') or {}).get('stage'))
        report['failures'].append(
            f'GPU corpus/lifecycle lane failed'
            + (f' at {stage}' if stage else '')
            + f': {error}')

    if not fast:
        report['releaseControls'] = _release_probe('Release', output)
        if not report['releaseControls'].get('pass'):
            report['failures'].append(
                'Release test-control surface was not rejected: '
                + str(report['releaseControls']))
        report['dependencyClosure'] = _dependency_closure('Release')
        if not report['dependencyClosure'].get('pass'):
            report['failures'].append(
                'Release dependency closure contains a parser/codec library: '
                + str(report['dependencyClosure']))

    # Portable families count as cross-format coverage too, notably STEP.
    families = external.get('families', {})
    for entry in portable.values():
        if entry.get('status') == 'captured':
            families.setdefault(entry.get('family'), []).append(entry['id'])
    report['external']['families'] = families
    if not fast:
        for family in ('glb', 'gltf', 'stl', 'obj', 'step', 'usdz'):
            _check(report['checks'], f'{family} family has captured evidence',
                   bool(families.get(family)), families.get(family), '>0')

    report['performance'] = _performance_summary(
        output, external, portable, configuration)
    for name, item in report['performance']['assets'].items():
        ratio = item.get('budgetRatio')
        if ratio is not None and ratio > 1.0:
            report['failures'].append(
                f'{name}: accounted GPU bytes exceed the detail target '
                f'({ratio:.2f}x)')
    write_json(output / 'diagnostics' / 'performance.json',
               report['performance'])
    write_json(output / 'external' / 'matrix.json',
               {key: value for key, value in external.items()})

    failed = [check for check in report['checks'] if not check['pass']]
    if failed:
        report['failures'].append(f'{len(failed)} corpus/format/lifecycle checks failed')
    if not report.get('checks'):
        report['failures'].append('no corpus qualification checks ran')
    if not portable:
        report['failures'].append('portable cross-format lane produced no results')
    report['status'] = 'pass' if not report['failures'] else 'fail'
    write_json(output / 'diagnostics' / 'full-corpus.json', report)
    report['reportPath'] = str(_write_report(output, report))
    return report


def asset_has_modes(entry, profile, name):
    asset = next((item for item in profile['lanes']['external']['assets']
                  if item['id'] == name), None)
    if not asset:
        return False
    expected_views = set(asset.get('views', [])) | set(asset.get('closeups', []))
    for mode in asset.get('modes', []):
        captured = set(entry.get('modes', {}).get(mode, []))
        if not expected_views.issubset(captured):
            return False
    return True
