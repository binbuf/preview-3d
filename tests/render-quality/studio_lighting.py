"""T28 Studio environment and first-open presentation verification.

The lane has three parts:

1. serial CPU lanes: the shipped Studio bake is measured for neutrality,
   broad-highlight and dark-reflection criteria, the direct/IBL split, and the
   contribution-based presentation selector;
2. a Debug GPU session on the generated ``lighting-probe`` fixture: material
   identity, exposure/background independence, IBL/direct isolation, world-
   stable vs camera-relative behaviour, preset stability and external
   Car/Camera/Suit nonregression regions;
3. a first-open framing review over the profile's default captures.

It never promotes a baseline. Images are saved under the output tree for the
separate review record.
"""
from __future__ import annotations

import json
import math
import struct
import subprocess
import time
from pathlib import Path

from common import RESULT_SCHEMA, decode_png, encoded_png, send_field, sha256_file, write_json
from diagnostics import (material_records, region_sample, sample_region,
                         set_channel)
from fixtures import EXPECTED_MATERIALS
from material_semantics import _check_material_records, _locate_probes
from viewer import Viewer

ROOT = Path(__file__).resolve().parents[2]
CORPUS = Path('D:/repos/binbuf/preview-3d-test-models')

SERIAL_LANES = [
    ('unit-studio-lighting', 'Tests.Unit.exe', ['[studio-lighting]']),
    ('unit-studio-ibl', 'Tests.Unit.exe', ['[studio-ibl]']),
    ('unit-tonemap-presets', 'Tests.Unit.exe', ['[tonemap],[studio-presets]']),
]

EXTERNAL = {
    'car': {
        'entry': 'Car.glb',
        # Keep the whole car (including the front bumper and windshield) in
        # frame. A 0.48 close-up crops the lower body and centres the visible
        # silhouette off-axis, which makes the framing check unreviewable.
        'view': {'yawDegrees': 18.0, 'pitchDegrees': 70.0, 'distanceScale': 1.0},
        'regions': {'windshield': [0.28, 0.16, 0.72, 0.58],
                    'paint': [0.34, 0.55, 0.72, 0.9]},
    },
    'camera': {
        'entry': 'Camera_01_4k.gltf',
        # The strap/base is part of the fixture; 0.42 cropped it and pushed the
        # camera body low. Frame the complete product and keep the lens region.
        'view': {'yawDegrees': 0.0, 'pitchDegrees': 90.0, 'distanceScale': 1.0},
        'regions': {'lens': [0.60, 0.40, 0.76, 0.62],
                    'black-body': [0.42, 0.40, 0.58, 0.66]},
    },
    'suit': {
        'entry': 'Advanced Crew Escape Suit.glb',
        'view': {'yawDegrees': 18.0, 'pitchDegrees': 70.0, 'distanceScale': 1.0},
        'regions': {'body': [0.35, 0.2, 0.65, 0.85]},
    },
}

CHANNELS = [1, 6, 7, 12, 14, 15, 16, 17, 19, 20, 21, 22, 31, 32]


def _progress(output, message):
    """Append a timestamped phase marker so a stalled lane is diagnosable.

    The harness only receives a tool's output when it exits. If a GPU call
    hangs, the last marker in ``diagnostics/progress.log`` names the phase that
    stalled; the viewer watchdog then kills the process after its no-progress
    budget instead of letting the task run to its wall-clock timeout.
    """
    line = f'{time.strftime("%H:%M:%S")} {message}'
    print(line, flush=True)
    try:
        path = Path(output) / 'diagnostics' / 'progress.log'
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('a', encoding='utf-8') as log:
            log.write(line + '\n')
    except OSError:
        pass


def _send(hwnd, field, value=0):
    return send_field(hwnd, field, value)


def _f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', float(value)))[0]


def _f32_value(bits):
    return struct.unpack('<f', struct.pack('<I', int(bits) & 0xFFFFFFFF))[0]


def _set_exposure(viewer, value):
    _send(viewer.hwnd, 92, _f32_bits(value))
    time.sleep(0.25)


def _set_environment_intensity(viewer, value):
    _send(viewer.hwnd, 98, _f32_bits(value))
    time.sleep(0.25)


def _set_direct_rig(viewer, scale):
    for field in (264, 265, 266):
        _send(viewer.hwnd, field, _f32_bits(scale))
    time.sleep(0.25)


def _set_lighting(viewer, mode):
    _send(viewer.hwnd, 88, mode)
    time.sleep(0.3)


