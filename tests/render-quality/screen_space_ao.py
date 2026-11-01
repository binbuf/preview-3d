"""T32 scale-aware screen-space ambient occlusion verification.

The CPU lane pins the per-mode calibration, scale-aware radius, indirect-only
compositing and the bounded target budget. The GPU lane drives the generated
``ao-probe`` fixture and records, in Studio, Clay and Directional:

* AO-only masks from the resolve diagnostic;
* same-camera on/off pairs and their changed-pixel/luma metrics;
* emission, black-body, glass-window and cutout regions that must not darken;
* background/silhouette pixels that must stay unoccluded;
* perspective and orthographic captures, scene-scale duplicates and an orbit
  sequence for mask stability;
* resize, generation replacement, forced fallback, allocation-failure fallback
  and device-loss recovery;
* the measured end-to-end frame cost and recorded AO pass time.

External Car/Camera/STL pairs are captured in a fresh viewer for review.
"""
from __future__ import annotations

import math
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

from common import RESULT_SCHEMA, encoded_png, send_field, sha256_file, write_json
from viewer import Viewer

ROOT = Path(__file__).resolve().parents[2]
CORPUS = Path('D:/repos/binbuf/preview-3d-test-models')
FAST = bool(os.environ.get('RENDER_QUALITY_FAST'))

SERIAL_LANES = [
    ('unit-screen-space-ao', 'Tests.Unit.exe', ['[screen-space-ao]']),
    ('unit-ground-shadow', 'Tests.Unit.exe', ['[ground-shadow]']),
    ('unit-directional-light', 'Tests.Unit.exe', ['[directional-light]']),
]

EXTERNAL = {
    'car': {
        'entry': 'Car.glb',
        'view': {'yawDegrees': 60.0, 'pitchDegrees': 35.0, 'distanceScale': 1.3},
        'regions': {'paint': [0.30, 0.45, 0.75, 0.92],
                    'front-tire': [0.12, 0.62, 0.38, 0.96],
                    'glass': [0.30, 0.24, 0.72, 0.50]},
    },
    'camera': {
        'entry': 'Camera_01_4k.gltf',
        'view': {'yawDegrees': 0.0, 'pitchDegrees': 25.0, 'distanceScale': 1.15},
        'regions': {'lens': [0.60, 0.36, 0.76, 0.60],
                    'strap': [0.36, 0.62, 0.70, 0.96]},
    },
    'stl': {
        'entry': 'base.stl',
        'view': {'yawDegrees': 25.0, 'pitchDegrees': 60.0, 'distanceScale': 1.0},
        'regions': {'relief': [0.2, 0.15, 0.8, 0.9]},
    },
}


def _f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', float(value)))[0]


def _f32(value):
    return struct.unpack('<f', struct.pack('<I', value & 0xFFFFFFFF))[0]


def _send(hwnd, field, value=0):
    return send_field(hwnd, field, value)


def _query(hwnd, field):
    return send_field(hwnd, field, -1) & 0xFFFFFFFF


def _check(checks, name, passed, actual=None, expected=None, tolerance=None):
    checks.append({'name': name, 'pass': bool(passed), 'actual': actual,
                   'expected': expected, 'tolerance': tolerance})


def _set_lighting(viewer, mode):
    _send(viewer.hwnd, 88, mode)
    time.sleep(0.35)


def _set_ao(viewer, enabled):
    _send(viewer.hwnd, 287, 1 if enabled else 0)
    time.sleep(0.25)


def _set_diag(viewer, value):
    _send(viewer.hwnd, 253, value)
    time.sleep(0.3)


def _set_debug_view(viewer, value):
    _send(viewer.hwnd, 298, value)
    time.sleep(0.25)


def _set_projection(viewer, orthographic):
    _send(viewer.hwnd, 35, 1 if orthographic else 0)
    time.sleep(0.4)


def _wait_ao_state(viewer, expected_fallback, timeout=10.0):
    deadline = time.monotonic() + timeout
    state = _ao_state(viewer)
    while time.monotonic() < deadline:
        state = _ao_state(viewer)
        if state['fallbackReason'] == expected_fallback:
            return state
        time.sleep(0.15)
    return state


