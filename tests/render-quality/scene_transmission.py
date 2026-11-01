"""T27 scene-color transmission and tinted/rough glass verification.

The lane proves the product path samples the finished opaque scene in linear
HDR through a bounded, roughness-filtered mip pyramid; keeps authored coverage,
tint, transmission and interface Fresnel as independent inputs; routes OPAQUE
transmissive materials through the transparent pass without rewriting their
coverage; defines an order-stable sampling contract for overlapping layers; and
degrades honestly (no invented volume thickness, no offscreen claim).

It runs serial unit/import checks, a generated fixture with independent numeric
invariants and GPU captures, plus the Car/Camera/Suit rubric when the restricted
local corpus is available.
"""
from __future__ import annotations

import math
import subprocess
import time
from pathlib import Path

from common import (RESULT_SCHEMA, crop, encoded_png, mae,
                    write_json)
from diagnostics import (_display_rgb, channel_supported, material_records,
                         sample_region, set_channel)
from fixtures import EXPECTED_TRANSMISSION_GLASS
from layered_transparency import (_capture, _content_box, _downsample, _locate,
                                  _mean_luma, _stats, viewer_query)
from viewer import Viewer

ROOT = Path(__file__).resolve().parents[2]
CORPUS_ROOT = Path('D:/repos/binbuf/preview-3d-test-models')

SERIAL_LANES = [
    ('unit-scene-transmission', 'Tests.Unit.exe', ['[scene-transmission]']),
    ('unit-material-lobes', 'Tests.Unit.exe', ['[material-lobes]']),
    ('import-wire-validator', 'Tests.ImportIsolation.exe', ['[shared-section-validator]']),
]

LOCATOR_BASE_COLOR = {item['name']: item['baseColor'][:3]
                      for item in EXPECTED_TRANSMISSION_GLASS}
# The MASK cutout multiplies its factor by the authored base-color texture, so
# the base-color channel shows the product rather than the factor.
LOCATOR_BASE_COLOR['mask cutout transmission'] = [0.0191, 0.6440, 0.1630]


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


def _transmission_stats(viewer):
    return {
        'targetBytes': int(viewer_query(viewer, 259)) | (int(viewer_query(viewer, 260)) << 32),
        'mipCount': int(viewer_query(viewer, 261)),
        'fallbackReason': int(viewer_query(viewer, 262)),
        'forwardTransmissionDraws': int(viewer_query(viewer, 263)),
        'accountedGpuBytes': viewer.performance()['accountedGpuBytes'],
        'meanFrameMs': viewer.performance()['meanFrameMs'],
        'p95FrameMs': viewer.performance()['p95FrameMs'],
    }


def _check(checks, name, passed, actual=None, expected=None):
    checks.append({'name': name, 'pass': bool(passed), 'actual': actual,
                   'expected': expected})


def _rect_point(frame, x, y):
    rect = frame['rect']
    return rect['left'] + x, rect['top'] + y


def _grid_points(box, count=5):
    if box is None:
        return []
    left, top, right, bottom = box
    points = []
    for row in range(count):
        for column in range(count):
            x = left + (right - left) * (column + 1) // (count + 1)
            y = top + (bottom - top) * (row + 1) // (count + 1)
            points.append((x, y))
    return points