def _check(checks, name, passed, actual=None, expected=None, tolerance=None):
    checks.append({'name': name, 'pass': bool(passed), 'actual': actual,
                   'expected': expected, 'tolerance': tolerance})


def _luma(rgb):
    if not rgb:
        return None
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]


def _finite(rgba):
    return rgba is not None and all(math.isfinite(v) for v in rgba)


def _pixel(frame, centroid):
    if centroid is None:
        return None
    x, y = centroid
    if not (0 <= x < frame['width'] and 0 <= y < frame['height']):
        return None
    index = (y * frame['width'] + x) * 3
    return [frame['rgb'][index], frame['rgb'][index + 1], frame['rgb'][index + 2]]


def _sample_probes(viewer, channels, settle=True):
    expected = EXPECTED_MATERIALS['lighting-probe']
    set_channel(viewer.hwnd, 1)
    locator = viewer.wait_stable()
    rect = locator['rect']
    centroids = _locate_probes(locator, expected)
    samples = []
    for material, centroid in zip(expected, centroids):
        entry = {'material': material['name'], 'centroid': centroid, 'channels': {}}
        if centroid is None:
            entry['error'] = 'probe not visible'
            samples.append(entry)
            continue
        x = rect['left'] + centroid[0]
        y = rect['top'] + centroid[1]
        for channel in channels:
            set_channel(viewer.hwnd, channel)
            entry['channels'][channel] = sample_region(
                viewer.hwnd, x - 4, y - 4, x + 4, y + 4).get('rgba')
        samples.append(entry)
    set_channel(viewer.hwnd, 0)
    display = viewer.wait_stable() if settle else viewer.capture()
    for entry in samples:
        entry['display'] = _pixel(display, entry.get('centroid'))
    return samples, centroids, display


def _value(samples, name, channel):
    for entry in samples:
        if entry.get('material') == name:
            return (entry.get('channels') or {}).get(channel)
    return None


def _region_stats(frame, centroid, radius):
    x, y = centroid
    width, height = frame['width'], frame['height']
    values = []
    for py in range(max(0, y - radius), min(height, y + radius + 1)):
        row = py * width
        for px in range(max(0, x - radius), min(width, x + radius + 1)):
            index = (row + px) * 3
            values.append((frame['rgb'][index], frame['rgb'][index + 1],
                           frame['rgb'][index + 2]))
    if not values:
        return None
    lumas = sorted(0.2126 * r + 0.7152 * g + 0.0722 * b for r, g, b in values)
    return {
        'count': len(values),
        'minLuma': round(lumas[0], 2),
        'maxLuma': round(lumas[-1], 2),
        'p95Luma': round(lumas[min(len(lumas) - 1, int(len(lumas) * 0.95))], 2),
        'clippedFraction': round(sum(1 for r, g, b in values
                                     if r >= 250 and g >= 250 and b >= 250)
                                 / len(values), 4),
    }


def _check_material_channels(checks, samples):
    for entry in samples:
        name = entry['material']
        channels = entry.get('channels') or {}
        for channel in CHANNELS:
            value = channels.get(channel)
            _check(checks, f'{name} channel {channel} finite', _finite(value),
                   value and value[:3], 'finite')
        metallic = channels.get(6)
        roughness = channels.get(7)
        if metallic and roughness:
            _check(checks, f'{name} metallic range', 0.0 <= metallic[0] <= 1.0,
                   metallic[0], '[0,1]')
            _check(checks, f'{name} roughness range', 0.0 <= roughness[0] <= 1.0,
                   roughness[0], '[0,1]')


