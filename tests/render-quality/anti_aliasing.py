"""T33 edge, specular and texture sampling verification.

The CPU lane pins the edge-AA policy and the semantic mip reduction. The GPU
lane drives the generated ``edge-probe`` fixture in Studio, Clay and
Directional and records:

* same-camera AA on/off pairs for sub-pixel wires, grille, engraved lettering,
  chrome, normal-map detail, clear glass and an alpha cutout;
* a 2x window box-reduced supersampled reference (not a runtime default) and
  the local edge error of AA-off/AA-on against it;
* picking equality with AA on/off (R32_UINT is never averaged);
* a full-window UI comparison (D2D text is outside the scene AA);
* matched small-orbit sequences, physical 1080p and fractional resize;
* resize/device-loss/forced-off lifecycle and the zero-allocation cost.
"""
from __future__ import annotations

import math
import os
import struct
import subprocess
import time
from pathlib import Path

import common
from common import RESULT_SCHEMA, decode_png, encoded_png, send_field, sha256_file, write_json
from viewer import Viewer

ROOT = Path(__file__).resolve().parents[2]
FAST = bool(os.environ.get('RENDER_QUALITY_FAST'))

SERIAL_LANES = [
    ('unit-anti-aliasing', 'Tests.Unit.exe', ['[anti-aliasing]']),
    ('import-texture-mips', 'Tests.ImportIsolation.exe', ['[texture-mips]']),
]

# Normalized role rectangles for the generated fixture. They are used for
# inspection metrics, never as renderer exceptions.
REGIONS = {
    'digits': [0.33, 0.37, 0.52, 0.50],
    'grille': [0.51, 0.36, 0.68, 0.51],
    'wires': [0.30, 0.47, 0.72, 0.55],
    'chrome': [0.37, 0.55, 0.47, 0.70],
    'normal': [0.45, 0.55, 0.55, 0.70],
    'lens': [0.51, 0.55, 0.62, 0.70],
    'grid-left': [0.04, 0.64, 0.30, 0.95],
}


def _f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', float(value)))[0]


def _query(hwnd, field):
    return send_field(hwnd, field, -1) & 0xFFFFFFFF


def _set_aa(viewer, enabled):
    send_field(viewer.hwnd, 300, 1 if enabled else 0)
    time.sleep(0.35)


def _set_lighting(viewer, mode):
    send_field(viewer.hwnd, 88, mode)
    time.sleep(0.35)


def _set_evidence_exposure(viewer):
    """A brighter, still neutral exposure for the edge/detail evidence."""
    send_field(viewer.hwnd, 92, _f32_bits(1.5))
    send_field(viewer.hwnd, 98, _f32_bits(1.3))
    time.sleep(0.3)


def _aa_state(viewer):
    return {
        'enabled': _query(viewer.hwnd, 300) != 0,
        'mode': _query(viewer.hwnd, 301),
        'fallbackReason': _query(viewer.hwnd, 302),
        'additionalTargetBytes': _query(viewer.hwnd, 303),
        'tapCount': _query(viewer.hwnd, 304),
        'resolveMicroseconds': _query(viewer.hwnd, 305),
    }


def _check(checks, name, passed, actual=None, expected=None, tolerance=None):
    checks.append({'name': name, 'pass': bool(passed), 'actual': actual,
                   'expected': expected, 'tolerance': tolerance})


def _save(viewer, output: Path, name: str, stable=True, settle=0.4):
    if settle:
        time.sleep(settle)
    frame = viewer.wait_stable() if stable else viewer.capture()
    path = output / 'images' / f'{name}.png'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
    return {'frame': frame, 'path': str(path), 'sha256': sha256_file(path),
            'width': frame['width'], 'height': frame['height'],
            'dpi': frame['dpi'], 'dpiScale': frame['dpiScale'],
            'client': frame['client']}


def _decode(record):
    return decode_png(Path(record['path']).read_bytes())


def _luma(rgb, index):
    return 0.2126 * rgb[index] + 0.7152 * rgb[index + 1] + 0.0722 * rgb[index + 2]


def _rect_pixels(width, height, rect):
    x0, y0, x1, y1 = rect
    return (max(0, int(x0 * width)), max(0, int(y0 * height)),
            min(width, int(x1 * width)), min(height, int(y1 * height)))


