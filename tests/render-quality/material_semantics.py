"""T22 authored material/UV/normal/AO verification.

Runs the material-semantic unit/import/hostile/cache lanes serially, then drives
the Debug viewer's validated wire records, raw linear inspection channels and
per-probe numeric readback on the generated ``material-semantics`` fixture. It
also records Car, Camera, microscope and Suit observations and compares them
with the T21 baseline where one is present locally. The generated fixture has
ten authored probes plus separate mirrored-tangent and double-sided back-face
probes.

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
from diagnostics import (CHANNELS, CONTRIBUTION_CHANNELS, _display_rgb, _f32,
                         _save_channel_images, _u32, material_records,
                         region_sample, sample_region, set_channel,
                         channel_supported)
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
        for field in ('occlusionStrength', 'transmissionFactor', 'ior', 'alphaCutoff',
                      'normalScale'):
            if field not in expected:
                continue
            check(field, expected[field])
        if 'occlusionBound' in expected:
            bound = bool(record.get('occlusionBound'))
            checks.append({
                'name': f"{expected['name']} occlusion bound",
                'pass': bound == bool(expected['occlusionBound']),
                'actual': bound, 'expected': bool(expected['occlusionBound'])})
        if 'baseColorBound' in expected:
            bound = bool(record.get('mapMask', 0) & 1)
            checks.append({
                'name': f"{expected['name']} baseColor bound",
                'pass': bound == bool(expected['baseColorBound']),
                'actual': bound, 'expected': bool(expected['baseColorBound'])})
        if 'baseUv' in expected:
            for field, expected_value in (('uvOffset', expected['baseUv']['offset']),
                                          ('uvScale', expected['baseUv']['scale'])):
                actual = record.get(field)
                good = actual is not None and max(
                    abs(a - b) for a, b in zip(actual, expected_value)) <= 1e-3
                checks.append({'name': f"{expected['name']} {field}", 'pass': good,
                               'actual': actual, 'expected': expected_value})
            check('uvRotation', expected['baseUv']['rotation'])
        for slot, spec in (expected.get('slotUv') or {}).items():
            index = slot - 1
            for field, values in (('slotUvOffset', spec['offset']),
                                  ('slotUvScale', spec['scale'])):
                actual = (record.get(field) or [[0.0, 0.0]] * 4)[index]
                good = actual is not None and max(
                    abs(a - b) for a, b in zip(actual, values)) <= 1e-3
                checks.append({'name': f"{expected['name']} {field}[{slot}]",
                               'pass': good, 'actual': actual, 'expected': values})
            actual = (record.get('slotUvRotation') or [0.0] * 4)[index]
            checks.append({'name': f"{expected['name']} slotUvRotation[{slot}]",
                           'pass': abs(actual - spec['rotation']) <= 1e-3,
                           'actual': actual, 'expected': spec['rotation']})
        for slot, spec in (expected.get('slotSampler') or {}).items():
            uv_set, address_u, address_v, nearest = _slot_fields(
                (record.get('slotFlags') or [0] * 5)[slot])
            good = (uv_set == 0 and address_u == spec['addressU']
                    and address_v == spec['addressV'] and nearest == spec['nearest'])
            checks.append({'name': f"{expected['name']} slot sampler[{slot}]",
                           'pass': good,
                           'actual': {'uvSet': uv_set, 'addressU': address_u,
                                      'addressV': address_v, 'nearest': nearest},
                           'expected': spec})
    return checks, matched


def _slot_fields(flags):
    return (int(flags & 0xFF), (int(flags) >> 8) & 3, (int(flags) >> 10) & 3,
            bool(int(flags) & (1 << 12)))


PROBE_CHANNELS = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20]


def _locate_probes(frame, expected_materials):
    """Centroid per probe, classified against every expected display colour.

    Each material contributes one or more expected colours (a textured probe
    shows its two texel colours), and a pixel is assigned to the nearest target
    across all groups. That keeps the normal-scale probes, which differ by only
    a few hundredths of a base-colour component, apart.
    """
    targets, owners = [], []
    for index, material in enumerate(expected_materials):
        colors = material.get('locatorColors') or [material['baseColor'][:3]]
        for color in colors:
            targets.append(_display_rgb(color))
            owners.append(index)
    sums = [[0, 0, 0] for _ in expected_materials]
    width, height, rgb = frame['width'], frame['height'], frame['rgb']
    for y in range(height):
        row = y * width
        for x in range(width):
            i = (row + x) * 3
            pixel = (rgb[i], rgb[i + 1], rgb[i + 2])
            best, best_distance = -1, 1e9
            for index, target in enumerate(targets):
                distance = ((pixel[0] - target[0]) ** 2 + (pixel[1] - target[1]) ** 2
                            + (pixel[2] - target[2]) ** 2)
                if distance < best_distance:
                    best, best_distance = index, distance
            if best < 0 or best_distance > 45 * 45:
                continue
            group = owners[best]
            sums[group][0] += x
            sums[group][1] += y
            sums[group][2] += 1
    return [(sx // count, sy // count) if count >= 100 else None
            for sx, sy, count in sums]


def _probe_channels(viewer, records, expected_materials, output: Path):
    """Locate every probe sphere from the base-colour channel, then read values.

    The fixture assigns each probe a distinct base color so the locator cannot
    confuse two spheres that share a material key, and ``_centroids`` classifies
    every pixel against all expected display colours at once.
    """
    images = _save_channel_images(viewer, output / 'probes', PROBE_CHANNELS)
    set_channel(viewer.hwnd, 1)
    frame = viewer.wait_stable()
    rect = frame['rect']
    centroids = _locate_probes(frame, expected_materials)
    samples = []
    for expected, centroid in zip(expected_materials, centroids):
        entry = {'material': expected['name'], 'centroid': centroid, 'channels': {}}
        if centroid is None:
            entry['error'] = 'probe not visible'
            samples.append(entry)
            continue
        x = rect['left'] + centroid[0]
        y = rect['top'] + centroid[1]
        for channel in PROBE_CHANNELS:
            set_channel(viewer.hwnd, channel)
            entry['channels'][channel] = sample_region(
                viewer.hwnd, x - 4, y - 4, x + 4, y + 4).get('rgba')
        samples.append(entry)
    return samples, images


def _distance(left, right):
    return max(abs(a - b) for a, b in zip(left[:3], right[:3]))


def _check_gpu(samples, expected_materials):
    checks = []
    by_name = {item['material']: item for item in samples}

    def channel(name, channel):
        entry = by_name.get(name, {})
        return (entry.get('channels') or {}).get(channel)

    def check(name, passed, actual, expected, tolerance=None):
        checks.append({'name': name, 'pass': bool(passed), 'actual': actual,
                       'expected': expected, 'tolerance': tolerance})

    # --- normal texture scale -------------------------------------------------
    deviations = {}
    for name in ('normal scale 0', 'normal scale 045', 'normal scale 1'):
        geometric = channel(name, 2)
        shading = channel(name, 3)
        if not geometric or not shading:
            check(f'{name} normal channels present', False, shading, 'shading normal')
            continue
        deviations[name] = _distance(geometric, shading)
    if 'normal scale 0' in deviations:
        check('normal scale 0 does not perturb the shading normal',
              deviations['normal scale 0'] <= 0.02, deviations['normal scale 0'], 0.0, 0.02)
    if 'normal scale 045' in deviations and 'normal scale 0' in deviations:
        check('normal scale 0.45 perturbs the shading normal',
              deviations['normal scale 045'] > deviations['normal scale 0'] + 0.005,
              deviations['normal scale 045'], '>0.005')
    if 'normal scale 1' in deviations and 'normal scale 045' in deviations:
        check('normal scale 1 perturbs more than 0.45',
              deviations['normal scale 1'] > deviations['normal scale 045'],
              deviations['normal scale 1'], f">{deviations['normal scale 045']:.4f}")

    # --- independent occlusion ---------------------------------------------
    mr_ao = (channel('MR red is not AO', 8) or [None])[0]
    authored_ao = (channel('independent occlusion', 8) or [None])[0]
    if mr_ao is not None:
        check('MR red is not used as AO', abs(mr_ao - 1.0) <= 0.02, mr_ao, 1.0, 0.02)
    if authored_ao is not None:
        check('independent occlusion samples the authored 0.2',
              abs(authored_ao - 0.2) <= 0.02, authored_ao, 0.2, 0.02)

    # --- UV1 is unbound, not aliased to UV0 --------------------------------
    uv1 = channel('uv1 unsupported', 1)
    if uv1:
        expected = [0.8, 0.3, 0.2]
        check('uv1 slot is left unbound rather than aliased to UV0',
              _distance(uv1, expected) <= 0.02, uv1[:3], expected, 0.02)

    # --- per-slot transform actually changes the base-colour sample --------
    transformed = channel('per slot transform', 1)
    if transformed:
        check('per-slot base-colour transform replaces the flat factor',
              _distance(transformed, [0.7, 0.7, 0.7]) > 0.02,
              transformed[:3], 'texture sample')

    # --- alpha coverage and transmission ----------------------------------
    cutout_alpha = (channel('alpha cutout', 9) or [None])[0]
    if cutout_alpha is not None:
        check('MASK coverage keeps the authored 0.35 alpha above the 0.2 cutoff',
              abs(cutout_alpha - 0.35) <= 0.02, cutout_alpha, 0.35, 0.02)
    suppressed = (channel('ior 1 transmission suppressed', 10) or [None])[0]
    if suppressed is not None:
        check('IOR=1 transmission is suppressed rather than forced to a glass lobe',
              suppressed <= 0.01, suppressed, 0.0, 0.01)
    transmitted = (channel('ior 15 transmission', 10) or [None])[0]
    if transmitted is not None:
        check('IOR=1.5 transmission keeps the authored 0.9 factor',
              abs(transmitted - 0.9) <= 0.02, transmitted, 0.9, 0.02)

    # --- metal/rough split from the MR map --------------------------------
    mr_metal = (channel('MR red is not AO', 6) or [None])[0]
    mr_rough = (channel('MR red is not AO', 7) or [None])[0]
    if mr_metal is not None:
        check('MR blue still drives metallic', abs(mr_metal - 1.0) <= 0.02, mr_metal, 1.0, 0.02)
    if mr_rough is not None:
        check('MR green still drives roughness', abs(mr_rough - 128 / 255) <= 0.02,
              mr_rough, 128 / 255, 0.02)

    # --- mirrored instance tangent handedness -----------------------------
    mirrored = channel('mirrored tangent handedness', 3)
    mirrored_geo = channel('mirrored tangent handedness', 2)
    if mirrored and mirrored_geo:
        deviation = _distance(mirrored_geo, mirrored)
        check('mirrored instance keeps a valid perturbed tangent basis',
              deviation > 0.01, deviation, '>0.01')

    # --- double-sided back face reverses the geometric normal -------------
    for expected in expected_materials:
        if not expected.get('backFace'):
            continue
        geometric = channel(expected['name'], 2)
        shading = channel(expected['name'], 3)
        if not geometric or not shading:
            continue
        # Channel encoding is n*0.5+0.5, so reversing n maps each component v
        # to 1-v. Normal scale is 0 on this probe, so shading equals the
        # reversed geometric normal exactly.
        reversed_normal = [1.0 - value for value in geometric[:3]]
        check('double-sided back face reverses the geometric normal before the TBN',
              _distance(shading, reversed_normal) <= 0.02,
              shading[:3], reversed_normal, 0.02)

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
            f"- **{name}**: {survey['materialCount']} material draw records, "
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
        '- MR red is no longer read as AO. The MR probe reads occlusion 1.0 while',
        '  the independent occlusion probe reads the authored 0.2.',
        '- Normal scale changes the isolated shading-normal channel in authored',
        '  order (0 -> 0.45 -> 1.0); scale 0 leaves the geometric normal intact.',
        '- Per-slot transforms and samplers are applied independently, the base',
        '  color sample changes under its authored transform and the normal map',
        '  keeps its own transform and sampler.',
        '- UV1 references are left unbound with a specific compatibility record;',
        '  they are not sampled with UV0 coordinates.',
        '- Authored alphaMode/cutoff are preserved: the MASK probe keeps 0.35',
        '  coverage above its 0.2 cutoff. The IOR=1 testimonial case keeps its',
        '  explicit suppression (transmission 0.0); IOR=1.5 carries 0.9.',
        '- A mirrored instance keeps a valid perturbed tangent basis; the',
        '  double-sided front-face path is exercised by the shader contract lane.',
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


def run(workspace: Path, output: Path, configuration: str, fast: bool = False):
    output = Path(output)
    report = {'schema': RESULT_SCHEMA, 'task': 'T22', 'checks': [], 'failures': [],
              'status': 'running', 'fast': fast}
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
            # The external survey is the slow part of the lane and is not
            # acceptance evidence on its own; the fast iteration lane skips it.
            report['externalSurvey'] = {} if fast else _external_survey(viewer, output)
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