def _check_calibration(checks, samples, display):
    expected = {item['name']: item for item in EXPECTED_MATERIALS['lighting-probe']}
    by_name = {entry['material']: entry for entry in samples}

    def display_of(name):
        return by_name.get(name, {}).get('display')

    white = display_of('light reference white')
    gray = display_of('light reference gray')
    black = display_of('light reference black')
    if white and gray and black:
        _check(checks, 'reference white/gray/black are ordered',
               _luma(white) > _luma(gray) + 20 > _luma(black) + 40,
               [_luma(white), _luma(gray), _luma(black)], 'white > gray > black')
        for label, value in (('white', white), ('gray', gray), ('black', black)):
            chroma = max(value) - min(value)
            _check(checks, f'reference {label} is neutral', chroma <= 4,
                   chroma, 0, 4)
        _check(checks, 'reference white keeps highlight headroom',
               100 <= _luma(white) <= 245, _luma(white), '[100,245]')
        _check(checks, 'reference gray lands in the mid tone', 50 <= _luma(gray) <= 180,
               _luma(gray), '[50,180]')
        _check(checks, 'reference black keeps shadow detail', 2 <= _luma(black) <= 95,
               _luma(black), '[2,95]')

    # White plastic / CAD: bright but not forced to a flat 255.
    for name in ('light white plastic', 'light white cad'):
        value = display_of(name)
        if value:
            _check(checks, f'{name} preserves upper-mid detail',
                   100 <= _luma(value) <= 240, _luma(value), '[100,240]')
            _check(checks, f'{name} has a small highlight headroom',
                   max(value) < 252, max(value), '<252')

    red = display_of('light red paint')
    if red:
        _check(checks, 'red paint keeps its hue', red[0] > red[1] * 1.6
               and red[0] > red[2] * 1.6, red, 'R dominant')
        _check(checks, 'red paint is not clipped to white', red[0] < 245, red[0], '<245')

    brass = display_of('light brass')
    chrome = display_of('light chrome')
    if brass:
        _check(checks, 'brass keeps its warm metal separation',
               brass[0] > brass[1] > brass[2], brass, 'R>G>B')
    if chrome:
        _check(checks, 'chrome stays neutral', max(chrome) - min(chrome) <= 8,
               chrome, '<=8')

    for name in ('light black leather', 'light black enamel'):
        value = display_of(name)
        channels = by_name.get(name, {}).get('channels', {})
        specular = channels.get(17)
        if value:
            _check(checks, f'{name} stays dark without crushing',
                   2 <= _luma(value) <= 95, _luma(value), '[2,95]')
        if specular:
            _check(checks, f'{name} keeps a reflection', _luma(specular) > 0.001,
                   _luma(specular), '>0.001')

    emission = by_name.get('light emission', {})
    if emission.get('display') and emission.get('channels', {}).get(19):
        _check(checks, 'emission reaches a readable highlight',
               max(emission['display']) >= 230, emission['display'], '>=230')
        _check(checks, 'emission is not crushed by exposure',
               max(emission['channels'][19][:3]) > 1.0,
               emission['channels'][19][:3], '>1.0')
        _check(checks, 'emission keeps a warm rolloff',
               emission['display'][0] >= emission['display'][2],
               emission['display'], 'R>=B')

    # A camera-facing material receives both the world IBL and the camera rig;
    # neither term may disappear or dominate to the point of a double-light
    # blowout. The report records the measured share for every probe.
    patch = by_name.get('light reference white', {}).get('channels', {})
    if patch.get(14) and patch.get(16):
        direct = _luma(patch[14])
        ibl = _luma(patch[16])
        total = direct + ibl
        share = direct / total if total > 1e-6 else None
        _check(checks, 'IBL and camera rig both illuminate the reference white',
               share is not None and 0.05 <= share <= 0.95,
               round(share, 4) if share is not None else None, '[0.05,0.95]')
        _check(checks, 'reference white total diffuse is bounded',
               0.01 <= total <= 4.0, round(total, 4), '[0.01,4.0]')

    # Chrome's specular lobe must contain a real highlight, not a uniform disk.
    chrome_entry = by_name.get('light chrome', {})
    if chrome_entry.get('centroid'):
        stats = _region_stats(display, chrome_entry['centroid'], 14)
        if stats:
            _check(checks, 'chrome has a readable specular highlight',
                   stats['maxLuma'] >= 70 and stats['p95Luma'] >= 45, stats,
                   'max>=70,p95>=45')
            _check(checks, 'chrome highlight is not a flat clipped disk',
                   stats['clippedFraction'] <= 0.35, stats, '<=0.35')


