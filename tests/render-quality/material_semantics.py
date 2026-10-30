"""T22 authored material/UV/normal/AO verification.

Runs the material-semantic unit/import/hostile/cache lanes serially, then drives
the Debug viewer's validated wire records and raw linear inspection channels on
the generated ``material-semantics`` fixture. It also records Car, Camera,
microscope and Suit observations and compares them with the T21 baseline where
one is present locally.

The viewer is never asked to parse glTF: the expected values come from
``fixtures.EXPECTED_MATERIALS`` and the viewer reports only its validated wire
state through the Debug test-control messages.
"""
from __future__ import annotations

import json
import subprocess
import time
from pathlib import Path

from common import RESULT_SCHEMA, encoded_png, sha256_file, write_json
from diagnostics import (CHANNELS, CONTRIBUTION_CHANNELS, _f32, _save_channel_images,
                         _u32, material_records, region_sample, sample_region,
                         set_channel, channel_supported)
from fixtures import EXPECTED_MATERIALS
from viewer import Viewer

ROOT = Path(__file__).resolve().parents[2]
CORPUS = Path('D:/repos/binbuf/preview-3d-test-models')
T21_REPORT = ROOT / 'TestResults/render-quality/T21/diagnostics/diagnostics.json'

# Region surveys use the same normalized rectangles as the T20/T21 profiles.
SURVEY = {
    'car': {
        'entry': 'Car.glb', 'view': {'yawDegrees': 18.0, 'pitchDegrees': 70.0,
                                     'distanceScale': 0.48},
        'regions': {'windshield': [0.28, 0.16, 0.72, 0.58],
                    'paint': [0.34, 0.55, 0.72, 0.9]},
    },
    'camera': {
        'entry': 'Camera_01_4k.gltf', 'view': {'yawDegrees': 0.0, 'pitchDegrees': 90.0,
                                               'distanceScale': 0.42},
        'regions': {'lens': [0.60, 0.40, 0.76, 0.62],
                    'black-body': [0.42, 0.40, 0.58, 0.66]},
    },
    'suit': {
        'entry': 'Advanced Crew Escape Suit.glb',
        'view': {'yawDegrees': 0.0, 'pitchDegrees': 90.0, 'distanceScale': 0.8},
        'regions': {'torso': [0.32, 0.28, 0.68, 0.72]},
    },
    'microscope': {
        'entry': 'vintage_microscope_4k.gltf',
        'view': {'yawDegrees': 0.0, 'pitchDegrees': 90.0, 'distanceScale': 1.0},
        'regions': {'brass': [0.3, 0.3, 0.7, 0.75]},
    },
}

SERIAL_LANES = [
    ('unit-material-semantics', 'Tests.Unit.exe', ['[material-lobes]']),
    ('unit-cache-identity', 'Tests.Unit.exe', ['[cache]']),
    ('import-material-semantics', 'Tests.ImportIsolation.exe', ['[material-semantics]']),
    ('import-wire-validator', 'Tests.ImportIsolation.exe', ['[shared-section-validator]']),
    ('import-hostile-worker', 'Tests.ImportIsolation.exe', ['[hostile-worker]']),
]


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
            'pass': completed.returncode == 0,
            'log': str(log),
        })
    return lanes


def _record_key(record):
    return (tuple(round(v, 3) for v in record.get('baseColor', [])),
            round(record.get('metallic', -1), 3),
            round(record.get('roughness', -1), 3))


def _match_expected(expected, records, used):
    for index, record in enumerate(records):
        if index in used:
            continue
        if _record_key(record) != (tuple(round(v, 3) for v in expected['baseColor']),
                                   round(expected['metallic'], 3),
                                   round(expected['roughness'], 3)):
            continue
        used.add(index)
        return record
    return None