def _ao_state(viewer):
    bytes_low = _query(viewer.hwnd, 290)
    bytes_high = _query(viewer.hwnd, 291)
    visible = 1 if _query(viewer.hwnd, 287) else 0
    fallback = _query(viewer.hwnd, 293)
    mode = 1 if (visible and fallback == 0) else (0 if fallback in (0, 7) else 2)
    return {
        'mode': mode,
        'visible': visible,
        'allocationBytes': bytes_low | (bytes_high << 32),
        'passMicroseconds': _query(viewer.hwnd, 292),
        'gpuMicroseconds': _query(viewer.hwnd, 299),
        'fallbackReason': fallback,
        'forced': _query(viewer.hwnd, 294),
        'strength': _f32(_query(viewer.hwnd, 295)),
        'worldRadius': _f32(_query(viewer.hwnd, 296)),
        'sampleCount': _query(viewer.hwnd, 297),
    }


def _save_capture(viewer, output: Path, name: str, stable=True, settle=0.35):
    if settle:
        time.sleep(settle)
    frame = viewer.wait_stable() if stable else viewer.capture()
    path = output / 'images' / f'{name}.png'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
    return {'frame': frame, 'path': str(path), 'sha256': sha256_file(path)}


def _decode(record):
    from common import decode_png
    return decode_png(Path(record['path']).read_bytes())


def _luma(rgb, index):
    return 0.2126 * rgb[index] + 0.7152 * rgb[index + 1] + 0.0722 * rgb[index + 2]


def _analyse_pair(on, off, threshold=2):
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
    background_pixels = 0
    background_changed = 0
    # The display-space dark background is one uniform colour. Use the
    # top-left pixel as its sample and count only near-identical off pixels, so
    # dark model surfaces are never mistaken for background.
    background = (b[0], b[1], b[2])
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
        'meanLumaOn': round(sum_on / count, 3),
        'meanLumaOff': round(sum_off / count, 3),
        'meanLumaDelta': round((sum_on - sum_off) / count, 4),
        'backgroundPixels': background_pixels,
        'backgroundMeanDelta': round(background_changed / max(1, background_pixels), 4),
    }


def _mask_stats(record):
    w, h, rgb = _decode(record)
    count = w * h
    values = [rgb[pixel * 3] for pixel in range(count)]
    occluded = sum(1 for value in values if value < 220)
    deep = sum(1 for value in values if value < 160)
    # Background corners are outside the 3D viewport and must stay 1.0.
    corners = []
    for x0, y0 in ((0.90, 0.05), (0.05, 0.90), (0.95, 0.92)):
        x = min(w - 1, int(x0 * w))
        y = min(h - 1, int(y0 * h))
        corners.append(values[y * w + x])
    return {
        'width': w, 'height': h, 'pixels': count,
        'minimum': min(values), 'maximum': max(values),
        'mean': round(sum(values) / count, 2),
        'occludedPixels': occluded,
        'occludedFraction': round(occluded / count, 5),
        'deepOccludedFraction': round(deep / count, 5),
        'cornerValues': corners,
    }


def _mask_overlap(first, second, threshold=220):
    w, h, a = _decode(first)
    w2, h2, b = _decode(second)
    if (w, h) != (w2, h2):
        return 0.0
    intersection = union = 0
    for pixel in range(w * h):
        av = a[pixel * 3] < threshold
        bv = b[pixel * 3] < threshold
        if av or bv:
            union += 1
            if av and bv:
                intersection += 1
    return 0.0 if union == 0 else intersection / union


def _region_delta(on, off, rect):
    w, h, a = _decode(on)
    w2, h2, b = _decode(off)
    if (w, h) != (w2, h2):
        return None
    x0, y0, x1, y1 = rect
    left = max(0, int(x0 * w)); right = min(w, int(x1 * w))
    top = max(0, int(y0 * h)); bottom = min(h, int(y1 * h))
    total = 0.0
    maximum = 0
    count = 0
    for y in range(top, bottom):
        for x in range(left, right):
            base = (y * w + x) * 3
            delta = max(abs(a[base] - b[base]), abs(a[base + 1] - b[base + 1]),
                        abs(a[base + 2] - b[base + 2]))
            total += delta
            maximum = max(maximum, delta)
            count += 1
    return {'meanDelta': round(total / max(1, count), 4), 'maxDelta': maximum,
            'pixels': count}


def _emissive_delta(on, off):
    """Mean on/off delta over pixels whose base colour is warm/emissive."""
    w, h, a = _decode(on)
    w2, h2, b = _decode(off)
    if (w, h) != (w2, h2):
        return None
    total = 0.0
    count = 0
    for pixel in range(w * h):
        base = pixel * 3
        red, green, blue = a[base], a[base + 1], a[base + 2]
        if red > 60 and red > blue + 25 and green > blue:
            delta = max(abs(red - b[base]), abs(green - b[base + 1]),
                        abs(blue - b[base + 2]))
            total += delta
            count += 1
    return {'pixels': count, 'meanDelta': round(total / max(1, count), 4)}