def _check_exposure(checks, viewer, white_centroid, output):
    """Exposure scales the model but never the display-space backdrop."""
    exposures = [0.5, 1.0, 2.0]
    values = {}
    backdrops = {}
    for exposure in exposures:
        _set_exposure(viewer, exposure)
        frame = viewer.wait_stable()
        values[exposure] = _pixel(frame, white_centroid)
        index = (4 * frame['width'] + 4) * 3
        backdrops[exposure] = list(frame['rgb'][index:index + 3])
        path = output / 'lighting' / f'exposure-{exposure:.1f}.png'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
    if all(values.get(e) for e in exposures):
        ordered = _luma(values[0.5]) < _luma(values[1.0]) < _luma(values[2.0])
        _check(checks, 'exposure increases the model tone monotonically', ordered,
               {str(e): _luma(values[e]) for e in exposures}, 'monotonic')
        ratio = (_luma(values[2.0]) - _luma(values[0.5])) / max(
            1e-6, _luma(values[1.0]))
        _check(checks, 'exposure preserves highlight rolloff', 0.15 <= ratio <= 1.4,
               round(ratio, 4), '[0.15,1.4]')
    _check(checks, 'exposure never changes the dark backdrop',
           max(abs(a - b) for a, b in zip(backdrops[0.5], backdrops[2.0])) <= 1,
           backdrops, '#1C1C1E stable')
    _set_exposure(viewer, 1.0)


def _check_isolation(checks, viewer):
    samples, _, _ = _sample_probes(viewer, [14, 15, 16, 17])
    white = next((e for e in samples if e['material'] == 'light reference white'), {})
    before = white.get('channels', {})
    _set_direct_rig(viewer, 0.0)
    direct_off, _, _ = _sample_probes(viewer, [14, 15, 16, 17])
    white_off = next((e for e in direct_off if e['material'] == 'light reference white'), {})
    after = white_off.get('channels', {})
    _check(checks, 'direct-off removes the direct diffuse lobe',
           _luma((after.get(14) or [0, 0, 0])[:3]) <= 1e-3,
           _luma((after.get(14) or [0, 0, 0])[:3]), 0.0, 1e-3)
    _check(checks, 'direct-off removes the direct specular lobe',
           _luma((after.get(15) or [0, 0, 0])[:3]) <= 1e-3,
           _luma((after.get(15) or [0, 0, 0])[:3]), 0.0, 1e-3)
    _check(checks, 'world-stable IBL survives direct-off',
           _luma((after.get(16) or [0, 0, 0])[:3]) > 0.005,
           _luma((after.get(16) or [0, 0, 0])[:3]), '>0.005')
    if before.get(16) and after.get(16):
        ratio = _luma(after[16][:3]) / max(1e-6, _luma(before[16][:3]))
        _check(checks, 'direct scales do not change environment diffuse',
               0.98 <= ratio <= 1.02, round(ratio, 5), '[0.98,1.02]')
    _set_direct_rig(viewer, 1.0)

    _set_environment_intensity(viewer, 0.0)
    zero, _, _ = _sample_probes(viewer, [14, 15, 16, 17, 19, 20], settle=False)
    white_zero = next((e for e in zero if e['material'] == 'light reference white'), {})
    channels = white_zero.get('channels', {})
    for channel in (14, 15, 16, 17):
        value = channels.get(channel)
        _check(checks, f'environment intensity 0 removes channel {channel}',
               _finite(value) and max(abs(v) for v in value[:3]) <= 1e-3,
               value and value[:3], 0.0, 1e-3)
    _set_environment_intensity(viewer, 1.0)
    set_channel(viewer.hwnd, 0)


def _check_world_stability(checks, viewer):
    """Orbiting keeps the camera-relative rig attached to the camera while the
    world-stable IBL sweeps across the surface as its world normal changes."""
    keys = [('front', 0.0), ('yaw-15', 15.0), ('yaw-30', 30.0)]
    env, direct, chrome = {}, {}, {}
    expected = EXPECTED_MATERIALS['lighting-probe']
    for name, yaw in keys:
        viewer.set_camera(yaw, 90.0, 1.0)
        set_channel(viewer.hwnd, 1)
        locator = viewer.wait_stable()
        centroids = _locate_probes(locator, expected)
        rect = locator['rect']

        def sample(material, channel):
            index = next(i for i, item in enumerate(expected)
                         if item['name'] == material)
            centroid = centroids[index]
            if centroid is None:
                return None
            x = rect['left'] + centroid[0]
            y = rect['top'] + centroid[1]
            set_channel(viewer.hwnd, channel)
            return _luma((sample_region(viewer.hwnd, x - 4, y - 4, x + 4, y + 4)
                          .get('rgba') or [0, 0, 0])[:3])

        direct[name] = sample('light white plastic', 14)
        env[name] = sample('light white plastic', 16)
        chrome[name] = sample('light chrome', 17)
    set_channel(viewer.hwnd, 0)
    if all(direct.values()) and all(env.values()) and all(chrome.values()):
        direct_values = list(direct.values())
        direct_spread = (max(direct_values) - min(direct_values)) / max(
            1e-6, sum(direct_values) / len(direct_values))
        env_values = list(env.values())
        env_spread = (max(env_values) - min(env_values)) / max(
            1e-6, sum(env_values) / len(env_values))
        chrome_values = list(chrome.values())
        chrome_spread = (max(chrome_values) - min(chrome_values)) / max(
            1e-6, sum(chrome_values) / len(chrome_values))
        _check(checks, 'camera-relative direct stays attached to the camera',
               direct_spread <= 0.15, round(direct_spread, 4), '<=0.15', 0.15)
        _check(checks, 'world-stable IBL diffuse follows the world normal',
               env_spread >= 0.25, round(env_spread, 4), '>=0.25', 0.25)
        _check(checks, 'world-stable IBL reflection follows the world normal',
               chrome_spread >= 0.25, round(chrome_spread, 4), '>=0.25', 0.25)
    else:
        _check(checks, 'orbit IBL/direct samples present', False,
               {'direct': direct, 'env': env, 'chrome': chrome}, 'samples')
    viewer.set_camera(0.0, 90.0, 1.0)