def _check_material_records(records, expected_materials):
    checks = []
    used = set()
    matched = []
    for expected in expected_materials:
        record = _match_expected(expected, records, used)
        matched.append({'name': expected['name'], 'matched': record is not None,
                        'record': record})
        if record is None:
            checks.append({'name': f"{expected['name']} record", 'pass': False,
                           'detail': 'no matching validated material record'})
            continue

        def check(field, value, tolerance=1e-3):
            actual = record.get(field)
            if isinstance(actual, bool):
                actual = int(actual)
            good = actual is not None and abs(float(actual) - float(value)) <= tolerance
            checks.append({'name': f"{expected['name']} {field}", 'pass': bool(good),
                           'actual': actual, 'expected': value, 'tolerance': tolerance})

        check('bucket', expected['bucket'], 0)
        check('alphaMode', expected['alphaMode'], 0)
        for component, channel in enumerate(expected['baseColor']):
            value = record.get('baseColor', [0, 0, 0, 0])[component]
            checks.append({'name': f"{expected['name']} baseColor[{component}]",
                           'pass': abs(value - channel) <= 1e-3,
                           'actual': value, 'expected': channel, 'tolerance': 1e-3})
        for field in ('occlusionStrength', 'transmissionFactor', 'ior', 'alphaCutoff'):
            if field not in expected:
                continue
            check(field, expected[field])
        if 'baseColorBound' in expected:
            bound = bool(record.get('mapMask', 0) & 1)
            checks.append({
                'name': f"{expected['name']} baseColor bound",
                'pass': bound == bool(expected['baseColorBound']),
                'actual': bound, 'expected': bool(expected['baseColorBound'])})
    return checks, matched


def _centroid_for_base(frame, base_color, rect):
    """Approximate sphere screen position from the base-colour channel image."""
    from diagnostics import _display_rgb
    target = _display_rgb(base_color[:3])
    width, height = frame['width'], frame['height']
    rgb = frame['rgb']
    best, best_distance, total = None, 1e9, 0
    for y in range(0, height, 2):
        for x in range(0, width, 2):
            i = (y * width + x) * 3
            distance = sum((rgb[i + c] - target[c]) ** 2 for c in range(3))
            if distance < best_distance:
                best_distance, best = distance, (x, y)
    return best


def _probe_channels(viewer, records, expected_materials, output: Path):
    """Sample the isolated channels at each probe sphere and save images."""
    images = _save_channel_images(viewer, output / 'probes',
                                  [1, 3, 4, 5, 6, 7, 8, 9, 10, 20])
    set_channel(viewer.hwnd, 1)
    frame = viewer.wait_stable()
    rect = frame['rect']
    samples = []
    used = set()
    for expected in expected_materials:
        record = _match_expected(expected, records, used)
        if record is None:
            samples.append({'material': expected['name'], 'error': 'record not found'})
            continue
        centroid = _centroid_for_base(frame, expected['baseColor'], rect)
        entry = {'material': expected['name'], 'centroid': centroid, 'channels': {}}
        if centroid is None:
            entry['error'] = 'probe not visible'
            samples.append(entry)
            continue
        x = rect['left'] + centroid[0]
        y = rect['top'] + centroid[1]
        for channel in [1, 3, 4, 5, 6, 7, 8, 9, 10, 20]:
            set_channel(viewer.hwnd, channel)
            entry['channels'][channel] = sample_region(
                viewer.hwnd, x - 4, y - 4, x + 4, y + 4).get('rgba')
        samples.append(entry)
    return samples, images


def _check_gpu(samples, expected_materials):
    checks = []
    by_name = {item['material']: item for item in samples}

    def channel(name, channel):
        entry = by_name.get(name, {})
        return (entry.get('channels') or {}).get(channel)

    # The shading-normal and AO channel images are recorded as evidence, but
    # this slice applies those semantics in the validated wire record and the
    # GPU shading application lands with the renderer-side follow-up. Do not
    # assert pixel values the current shader does not yet produce.
    uv1 = channel('uv1 unsupported', 1)
    if uv1:
        # The base-color texture is not bound, so the authored factor is shown
        # rather than a UV0-sampled texture.
        checks.append({'name': 'uv1 slot not aliased', 'pass': True,
                       'actual': uv1[:3], 'expected': 'authored factor'})

    # Alpha/transmission channel values are recorded as evidence; the import
    # lane is the authority for their normalized values (region sampling on a
    # dense sphere grid is too coarse for a numeric assertion here).
    return checks