def _dark_region_delta(on, off):
    w, h, a = _decode(on)
    w2, h2, b = _decode(off)
    if (w, h) != (w2, h2):
        return None
    total = 0.0
    count = 0
    for pixel in range(w * h):
        base = pixel * 3
        if _luma(a, base) < 24.0 and _luma(b, base) < 24.0:
            delta = max(abs(a[base] - b[base]), abs(a[base + 1] - b[base + 1]),
                        abs(a[base + 2] - b[base + 2]))
            total += delta
            count += 1
    return {'pixels': count, 'meanDelta': round(total / max(1, count), 4)}


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


def _mode_lane(viewer, output: Path, fixture: Path, checks):
    _set_ao(viewer, True)
    _set_lighting(viewer, 0)
    viewer.set_camera(10.0, 22.0, 1.0)
    results = {}
    images = []
    for mode_name, mode in (('studio', 0), ('clay', 1), ('directional', 2)):
        _set_lighting(viewer, mode)
        if mode == 2:
            _send(viewer.hwnd, 277, _f32_bits(0.86))
            _send(viewer.hwnd, 278, _f32_bits(0.78))
            time.sleep(0.25)
        _set_ao(viewer, True)
        _set_diag(viewer, 5)
        mask = _save_capture(viewer, output, f'ao-mask-{mode_name}', stable=False)
        mask_stats = _mask_stats(mask)
        _set_diag(viewer, 0)
        _set_ao(viewer, True)
        on = _save_capture(viewer, output, f'ao-{mode_name}-on')
        state = _ao_state(viewer)
        _set_ao(viewer, False)
        off = _save_capture(viewer, output, f'ao-{mode_name}-off')
        _set_ao(viewer, True)
        metrics = _analyse_pair(on, off)
        emissive = _emissive_delta(on, off)
        dark = _dark_region_delta(on, off)
        transparent = _region_delta(on, off, [0.60, 0.10, 0.92, 0.45])
        results[mode_name] = {
            'mask': {'path': mask['path'], 'sha256': mask['sha256'], **mask_stats},
            'on': {'path': on['path'], 'sha256': on['sha256'],
                   'stats': on['frame']['stats']},
            'off': {'path': off['path'], 'sha256': off['sha256']},
            'metrics': metrics, 'state': state,
            'emissive': emissive, 'dark': dark, 'transparent': transparent,
        }
        images.extend([mask['path'], on['path'], off['path']])

        _check(checks, f'{mode_name} AO is active', state['mode'] == 1,
               state['mode'], 1)
        _check(checks, f'{mode_name} uses the bounded kernel',
               state['sampleCount'] == 12, state['sampleCount'], 12)
        _check(checks, f'{mode_name} has bounded targets',
               0 < state['allocationBytes'] < 32 * 1024 * 1024,
               state['allocationBytes'], '(0,32MB)')
        _check(checks, f'{mode_name} has a scene-scale radius',
               state['worldRadius'] > 0.0, state['worldRadius'], '>0')
        _check(checks, f'{mode_name} cavity/contact darkening is visible',
               metrics.get('changedPixels', 0) >= 0.0015 * metrics.get('pixels', 1),
               metrics.get('changedPixels'), '>=0.15% of viewport')
        _check(checks, f'{mode_name} darkening is restrained',
               metrics.get('changedFraction', 1.0) <= 0.20,
               metrics.get('changedFraction'), '<=0.20')
        _check(checks, f'{mode_name} is not a global exposure change',
               abs(metrics.get('meanLumaDelta', 99.0)) <= 2.0,
               metrics.get('meanLumaDelta'), '|delta| <= 2')
        _check(checks, f'{mode_name} AO mask has cavity pixels',
               mask_stats['occludedFraction'] >= 0.003,
               mask_stats['occludedFraction'], '>=0.3%')
        _check(checks, f'{mode_name} AO mask preserves background',
               min(mask_stats['cornerValues']) >= 250,
               mask_stats['cornerValues'], 'corners >= 250')
        if mode_name != 'clay':
            # Clay is an effective opaque geometry-review material and
            # deliberately suppresses authored emission (T29); the emission
            # invariant applies to the material-aware modes.
            _check(checks, f'{mode_name} emission is not darkened',
                   (emissive or {}).get('pixels', 0) > 20
                   and (emissive or {}).get('meanDelta', 99) <= 2.0,
                   emissive, 'pixels>20, delta<=2')
        _check(checks, f'{mode_name} black body is not globally darkened',
               (dark or {}).get('meanDelta', 0) <= 2.0,
               dark, 'delta<=2')
        _check(checks, f'{mode_name} transparent window/cutout has no AO sheet',
               (transparent or {}).get('meanDelta', 99) <= 1.5,
               transparent, 'mean delta<=1.5')
        _check(checks, f'{mode_name} background is halo-free',
               metrics.get('backgroundPixels', 0) > 5000
               and metrics.get('backgroundMeanDelta', 99) <= 0.5,
               {'pixels': metrics.get('backgroundPixels'),
                'meanDelta': metrics.get('backgroundMeanDelta')}, '>5000, <=0.5')

    # Projection independence: the same fixture must produce a mask in both
    # perspective and orthographic frames (blended projection is not a test
    # target; the existing camera toggle animates through it).
    _set_lighting(viewer, 0)
    projection = {}
    for name, orthographic in (('perspective', False), ('orthographic', True)):
        _set_projection(viewer, orthographic)
        _set_ao(viewer, True)
        _set_diag(viewer, 5)
        mask = _save_capture(viewer, output, f'ao-{name}-mask', stable=False)
        _set_diag(viewer, 0)
        stats = _mask_stats(mask)
        projection[name] = {'state': _ao_state(viewer), 'mask': stats,
                            'path': mask['path']}
        _check(checks, f'AO is active in {name}',
               projection[name]['state']['mode'] == 1
               and stats['occludedFraction'] >= 0.002,
               {'mode': projection[name]['state']['mode'],
                'occluded': stats['occludedFraction']}, 'mode 1, occluded>=0.2%')
    _set_projection(viewer, False)
    images.extend([projection['perspective']['path'], projection['orthographic']['path']])

    return results, images, projection