def _pair_metrics(on, off, threshold=2):
    w, h, a = _decode(on)
    w2, h2, b = _decode(off)
    if (w, h) != (w2, h2):
        return {'error': 'capture extent changed'}
    count = w * h
    changed = 0
    total = 0
    maximum = 0
    sum_on = 0.0
    sum_off = 0.0
    background = (b[0], b[1], b[2])
    background_pixels = 0
    background_changed = 0
    for pixel in range(count):
        base = pixel * 3
        delta = max(abs(a[base] - b[base]), abs(a[base + 1] - b[base + 1]),
                    abs(a[base + 2] - b[base + 2]))
        if delta > threshold:
            changed += 1
            total += delta
            maximum = max(maximum, delta)
        sum_on += _luma(a, base)
        sum_off += _luma(b, base)
        if (abs(b[base] - background[0]) <= 2
                and abs(b[base + 1] - background[1]) <= 2
                and abs(b[base + 2] - background[2]) <= 2):
            background_pixels += 1
            background_changed += delta
    return {
        'width': w, 'height': h, 'pixels': count,
        'changedPixels': changed,
        'changedFraction': round(changed / count, 5),
        'meanDiff': round(total / max(1, changed), 2),
        'maxDiff': maximum,
        'meanLumaDelta': round((sum_on - sum_off) / count, 4),
        'backgroundPixels': background_pixels,
        'backgroundMeanDelta': round(background_changed / max(1, background_pixels), 4),
    }


def _region_metrics(record, rect):
    w, h, rgb = _decode(record)
    left, top, right, bottom = _rect_pixels(w, h, rect)
    values = []
    strong = 0
    for y in range(top, bottom):
        for x in range(left, right):
            index = (y * w + x) * 3
            luma = _luma(rgb, index)
            values.append(luma)
            if x + 1 < right:
                nxt = _luma(rgb, index + 3)
                if abs(luma - nxt) > 18.0:
                    strong += 1
            if y + 1 < bottom:
                nxt = _luma(rgb, index + w * 3)
                if abs(luma - nxt) > 18.0:
                    strong += 1
    mean = sum(values) / max(1, len(values))
    variance = sum((value - mean) ** 2 for value in values) / max(1, len(values))
    return {'pixels': len(values), 'strongEdgeSamples': strong,
            'lumaStdDev': round(math.sqrt(variance), 4),
            'meanLuma': round(mean, 3), 'rect': rect}


def _region_diff(on, off, rect, threshold=2):
    w, h, a = _decode(on)
    w2, h2, b = _decode(off)
    if (w, h) != (w2, h2):
        return None
    left, top, right, bottom = _rect_pixels(w, h, rect)
    count = maximum = changed = 0
    total = 0
    for y in range(top, bottom):
        for x in range(left, right):
            index = (y * w + x) * 3
            delta = max(abs(a[index] - b[index]), abs(a[index + 1] - b[index + 1]),
                        abs(a[index + 2] - b[index + 2]))
            if delta > threshold:
                changed += 1
                total += delta
                maximum = max(maximum, delta)
            count += 1
    return {'pixels': count, 'changedPixels': changed,
            'changedFraction': round(changed / max(1, count), 5),
            'meanDiff': round(total / max(1, changed), 3), 'maxDiff': maximum}