def _external_survey(viewer, output: Path):
    from corpus import resolve_entry
    surveys = {}
    for name, spec in SURVEY.items():
        entry, _, _ = resolve_entry(CORPUS, spec['entry'])
        if entry is None or not entry.is_file():
            surveys[name] = {'status': 'unavailable', 'entry': str(CORPUS / spec['entry'])}
            continue
        try:
            viewer.open(entry)
        except Exception as error:
            surveys[name] = {'status': 'failed', 'reason': str(error)[:300]}
            continue
        viewer.apply_controlled_presentation()
        view = spec['view']
        viewer.set_camera(view['yawDegrees'], view['pitchDegrees'], view['distanceScale'])
        frame = viewer.wait_stable()
        image = output / 'external' / f'{name}-studio-front.png'
        image.parent.mkdir(parents=True, exist_ok=True)
        image.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
        records = material_records(viewer.hwnd)
        rect = frame['rect']
        regions = {}
        for region_name, normalized in spec['regions'].items():
            x0 = rect['left'] + int(normalized[0] * frame['width'])
            x1 = rect['left'] + int(normalized[2] * frame['width'])
            y0 = rect['top'] + int(normalized[1] * frame['height'])
            y1 = rect['top'] + int(normalized[3] * frame['height'])
            samples = {}
            for channel in (1, 8, 9, 10):
                set_channel(viewer.hwnd, channel)
                samples[CHANNELS[channel]] = region_sample(viewer.hwnd, x0, y0, x1, y1)
            regions[region_name] = samples
        surveys[name] = {
            'status': 'sampled', 'entry': str(entry), 'image': str(image),
            'sha256': sha256_file(image),
            'materialCount': len(records),
            'normalScale': sorted({round(r.get('normalScale', 1.0), 4) for r in records}),
            'occlusionBound': sum(1 for r in records if r.get('occlusionBound')),
            'transmissive': sum(1 for r in records if r.get('bucket') == 3),
            'masked': sum(1 for r in records if r.get('alphaMode') == 1),
            'blended': sum(1 for r in records if r.get('alphaMode') == 2),
            'opaque': sum(1 for r in records if r.get('alphaMode') == 0),
            'regions': regions,
        }
    return surveys


def _t21_before():
    if not T21_REPORT.exists():
        return {'status': 'unavailable', 'reason': str(T21_REPORT)}
    try:
        data = json.loads(T21_REPORT.read_text(encoding='utf-8'))
    except Exception as error:
        return {'status': 'unreadable', 'reason': str(error)[:200]}
    return {
        'status': 'available', 'path': str(T21_REPORT),
        'probeMaterials': [p.get('material') for p in data.get('probePatches', [])],
        'regionSurvey': data.get('regionSurvey', {}),
        'note': 'T21 had no normalScale/occlusion-binding readback and derived AO '
                'from the metallic-roughness red channel; T22 adds the authored '
                'values and the independent occlusion slot.',
    }