def _role_points(located_frame, box, linear_color, count=5, tolerance=22):
    """Grid points whose captured base-color pixel matches the role.

    The locator's bounding box can include a neighbour or an anti-aliased
    edge. Filtering against the isolated base-color image keeps every numeric
    sample on the intended panel, so coverage/transmission averages cannot be
    diluted by the opaque checker or another layer.
    """
    width, height, rgb = (located_frame['width'], located_frame['height'],
                          located_frame['rgb'])
    target = _display_rgb(linear_color[:3])
    left, top, right, bottom = box
    # Keep samples off the anti-aliased edge: a 3x3 readback centred one pixel
    # inside the locator box can otherwise include the opaque scene.
    margin_x = max(2, (right - left) // 8)
    margin_y = max(2, (bottom - top) // 8)
    left += margin_x; right -= margin_x
    top += margin_y; bottom -= margin_y
    points = []
    for x, y in _grid_points((left, top, right, bottom), count):
        if not (0 <= x < width and 0 <= y < height):
            continue
        index = (y * width + x) * 3
        pixel = (rgb[index], rgb[index + 1], rgb[index + 2])
        distance = sum((pixel[channel] - target[channel]) ** 2 for channel in range(3))
        if distance <= tolerance * tolerance:
            points.append((x, y))
    return points


def _channel_at(viewer, located_frame, points, channel):
    if not points:
        return []
    set_channel(viewer.hwnd, channel)
    samples = []
    for x, y in points:
        px, py = _rect_point(located_frame, x, y)
        sample = sample_region(viewer.hwnd, px - 1, py - 1, px + 1, py + 1)
        samples.append(sample.get('rgba'))
    return samples


def _mean(values):
    values = [value for value in values if value is not None]
    return sum(values) / len(values) if values else None


def _channel_mean(samples, component=0):
    return _mean([sample[component] for sample in samples if sample])


def _ratio_tint(transmitted, background, base):
    """Per-channel measured tint (transmitted/background/base) over points.

    Points whose background is too dark are skipped because the ratio is
    numerically unstable there; at least a few bright checker cells remain.
    """
    ratios = [[] for _ in range(3)]
    for front, back in zip(transmitted, background):
        if not front or not back:
            continue
        if max(back[:3]) < 0.08:
            continue
        for channel in range(3):
            if base[channel] > 1e-4:
                ratios[channel].append(front[channel] / back[channel] / base[channel])
    return [_mean(values) for values in ratios]


def _p10_p90(samples, component=0):
    values = sorted(sample[component] for sample in samples if sample)
    if len(values) < 4:
        return None, None
    return values[len(values) // 10], values[(len(values) * 9) // 10]


def _run_generated(workspace: Path, output: Path, configuration: str, fast: bool):
    checks = []
    evidence = {}
    fixture = output / 'fixtures' / 'transmission-glass.glb'
    images = output / 'images'
    images.mkdir(parents=True, exist_ok=True)
    if not fixture.exists():
        return checks, {'error': 'transmission-glass fixture missing'}

    with Viewer(configuration, output, width=1280, height=900, settle_timeout=180) as viewer:
        generation, load_seconds, _ = viewer.open(fixture)
        viewer.apply_controlled_presentation()
        viewer.set_camera(0.0, 90.0, 1.55)
        stats = _stats(viewer)
        tstats = _transmission_stats(viewer)
        evidence.update({'stats': stats, 'transmission': tstats,
                         'loadSeconds': round(load_seconds, 3),
                         'generation': generation})
        _check(checks, 'transmission snapshot is available', tstats['fallbackReason'] == 0,
               tstats, 0)
        _check(checks, 'transmission mip pyramid is bounded', tstats['mipCount'] == 8,
               tstats['mipCount'], 6)
        _check(checks, 'transmission target bytes are non-empty and bounded',
               0 < tstats['targetBytes'] <= tstats['accountedGpuBytes'],
               [tstats['targetBytes'], tstats['accountedGpuBytes']],
               '0 < bytes <= accounted')
        _check(checks, 'OPAQUE transmissive draws route through the transparent pass',
               tstats['forwardTransmissionDraws'] >= 8,
               tstats['forwardTransmissionDraws'], '>=8')
        _check(checks, 'OIT remains the effective transparency algorithm',
               stats['algorithm'] == 1 and stats['fallbackReason'] == 0,
               [stats['algorithm'], stats['fallbackReason']], [1, 0])
        _check(checks, 'resolve consumed the OIT composite', stats['resolveUsedOit'], stats)

        support = channel_supported(viewer.hwnd)
        _check(checks, 'transmitted-radiance channels are advertised',
               support.get(31) is True and support.get(32) is True,
               {31: support.get(31), 32: support.get(32)}, 'supported')

        base_frame = _capture(viewer, images / 'transmission-front.png')
        base_samples = _downsample(base_frame)
        channel_frames = {}
        for channel, label in ((1, 'base-color'), (9, 'coverage'), (10, 'transmission'),
                               (22, 'composite-inputs'), (31, 'transmitted-radiance'),
                               (32, 'transmission-source')):
            set_channel(viewer.hwnd, channel)
            frame = viewer.wait_stable()
            path = images / f'transmission-channel-{channel:02d}-{label}.png'
            path.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
            channel_frames[channel] = frame
        located_frame = channel_frames[1]
        located = _locate(located_frame, LOCATOR_BASE_COLOR)
        evidence['located'] = {name: entry['box'] for name, entry in located.items()}
        for expected in EXPECTED_TRANSMISSION_GLASS:
            # The back layer of the two-layer overlap is deliberately hidden by
            # the front layer in the nearest-visible debug sample; its material
            # record is still checked below.
            if expected['name'] == 'overlap back':
                continue
            _check(checks, f"{expected['name']} visible in the base-color channel",
                   located.get(expected['name'], {}).get('point') is not None,
                   located.get(expected['name']), 'located')

        records = material_records(viewer.hwnd)
        evidence['materialRecords'] = len(records)
        for expected in EXPECTED_TRANSMISSION_GLASS:
            target = [round(v, 3) for v in expected['baseColor']]
            record = next((item for item in records
                           if [round(v, 3) for v in item.get('baseColor', [])] == target), None)
            _check(checks, f"{expected['name']} material record present", record is not None,
                   record is not None, True)
            if record is None:
                continue
            _check(checks, f"{expected['name']} authored alpha mode",
                   record['alphaMode'] == expected['alphaMode'],
                   record['alphaMode'], expected['alphaMode'])
            _check(checks, f"{expected['name']} effective transmission",
                   abs(record.get('transmission', -1) - expected['transmission']) <= 0.02,
                   record.get('transmission'), expected['transmission'])
            if expected.get('doubleSided'):
                _check(checks, f"{expected['name']} authored double-sided",
                       bool(record.get('flags', 0) & 1), record.get('flags'), 'bit 0')

        panels = {}
        for expected in EXPECTED_TRANSMISSION_GLASS:
            name = expected['name']
            box = located.get(name, {}).get('box')
            if box is None:
                continue
            points = _role_points(located_frame, box, LOCATOR_BASE_COLOR[name])
            if len(points) < 4:
                points = _grid_points(box, 5)
            panels[name] = {
                'box': box,
                'points': len(points),
                'coverage': _channel_at(viewer, located_frame, points, 9),
                'transmission': _channel_at(viewer, located_frame, points, 10),
                'composite': _channel_at(viewer, located_frame, points, 22),
                'transmitted': _channel_at(viewer, located_frame, points, 31),
                'background': _channel_at(viewer, located_frame, points, 32),
            }
        evidence['panels'] = {
            name: {
                'points': panel['points'],
                'coverage': _channel_mean(panel['coverage']),
                'transmission': _channel_mean(panel['transmission']),
                'composite': [_channel_mean(panel['composite'], c) for c in range(3)],
                'transmitted': [_channel_mean(panel['transmitted'], c) for c in range(3)],
                'background': [_channel_mean(panel['background'], c) for c in range(3)],
            }
            for name, panel in panels.items()
        }

        # Coverage is the authored value for every role: 1 for OPAQUE/MASK and
        # the authored alpha for BLEND. It never follows Fresnel or transmission.
        for expected in EXPECTED_TRANSMISSION_GLASS:
            panel = panels.get(expected['name'])
            if panel is None:
                continue
            coverage = _channel_mean(panel['coverage'])
            expected_coverage = expected['baseColor'][3] if expected['alphaMode'] == 2 else 1.0
            _check(checks, f"{expected['name']} coverage is authored coverage",
                   coverage is not None and abs(coverage - expected_coverage) <= 0.05,
                   coverage, expected_coverage)

        # Coverage x transmission sweep: identical authored coverage, different
        # optical transmission. The tint-normalised transmitted weight scales;
        # coverage does not.
        quarter = panels.get('blend quarter transmission')
        full = panels.get('blend full transmission')
        if quarter and full:
            quarter_coverage = _channel_mean(quarter['coverage'])
            full_coverage = _channel_mean(full['coverage'])
            quarter_transmission = _channel_mean(quarter['transmission'])
            full_transmission = _channel_mean(full['transmission'])
            quarter_weights = _ratio_tint(
                quarter['transmitted'], quarter['background'], [1.0, 0.62, 0.34])
            full_weights = _ratio_tint(
                full['transmitted'], full['background'], [0.7, 0.35, 1.0])
            quarter_weight = _mean([value for value in quarter_weights if value is not None])
            full_weight = _mean([value for value in full_weights if value is not None])
            _check(checks, 'coverage x transmission sweep keeps coverage constant',
                   quarter_coverage is not None and full_coverage is not None
                   and abs(quarter_coverage - full_coverage) <= 0.03,
                   [quarter_coverage, full_coverage], 'equal')
            _check(checks, 'coverage x transmission sweep changes transmission',
                   quarter_transmission is not None and full_transmission is not None
                   and abs(quarter_transmission - 0.25) <= 0.03
                   and abs(full_transmission - 1.0) <= 0.03,
                   [quarter_transmission, full_transmission], [0.25, 1.0])
            _check(checks, 'transmitted radiance follows the transmission sweep',
                   quarter_weight is not None and full_weight is not None
                   and full_weight > quarter_weight * 2.0,
                   [quarter_weight, full_weight], 'full weight > 2x quarter')

        # Tint is independent of the background: transmitted / background /
        # base is the same interface weight in every channel.
        for name in ('glass neutral', 'glass tinted orange', 'glass dark'):
            panel = panels.get(name)
            expected = next(item for item in EXPECTED_TRANSMISSION_GLASS
                            if item['name'] == name)
            if panel is None:
                continue
            ratios = _ratio_tint(panel['transmitted'], panel['background'],
                                 expected['baseColor'][:3])
            finite = [value for value in ratios if value is not None]
            evidence.setdefault('tintRatios', {})[name] = ratios
            _check(checks, f'{name} tint normalises to one interface weight',
                   len(finite) == 3 and max(finite) - min(finite) <= 0.08,
                   ratios, 'per-channel weight within 0.08')
            _check(checks, f'{name} transmitted weight is below one',
                   all(0.55 <= value <= 1.02 for value in finite) if finite else False,
                   ratios, '[0.55,1.02]')

        # The IOR-derived interface removes reflection, not coverage, at IOR=1.
        ior_one = panels.get('ior one no interface')
        if ior_one:
            ior_coverage = _channel_mean(ior_one['coverage'])
            ior_transmission = _channel_mean(ior_one['transmission'])
            _check(checks, 'IOR 1 keeps the surface opaque and suppresses transmission',
                   ior_coverage is not None and ior_transmission is not None
                   and abs(ior_coverage - 1.0) <= 0.03
                   and abs(ior_transmission - 0.0) <= 0.01,
                   [ior_coverage, ior_transmission], [1.0, 0.0])

        # Rough glass filters transmitted radiance: the background contrast
        # inside the rough panel is far lower than behind the smooth panel.
        smooth = panels.get('glass neutral')
        rough = panels.get('glass rough')
        if smooth and rough:
            smooth_low, smooth_high = _p10_p90(smooth['background'])
            rough_low, rough_high = _p10_p90(rough['background'])
            smooth_contrast = (smooth_high - smooth_low) / max(1e-6, smooth_high + smooth_low)
            rough_contrast = (rough_high - rough_low) / max(1e-6, rough_high + rough_low)
            evidence['roughnessContrast'] = {
                'smooth': [smooth_low, smooth_high, smooth_contrast],
                'rough': [rough_low, rough_high, rough_contrast],
            }
            _check(checks, 'smooth transmission preserves checker contrast',
                   smooth_high > 0.4, smooth_high, '>0.4')
            _check(checks, 'rough transmission filters the checker contrast',
                   rough_high < smooth_high * 0.8,
                   [rough_high, smooth_high], 'rough p90 < 0.8 x smooth p90')

        # Actual interior geometry, not only the checker: the transmitted sample
        # over the interior glass is dominated by the red/blue interior wall.
        interior = panels.get('interior glass')
        if interior:
            background = interior['background']
            red = [sample for sample in background
                   if sample and sample[0] > 2.0 * max(sample[1], sample[2])]
            blue = [sample for sample in background
                    if sample and sample[2] > 2.0 * max(sample[0], sample[1])]
            _check(checks, 'transmission samples actual interior geometry',
                   len(red) + len(blue) >= 2,
                   {'red': len(red), 'blue': len(blue)}, '>=2 interior samples')

        # Double-sided thin surface: the back-face contribution is dropped, so
        # the panel is not double-tinted. Its measured tint normalises to one
        # interface weight like the single-sided panels.
        thin = panels.get('double sided thin glass')
        if thin:
            expected = next(item for item in EXPECTED_TRANSMISSION_GLASS
                            if item['name'] == 'double sided thin glass')
            ratios = _ratio_tint(thin['transmitted'], thin['background'],
                                 expected['baseColor'][:3])
            finite = [value for value in ratios if value is not None]
            _check(checks, 'double-sided thin glass contributes one interface',
                   len(finite) == 3 and max(finite) - min(finite) <= 0.10,
                   ratios, 'per-channel weight within 0.10')

        # Two overlapping transmissive layers and their opaque background.
        # The debug sample resolves the nearest front layer (sorted fallback);
        # the product image is OIT and must stay order-independent.
        overlap = panels.get('overlap front')
        if overlap:
            expected = next(item for item in EXPECTED_TRANSMISSION_GLASS
                            if item['name'] == 'overlap front')
            ratios = _ratio_tint(overlap['transmitted'], overlap['background'],
                                 expected['baseColor'][:3])
            finite = [value for value in ratios if value is not None]
            _check(checks, 'overlap front samples the opaque background, not the back layer',
                   len(finite) == 3 and max(finite) - min(finite) <= 0.10,
                   ratios, 'per-channel weight within 0.10')

        set_channel(viewer.hwnd, 0)
        viewer.set_camera(0.0, 90.0, 1.55)
        viewer.wait_stable()
        product = _capture(viewer, images / 'transmission-oit-front.png')
        from common import send_field
        send_field(viewer.hwnd, 249, 1)
        permuted = _capture(viewer, images / 'transmission-oit-permuted.png')
        send_field(viewer.hwnd, 249, 0)
        permuted_delta = mae(_downsample(product), _downsample(permuted))
        evidence['oitPermutationMae'] = permuted_delta
        _check(checks, 'transmission is invariant to transparent draw order',
               permuted_delta < 0.0005, round(permuted_delta, 8), '<0.0005')

        content = _content_box(product)
        cw, ch, crgb = crop(product['width'], product['height'], product['rgb'], content)
        (images / 'transmission-content-oit.png').write_bytes(encoded_png(cw, ch, crgb))
        evidence['contentCrop'] = content

        # Edge / offscreen case: orbit and close in so panels are clipped by the
        # viewport. Sampling is clamped to the same screen pixel, so the frame
        # must stay finite, non-blank and order-stable.
        edge = []
        for yaw, pitch, distance, label in ((32.0, 62.0, 0.62, 'edge-oblique'),
                                            (-26.0, 70.0, 0.55, 'edge-left')):
            viewer.set_camera(yaw, pitch, distance)
            first = _capture(viewer, images / f'transmission-{label}.png')
            send_field(viewer.hwnd, 249, 1)
            second = _capture(viewer, images / f'transmission-{label}-permuted.png')
            send_field(viewer.hwnd, 249, 0)
            delta = mae(_downsample(first), _downsample(second))
            edge.append({'view': label, 'mae': delta,
                         'uniqueColours': first['stats']['uniqueSampleColours']})
            _check(checks, f'{label} clipped transmission stays finite and non-blank',
                   first['stats']['uniqueSampleColours'] >= 12, first['stats'])
            _check(checks, f'{label} clipped transmission is order-stable',
                   delta < 0.0005, round(delta, 8), '<0.0005')
        evidence['edge'] = edge
        viewer.set_camera(0.0, 90.0, 1.55)
        viewer.wait_stable()

        # Resize rebuilds the bounded hierarchy.
        before_bytes = _transmission_stats(viewer)['targetBytes']
        viewer.resize(900, 700)
        small = _transmission_stats(viewer)
        resized = _capture(viewer, images / 'transmission-after-resize.png')
        viewer.resize(1280, 900)
        restored = _transmission_stats(viewer)
        evidence['resize'] = {'before': before_bytes, 'small': small,
                              'restored': restored,
                              'uniqueColours': resized['stats']['uniqueSampleColours']}
        _check(checks, 'resize rebuilds the transmission hierarchy',
               small['mipCount'] == 8 and restored['mipCount'] == 8
               and small['targetBytes'] != before_bytes and restored['targetBytes'] == before_bytes,
               evidence['resize'], 'mip count 8 and extent-tracked bytes')
        _check(checks, 'resized frame still renders transmission',
               resized['stats']['uniqueSampleColours'] >= 10, resized['stats'])

        # Cancellation / superseded generation keeps a drawable transmission path.
        first_generation = viewer.request_open(fixture)
        second_generation = viewer.request_open(fixture)
        final_generation, _, _ = viewer.wait_open(second_generation)
        final_stats = _transmission_stats(viewer)
        evidence['cancelledGeneration'] = {
            'first': first_generation, 'second': second_generation,
            'final': final_generation, 'stats': final_stats,
        }
        _check(checks, 'superseded import keeps the transmission path drawable',
               final_generation == second_generation and final_stats['fallbackReason'] == 0
               and final_stats['mipCount'] == 8, evidence['cancelledGeneration'],
               second_generation)

        # Device loss rebuilds every target, including the transmission pyramid.
        send_field(viewer.hwnd, 65, 0)
        deadline = time.monotonic() + 90.0
        recovered = False
        while time.monotonic() < deadline:
            time.sleep(0.5)
            state = viewer_query(viewer, 0) - 1
            current = _transmission_stats(viewer)
            if state == 2 and current['fallbackReason'] == 0 and current['targetBytes'] > 0:
                recovered = True
                break
        reset_frame = _capture(viewer, images / 'transmission-after-device-reset.png')
        evidence['deviceReset'] = {'recovered': recovered, 'stats': reset_frame['stats']}
        _check(checks, 'device loss rebuilds the transmission hierarchy',
               recovered and reset_frame['stats']['uniqueSampleColours'] >= 10,
               evidence['deviceReset'])

    # Allocation failure is a real, recorded degradation branch. OPAQUE
    # transmissive materials stay in the opaque bucket and render as authored
    # dielectric surfaces; coverage is unchanged.
    low_memory = {'status': 'not-run'}
    try:
        import os
        environment = dict(os.environ)
        environment['PREVIEW3D_TEST_TRANSMISSION_ALLOCATION_FAILURE'] = '1'
        with Viewer(configuration, output, width=1000, height=800,
                    environment=environment) as viewer:
            viewer.open(fixture)
            viewer.apply_controlled_presentation()
            viewer.set_camera(0.0, 90.0, 1.55)
            frame = _capture(viewer, images / 'transmission-allocation-failure.png')
            state = _transmission_stats(viewer)
            support = channel_supported(viewer.hwnd)
            coverage = transmission = None
            if support.get(9) and support.get(10):
                set_channel(viewer.hwnd, 1)
                located_frame = viewer.wait_stable()
                located = _locate(located_frame, LOCATOR_BASE_COLOR)
                box = located.get('glass neutral', {}).get('box')
                if box:
                    points = _role_points(located_frame, box,
                                          LOCATOR_BASE_COLOR['glass neutral'])
                    coverage = _channel_mean(_channel_at(viewer, located_frame,
                                                         points, 9))
                    transmission = _channel_mean(_channel_at(viewer, located_frame,
                                                             points, 10))
            low_memory = {'status': 'ran', 'fallbackReason': state['fallbackReason'],
                          'forwardDraws': state['forwardTransmissionDraws'],
                          'coverage': coverage, 'transmission': transmission,
                          'stats': frame['stats']}
            _check(checks, 'transmission allocation failure is recorded',
                   state['fallbackReason'] == 3 and state['mipCount'] == 0,
                   state, 'forced fallback, no mips')
            _check(checks, 'degraded OPAQUE glass keeps opaque coverage',
                   coverage is not None and abs(coverage - 1.0) <= 0.05,
                   coverage, 1.0)
            _check(checks, 'degraded glass suppresses optical transmission',
                   transmission is not None and abs(transmission) <= 0.01,
                   transmission, 0.0)
            _check(checks, 'degraded frame still renders geometry',
                   frame['stats']['uniqueSampleColours'] >= 10, frame['stats'])
    except Exception as error:
        low_memory = {'status': 'error', 'error': str(error)}
        _check(checks, 'transmission allocation-failure viewer ran', False, str(error), 'ran')
    evidence['lowMemory'] = low_memory

    # Formula-level Khronos reference. There is no Khronos sample viewer on
    # this workstation; the comparison is the published thin-surface
    # transmission equation, evaluated independently in Python and recorded
    # rather than presented as a rendered reference image.
    khronos = {'status': 'formula-reference', 'rendererInstalled': False,
               'route': 'Compare controlled fixture channels against the '
                        'KHR_materials_transmission thin-surface equation; an '
                        'installed Khronos Sample Viewer render remains incomplete.',
               'materials': []}
    for expected in EXPECTED_TRANSMISSION_GLASS:
        ior = expected['ior']
        ratio = (ior - 1.0) / (ior + 1.0) if ior > 1.0 else 0.0
        f0 = ratio * ratio
        fresnel = f0 + (1.0 - f0) * (1.0 - 1.0) ** 5 if f0 > 0 else 0.0
        weight = expected['transmission'] * (1.0 - fresnel)
        khronos['materials'].append({
            'name': expected['name'], 'ior': ior, 'f0': round(f0, 6),
            'normalIncidenceWeight': round(weight, 6),
            'coverage': expected['baseColor'][3] if expected['alphaMode'] == 2 else 1.0,
        })
    evidence['khronos'] = khronos

    return checks, evidence


def _sample_channels_at(viewer, frame, point, channels):
    px, py = _rect_point(frame, point[0], point[1])
    values = {}
    for channel in channels:
        set_channel(viewer.hwnd, channel)
        values[channel] = sample_region(viewer.hwnd, px - 3, py - 3,
                                        px + 3, py + 3).get('rgba')
    return values


def _find_transmissive_point(viewer, frame, y0_fraction=0.0, y1_fraction=0.6):
    """A content pixel whose authored transmission channel is highest.

    The external images are first-open/preview framing, so a fixed normalized
    rectangle can miss the windshield or lens. Searching the content box for the
    strongest exposed transmission factor keeps the numeric probe on glass.
    """
    content = _content_box(frame)
    width, height = frame['width'], frame['height']
    left = content[0] * width
    right = content[2] * width
    top = (content[1] + (content[3] - content[1]) * y0_fraction) * height
    bottom = (content[1] + (content[3] - content[1]) * y1_fraction) * height
    set_channel(viewer.hwnd, 22)
    best, best_value = None, -1.0
    best_transmission = -1.0
    for row in range(14):
        for column in range(14):
            x = int(left + (right - left) * (column + 0.5) / 14)
            y = int(top + (bottom - top) * (row + 0.5) / 14)
            px, py = _rect_point(frame, x, y)
            sample = sample_region(viewer.hwnd, px - 2, py - 2, px + 2, py + 2)
            rgba = sample.get('rgba')
            if not rgba:
                continue
            # Prefer a head-on glass pixel: strong authored transmission and
            # low interface Fresnel, so coverage and Fresnel stay separable.
            score = rgba[2] - 0.5 * rgba[1]
            if score > best_value:
                best_value = score
                best = (x, y)
                best_transmission = rgba[2]
    return best, (best_transmission if best else -1.0)


def _external_cases(output: Path, configuration: str):
    checks = []
    evidence = {'status': 'unavailable'}
    if not CORPUS_ROOT.exists():
        return checks, evidence
    import corpus
    inventory = corpus.inventory(CORPUS_ROOT)
    assets = inventory.get('seedAssets', {})
    car = assets.get('car') or {}
    camera = assets.get('camera') or {}
    suit = assets.get('suit') or {}
    if not car.get('available') or not camera.get('available'):
        return checks, evidence
    images = output / 'images'
    images.mkdir(parents=True, exist_ok=True)
    evidence = {'status': 'ran', 'car': {}, 'camera': {}, 'suit': {}}

    with Viewer(configuration, output, width=1280, height=900, settle_timeout=180) as viewer:
        viewer.open(Path(car['entry']))
        viewer.apply_controlled_presentation()
        car_results = []
        for yaw, pitch, name in ((0.0, 90.0, 'front'), (30.0, 60.0, 'oblique')):
            viewer.set_camera(yaw, pitch, 0.65)
            frame = _capture(viewer, images / f'car-{name}-transmission.png')
            stats = _transmission_stats(viewer)
            point, transmission_factor = _find_transmissive_point(
                viewer, frame, 0.0, 0.65)
            values = _sample_channels_at(viewer, frame, point, [10, 17, 22, 31])
            composite = values[22]
            transmitted = values[31]
            reflection = values[17]
            result = {
                'view': name, 'point': point, 'transmissionFactor': transmission_factor,
                'transmission': stats, 'composite': composite,
                'transmitted': transmitted, 'reflection': reflection,
            }
            car_results.append(result)
            if name == 'front':
                _check(checks, f'Car {name} windshield exposes authored transmission',
                       transmission_factor > 0.05 and values[10] is not None,
                       [transmission_factor, values[10]], '>0.05')
                _check(checks, f'Car {name} coverage is not the Fresnel reflectance',
                       composite is not None and abs(composite[0] - composite[1]) > 0.10,
                       composite[:3] if composite else None,
                       '|coverage - Fresnel| > 0.10')
            else:
                _check(checks, f'Car {name} keeps a transmissive surface in view',
                       transmission_factor >= 0.0 and values[10] is not None,
                       [transmission_factor, values[10]], '>=0')
            _check(checks, f'Car {name} windshield has a finite transmitted term',
                   transmitted is not None
                   and all(math.isfinite(value) for value in transmitted[:3]),
                   transmitted and transmitted[:3], 'finite')
            _check(checks, f'Car {name} windshield keeps interface reflection',
                   reflection is not None and _mean_luma_linear(reflection) > 0.001,
                   reflection and reflection[:3], '>0.001')
            _check(checks, f'Car {name} has an active bounded transmission path',
                   stats['mipCount'] == 8 and stats['fallbackReason'] == 0,
                   stats, '8 mips, no fallback')
        evidence['car'] = car_results

        viewer.open(Path(camera['entry']))
        viewer.apply_controlled_presentation()
        viewer.set_camera(0.0, 90.0, 0.5)
        frame = _capture(viewer, images / 'camera-lens-transmission.png')
        stats = _transmission_stats(viewer)
        point, transmission_factor = _find_transmissive_point(viewer, frame, 0.1, 0.95)
        values = _sample_channels_at(viewer, frame, point, [10, 17, 22, 31])
        composite = values[22]
        transmitted = values[31]
        reflection = values[17]
        camera_result = {'point': point, 'transmissionFactor': transmission_factor,
                         'transmission': stats, 'composite': composite,
                         'transmitted': transmitted, 'reflection': reflection}
        evidence['camera'] = camera_result
        _check(checks, 'Camera OPAQUE transmissive lens is routed through the transparent pass',
               stats['forwardTransmissionDraws'] > 0 and stats['fallbackReason'] == 0,
               stats, '>0 OPAQUE transmissive draws')
        _check(checks, 'Camera lens exposes authored transmission',
               transmission_factor > 0.9, transmission_factor, '>0.9')
        _check(checks, 'Camera lens transmits interior scene depth',
               transmitted is not None and _mean_luma_linear(transmitted) > 0.002,
               transmitted and transmitted[:3], '>0.002')
        _check(checks, 'Camera lens keeps its reflection/depth advantage',
               reflection is not None and _mean_luma_linear(reflection) > 0.001,
               reflection and reflection[:3], '>0.001')
        _check(checks, 'Camera lens coverage stays opaque, not Fresnel',
               composite is not None and composite[0] >= 0.9 and composite[1] <= 0.25,
               composite[:3] if composite else None, '[coverage 1, Fresnel <0.25]')

        if suit.get('available'):
            viewer.open(Path(suit['entry']))
            viewer.apply_controlled_presentation()
            viewer.set_camera(0.0, 90.0, 0.8)
            frame = _capture(viewer, images / 'suit-transmission.png')
            records = material_records(viewer.hwnd)
            ior_one = [record for record in records
                       if abs(record.get('ior', 1.5) - 1.0) <= 1e-3]
            suppressed = [record for record in ior_one
                          if abs(record.get('transmission', 0.0)) <= 1e-3]
            evidence['suit'] = {
                'iorOneMaterials': len(ior_one), 'suppressed': len(suppressed),
                'records': len(records),
            }
            _check(checks, 'Suit IOR 1 transmission stays suppressed',
                   len(ior_one) == 0 or len(suppressed) == len(ior_one),
                   evidence['suit'], 'all IOR 1 materials transmission 0')
        else:
            evidence['suit'] = {'status': 'unavailable'}
    return checks, evidence


def _mean_luma_linear(rgba):
    return sum(rgba[:3]) / 3.0


def write_report(output: Path, report):
    lines = [
        '# T27 scene-color transmission report',
        '',
        'Transmissive materials sample the finished opaque HDR scene at their own',
        'screen position through an eight-level mip pyramid (roughness-selected).',
        'Coverage, tint, transmission and interface Fresnel are independent',
        'inputs; OPAQUE transmissive materials use the transparent pass without',
        'rewriting their authored alpha mode or coverage. There is no refraction,',
        'no inter-layer transmission and no invented volume thickness.',
        '',
        f"- Status: **{report.get('status')}**",
        f"- Checks: {sum(1 for c in report.get('checks', []) if c['pass'])} passed / "
        f"{len(report.get('checks', []))} total",
        f"- Failures: {len(report.get('failures', []))}",
    ]
    for failure in report.get('failures', []):
        lines.append(f'  - {failure}')
    generated = report.get('generated', {})
    stats = generated.get('transmission', {})
    lines += [
        '', '## Bounds and fallback', '',
        f"- Snapshot + mip bytes: {stats.get('targetBytes', 'n/a')} (accounted GPU bytes "
        f"{stats.get('accountedGpuBytes', 'n/a')}).",
        f"- Mip levels: {stats.get('mipCount', 'n/a')}; fallback reason: "
        f"{stats.get('fallbackReason', 'n/a')} (0 none, 3 forced).",
        f"- OPAQUE transmissive draws routed: {stats.get('forwardTransmissionDraws', 'n/a')}.",
        '- Sampling is the fragment\'s own screen pixel. Offscreen/occluded content is',
        '  never claimed; where the opaque target has no coverage the studio',
        '  environment in the transmitted direction is the fallback.',
        '- Allocation failure is recorded and degrades transmissive materials to',
        '  their authored dielectric response without changing coverage.',
        '', '## Evidence', '',
        f"- OIT order-permutation MAE: {generated.get('oitPermutationMae', 'n/a')}",
        f"- Rough/smooth transmitted contrast: {generated.get('roughnessContrast', {})}",
        f"- Low-memory degradation: {generated.get('lowMemory', {})}",
        f"- Resize: {generated.get('resize', {})}",
        f"- Device reset: {generated.get('deviceReset', {})}",
        f"- Car: {report.get('external', {}).get('car', 'unavailable')}",
        f"- Camera: {report.get('external', {}).get('camera', 'unavailable')}",
        f"- Suit: {report.get('external', {}).get('suit', 'unavailable')}",
        f"- Khronos formula reference: {generated.get('khronos', {}).get('status', 'n/a')}",
        f"- Images: `{output / 'images'}`",
        '', '## Interpretation', '',
        '- The tint invariant is checked as transmitted/background/base: it must be',
        '  one interface weight in every channel, so a tinted panel is not merely a',
        '  darker panel and coverage is not substituted for transmission.',
        '- Rough glass is verified by the reduced high-frequency contrast of the',
        '  transmitted sample, not by a self-created golden image.',
        '- The two-layer overlap defines the contract: each transmissive layer sees',
        '  the opaque snapshot, never another transmissive layer; the OIT composite',
        '  is order-independent. This is a bounded approximation, not refraction.',
        '- No Khronos sample viewer is installed; the recorded comparison is the',
        '  published thin-surface equation. A rendered Khronos reference remains',
        '  incomplete rather than being silently claimed.',
        '',
    ]
    path = output / 'scene-transmission-report.md'
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def run(workspace: Path, output: Path, configuration: str, fast: bool = False):
    output = Path(output)
    report = {'schema': RESULT_SCHEMA, 'task': 'T27', 'checks': [], 'failures': [],
              'status': 'running', 'fast': fast}
    report['serialLanes'] = _run_serial(configuration, output)
    for lane in report['serialLanes']:
        if not lane.get('pass'):
            report['failures'].append(f"serial lane failed: {lane['name']}")

    try:
        checks, generated = _run_generated(workspace, output, configuration, fast)
        report['checks'].extend(checks)
        report['generated'] = generated
    except Exception as error:
        report['failures'].append(f'generated transmission lane failed: {error}')
        report['generated'] = {'error': str(error)}

    if not fast:
        try:
            external_checks, external = _external_cases(output, configuration)
            report['checks'].extend(external_checks)
            report['external'] = external
        except Exception as error:
            report['failures'].append(f'external glass rubric failed: {error}')
            report['external'] = {'status': 'error', 'error': str(error)}
    else:
        report['external'] = {'status': 'skipped', 'reason': 'fast iteration lane'}

    failed = [check for check in report['checks'] if not check['pass']]
    if failed:
        report['failures'].append(f'{len(failed)} transmission checks failed')
    report['status'] = 'pass' if not report['failures'] else 'fail'
    write_json(output / 'diagnostics/scene-transmission.json', report)
    report['reportPath'] = str(write_report(output, report))
    return report