def _downsample2(width, height, rgb):
    out_w, out_h = max(1, width // 2), max(1, height // 2)
    out = bytearray(out_w * out_h * 3)
    for y in range(out_h):
        for x in range(out_w):
            total = [0, 0, 0]
            for dy in (0, 1):
                for dx in (0, 1):
                    source = ((y * 2 + dy) * width + (x * 2 + dx)) * 3
                    for channel in range(3):
                        total[channel] += rgb[source + channel]
            target = (y * out_w + x) * 3
            for channel in range(3):
                out[target + channel] = (total[channel] + 2) // 4
    return out_w, out_h, bytes(out)


def _reference_pair_error(off, on, reference, rect=None):
    """Compare AA-off/on to the SSAA reference on one shared edge mask.

    The mask is the union of pixels where either candidate differs strongly
    from the reference. Using one mask prevents AA-on from gaining a cheaper
    denominator simply by creating new difference pixels. `rect` restricts the
    comparison to an inspected detail region; a whole-frame MAE is recorded but
    is not the acceptance authority.
    """
    w, h, a = _decode(off)
    w2, h2, b = _decode(on)
    rw, rh, ref = reference
    if (w, h) != (rw, rh) or (w2, h2) != (rw, rh):
        return {'error': f'reference extent {rw}x{rh} != {w}x{h}/{w2}x{h2}'}
    left, top, right, bottom = (0, 0, w, h) if rect is None else _rect_pixels(w, h, rect)
    pixels = []
    for y in range(top, bottom):
        for x in range(left, right):
            pixels.append(y * w + x)
    mask = []
    for pixel in pixels:
        base = pixel * 3
        d_off = max(abs(a[base] - ref[base]), abs(a[base + 1] - ref[base + 1]),
                    abs(a[base + 2] - ref[base + 2]))
        d_on = max(abs(b[base] - ref[base]), abs(b[base + 1] - ref[base + 1]),
                   abs(b[base + 2] - ref[base + 2]))
        if d_off > 20 or d_on > 20:
            mask.append(pixel)
    totals = [0, 0]
    maxima = [0, 0]
    improved = 0
    for pixel in mask:
        base = pixel * 3
        d_off = max(abs(a[base] - ref[base]), abs(a[base + 1] - ref[base + 1]),
                    abs(a[base + 2] - ref[base + 2]))
        d_on = max(abs(b[base] - ref[base]), abs(b[base + 1] - ref[base + 1]),
                   abs(b[base + 2] - ref[base + 2]))
        totals[0] += d_off
        totals[1] += d_on
        maxima[0] = max(maxima[0], d_off)
        maxima[1] = max(maxima[1], d_on)
        if d_on < d_off:
            improved += 1
    return {
        'edgePixels': len(mask),
        'edgeFraction': round(len(mask) / max(1, len(pixels)), 5),
        'off': {'edgeMae': round(totals[0] / max(1, len(mask)), 3),
                'maxDiff': maxima[0]},
        'on': {'edgeMae': round(totals[1] / max(1, len(mask)), 3),
               'maxDiff': maxima[1]},
        'improvedFraction': round(improved / max(1, len(mask)), 4),
        'rect': rect,
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
        lanes.append({'name': name, 'command': ' '.join(command),
                      'exitCode': completed.returncode,
                      'seconds': round(time.monotonic() - started, 2),
                      'pass': completed.returncode == 0, 'log': str(log)})
    return lanes


def _mode_lane(viewer, output: Path, checks):
    _set_aa(viewer, True)
    results = {}
    images = []
    for mode_name, mode, yaw, pitch in (
            ('studio', 0, 8.0, 72.5), ('clay', 1, 8.0, 72.5),
            ('directional', 2, 8.0, 72.5)):
        _set_lighting(viewer, mode)
        if mode == 2:
            send_field(viewer.hwnd, 277, _f32_bits(0.86))
            send_field(viewer.hwnd, 278, _f32_bits(0.78))
            time.sleep(0.25)
        viewer.set_camera(yaw, pitch, 1.23)
        _set_aa(viewer, True)
        on = _save(viewer, output, f'aa-{mode_name}-on')
        state = _aa_state(viewer)
        _set_aa(viewer, False)
        off = _save(viewer, output, f'aa-{mode_name}-off')
        _set_aa(viewer, True)
        metrics = _pair_metrics(on, off)
        digits = _region_metrics(on, REGIONS['digits'])
        digits_off = _region_metrics(off, REGIONS['digits'])
        text_diff = _region_diff(on, off, REGIONS['digits'])
        wire_diff = _region_diff(on, off, REGIONS['wires'])
        normal_diff = _region_diff(on, off, REGIONS['normal'])
        results[mode_name] = {
            'on': {'path': on['path'], 'sha256': on['sha256'], 'state': state,
                   'dpi': on['dpi'], 'dpiScale': on['dpiScale']},
            'off': {'path': off['path'], 'sha256': off['sha256']},
            'metrics': metrics,
            'digits': {'on': digits, 'off': digits_off, 'diff': text_diff},
            'wireDiff': wire_diff, 'normalDiff': normal_diff,
        }
        images.extend([on['path'], off['path']])
        _check(checks, f'{mode_name} AA is active', state['mode'] == 1, state, 'mode 1')
        _check(checks, f'{mode_name} AA uses the bounded tap count',
               state['tapCount'] == 7, state['tapCount'], 7)
        _check(checks, f'{mode_name} AA allocates no render target',
               state['additionalTargetBytes'] == 0, state['additionalTargetBytes'], 0)
        _check(checks, f'{mode_name} AA changes edge pixels',
               metrics.get('changedPixels', 0) >= 0.001 * metrics.get('pixels', 1),
               metrics.get('changedPixels'), '>=0.1% of viewport')
        _check(checks, f'{mode_name} AA is not a global blur/exposure change',
               abs(metrics.get('meanLumaDelta', 99)) <= 1.0
               and metrics.get('changedFraction', 1.0) <= 0.35,
               metrics, '|luma|<=1, changed<=35%')
        _check(checks, f'{mode_name} background stays halo-free',
               metrics.get('backgroundMeanDelta', 99) <= 0.5,
               metrics.get('backgroundMeanDelta'), '<=0.5')
        _check(checks, f'{mode_name} sub-pixel wires are filtered',
               (wire_diff or {}).get('changedPixels', 0) > 0, wire_diff, '>0')
        _check(checks, f'{mode_name} engraved lettering keeps its contrast',
               (digits['strongEdgeSamples']
                >= 0.80 * max(1, digits_off['strongEdgeSamples']))
               and digits['lumaStdDev'] >= 0.80 * max(0.01, digits_off['lumaStdDev']),
               {'on': digits, 'off': digits_off}, 'edge/std >=80% of AA-off')
        _check(checks, f'{mode_name} normal-map detail changes without a global blur',
               (normal_diff or {}).get('changedFraction', 1.0) <= 0.35,
               normal_diff, 'changed <=35%')
    return results, images


def _picking_lane(viewer, output: Path, checks):
    _set_lighting(viewer, 0)
    viewer.set_camera(8.0, 72.5, 1.23)
    _set_aa(viewer, True)
    frame = _save(viewer, output, 'aa-pick-on')
    # Search a coarse image grid for real hits rather than hard-coding a
    # projected part position; the oblique canonical camera is deterministic.
    points = []
    image_width = frame['width']
    image_height = frame['height']
    for gy in range(8, 19):
        for gx in range(6, 16):
            if len(points) >= 5:
                break
            point = (gx / 20.0 * image_width, gy / 20.0 * image_height)
            probe = viewer.pick_image(frame['frame'], *point)
            if probe.get('id'):
                points.append(point)
        if len(points) >= 5:
            break
    on_ids = [(point, viewer.pick_image(frame['frame'], *point)) for point in points]
    _set_aa(viewer, False)
    frame_off = _save(viewer, output, 'aa-pick-off')
    off_ids = [(point, viewer.pick_image(frame_off['frame'], *point)) for point in points]
    _set_aa(viewer, True)
    mismatches = []
    hits = 0
    for (point, a), (_, b) in zip(on_ids, off_ids):
        if a.get('id'):
            hits += 1
        if a.get('id') != b.get('id') or a.get('hit') != b.get('hit'):
            mismatches.append({'point': point, 'on': a, 'off': b})
    record = {'points': [{'point': p, 'on': a, 'off': b}
                         for (p, a), (_, b) in zip(on_ids, off_ids)],
              'hits': hits, 'mismatches': mismatches,
              'images': [frame['path'], frame_off['path']]}
    _check(checks, 'picking hits scene geometry', hits >= 2, hits, '>=2')
    _check(checks, 'AA never changes or averages the R32_UINT pick id',
           not mismatches, mismatches, 'no mismatch')
    return record


def _ui_lane(viewer, output: Path, checks):
    """The D2D/D3D11On12 overlay is recorded after the resolve: AA on/off must
    leave a static toolbar text region byte-identical (within capture noise)."""
    _set_aa(viewer, True)
    time.sleep(0.4)
    w, h, first = common.capture_window(viewer.hwnd)
    time.sleep(0.3)
    _, _, baseline = common.capture_window(viewer.hwnd)
    _set_aa(viewer, False)
    time.sleep(0.4)
    _, _, off = common.capture_window(viewer.hwnd)
    _set_aa(viewer, True)
    scale = viewer.capture()['dpiScale']
    x0 = int(60 * scale); x1 = min(w, int(520 * scale))
    y0 = int(8 * scale); y1 = min(h, int(42 * scale))

    def region_delta(a, b):
        count = maximum = 0
        total = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                index = (y * w + x) * 3
                delta = max(abs(a[index] - b[index]), abs(a[index + 1] - b[index + 1]),
                            abs(a[index + 2] - b[index + 2]))
                total += delta
                maximum = max(maximum, delta)
                count += 1
        return {'pixels': count, 'meanDelta': round(total / max(1, count), 4),
                'maxDelta': maximum}

    noise = region_delta(first, baseline)
    changed = region_delta(first, off)
    path = output / 'images' / 'aa-ui-on-off.png'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(encoded_png(w, h, off))
    record = {'region': [x0, y0, x1, y1], 'captureNoise': noise,
              'aaOnOffDelta': changed, 'path': str(path)}
    _check(checks, 'UI text is outside scene AA (no D2D text delta)',
           changed['maxDelta'] <= max(1, noise['maxDelta'] + 1),
           {'noise': noise, 'aa': changed}, '<= capture noise +1')
    return record


def _coverage_lane(viewer, output: Path, checks):
    """AA runs after the OIT/coverage composite and must not create or close a
    cutout hole or reorder transparent surfaces. Uses the established T29/T32
    cutout and T26 layered-glass fixtures."""
    results = {}
    cutout_fixture = output / 'fixtures' / 'ao-probe.glb'
    if cutout_fixture.exists():
        viewer.open(cutout_fixture)
        viewer.apply_controlled_presentation()
        viewer.set_camera(10.0, 22.0, 1.0)
        _set_aa(viewer, True)
        on = _save(viewer, output, 'aa-cutout-on')
        _set_aa(viewer, False)
        off = _save(viewer, output, 'aa-cutout-off')
        _set_aa(viewer, True)
        rect = [0.74, 0.59, 0.86, 0.76]

        def hole_pixels(record):
            w, h, rgb = _decode(record)
            left, top, right, bottom = _rect_pixels(w, h, rect)
            dark = 0
            total = 0
            for y in range(top, bottom):
                for x in range(left, right):
                    if _luma(rgb, (y * w + x) * 3) < 50.0:
                        dark += 1
                    total += 1
            return dark, total

        on_dark, total = hole_pixels(on)
        off_dark, _ = hole_pixels(off)
        results['cutout'] = {'onDark': on_dark, 'offDark': off_dark,
                             'pixels': total, 'on': on['path'], 'off': off['path']}
        delta = abs(on_dark - off_dark) / max(1, total)
        _check(checks, 'MASK cutout holes stay open and unchanged by AA',
               on_dark > 0 and delta <= 0.01, results['cutout'],
               'holes>0, coverage delta<=1%')
    else:
        _check(checks, 'cutout fixture available', False, str(cutout_fixture), 'exists')

    glass_fixture = output / 'fixtures' / 'layered-glass.glb'
    if glass_fixture.exists():
        viewer.open(glass_fixture)
        viewer.apply_controlled_presentation()
        viewer.set_camera(12.0, 24.0, 1.0)
        _set_aa(viewer, True)
        on = _save(viewer, output, 'aa-oit-on')
        _set_aa(viewer, False)
        off = _save(viewer, output, 'aa-oit-off')
        _set_aa(viewer, True)
        diff = _pair_metrics(on, off)
        results['oit'] = {'on': on['path'], 'off': off['path'], 'metrics': diff,
                          'algorithm': _query(viewer.hwnd, 243)}
        _check(checks, 'OIT stays resolved before the edge filter',
               results['oit']['algorithm'] == 1
               and diff.get('changedFraction', 1.0) <= 0.20,
               {'algorithm': results['oit']['algorithm'],
                'changedFraction': diff.get('changedFraction')},
               'algorithm 1, changed<=20%')
    else:
        _check(checks, 'OIT fixture available', False, str(glass_fixture), 'exists')

    # Restore the edge fixture for the remaining UI/motion/size/reference lanes.
    viewer.open(output / 'fixtures' / 'edge-probe.glb')
    viewer.apply_controlled_presentation()
    _set_evidence_exposure(viewer)
    send_field(viewer.hwnd, 104, 1)
    send_field(viewer.hwnd, 196, 1)
    send_field(viewer.hwnd, 102, 1)
    viewer.set_camera(8.0, 72.5, 1.23)
    return results


def _motion_lane(viewer, output: Path, checks):
    _set_lighting(viewer, 0)
    steps = []
    for yaw in (8.0, 12.0, 16.0):
        viewer.set_camera(yaw, 72.5, 1.23)
        _set_aa(viewer, True)
        on = _save(viewer, output, f'aa-orbit-{int(yaw):02d}-on')
        _set_aa(viewer, False)
        off = _save(viewer, output, f'aa-orbit-{int(yaw):02d}-off')
        _set_aa(viewer, True)
        diff = _pair_metrics(on, off)
        digits = _region_metrics(on, REGIONS['digits'])
        digits_off = _region_metrics(off, REGIONS['digits'])
        steps.append({'yaw': yaw, 'metrics': diff,
                      'digitsOn': digits, 'digitsOff': digits_off,
                      'on': on['path'], 'off': off['path']})
        _check(checks, f'orbit {yaw:g} AA still changes edges',
               diff.get('changedPixels', 0) > 0, diff.get('changedPixels'), '>0')
        _check(checks, f'orbit {yaw:g} lettering contrast retained',
               digits['strongEdgeSamples']
               >= 0.78 * max(1, digits_off['strongEdgeSamples']),
               {'on': digits['strongEdgeSamples'],
                'off': digits_off['strongEdgeSamples']}, '>=78%')
    fractions = [step['metrics'].get('changedFraction', 0.0) for step in steps]
    if fractions:
        ratio = max(fractions) / max(min(fractions), 1e-6)
        _check(checks, 'AA coverage is stable under small orbit motion',
               ratio <= 2.5, round(ratio, 3), '<=2.5')
    return steps


def _size_lane(viewer, output: Path, checks):
    _set_lighting(viewer, 0)
    viewer.set_camera(8.0, 72.5, 1.23)
    base = viewer.capture()
    chrome = base['client'][1] - base['height']
    results = {}
    for name, width, height in (('physical-1080p', 1920, 1080),
                                ('physical-1440p', 2560, 1440),
                                ('fractional-1537x901', 1537, 901)):
        viewer.resize_client(width, height + chrome)
        _set_aa(viewer, True)
        capture = _save(viewer, output, f'aa-{name}')
        state = _aa_state(viewer)
        results[name] = {'width': capture['width'], 'height': capture['height'],
                         'clientWidth': capture['client'][0],
                         'clientHeight': capture['client'][1],
                         'dpi': capture['dpi'], 'dpiScale': capture['dpiScale'],
                         'state': state, 'path': capture['path'],
                         'sha256': capture['sha256']}
        _check(checks, f'{name} keeps AA active at physical viewport pixels',
               state['mode'] == 1 and capture['width'] == width
               and capture['height'] == height,
               {'state': state, 'width': capture['width'],
                'height': capture['height']},
               {'mode': 1, 'width': width, 'height': height})
    viewer.resize_client(1200, 860)

    # Simulated UI DPI: the Debug field rescales chrome layout, so the 3D
    # viewport aspect and the text/overlay geometry change without relaunching.
    dpi_results = {}
    for dpi in (96, 144, 192):
        send_field(viewer.hwnd, 67, dpi)
        time.sleep(0.6)
        _set_aa(viewer, True)
        capture = _save(viewer, output, f'aa-dpi-{dpi}')
        state = _aa_state(viewer)
        dpi_results[str(dpi)] = {'width': capture['width'],
                                 'height': capture['height'],
                                 'state': state, 'path': capture['path']}
        _check(checks, f'AA survives simulated {dpi} DPI',
               state['mode'] == 1 and capture['width'] > 64,
               {'state': state, 'width': capture['width']}, 'mode 1, drawable')
    send_field(viewer.hwnd, 67, 144)
    time.sleep(0.5)
    results['dpi'] = dpi_results
    return results


def _ssaa_reference(viewer, output: Path, configuration, views, checks):
    """A 2x client capture, box-reduced, is the deterministic SSAA reference.

    It is a validation artifact only: the product still renders at the sampled
    viewport size.
    """
    if FAST:
        return {'status': 'skipped', 'reason': 'fast iteration lane'}
    frame = viewer.capture()
    client_w, client_h = frame['client']
    chrome = client_h - frame['height']
    target = (2 * client_w, 2 * frame['height'] + chrome)
    results = {}
    with Viewer(configuration, output / 'ssaa', width=target[0] + 40,
                height=target[1] + 60) as reference:
        reference.resize_client(*target)
        reference.open(output / 'fixtures' / 'edge-probe.glb')
        reference.apply_controlled_presentation()
        _set_evidence_exposure(reference)
        send_field(reference.hwnd, 104, 1)
        send_field(reference.hwnd, 196, 1)
        send_field(reference.hwnd, 102, 1)
        send_field(reference.hwnd, 300, 0)  # plain sampled reference
        for name, yaw, pitch, mode in views:
            _set_lighting(reference, mode)
            reference.set_camera(yaw, pitch, 1.23)
            capture = _save(reference, output, f'aa-ssaa-{name}', settle=0.4)
            width, height, rgb = _decode(capture)
            rw, rh, reduced = _downsample2(width, height, rgb)
            path = output / 'images' / f'aa-reference-{name}.png'
            path.write_bytes(encoded_png(rw, rh, reduced))
            results[name] = {'path': str(path), 'width': rw, 'height': rh,
                             'source': capture['path'], 'sha256': sha256_file(path),
                             'reduced': (rw, rh, reduced)}
            _check(checks, f'{name} supersampled reference is 2x the viewport',
                   (rw, rh) == (frame['width'], frame['height']),
                   [rw, rh], [frame['width'], frame['height']])
    return results


def _cost_lane(viewer, output: Path, checks):
    def sample(enabled):
        _set_aa(viewer, enabled)
        time.sleep(1.2)
        viewer.wait_stable()
        values = []
        for _ in range(7):
            values.append(viewer.performance()['meanFrameMs'])
            time.sleep(0.35)
        values.sort()
        return values

    on = sample(True)
    off = sample(False)
    _set_aa(viewer, True)
    state = _aa_state(viewer)
    _check(checks, 'AA adds no target allocation',
           state['additionalTargetBytes'] == 0, state['additionalTargetBytes'], 0)
    _check(checks, 'AA resolve record time is measured',
           state['resolveMicroseconds'] >= 0, state['resolveMicroseconds'], '>=0')
    return {'onMedianFrameMs': round(on[len(on) // 2], 3),
            'offMedianFrameMs': round(off[len(off) // 2], 3),
            'deltaMs': round(on[len(on) // 2] - off[len(off) // 2], 3),
            'state': state,
            'samples': {'on': on, 'off': off},
            'note': 'Debug frame time includes adaptive refresh/settle work and '
                    'is recorded, not asserted as a release claim.'}


def _lifecycle_lane(viewer, output: Path, checks):
    _set_lighting(viewer, 0)
    _set_aa(viewer, True)
    viewer.set_camera(8.0, 72.5, 1.23)
    before = _aa_state(viewer)
    viewer.resize(900, 700)
    after_resize = _aa_state(viewer)
    _check(checks, 'resize keeps the depth-aware AA path',
           after_resize['mode'] == 1 and after_resize['tapCount'] == 7,
           after_resize, 'mode 1, 7 taps')
    viewer.resize(1200, 860)
    viewer.set_camera(8.0, 72.5, 1.23)
    time.sleep(0.6)

    _set_aa(viewer, False)
    forced = _aa_state(viewer)
    forced_image = _save(viewer, output, 'aa-forced-off')
    _set_aa(viewer, True)
    restored = _aa_state(viewer)
    _check(checks, 'Debug AA-off is explicitly labelled',
           forced['mode'] == 0 and forced['fallbackReason'] == 1, forced,
           'mode 0, reason 1')
    _check(checks, 'AA returns after the Debug off state',
           restored['mode'] == 1 and restored['fallbackReason'] == 0, restored,
           'mode 1, reason 0')

    before_recovery = send_field(viewer.hwnd, 66) & 0xFFFFFFFF
    send_field(viewer.hwnd, 65, 0)
    deadline = time.monotonic() + 90.0
    recovered = False
    while time.monotonic() < deadline:
        time.sleep(0.5)
        state_now = (send_field(viewer.hwnd, 0, 0) & 0xFFFFFFFF) - 1
        current = _aa_state(viewer)
        if state_now == 2 and current['mode'] == 1:
            recovered = True
            break
    after_recovery = send_field(viewer.hwnd, 66) & 0xFFFFFFFF
    reset = _save(viewer, output, 'aa-after-device-reset')
    _check(checks, 'device loss rebuilds the depth SRV and AA path',
           recovered and reset['frame']['stats']['uniqueSampleColours'] >= 8,
           {'recovered': recovered, 'before': before_recovery,
            'after': after_recovery, 'state': _aa_state(viewer)}, 'recovered, drawable')
    return {'before': before, 'afterResize': after_resize, 'forced': forced,
            'restored': restored,
            'deviceReset': {'recovered': recovered,
                            'recoveryCountBefore': before_recovery,
                            'recoveryCountAfter': after_recovery}}


def _write_report(output: Path, report) -> Path:
    lines = [
        '# T33 edge, specular and texture sampling report', '',
        'Generated from the CPU/import policy lanes and same-camera Debug GPU',
        'AA on/off pairs on the generated edge fixture, plus a 2x box-reduced',
        'supersampled reference for validation.', '',
        '## Checks', '',
        f"- Status: **{report.get('status')}**",
        f"- Checks: {sum(1 for c in report.get('checks', []) if c['pass'])} passed / "
        f"{len(report.get('checks', []))} total",
        f"- Failures: {len(report.get('failures', []))}",
    ]
    for failure in report.get('failures', []):
        lines.append(f'  - {failure}')
    lines += ['', '## Same-camera AA pairs', '',
              '| mode | changed | fraction | mean luma delta | wire region |',
              '| --- | --- | --- | --- | --- |']
    for mode, data in report.get('pairs', {}).items():
        metrics = data['metrics']
        lines.append(
            f"| {mode} | {metrics.get('changedPixels')} | "
            f"{metrics.get('changedFraction')} | {metrics.get('meanLumaDelta')} | "
            f"{(data.get('wireDiff') or {}).get('changedPixels')} |")
    lines += ['', '## Supersampled-reference edge error', '',
              '| view | AA-off edge MAE | AA-on edge MAE | edge pixels | improved |',
              '| --- | --- | --- | --- | --- |']
    for name, data in report.get('referenceComparison', {}).items():
        whole = data.get('wholeFrame', {})
        lines.append(f"| {name} | {whole.get('off', {}).get('edgeMae')} | "
                     f"{whole.get('on', {}).get('edgeMae')} | "
                     f"{whole.get('edgePixels')} | {whole.get('improvedFraction')} |")
    lines += ['', '## Captured review images', '']
    for entry in report.get('images', []):
        lines.append(f"- `{entry}`")
    lines += [
        '', '## Interpretation', '',
        '- The default is a single-sample HDR scene plus a depth-aware spatial',
        '  edge filter in the resolve. MSAA was rejected because the opaque',
        '  material pass writes the R32_UINT pick id in the same MRT set and a',
        '  resolved integer id would average instance ids. Bounded 2x SSAA is',
        '  retained only as a deterministic validation reference.',
        '- Picking stays a separate nearest-visible R32_UINT target; AA on/off',
        '  produces identical ids. The D3D11On12/Direct2D overlay is recorded',
        '  after the resolve, so toolbar text is unchanged.',
        '- Linear/mipmap slots use bounded 4x anisotropic filtering; authored',
        '  nearest slots keep the point sampler. MR mips preserve GGX alpha',
        '  variance, color mips reduce in linear light with coverage-weighted',
        '  alpha, and normal mips renormalize.',
        '', '## Evidence', '',
        f"- `{output / 'diagnostics' / 'anti-aliasing.json'}`",
        f"- `{output / 'images'}` AA pairs, orbit, sizes, UI and references",
        '', ]
    path = output / 'anti-aliasing-report.md'
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def run(workspace: Path, output: Path, configuration: str, fast: bool = False):
    output = Path(output)
    report = {'schema': RESULT_SCHEMA, 'task': 'T33', 'checks': [], 'failures': [],
              'status': 'running', 'fast': fast}
    report['serialLanes'] = _run_serial(configuration, output)
    for lane in report['serialLanes']:
        if not lane.get('pass'):
            report['failures'].append(f"serial lane failed: {lane['name']}")

    fixture = output / 'fixtures' / 'edge-probe.glb'
    if not fixture.exists():
        report['failures'].append(f'edge-probe fixture missing: {fixture}')
        report['status'] = 'fail'
        write_json(output / 'diagnostics' / 'anti-aliasing.json', report)
        return report

    images = []
    try:
        with Viewer(configuration, output, width=1200, height=860) as viewer:
            viewer.open(fixture)
            viewer.apply_controlled_presentation()
            _set_evidence_exposure(viewer)
            send_field(viewer.hwnd, 104, 1)   # grid on: glancing procedural lines
            send_field(viewer.hwnd, 196, 1)   # ground highlight
            send_field(viewer.hwnd, 102, 1)   # contact shadow on
            _set_aa(viewer, True)
            report['generation'] = viewer.model_state()['generation']
            report['boundsDimensions'] = viewer.model_state()['dimensions']

            pairs, mode_images = _mode_lane(viewer, output, report['checks'])
            report['pairs'] = pairs
            images.extend(mode_images)

            report['picking'] = _picking_lane(viewer, output, report['checks'])
            images.extend(report['picking'].get('images', []))
            report['coverage'] = _coverage_lane(viewer, output, report['checks'])
            for item in report['coverage'].values():
                images.extend([item.get('on'), item.get('off')])
            report['ui'] = _ui_lane(viewer, output, report['checks'])
            report['motion'] = _motion_lane(viewer, output, report['checks'])
            images.extend([step['on'] for step in report['motion']]
                          + [step['off'] for step in report['motion']])
            report['sizes'] = _size_lane(viewer, output, report['checks'])
            for name, item in report['sizes'].items():
                if name == 'dpi':
                    images.extend(entry['path'] for entry in item.values())
                else:
                    images.append(item['path'])
            report['lifecycle'] = _lifecycle_lane(viewer, output, report['checks'])
            report['cost'] = _cost_lane(viewer, output, report['checks'])

            views = [('studio', 8.0, 72.5, 0), ('clay', 8.0, 72.5, 1),
                     ('directional', 8.0, 72.5, 2)]
            reference = _ssaa_reference(viewer, output, configuration, views,
                                        report['checks'])
            report['supersampledReference'] = {
                key: {k: v for k, v in value.items() if k != 'reduced'}
                if isinstance(value, dict) else value
                for key, value in reference.items()}
            comparisons = {}
            if isinstance(reference, dict) and 'status' not in reference:
                for name, _, _, _ in views:
                    on = pairs[name]['on']
                    off = pairs[name]['off']
                    reduced = reference[name]['reduced']
                    regions = {}
                    for region_name, rect in REGIONS.items():
                        region = _reference_pair_error(off, on, reduced, rect)
                        regions[region_name] = region
                        off_mae = region.get('off', {}).get('edgeMae', 0)
                        on_mae = region.get('on', {}).get('edgeMae', 999)
                        _check(report['checks'],
                               f'{name}/{region_name} AA reduces SSAA edge error',
                               region.get('edgePixels', 0) > 0
                               and on_mae <= 0.998 * off_mae
                               and region.get('improvedFraction', 0) >= 0.08,
                               {'off': region.get('off'), 'on': region.get('on'),
                                'improvedFraction': region.get('improvedFraction'),
                                'edgePixels': region.get('edgePixels')},
                               'AA-on <= 0.998 x AA-off, improved >=8%')
                    pair = _reference_pair_error(off, on, reduced)
                    comparisons[name] = {'regions': regions,
                                         'wholeFrame': pair,
                                         'reference': reference[name]['path']}
                images.extend(reference[name]['path'] for name in reference)
            report['referenceComparison'] = comparisons
            report['images'] = images
    except Exception as error:
        report['failures'].append(f'GPU AA lane failed: {error}')

    if not report.get('pairs'):
        report['failures'].append('no generated AA pairs were captured')
    failed = [check for check in report['checks'] if not check['pass']]
    if failed:
        report['failures'].append(f'{len(failed)} edge/sampling checks failed')
    report['status'] = 'pass' if not report['failures'] else 'fail'
    write_json(output / 'diagnostics' / 'anti-aliasing.json', report)
    report['reportPath'] = str(_write_report(output, report))
    return report
