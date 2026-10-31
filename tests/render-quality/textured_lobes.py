"""T25 texture-driven extension-lobe verification.

Runs the material-lobes unit lane, the glTF textured-lobe import lane and the
wire/hostile validator lanes serially, then drives the Debug viewer on the
generated ``textured-lobes`` fixture. The viewer reports only validated wire
values and raw effective-input channels; it never parses the glTF document.

Each camera-facing probe carries a map whose channel varies left-to-right, so a
single quad proves per-texel support: the lane locates the probe from its
isolated base-color channel, samples two points on the diagnostic lobe channel
and requires a real change. A plain quad and an unsupported-UV1 quad hold the
factor-only baseline constant.
"""
from __future__ import annotations

import json
import subprocess
import time
from pathlib import Path

from common import (RESULT_SCHEMA, encoded_png, send_field, sha256_file,
                    write_json)
from diagnostics import (_display_rgb, _save_channel_images, channel_supported,
                         material_records, sample_region, set_channel)
from fixtures import EXPECTED_TEXTURED_LOBES
from viewer import Viewer

ROOT = Path(__file__).resolve().parents[2]

SERIAL_LANES = [
    ('unit-material-lobes', 'Tests.Unit.exe', ['[material-lobes]']),
    ('import-textured-lobes', 'Tests.ImportIsolation.exe', ['[textured-lobes]']),
    ('import-wire-validator', 'Tests.ImportIsolation.exe', ['[shared-section-validator]']),
    ('import-hostile-worker', 'Tests.ImportIsolation.exe', ['[hostile-worker]']),
    ('import-texture-budget', 'Tests.ImportIsolation.exe', ['[texture-budget]']),
    ('import-coarse-detail', 'Tests.ImportIsolation.exe',
     ['[coarse-proxy],[detail-budget]']),
    ('import-texture-decode', 'Tests.ImportIsolation.exe', ['[texture-decode]']),
    ('unit-cache-identity', 'Tests.Unit.exe', ['[cache]']),
]

EVIDENCE_CHANNELS = [12, 24, 25, 26, 27, 28, 29, 30]
# Which component of a diagnostic channel changes with the authored map.
CHANNEL_COMPONENT = {12: 0, 24: 1, 25: 0, 26: 0, 27: 0, 28: 0, 29: 0, 30: 0}


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


def _match_record(expected, records, used):
    target = tuple(round(v, 3) for v in expected['baseColor'])
    for index, record in enumerate(records):
        if index in used:
            continue
        actual = tuple(round(v, 3) for v in record.get('baseColor', []))
        if actual == target:
            used.add(index)
            return record
    return None


def _check_records(records, expected_materials):
    checks = []
    matched = []
    used = set()
    for expected in expected_materials:
        record = _match_record(expected, records, used)
        matched.append({'name': expected['name'], 'matched': record is not None,
                        'record': record})
        if record is None:
            checks.append({'name': f"{expected['name']} record", 'pass': False,
                           'detail': 'no matching validated material record'})
            continue
        checks.append({'name': f"{expected['name']} bucket",
                       'pass': record.get('bucket') == expected['bucket'],
                       'actual': record.get('bucket'), 'expected': expected['bucket']})
        slots = {item['slot']: item for item in record.get('extensionSlots', [])}
        wanted = {item['slot']: item for item in expected.get('extensionSlots', [])}
        for slot in range(5, 13):
            spec = wanted.get(slot)
            actual = slots.get(slot, {})
            bound = bool(actual.get('bound'))
            expect_bound = bool(spec and spec.get('bound'))
            checks.append({'name': f"{expected['name']} slot {slot} bound",
                           'pass': bound == expect_bound,
                           'actual': bound, 'expected': expect_bound})
            if spec is None:
                continue
            uv_set = int(actual.get('slotFlags', 0)) & 0xFF
            checks.append({'name': f"{expected['name']} slot {slot} uv set",
                           'pass': uv_set == spec['uvSet'],
                           'actual': uv_set, 'expected': spec['uvSet']})
            checks.append({'name': f"{expected['name']} slot {slot} color space",
                           'pass': actual.get('colorSpace') == spec['colorSpace'],
                           'actual': actual.get('colorSpace'),
                           'expected': spec['colorSpace']})
            if 'uvOffset' in spec:
                offset = actual.get('uvOffset', [0.0, 0.0])
                good = max(abs(a - b) for a, b in zip(offset, spec['uvOffset'])) <= 1e-3
                checks.append({'name': f"{expected['name']} slot {slot} uv offset",
                               'pass': good, 'actual': offset,
                               'expected': spec['uvOffset']})
            if 'uvScale' in spec:
                scale = actual.get('uvScale', [1.0, 1.0])
                good = max(abs(a - b) for a, b in zip(scale, spec['uvScale'])) <= 1e-3
                checks.append({'name': f"{expected['name']} slot {slot} uv scale",
                               'pass': good, 'actual': scale,
                               'expected': spec['uvScale']})
        if 'sharedImageSlots' in expected:
            first, second = expected['sharedImageSlots']
            first_id = slots.get(first, {}).get('chunkId', 0)
            second_id = slots.get(second, {}).get('chunkId', 0)
            checks.append({
                'name': f"{expected['name']} shared image decodes per color semantics",
                'pass': first_id != 0 and second_id != 0 and first_id != second_id,
                'actual': [first_id, second_id],
                'expected': 'two distinct image chunks'})
        if 'clearcoatNormalScale' in expected:
            actual = record.get('clearcoatNormalScale')
            checks.append({'name': f"{expected['name']} clearcoat normal scale",
                           'pass': actual is not None
                           and abs(actual - expected['clearcoatNormalScale']) <= 1e-3,
                           'actual': actual,
                           'expected': expected['clearcoatNormalScale']})
    return checks, matched


