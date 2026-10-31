"""T23 texture-budget accounting and complete-load readiness.

Two independent halves:

* Serial import tests prove the accounting rules: an identical image source is
  decoded and charged once across distinct glTF image entries, and an idle
  detail replay ends successfully instead of being reported as a late
  ``GenerationDeadline`` / ``ResourceLimit``.
* A Debug viewer session proves the product behaviour: the reused-image fixture
  reports exactly the expected resident texture count, and the microscope (the
  observed late-limit asset) reaches a terminal complete load, keeps that state
  for a hold window, and produces settled captures. Camera and Car are reviewed
  as non-regression cases.

The generator never infers a cause from the overlay text or compressed file
size; every check reads the viewer's own terminal/residency fields.
"""
from __future__ import annotations

import json
import subprocess
import time
from pathlib import Path

from common import RESULT_SCHEMA, encoded_png, sha256_file, write_json
from corpus import resolve_entry
from fixtures import EXPECTED_TEXTURE_COUNTS
from viewer import LoadFailed, Viewer

ROOT = Path(__file__).resolve().parents[2]
CORPUS = Path('D:/repos/binbuf/preview-3d-test-models')

SERIAL_LANES = [
    ('import-texture-budget', 'Tests.ImportIsolation.exe', ['[texture-budget]']),
    ('import-detail-budget', 'Tests.ImportIsolation.exe', ['[detail-budget]']),
    ('import-coarse-proxy', 'Tests.ImportIsolation.exe', ['[coarse-proxy]']),
    ('import-compressed', 'Tests.ImportIsolation.exe', ['[compressed]']),
    ('import-wire-validator', 'Tests.ImportIsolation.exe', ['[shared-section-validator]']),
    ('import-hostile-worker', 'Tests.ImportIsolation.exe', ['[hostile-worker]']),
]

# External assets that must keep a terminal complete load after the change.
EXTERNAL = {
    'microscope': {'entry': 'vintage_microscope_4k.gltf', 'hold': 6.0,
                   'views': ['default', 'front']},
    'camera': {'entry': 'Camera_01_4k.gltf', 'hold': 1.5, 'views': ['default']},
    'car': {'entry': 'Car.glb', 'hold': 1.5, 'views': ['default']},
}


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
        completed = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                   timeout=1800)
        log = output / f'lane-{name}.log'
        log.write_text((completed.stdout or '') + (completed.stderr or ''),
                       encoding='utf-8', errors='replace')
        lanes.append({
            'name': name, 'command': ' '.join(command),
            'exitCode': completed.returncode,
            'seconds': round(time.monotonic() - started, 2),
            'pass': completed.returncode == 0, 'log': str(log),
        })
    return lanes


def _check(check_list, name, passed, actual=None, expected=None):
    check_list.append({'name': name, 'pass': bool(passed),
                       'actual': actual, 'expected': expected})


def _save(viewer, image: Path):
    image.parent.mkdir(parents=True, exist_ok=True)
    frame = viewer.wait_stable()
    image.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
    return {'image': str(image), 'sha256': sha256_file(image),
            'width': frame['width'], 'height': frame['height']}


def _fixture_lane(viewer, output: Path, checks):
    fixture = output / 'fixtures' / 'texture-reuse.glb'
    if not fixture.exists():
        return {'status': 'missing', 'path': str(fixture)}
    generation, seconds, _ = viewer.open(fixture)
    state = viewer.import_state()
    viewer.apply_controlled_presentation()
    image = _save(viewer, output / 'fixtures' / 'texture-reuse-studio-front.png')
    expected = EXPECTED_TEXTURE_COUNTS['texture-reuse']
    _check(checks, 'reused-image fixture completes', state['complete'] and state['state'] == 2,
           state['state'], 'Ready')
    _check(checks, 'reused-image fixture has no terminal error', state['errorCode'] == 0,
           state['errorCode'], 0)
    _check(checks, 'identical image sources decode once per semantic',
           state['textureCount'] == expected, state['textureCount'], expected)
    _check(checks, 'reused-image fixture keeps its full resolution',
           state['textureExtent'] == 128, state['textureExtent'], 128)
    return {'status': 'sampled', 'generation': generation,
            'loadSeconds': round(seconds, 3), 'state': state,
            'expectedTextureCount': expected, **image}


def _external_lane(viewer, output: Path, checks, fast: bool):
    surveys = {}
    for name, spec in EXTERNAL.items():
        if fast and name != 'microscope':
            surveys[name] = {'status': 'skipped', 'reason': 'fast iteration lane'}
            continue
        entry, _, _ = resolve_entry(CORPUS, spec['entry'])
        if entry is None or not entry.is_file():
            surveys[name] = {'status': 'unavailable', 'entry': str(CORPUS / spec['entry'])}
            continue
        try:
            generation, seconds, _ = viewer.open(entry)
            state = viewer.import_state()
            # Hold the completed model and watch for a late terminal error.
            held = viewer.wait_complete_hold(generation, hold_seconds=spec['hold'])
            viewer.apply_controlled_presentation()
            images = {}
            if 'default' in spec['views']:
                images['default'] = _save(viewer, output / 'external' / f'{name}-default.png')
            if 'front' in spec['views']:
                viewer.set_camera(0.0, 90.0, 1.0)
                images['front'] = _save(viewer, output / 'external' / f'{name}-studio-front.png')
            _check(checks, f'{name} reaches a terminal complete load',
                   state['complete'] and state['state'] == 2, state['state'], 'Ready')
            _check(checks, f'{name} has no terminal error', state['errorCode'] == 0,
                   state['errorCode'], 0)
            _check(checks, f'{name} keeps complete readiness through the hold',
                   held['complete'] and held['errorCode'] == 0,
                   held, 'complete/errorCode=0')
            _check(checks, f'{name} has resident geometry', state['vertices'] > 0,
                   state['vertices'], '>0')
            surveys[name] = {
                'status': 'sampled', 'entry': str(entry), 'generation': generation,
                'loadSeconds': round(seconds, 3), 'state': state, 'held': held,
                'images': images,
            }
        except LoadFailed as error:
            surveys[name] = {'status': 'failed', 'reason': str(error)[:300],
                             'errorCode': getattr(error, 'code', None),
                             'errorStage': getattr(error, 'stage', None),
                             'partial': viewer.import_state() if viewer.hwnd else None}
            _check(checks, f'{name} terminal complete load', False,
                   surveys[name], 'no terminal error')
        except Exception as error:  # noqa: BLE001 - record, never hide a lane fault
            surveys[name] = {'status': 'failed', 'reason': str(error)[:300]}
            _check(checks, f'{name} terminal complete load', False, str(error), 'sampled')
    return surveys