def _scale_lane(viewer, output: Path, fixture: Path, normal_mask, checks):
    # Zoom: the same scene at half and double distance keeps a mask (the
    # screen-space radius cap bounds work at close zoom).
    zoom = {}
    for name, distance in (('near', 0.55), ('far', 1.8)):
        viewer.set_camera(10.0, 22.0, distance)
        time.sleep(0.4)
        _set_diag(viewer, 5)
        mask = _save_capture(viewer, output, f'ao-zoom-{name}-mask', stable=False)
        _set_diag(viewer, 0)
        zoom[name] = _mask_stats(mask)
        _check(checks, f'AO survives {name} zoom',
               zoom[name]['occludedFraction'] > 0.0,
               zoom[name]['occludedFraction'], '>0')
    if zoom['near']['occludedFraction'] > 0 and zoom['far']['occludedFraction'] > 0:
        ratio = max(zoom['near']['occludedFraction'], zoom['far']['occludedFraction']) / min(
            zoom['near']['occludedFraction'], zoom['far']['occludedFraction'])
        _check(checks, 'AO radius is bounded across zoom', ratio <= 8.0,
               round(ratio, 2), '<=8')

    # Scene-scale duplicate: the same assembly at 8x must not lose the mask or
    # turn into a whole-frame darkening. The bounds-derived radius makes the
    # pattern comparable rather than model-unit-fixed.
    large = Path(output / 'fixtures' / 'ao-probe-large.glb')
    if large.exists():
        viewer.open(large)
        viewer.apply_controlled_presentation()
        _set_ao(viewer, True)
        _send(viewer.hwnd, 104, 0)
        _send(viewer.hwnd, 196, 0)
        _send(viewer.hwnd, 102, 0)
        viewer.set_camera(10.0, 22.0, 1.0)
        time.sleep(0.8)
        _set_diag(viewer, 5)
        mask = _save_capture(viewer, output, 'ao-large-mask', stable=False)
        _set_diag(viewer, 0)
        large_stats = _mask_stats(mask)
        ratio = (large_stats['occludedFraction']
                 / max(normal_mask['occludedFraction'], 1e-6))
        _check(checks, 'scene-scale duplicate keeps an AO mask',
               large_stats['occludedFraction'] > 0.0,
               large_stats['occludedFraction'], '>0')
        _check(checks, 'AO radius follows scene scale',
               0.2 <= ratio <= 5.0, round(ratio, 3), '[0.2,5]')
        # Restore the standard fixture for the remaining checks.
        viewer.open(fixture)
        viewer.apply_controlled_presentation()
        _set_ao(viewer, True)
        _send(viewer.hwnd, 104, 0)
        _send(viewer.hwnd, 196, 0)
        _send(viewer.hwnd, 102, 0)
        viewer.set_camera(10.0, 22.0, 1.0)
        time.sleep(0.6)
    else:
        _check(checks, 'scene-scale fixture available', False, str(large), 'exists')
    return zoom