def _locate_boxes(frame, expected_materials):
    """Bounding boxes per probe, classified by the isolated base-color channel."""
    targets = [_display_rgb(item['baseColor'][:3]) for item in expected_materials]
    boxes = [[10 ** 9, 10 ** 9, -1, -1, 0] for _ in expected_materials]
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
            if best < 0 or best_distance > 40 * 40:
                continue
            box = boxes[best]
            box[0] = min(box[0], x)
            box[1] = min(box[1], y)
            box[2] = max(box[2], x)
            box[3] = max(box[3], y)
            box[4] += 1
    return [tuple(box[:4]) if box[4] >= 200 else None for box in boxes]


def _sample_channels(viewer, boxes, expected_materials, output: Path):
    """Sample the left/right third of every probe on the lobe channels."""
    images = _save_channel_images(viewer, output, EVIDENCE_CHANNELS)
    frame = viewer.wait_stable()
    rect = frame['rect']
    samples = []
    for expected, box in zip(expected_materials, boxes):
        entry = {'material': expected['name'], 'box': box, 'channels': {}}
        if box is None:
            entry['error'] = 'probe not visible'
            samples.append(entry)
            continue
        x0, y0, x1, y1 = box
        width, height = max(1, x1 - x0), max(1, y1 - y0)
        y = rect['top'] + y0 + height // 2
        points = {
            'left': rect['left'] + x0 + int(width * 0.28),
            'right': rect['left'] + x0 + int(width * 0.72),
        }
        for channel in EVIDENCE_CHANNELS:
            set_channel(viewer.hwnd, channel)
            values = {}
            for side, x in points.items():
                values[side] = sample_region(viewer.hwnd, x - 1, y - 1, x + 1, y + 1).get('rgba')
            entry['channels'][channel] = values
        samples.append(entry)
    return samples, images


def _component(value, channel):
    if not value:
        return None
    return value[CHANNEL_COMPONENT.get(channel, 0)]


def _check_gpu(samples, expected_materials):
    checks = []
    by_name = {item['material']: item for item in samples}

    def add(name, passed, actual, expected, tolerance=None):
        checks.append({'name': name, 'pass': bool(passed), 'actual': actual,
                       'expected': expected, 'tolerance': tolerance})

    for expected in expected_materials:
        name = expected['name']
        entry = by_name.get(name, {})
        if entry.get('error'):
            add(f'{name} visible', False, entry['error'], 'probe visible')
            continue
        for channel in expected.get('channels', []):
            values = (entry.get('channels') or {}).get(channel, {})
            left = _component(values.get('left'), channel)
            right = _component(values.get('right'), channel)
            if left is None or right is None:
                add(f'{name} channel {channel} sampled', False, values, 'two samples')
                continue
            delta = abs(right - left)
            threshold = expected.get('deltas', {}).get(channel, 0.02)
            add(f'{name} channel {channel} map varies', delta > threshold,
                round(delta, 5), f'>{threshold}')
            low, high = expected.get('ranges', {}).get(channel, [None, None])
            if low is not None:
                add(f'{name} channel {channel} inside authored envelope',
                    low - 0.02 <= left <= high + 0.02
                    and low - 0.02 <= right <= high + 0.02,
                    [round(left, 5), round(right, 5)], [low, high])
        for channel, values in expected.get('constant', {}).items():
            channel_values = (entry.get('channels') or {}).get(channel, {})
            for side in ('left', 'right'):
                actual = (channel_values.get(side) or [0.0, 0.0, 0.0])[:3]
                good = max(abs(a - b) for a, b in zip(actual, values)) <= 0.02
                add(f'{name} channel {channel} {side} stays factor-only',
                    good, [round(v, 5) for v in actual], values)
    return checks