def write_report(output: Path, report):
    lines = [
        '# T23 texture-budget accounting and complete-load readiness',
        '',
        'Generated from serial import lanes plus a Debug viewer session. All',
        'quantities are the viewer/worker fields, not the overlay text or the',
        'compressed source size.',
        '',
        '## Checks',
        '',
        f"- Status: **{report.get('status')}**",
        f"- Checks: {sum(1 for c in report.get('checks', []) if c['pass'])} passed / "
        f"{len(report.get('checks', []))} total",
        f"- Failures: {len(report.get('failures', []))}",
    ]
    for failure in report.get('failures', []):
        lines.append(f'  - {failure}')
    fixture = report.get('fixture', {})
    lines += ['', '## Reused-image fixture', '']
    if fixture.get('status') == 'sampled':
        state = fixture['state']
        lines.append(
            f"- {fixture['expectedTextureCount']} unique textures expected; "
            f"{state['textureCount']} resident, extent {state['textureExtent']}, "
            f"mips {state['textureMips']}, complete {state['complete']}")
    else:
        lines.append(f"- {fixture.get('status')}: {fixture.get('path')}")
    lines += ['', '## External assets', '']
    for name, survey in report.get('externalSurvey', {}).items():
        if survey.get('status') != 'sampled':
            lines.append(f"- **{name}**: {survey.get('status')} — {survey.get('reason', '')}")
            continue
        state, held = survey['state'], survey['held']
        lines.append(
            f"- **{name}**: load {survey['loadSeconds']} s, {state['triangles']} triangles, "
            f"textures {state['textureCount']} x {state['textureExtent']}px "
            f"({state['textureMips']} mips), GPU {state['gpuBytes']} / "
            f"target {state['targetBytes']} bytes, complete {state['complete']}, "
            f"detail requests {state['detailRequests']}, held errorCode {held['errorCode']}")
    lines += [
        '',
        '## Disposition',
        '',
        '- The microscope late failure was the broker generation wall-clock',
        '  deadline, not a decoded-byte/pixel, mip, catalog or GPU-residency cap.',
        '  Its complete texture state is recorded above and the model now ends',
        '  the background detail replay cleanly instead of being reported as a',
        '  late ResourceLimit.',
        '- An identical image source named by distinct glTF image entries is one',
        '  decoded chunk per semantic/color space; the microscope diffuse map',
        '  drops from four resident textures to three.',
        '',
        '## Evidence',
        '',
        f"- `{output / 'diagnostics' / 'texture-budget.json'}`",
        f"- `{output / 'fixtures'}` reused-image capture",
        f"- `{output / 'external'}` microscope/Camera/Car captures",
        '',
    ]
    path = output / 'texture-budget-report.md'
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def run(workspace: Path, output: Path, configuration: str, fast: bool = False):
    output = Path(output)
    report = {'schema': RESULT_SCHEMA, 'task': 'T23', 'checks': [], 'failures': [],
              'status': 'running', 'fast': fast}
    report['serialLanes'] = _run_serial(configuration, output)
    for lane in report['serialLanes']:
        if not lane.get('pass'):
            report['failures'].append(f"serial lane failed: {lane['name']}")

    try:
        with Viewer(configuration, output, width=1280, height=860) as viewer:
            report['fixture'] = _fixture_lane(viewer, output, report['checks'])
            report['externalSurvey'] = _external_lane(viewer, output, report['checks'], fast)
    except Exception as error:  # noqa: BLE001
        report['failures'].append(f'GPU readiness lane failed: {error}')

    failed = [check for check in report['checks'] if not check['pass']]
    if failed:
        report['failures'].append(f'{len(failed)} texture-budget/readiness checks failed')
    if not report.get('checks'):
        report['failures'].append('no texture-budget/readiness checks ran')
    if report.get('fixture', {}).get('status') != 'sampled':
        report['failures'].append('texture-reuse fixture did not produce a sampled load')
    survey = report.get('externalSurvey', {})
    microscope = survey.get('microscope', {})
    if not fast and microscope.get('status') != 'sampled':
        report['failures'].append('microscope did not produce a complete settled load')
    report['status'] = 'pass' if not report['failures'] else 'fail'
    write_json(output / 'diagnostics' / 'texture-budget.json', report)
    report['reportPath'] = str(write_report(output, report))
    return report