def _check_mode_ibl(checks, viewer, studio):
    _set_lighting(viewer, 2)
    directional, _, _ = _sample_probes(viewer, [16, 17])
    for item in EXPECTED_MATERIALS['lighting-probe']:
        name = item['name']
        for channel, tolerance in ((16, 0.006), (17, 0.012)):
            a = _value(studio, name, channel)
            b = _value(directional, name, channel)
            if not a or not b:
                _check(checks, f'{name} mode IBL channel {channel} present', False,
                       None, 'sample')
                continue
            delta = max(abs(x - y) for x, y in zip(a[:3], b[:3]))
            _check(checks, f'{name} Studio/Directional IBL {channel} match',
                   delta <= tolerance, round(delta, 6), 0.0, tolerance)
    _set_lighting(viewer, 0)


def _check_preset_stability(checks, viewer):
    # A manual preset survives lighting-mode changes.
    _send(viewer.hwnd, 90, 1)  # Studio CAD manual preset
    time.sleep(0.25)
    for mode in (0, 1, 2, 0):
        _set_lighting(viewer, mode)
        _check(checks, f'manual preset survives lighting mode {mode}',
               _send(viewer.hwnd, 89, 0) == 1, _send(viewer.hwnd, 89, 0), 1)
    _send(viewer.hwnd, 94, 4)
    _send(viewer.hwnd, 198, 0x336699)
    _send(viewer.hwnd, 196, 1)
    time.sleep(0.2)
    _send(viewer.hwnd, 90, 0)
    time.sleep(0.25)
    _check(checks, 'manual preset does not rewrite the background',
           _send(viewer.hwnd, 93, 0) == 4, _send(viewer.hwnd, 93, 0), 4)
    _check(checks, 'manual preset does not rewrite the custom color',
           _send(viewer.hwnd, 197, 0) == 0x336699, hex(_send(viewer.hwnd, 197, 0)),
           hex(0x336699))
    _check(checks, 'manual preset does not rewrite the ground highlight',
           _send(viewer.hwnd, 195, 0) == 1, _send(viewer.hwnd, 195, 0), 1)
    _send(viewer.hwnd, 94, 0)
    _send(viewer.hwnd, 196, 1)
    _send(viewer.hwnd, 90, 0)


def _environment_identity(checks, viewer):
    version = _send(viewer.hwnd, 267, 0)
    identity = _send(viewer.hwnd, 268, 0) | (_send(viewer.hwnd, 269, 0) << 32)
    resident = _send(viewer.hwnd, 270, 0) | (_send(viewer.hwnd, 271, 0) << 32)
    bake_us = _send(viewer.hwnd, 272, 0)
    variant = _send(viewer.hwnd, 273, 0)
    _check(checks, 'Studio bake version is explicit', version == 2, version, 2)
    _check(checks, 'Studio bake variant is the product procedural bake',
           variant == 0, variant, 0)
    _check(checks, 'Studio bake identity is present', identity != 0, hex(identity),
           'nonzero')
    _check(checks, 'Studio environment residency is bounded',
           1024 * 1024 <= resident <= 8 * 1024 * 1024, resident,
           '[1MiB,8MiB]')
    _check(checks, 'Studio bake startup cost is bounded',
           0 < bake_us <= 2000000, bake_us, '<=2000000us')
    return {'version': version, 'variant': variant, 'identity': identity,
            'residentBytes': resident, 'bakeMicroseconds': bake_us}