def write_report(output: Path, report):
    lines = [
        '# T25 textured extension-lobe report',
        '',
        'Generated from serial unit/import/wire/hostile lanes plus a Debug GPU',
        'session that samples left and right of each camera-facing probe on the',
        'effective-lobe diagnostic channels.',
        '',
        f"- Status: **{report.get('status')}**",
        f"- Checks: {sum(1 for c in report.get('checks', []) if c['pass'])} passed / "
        f"{len(report.get('checks', []))} total",
        f"- Failures: {len(report.get('failures', []))}",
    ]
    for failure in report.get('failures', []):
        lines.append(f'  - {failure}')
    lines += ['', '## Probe channels', '']
    for expected in EXPECTED_TEXTURED_LOBES:
        lines.append(
            f"- **{expected['name']}**: varied {expected.get('channels', [])}, "
            f"constant {sorted(expected.get('constant', {}))}")
    lines += ['', '## Interpretation', '',
              '- Each mapped probe changes its effective-input channel across the',
              '  quad, so the authored map is sampled, not replaced by its factor.',
              '- The plain quad keeps every extension lobe at its factor-only',
              '  default; the unsupported UV1 clearcoat map stays unbound rather',
              '  than aliasing UV0.',
              '- Specular factor and color share one authored image but decode into',
              '  distinct linear and sRGB chunks.',
              '- Anisotropy, iridescence, volume and dispersion remain explicitly',
              '  unsupported and are not claimed by this lane.',
              '', '## Evidence', '',
              f"- `{output / 'diagnostics' / 'textured-lobes.json'}`",
              f"- `{output / 'channels'}` channel images",
              f"- `{output / 'textured-lobes-studio.png'}` fixture capture", '']
    path = output / 'textured-lobes-report.md'
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def run(workspace: Path, output: Path, configuration: str, fast: bool = False):
    output = Path(output)
    report = {'schema': RESULT_SCHEMA, 'task': 'T25', 'checks': [], 'failures': [],
              'status': 'running', 'fast': fast}
    report['serialLanes'] = _run_serial(configuration, output)
    for lane in report['serialLanes']:
        if not lane.get('pass'):
            report['failures'].append(f"serial lane failed: {lane['name']}")

    fixture_path = output / 'fixtures' / 'textured-lobes.glb'
    if not fixture_path.exists():
        report['failures'].append(f'textured-lobes fixture missing: {fixture_path}')
        report['status'] = 'fail'
        write_json(output / 'diagnostics/textured-lobes.json', report)
        return report

    try:
        with Viewer(configuration, output, width=1280, height=860) as viewer:
            viewer.open(fixture_path)
            viewer.apply_controlled_presentation()
            viewer.set_camera(0.0, 90.0, 1.35)
            report['channelSupport'] = channel_supported(viewer.hwnd)
            records = material_records(viewer.hwnd)
            report['materialRecords'] = records
            checks, matched = _check_records(records, EXPECTED_TEXTURED_LOBES)
            report['checks'].extend(checks)
            report['materialChecks'] = matched
            set_channel(viewer.hwnd, 1)
            frame = viewer.wait_stable()
            capture = output / 'textured-lobes-studio.png'
            capture.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
            report['capture'] = {'image': str(capture), 'sha256': sha256_file(capture)}
            boxes = _locate_boxes(frame, EXPECTED_TEXTURED_LOBES)
            report['probeBoxes'] = boxes
            samples, images = _sample_channels(viewer, boxes, EXPECTED_TEXTURED_LOBES, output)
            report['probeSamples'] = samples
            report['probeImages'] = images
            report['checks'].extend(_check_gpu(samples, EXPECTED_TEXTURED_LOBES))
            # T25 reset coverage: one injected device removal must rebuild the
            # device and reconstruct the authored map bindings from the retained
            # source. A fallback descriptor that lost the map is a failure.
            send_field(viewer.hwnd, 65, 0)
            deadline = time.monotonic() + 90.0
            recovered = False
            reset_records = []
            while time.monotonic() < deadline:
                time.sleep(0.5)
                reset_records = material_records(viewer.hwnd)
                if len(reset_records) != len(EXPECTED_TEXTURED_LOBES):
                    continue
                specular = next((item for item in reset_records
                                 if round(item.get('baseColor', [0])[0], 3) == 0.9), None)
                if specular and any(slot.get('bound') for slot in specular.get('extensionSlots', [])):
                    recovered = True
                    break
            viewer.wait_stable()
            post = output / 'textured-lobes-after-device-reset.png'
            post_frame = viewer.capture()
            post.write_bytes(encoded_png(post_frame['width'], post_frame['height'],
                                         post_frame['rgb']))
            report['deviceReset'] = {
                'recovered': recovered, 'image': str(post),
                'sha256': sha256_file(post),
                'records': len(reset_records)}
            report['checks'].append({
                'name': 'device reset rebuild reconstructs authored map bindings',
                'pass': recovered, 'actual': len(reset_records),
                'expected': f'{len(EXPECTED_TEXTURED_LOBES)} records with bound maps'})
    except Exception as error:
        report['failures'].append(f'GPU diagnostic lane failed: {error}')

    for channel in EVIDENCE_CHANNELS:
        if not report.get('channelSupport', {}).get(channel):
            report['failures'].append(f'debug channel {channel} was not advertised')
    if not any(box is not None for box in report.get('probeBoxes', [])):
        report['failures'].append('no textured-lobe probe was located')
    failed = [check for check in report['checks'] if not check['pass']]
    if failed:
        report['failures'].append(f'{len(failed)} textured-lobe checks failed')
    report['status'] = 'pass' if not report['failures'] else 'fail'
    write_json(output / 'diagnostics/textured-lobes.json', report)
    report['reportPath'] = str(write_report(output, report))
    return report