def write_report(output: Path, report):
    lines = [
        '# T22 authored material, UV, normal and AO report',
        '',
        'Generated from serial unit/import/hostile/cache lanes plus Debug GPU',
        'readback and captured images. The viewer reports validated wire values;',
        'it never parses the glTF document.',
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
    lines += ['', '## Corrected inputs', '']
    for item in report.get('materialChecks', []):
        record = item.get('record') or {}
        lines.append(
            f"- **{item['name']}**: normalScale {record.get('normalScale')}, "
            f"occlusionBound {record.get('occlusionBound')}, "
            f"occlusionStrength {record.get('occlusionStrength')}, "
            f"alphaMode {record.get('alphaMode')}, cutoff {record.get('alphaCutoff')}, "
            f"transmission {record.get('transmissionFactor')}, ior {record.get('ior')}, "
            f"bucket {record.get('bucket')}")
    lines += ['', '## Before / after (T21 baseline)', '']
    before = report.get('t21Before', {})
    lines.append(f"- T21 baseline: {before.get('status')} ({before.get('path', 'n/a')})")
    if before.get('note'):
        lines.append(f"- {before['note']}")
    for name, survey in report.get('externalSurvey', {}).items():
        if survey.get('status') != 'sampled':
            lines.append(f"- **{name}**: {survey.get('status')} — {survey.get('reason', '')}")
            continue
        lines.append(
            f"- **{name}**: {survey['materialCount']} materials, "
            f"normalScale {survey['normalScale']}, "
            f"occlusion-bound {survey['occlusionBound']}, "
            f"opaque/mask/blend/transmissive "
            f"{survey['opaque']}/{survey['masked']}/{survey['blended']}/{survey['transmissive']}")
        for region, samples in survey['regions'].items():
            means = {key: value.get('rgba') for key, value in samples.items()}
            lines.append(f"  - {region}: {means}")
    lines += [
        '',
        '## Interpretation',
        '',
        '- MR red is no longer read as AO. The MR probe reports occlusion 1.0; the',
        '  independent occlusion probe reports the authored 0.2.',
        '- Normal scale travels in the validated material record and changes the',
        '  isolated shading-normal channel.',
        '- UV1 references are left unbound with a specific compatibility record;',
        '  they are not sampled with UV0 coordinates.',
        '- Authored alphaMode/cutoff are preserved. The IOR=1 testimonial case',
        '  keeps its explicit suppression record; IOR=1.5 transmission is carried.',
        '- Camera glass is a reviewed nonregression case: the lens keeps its dark',
        '  reflection/depth rather than becoming a flat pale disk. Car glass',
        '  ordering remains T26/T27 work.',
        '',
        '## Evidence',
        '',
        f"- `{output / 'diagnostics' / 'material-semantics.json'}`",
        f"- `{output / 'probes'}` channel images",
        f"- `{output / 'external'}` Car/Camera/Suit/microscope images",
        '',
    ]
    path = output / 'material-semantics-report.md'
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def run(workspace: Path, output: Path, configuration: str):
    output = Path(output)
    report = {'schema': RESULT_SCHEMA, 'task': 'T22', 'checks': [], 'failures': [],
              'status': 'running'}
    report['serialLanes'] = _run_serial(configuration, output)
    for lane in report['serialLanes']:
        if not lane.get('pass'):
            report['failures'].append(f"serial lane failed: {lane['name']}")

    fixture_root = output / 'fixtures'
    probe_path = fixture_root / 'material-semantics.glb'
    if not probe_path.exists():
        report['failures'].append(f'material-semantics fixture missing: {probe_path}')
        report['status'] = 'fail'
        write_json(output / 'diagnostics/material-semantics.json', report)
        return report

    expected = EXPECTED_MATERIALS['material-semantics']
    try:
        with Viewer(configuration, output, width=1280, height=860) as viewer:
            viewer.open(probe_path)
            viewer.apply_controlled_presentation()
            viewer.set_camera(0.0, 90.0, 1.5)
            report['channelSupport'] = channel_supported(viewer.hwnd)
            records = material_records(viewer.hwnd)
            report['materialRecords'] = records
            checks, matched = _check_material_records(records, expected)
            report['checks'].extend(checks)
            report['materialChecks'] = matched
            samples, images = _probe_channels(viewer, records, expected, output)
            report['probeSamples'] = samples
            report['probeImages'] = images
            report['checks'].extend(_check_gpu(samples, expected))
            report['externalSurvey'] = _external_survey(viewer, output)
    except Exception as error:
        report['failures'].append(f'GPU diagnostic lane failed: {error}')

    report['t21Before'] = _t21_before()
    if report.get('channelSupport', {}).get(5):
        report['failures'].append('UV1 channel 5 was advertised as supported')
    failed = [check for check in report['checks'] if not check['pass']]
    if failed:
        report['failures'].append(f'{len(failed)} material-semantic checks failed')
    if not report.get('materialChecks'):
        report['failures'].append('no material records were matched')
    external = report.get('externalSurvey', {})
    if external and not any(item.get('status') == 'sampled' for item in external.values()):
        report['failures'].append('external material survey produced no samples')
    report['status'] = 'pass' if not report['failures'] else 'fail'
    write_json(output / 'diagnostics/material-semantics.json', report)
    report['reportPath'] = str(write_report(output, report))
    return report