def _external_review(viewer, output, checks, fast):
    surveys = {}
    from corpus import resolve_entry
    for name, spec in EXTERNAL.items():
        entry, _, _ = resolve_entry(CORPUS, spec['entry'])
        if entry is None or not entry.is_file():
            surveys[name] = {'status': 'unavailable', 'entry': str(CORPUS / spec['entry'])}
            continue
        _progress(output, f'external {name}: loading {entry.name}')
        try:
            viewer.open(entry)
        except Exception as error:
            _progress(output, f'external {name}: load failed: {error}')
            surveys[name] = {'status': 'failed', 'reason': str(error)[:300]}
            continue
        _progress(output, f'external {name}: loaded')
        record = {'status': 'sampled', 'entry': str(entry), 'modes': {}}
        viewer.apply_controlled_presentation()
        _set_lighting(viewer, 0)
        _set_environment_intensity(viewer, 1.0)
        view = spec['view']
        viewer.set_camera(view['yawDegrees'], view['pitchDegrees'],
                          view['distanceScale'])
        frame = viewer.wait_stable()
        image = output / 'external' / f'{name}-studio.png'
        image.parent.mkdir(parents=True, exist_ok=True)
        image.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
        rect = frame['rect']
        regions = {}
        for region_name, normalized in spec['regions'].items():
            x0 = rect['left'] + int(normalized[0] * frame['width'])
            x1 = rect['left'] + int(normalized[2] * frame['width'])
            y0 = rect['top'] + int(normalized[1] * frame['height'])
            y1 = rect['top'] + int(normalized[3] * frame['height'])
            samples = {}
            for channel in (1, 14, 16, 17, 20):
                set_channel(viewer.hwnd, channel)
                samples[channel] = region_sample(viewer.hwnd, x0, y0, x1, y1).get('rgba')
            regions[region_name] = samples
        set_channel(viewer.hwnd, 0)
        _send(viewer.hwnd, 104, 0)
        _send(viewer.hwnd, 102, 0)
        _send(viewer.hwnd, 196, 0)
        set_channel(viewer.hwnd, 9)
        time.sleep(0.3)
        coverage_frame = viewer.capture()
        set_channel(viewer.hwnd, 0)
        viewer.apply_controlled_presentation()
        bbox = _coverage_bbox(coverage_frame)
        record['modes']['studio'] = {
            'image': str(image), 'sha256': sha256_file(image),
            'stats': frame['stats'], 'regions': regions, 'framing': bbox}
        surveys[name] = record
        _check_coverage(checks, name, bbox)
        stats = frame.get('stats') or {}
        _check(checks, f'{name} studio frame visible',
               stats.get('uniqueSampleColours', 0) >= 8
               and stats.get('lumaStdDev', 0) >= 1.0,
               stats.get('uniqueSampleColours'), '>=8')
        for region_name, samples in regions.items():
            hdr = samples.get(20)
            _check(checks, f'{name} {region_name} HDR finite', _finite(hdr),
                   hdr and hdr[:3], 'finite')
            if hdr:
                _check(checks, f'{name} {region_name} not black',
                       max(hdr[:3]) > 1e-4, hdr[:3], '>1e-4')
                _check(checks, f'{name} {region_name} bounded',
                       max(hdr[:3]) < 200.0, hdr[:3], '<200')
        if name == 'car':
            paint = regions.get('paint', {})
            windshield = regions.get('windshield', {})
            if paint.get(17) and windshield.get(17):
                _check(checks, 'Car paint keeps a stronger reflection than the windshield',
                       _luma(paint[17][:3]) > _luma(windshield[17][:3]) * 0.02,
                       [_luma(paint[17][:3]), _luma(windshield[17][:3])],
                       'paint > 0.02x windshield')
        if name == 'camera':
            lens = regions.get('lens', {})
            if lens.get(17):
                _check(checks, 'Camera lens keeps a dark reflection with depth',
                       _luma(lens[17][:3]) > 0.002
                       and _luma(lens[17][:3]) < 0.5,
                       _luma(lens[17][:3]), '[0.002,0.5]')
    return surveys