def _motion_lane(viewer, output: Path, checks):
    masks = []
    for yaw in (8.0, 12.0, 16.0):
        viewer.set_camera(yaw, 22.0, 1.0)
        time.sleep(0.4)
        _set_diag(viewer, 5)
        masks.append(_save_capture(viewer, output, f'ao-orbit-{int(yaw):02d}', stable=False))
        _set_diag(viewer, 0)
    overlaps = [_mask_overlap(masks[index], masks[index + 1]) for index in range(len(masks) - 1)]
    fractions = [_mask_stats(mask)['occludedFraction'] for mask in masks]
    _check(checks, 'AO mask is stable under small orbit motion',
           all(value >= 0.25 for value in overlaps),
           [round(value, 3) for value in overlaps], '>=0.25')
    if fractions:
        ratio = max(fractions) / max(min(fractions), 1e-6)
        _check(checks, 'orbit does not jump the AO coverage', ratio <= 2.0,
               round(ratio, 3), '<=2')
    return {'masks': [mask['path'] for mask in masks],
            'overlaps': [round(value, 3) for value in overlaps],
            'occludedFractions': fractions}


def _lifecycle_lane(viewer, output: Path, fixture: Path, checks):
    _set_lighting(viewer, 0)
    _set_ao(viewer, True)
    viewer.set_camera(10.0, 22.0, 1.0)
    time.sleep(0.5)
    before = _ao_state(viewer)

    viewer.resize(900, 700)
    after_resize = _ao_state(viewer)
    _check(checks, 'resize recreates viewport-sized AO targets',
           after_resize['mode'] == 1 and after_resize['allocationBytes'] > 0,
           after_resize, 'mode 1, bytes>0')
    _check(checks, 'resize changes the AO allocation with the extent',
           after_resize['allocationBytes'] != before['allocationBytes'],
           [before['allocationBytes'], after_resize['allocationBytes']],
           'different extent, different bytes')
    viewer.resize(1200, 860)

    # Superseded generation: only the published model may produce the mask.
    stl = output / 'fixtures' / 'stl-box.stl'
    if stl.exists():
        first = viewer.request_open(fixture)
        second = viewer.request_open(stl)
        final_generation, _, _ = viewer.wait_open(second)
        state = _ao_state(viewer)
        _check(checks, 'superseded import publishes AO for the final model',
               final_generation == second and state['mode'] == 1,
               {'first': first, 'second': second, 'final': final_generation,
                'state': state}, 'final generation, mode 1')
        viewer.open(fixture)
        viewer.apply_controlled_presentation()
        _set_ao(viewer, True)
        _send(viewer.hwnd, 104, 0)
        _send(viewer.hwnd, 196, 0)
        _send(viewer.hwnd, 102, 0)
        viewer.set_camera(10.0, 22.0, 1.0)
        time.sleep(0.6)
    else:
        _check(checks, 'STL fixture available for supersession', False, str(stl), 'exists')

    # Forced AO-off fallback must be recorded and must not alter direct light,
    # emission or the display transform. Re-seat the camera first: the resize
    # above can still be easing, and a sub-pixel camera drift would masquerade
    # as an AO delta.
    viewer.set_camera(10.0, 22.0, 1.0)
    time.sleep(1.2)
    viewer.wait_stable()
    _set_ao(viewer, True)
    _send(viewer.hwnd, 294, 1)
    forced = _wait_ao_state(viewer, 2)
    time.sleep(0.4)
    forced_image = _save_capture(viewer, output, 'ao-forced-on')
    # The forced fallback must match the ordinary AO-disabled path, not the
    # AO-applied image. Compare against a real toggle-off rather than the
    # restored state.
    _set_ao(viewer, False)
    time.sleep(0.35)
    disabled_image = _save_capture(viewer, output, 'ao-disabled-reference')
    metrics = _analyse_pair(forced_image, disabled_image, threshold=1)
    _check(checks, 'forced AO-off fallback is recorded',
           forced['mode'] == 2 and forced['fallbackReason'] == 2, forced,
           'mode 2, reason 2')
    _check(checks, 'forced AO-off fallback matches the AO-disabled image',
           metrics.get('changedPixels', 1) == 0, metrics, 'changed 0')
    _set_ao(viewer, True)
    _send(viewer.hwnd, 294, 0)
    _wait_ao_state(viewer, 0)
    time.sleep(0.35)
    restored = _ao_state(viewer)
    _check(checks, 'AO returns after the forced fallback', restored['mode'] == 1,
           restored, 'mode 1')

    # Device loss rebuilds the viewport-sized AO resources.
    viewer.wait_stable()
    before_recovery = _send(viewer.hwnd, 66) & 0xFFFFFFFF
    _send(viewer.hwnd, 65, 0)
    deadline = time.monotonic() + 90.0
    recovered = False
    while time.monotonic() < deadline:
        time.sleep(0.5)
        state_now = (_send(viewer.hwnd, 0) & 0xFFFFFFFF) - 1
        current = _ao_state(viewer)
        if state_now == 2 and current['mode'] == 1 and current['allocationBytes'] > 0:
            recovered = True
            break
    after_recovery = _send(viewer.hwnd, 66) & 0xFFFFFFFF
    reset = _save_capture(viewer, output, 'ao-after-device-reset')
    _check(checks, 'device loss rebuilds the AO targets',
           recovered and reset['frame']['stats']['uniqueSampleColours'] >= 8,
           {'recovered': recovered, 'before': before_recovery,
            'after': after_recovery, 'state': _ao_state(viewer)}, 'recovered, drawable')
    return {'forced': forced, 'restored': restored,
            'deviceReset': {'recovered': recovered,
                            'recoveryCountBefore': before_recovery,
                            'recoveryCountAfter': after_recovery}}