def _coverage_bbox(frame):
    """Silhouette bbox from diagnostic channel 9 (effective coverage)."""
    width, height = frame['width'], frame['height']
    min_x, min_y, max_x, max_y = width, height, -1, -1
    rgb = frame['rgb']
    for y in range(height):
        row = y * width
        for x in range(width):
            index = (row + x) * 3
            luma = 0.2126 * rgb[index] + 0.7152 * rgb[index + 1] + 0.0722 * rgb[index + 2]
            if luma > 96:
                min_x = min(min_x, x)
                min_y = min(min_y, y)
                max_x = max(max_x, x)
                max_y = max(max_y, y)
    if max_x < min_x or max_y < min_y:
        return None
    return {
        'coverageWidth': round((max_x - min_x) / width, 4),
        'coverageHeight': round((max_y - min_y) / height, 4),
        'centerX': round((min_x + max_x) / (2.0 * width), 4),
        'centerY': round((min_y + max_y) / (2.0 * height), 4),
    }


def _check_coverage(checks, name, bbox):
    if not bbox:
        _check(checks, f'{name} first-open framing contains a model', False,
               None, 'coverage silhouette')
        return
    _check(checks, f'{name} object coverage is useful',
           0.08 <= bbox['coverageWidth'] <= 0.92
           and 0.08 <= bbox['coverageHeight'] <= 0.95,
           [bbox['coverageWidth'], bbox['coverageHeight']],
           '[0.08,0.92]x[0.08,0.95]')
    _check(checks, f'{name} object framing is centred',
           abs(bbox['centerX'] - 0.5) <= 0.12
           and abs(bbox['centerY'] - 0.5) <= 0.15,
           [bbox['centerX'], bbox['centerY']], 'centre +/- 0.12/0.15')


def _generated_framing(viewer, output, checks):
    results = {}
    fixtures = {
        'lighting-probe': output / 'fixtures' / 'lighting-probe.glb',
        'sphere-grid': output / 'fixtures' / 'sphere-grid.glb',
        'stl-box': output / 'fixtures' / 'stl-box.stl',
        'glass-reference': output / 'fixtures' / 'glass-reference.glb',
    }
    for name, path in fixtures.items():
        if not path.exists():
            results[name] = {'status': 'missing', 'path': str(path)}
            continue
        try:
            viewer.open(path)
            viewer.apply_controlled_presentation()
            # The grid/ground are display-space product layers and must not
            # extend the silhouette bbox.
            _send(viewer.hwnd, 104, 0)
            _send(viewer.hwnd, 102, 0)
            _send(viewer.hwnd, 196, 0)
            set_channel(viewer.hwnd, 9)
            time.sleep(0.3)
            frame = viewer.capture()
            set_channel(viewer.hwnd, 0)
            bbox = _coverage_bbox(frame)
            results[name] = {'status': 'measured', **(bbox or {}),
                             'path': str(path)}
            _check_coverage(checks, name, bbox)
        except Exception as error:
            results[name] = {'status': 'failed', 'reason': str(error)[:300]}
            _check(checks, f'{name} framing capture', False, None, 'silhouette')
    return results


def _run_serial(configuration, output, fast):
    executable_dir = ROOT / f'x64/{configuration}'
    lanes = []
    for name, executable, filters in SERIAL_LANES:
        if fast and name != 'unit-studio-lighting':
            lanes.append({'name': name, 'skipped': True, 'pass': True,
                          'reason': 'fast iteration lane'})
            continue
        path = executable_dir / executable
        command = [str(path), *filters]
        started = time.monotonic()
        if not path.exists():
            lanes.append({'name': name, 'command': ' '.join(command), 'pass': False,
                          'reason': 'test executable missing'})
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