def _cost_lane(viewer, output: Path, checks):
    # End-to-end frame time with AO on versus off (5 samples each) plus the
    # recorded pass time. This is workstation evidence, not a release claim.
    def sample(enabled):
        _set_ao(viewer, enabled)
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
    _set_ao(viewer, True)
    # Use the median: the Debug viewer's first frames after a state change are
    # dominated by settle work and would otherwise invert the delta.
    on_median = on[len(on) // 2]
    off_median = off[len(off) // 2]
    state = _ao_state(viewer)
    _check(checks, 'AO GPU pass time is recorded', state['gpuMicroseconds'] > 0,
           state['gpuMicroseconds'], '>0 us')
    _check(checks, 'AO CPU record time is recorded', state['passMicroseconds'] >= 0,
           state['passMicroseconds'], '>=0')
    return {'onMedianFrameMs': round(on_median, 3),
            'offMedianFrameMs': round(off_median, 3),
            'deltaMs': round(on_median - off_median, 3),
            'passMicroseconds': state['passMicroseconds'],
            'gpuMicroseconds': state['gpuMicroseconds'],
            'samples': {'on': on, 'off': off},
            'frameTimeNote': 'Debug viewer frame time is dominated by adaptive '
                             'refresh/settle work and is recorded, not asserted; '
                             'the bounded AO pass GPU time is field 299.'}


def _external_survey(viewer, output: Path, checks):
    from corpus import resolve_entry
    viewer.settle_timeout = max(viewer.settle_timeout, 240.0)
    surveys = {}
    for name, spec in EXTERNAL.items():
        entry, _, _ = resolve_entry(CORPUS, spec['entry'])
        if entry is None or not entry.is_file():
            surveys[name] = {'status': 'unavailable', 'entry': str(CORPUS / spec['entry'])}
            _check(checks, f'{name} external observation recorded', False,
                   'unavailable', 'sampled')
            continue
        try:
            viewer.open(entry)
        except Exception as error:
            surveys[name] = {'status': 'failed', 'reason': str(error)[:300]}
            _check(checks, f'{name} external observation recorded', False,
                   surveys[name]['status'], 'sampled')
            continue
        viewer.apply_controlled_presentation()
        _send(viewer.hwnd, 104, 0)
        _send(viewer.hwnd, 196, 0)
        _send(viewer.hwnd, 102, 0)
        viewer.set_camera(spec['view']['yawDegrees'], spec['view']['pitchDegrees'],
                          spec['view']['distanceScale'])
        time.sleep(0.6)
        record = {'status': 'sampled', 'entry': str(entry), 'modes': {}}
        for mode_name, mode in (('studio', 0), ('clay', 1), ('directional', 2)):
            _set_lighting(viewer, mode)
            if mode == 2:
                _send(viewer.hwnd, 277, _f32_bits(0.86))
                _send(viewer.hwnd, 278, _f32_bits(0.78))
                time.sleep(0.25)
            _set_ao(viewer, True)
            _set_diag(viewer, 5)
            mask = _save_capture(viewer, output, f'external-{name}-{mode_name}-mask',
                                 stable=False)
            _set_diag(viewer, 0)
            on = _save_capture(viewer, output, f'external-{name}-{mode_name}-on')
            _set_ao(viewer, False)
            off = _save_capture(viewer, output, f'external-{name}-{mode_name}-off')
            _set_ao(viewer, True)
            metrics = _analyse_pair(on, off)
            state = _ao_state(viewer)
            record['modes'][mode_name] = {
                'on': {'path': on['path'], 'sha256': on['sha256'],
                       'stats': on['frame']['stats']},
                'off': {'path': off['path'], 'sha256': off['sha256']},
                'mask': {'path': mask['path'], **_mask_stats(mask)},
                'metrics': metrics, 'state': state,
            }
            _check(checks, f'{name}/{mode_name} AO pair captured',
                   metrics.get('changedPixels', 0) > 0,
                   metrics.get('changedPixels'), '>0')
            _check(checks, f'{name}/{mode_name} background halo-free',
                   metrics.get('backgroundMeanDelta', 99) <= 0.5,
                   metrics.get('backgroundMeanDelta'), '<=0.5')
        surveys[name] = record
    return surveys


def _write_report(output: Path, report) -> Path:
    lines = [
        '# T32 scale-aware screen-space ambient occlusion report', '',
        'Generated from the CPU calibration/budget lane and Debug GPU same-camera',
        'AO on/off pairs, AO-only masks and lifecycle checks in Studio, Clay and',
        'Directional.', '',
        '## Checks', '',
        f"- Status: **{report.get('status')}**",
        f"- Checks: {sum(1 for c in report.get('checks', []) if c['pass'])} passed / "
        f"{len(report.get('checks', []))} total",
        f"- Failures: {len(report.get('failures', []))}",
    ]
    for failure in report.get('failures', []):
        lines.append(f'  - {failure}')
    lines += ['', '## Generated AO pairs', '',
              '| mode | AO target | changed | fraction | mean luma delta | mask occluded |',
              '| --- | --- | --- | --- | --- | --- |']
    for mode, data in report.get('pairs', {}).items():
        metrics = data['metrics']
        lines.append(
            f"| {mode} | mode {data['state']['mode']} | {metrics.get('changedPixels')} | "
            f"{metrics.get('changedFraction')} | {metrics.get('meanLumaDelta')} | "
            f"{data['mask']['occludedFraction']} |")
    lines += ['', '## Captured review images', '']
    for entry in report.get('images', []):
        lines.append(f"- `{entry}`")
    lines += [
        '', '## Interpretation', '',
        '- AO is reconstructed from the existing scene depth with the inverse',
        '  view-projection; the same shader handles perspective, orthographic and',
        '  the blended projection. The world radius is a fraction of the fitted',
        '  scene radius converted to a bounded pixel radius from the reconstructed',
        '  frame, so the mask is not fixed to one model size.',
        '- The opaque material pass writes its environment/IBL share to a second',
        '  HDR target. The resolve subtracts only that share before tone mapping,',
        '  so direct light, emission, transparent reflection and the display',
        '  transform are never multiplied by an AO mask.',
        '- Transparent and MASK surfaces do not write the opaque depth used by the',
        '  AO passes: a window or a cutout hole cannot become an opaque AO sheet.',
        '- The mask is a fixed 12-sample spiral plus an edge-aware 5x5 bilateral',
        '  filter. There is no temporal history or reprojection.',
        '', '## Evidence', '',
        f"- `{output / 'diagnostics' / 'screen-space-ao.json'}`",
        f"- `{output / 'images'}` AO-on/off pairs and AO-only masks",
        '', ]
    path = output / 'screen-space-ao-report.md'
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def _fallback_launch(configuration: str, output: Path, fixture: Path):
    """A separate process with the Debug allocation-failure switch set."""
    report = {'status': 'skipped', 'reason': 'fast iteration lane'}
    if FAST:
        return report
    driver = output / 'ao-fallback-driver.py'
    driver.write_text(
        'import os, sys, time\n'
        f'sys.path.insert(0, {str(ROOT / "tests" / "render-quality")!r})\n'
        'from common import send_field\n'
        'from viewer import Viewer\n'
        'with Viewer("' + configuration + '", r"' + str(output) + '", width=900, height=700) as v:\n'
        '    v.open(r"' + str(fixture) + '")\n'
        '    v.apply_controlled_presentation()\n'
        '    send_field(v.hwnd, 104, 0)\n'
        '    send_field(v.hwnd, 196, 0)\n'
        '    send_field(v.hwnd, 102, 0)\n'
        '    send_field(v.hwnd, 287, 1)\n'
        '    time.sleep(0.6)\n'
        '    low = send_field(v.hwnd, 290, -1) & 0xFFFFFFFF\n'
        '    high = send_field(v.hwnd, 291, -1) & 0xFFFFFFFF\n'
        '    print("visible", send_field(v.hwnd, 287, -1) & 0xFFFFFFFF,\n'
        '          "fallback", send_field(v.hwnd, 293, -1) & 0xFFFFFFFF,\n'
        '          "bytes", low | (high << 32))\n',
        encoding='utf-8')
    environment = dict(os.environ)
    environment['PREVIEW3D_TEST_AO_ALLOCATION_FAILURE'] = '1'
    completed = subprocess.run(
        [sys.executable, str(driver)], cwd=ROOT, capture_output=True, text=True,
        timeout=300, env=environment)
    text = (completed.stdout or '') + (completed.stderr or '')
    report = {'status': 'sampled' if completed.returncode == 0 else 'failed',
              'exitCode': completed.returncode, 'output': text[-2000:]}
    if completed.returncode != 0:
        report['reason'] = 'allocation-failure launch failed'
    return report


def run(workspace: Path, output: Path, configuration: str, fast: bool = False):
    output = Path(output)
    report = {'schema': RESULT_SCHEMA, 'task': 'T32', 'checks': [], 'failures': [],
              'status': 'running', 'fast': fast}
    report['serialLanes'] = _run_serial(configuration, output)
    for lane in report['serialLanes']:
        if not lane.get('pass'):
            report['failures'].append(f"serial lane failed: {lane['name']}")

    fixture = output / 'fixtures' / 'ao-probe.glb'
    if not fixture.exists():
        report['failures'].append(f'ao-probe fixture missing: {fixture}')
        report['status'] = 'fail'
        write_json(output / 'diagnostics' / 'screen-space-ao.json', report)
        return report

    images = []
    try:
        with Viewer(configuration, output, width=1200, height=860) as viewer:
            viewer.open(fixture)
            viewer.apply_controlled_presentation()
            _send(viewer.hwnd, 104, 0)  # grid off
            _send(viewer.hwnd, 196, 0)  # highlight off
            _send(viewer.hwnd, 102, 0)  # contact shadow off: isolate AO
            _set_ao(viewer, True)
            report['generation'] = viewer.model_state()['generation']
            report['boundsDimensions'] = viewer.model_state()['dimensions']

            pairs, mode_images, projection = _mode_lane(viewer, output, fixture, report['checks'])
            report['pairs'] = pairs
            images.extend(mode_images)
            report['projection'] = projection

            normal_mask = pairs['studio']['mask']
            report['zoom'] = _scale_lane(viewer, output, fixture, normal_mask, report['checks'])
            report['motion'] = _motion_lane(viewer, output, report['checks'])
            report['lifecycle'] = _lifecycle_lane(viewer, output, fixture, report['checks'])
            report['cost'] = _cost_lane(viewer, output, report['checks'])
            report['images'] = images
    except Exception as error:
        report['failures'].append(f'GPU AO lane failed: {error}')

    if not fast:
        try:
            with Viewer(configuration, output, width=1200, height=860) as external:
                report['externalSurvey'] = _external_survey(external, output, report['checks'])
        except Exception as error:
            report['externalSurvey'] = {'status': 'failed', 'reason': str(error)}
            _check(report['checks'], 'external AO survey completed', False,
                   str(error), 'sampled')
    else:
        report['externalSurvey'] = {}
        report['externalNote'] = 'skipped by the fast iteration lane'

    report['fallbackLaunch'] = _fallback_launch(configuration, output, fixture)
    if report['fallbackLaunch'].get('status') == 'sampled':
        _check(report['checks'], 'allocation-failure launch records AO fallback',
               'fallback 1' in report['fallbackLaunch'].get('output', ''),
               report['fallbackLaunch'].get('output', '')[-200:],
               'fallback 1')
    elif report['fallbackLaunch'].get('status') == 'failed':
        _check(report['checks'], 'allocation-failure launch completed', False,
               report['fallbackLaunch'], 'sampled')

    if not report.get('pairs'):
        report['failures'].append('no generated AO pairs were captured')
    failed = [check for check in report['checks'] if not check['pass']]
    if failed:
        report['failures'].append(f'{len(failed)} screen-space AO checks failed')
    report['status'] = 'pass' if not report['failures'] else 'fail'
    write_json(output / 'diagnostics' / 'screen-space-ao.json', report)
    report['reportPath'] = str(_write_report(output, report))
    return report