def write_report(output: Path, report) -> Path:
    lines = [
        '# T28 Studio lighting calibration report', '',
        f"- Status: **{report.get('status')}**",
        f"- Checks: {sum(1 for c in report.get('checks', []) if c['pass'])} passed / "
        f"{len(report.get('checks', []))} total",
        f"- Failures: {len(report.get('failures', []))}", '',
        '## Environment', '',
        f"- Identity: `{report.get('environment', {})}`", '',
        '## Framing', '',
    ]
    for name, item in report.get('framing', {}).items():
        lines.append(f"- **{name}**: {item}")
    lines += ['', '## External observations', '']
    for name, survey in report.get('externalSurvey', {}).items():
        lines.append(f"- **{name}**: {survey.get('status')} — {survey.get('reason', '')}")
        for mode, data in survey.get('modes', {}).items():
            lines.append(f"  - {mode}: `{data.get('image')}`")
    lines += ['', '## Failures', '']
    lines += [f"- {failure}" for failure in report.get('failures', [])] or ['- none']
    path = output / 'studio-lighting-report.md'
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def run(workspace: Path, output: Path, configuration: str, fast: bool = False):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    progress = output / 'diagnostics' / 'progress.log'
    try:
        progress.unlink()
    except OSError:
        pass
    _progress(output, f'T28 Studio lane start (fast={fast})')
    report = {'schema': RESULT_SCHEMA, 'task': 'T28', 'checks': [],
              'failures': [], 'status': 'running', 'fast': fast}
    report['serialLanes'] = _run_serial(configuration, output, fast)
    _progress(output, 'serial lanes complete')
    for lane in report['serialLanes']:
        if not lane.get('pass'):
            report['failures'].append(f"serial lane failed: {lane['name']}")

    fixture = output / 'fixtures' / 'lighting-probe.glb'
    if not fixture.exists():
        report['failures'].append(f'lighting-probe fixture missing: {fixture}')
        report['status'] = 'fail'
        write_json(output / 'diagnostics' / 'studio-lighting.json', report)
        return report

    try:
        _progress(output, 'opening viewer')
        with Viewer(configuration, output, width=1280, height=900) as viewer:
            viewer.open(fixture)
            _progress(output, 'lighting fixture loaded')
            viewer.apply_controlled_presentation()
            # Numeric probes need the neutral backdrop; the product grid,
            # contact shadow and ground highlight would compete with them. The
            # profile's capture lane keeps the product presentation.
            _send(viewer.hwnd, 104, 0)
            _send(viewer.hwnd, 102, 0)
            _send(viewer.hwnd, 196, 0)
            viewer.set_camera(0.0, 90.0, 1.0)
            report['cameraState'] = viewer.camera_state()
            report['environment'] = _environment_identity(report['checks'], viewer)

            _progress(output, 'material records')
            records = material_records(viewer.hwnd)
            report['materialRecords'] = records
            identity_checks, matched = _check_material_records(
                records, EXPECTED_MATERIALS['lighting-probe'])
            report['materialChecks'] = matched
            report['checks'].extend(identity_checks)

            _progress(output, 'probe samples')
            samples, centroids, display = _sample_probes(viewer, CHANNELS)
            report['studioSamples'] = samples
            report['centroids'] = centroids
            _check_material_channels(report['checks'], samples)
            _check_calibration(report['checks'], samples, display)
            _progress(output, 'exposure')
            white = next((e for e in samples
                          if e['material'] == 'light reference white'), {})
            if white.get('centroid'):
                _check_exposure(report['checks'], viewer, white['centroid'], output)
            _progress(output, 'direct/IBL isolation')
            _check_isolation(report['checks'], viewer)
            _progress(output, 'world stability')
            _check_world_stability(report['checks'], viewer)
            _progress(output, 'mode IBL')
            studio, _, _ = _sample_probes(viewer, [16, 17])
            report['studioIblSamples'] = studio
            _check_mode_ibl(report['checks'], viewer, studio)
            _progress(output, 'preset stability')
            _check_preset_stability(report['checks'], viewer)
            viewer.apply_controlled_presentation()
            _set_lighting(viewer, 0)
            _progress(output, 'first-open framing')
            report['framing'] = _generated_framing(
                viewer, output, report['checks'])

            if not fast:
                _progress(output, 'external Car/Camera/Suit review')
                report['externalSurvey'] = _external_review(
                    viewer, output, report['checks'], fast)
            else:
                report['externalSurvey'] = {}
                report['externalNote'] = 'skipped by the fast iteration lane'
            report['presentationState'] = viewer.presentation_state()
            report['performance'] = viewer.performance()
    except Exception as error:
        _progress(output, f'GPU Studio lighting lane failed: {error}')
        report['failures'].append(f'GPU Studio lighting lane failed: {error}')

    if not report.get('studioSamples'):
        report['failures'].append('no Studio probe samples were captured')
    failed = [check for check in report['checks'] if not check['pass']]
    if failed:
        report['failures'].append(f'{len(failed)} Studio calibration checks failed')
    if not report.get('materialChecks'):
        report['failures'].append('no Studio material records were matched')
    report['status'] = 'pass' if not report['failures'] else 'fail'
    write_json(output / 'diagnostics' / 'studio-lighting.json', report)
    report['reportPath'] = str(write_report(output, report))
    _progress(output, f'T28 Studio lane {report["status"]} '
                       f'({len(report["checks"])} checks)')
    return report
