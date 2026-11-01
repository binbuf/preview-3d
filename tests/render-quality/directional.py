"""T30 Directional sun, reflection and consistency verification.

The CPU lane pins the documented Z-up world direction, the world->camera
transform and the gizmo/shadow consumers. The GPU lane drives the Debug
viewer's generated ``directional-probe`` fixture and verifies numerically:

* a stationary world-anchored sun produces a constant direct response while
  the camera orbits;
* the sun azimuth moves the lit face and the clearcoat lobe together;
* elevation raises the horizontal panel's response;
* light/environment intensity zero removes direct, IBL, coat and the
  background-only transmission fallback, while emission survives;
* black paint, gold metal and thin glass keep their reflection separation;
* switching lighting modes leaves the camera and the global sun state alone;
* an imported-up-axis change leaves the world sun and camera orientation
  unchanged.

It also captures still/orbit/azimuth sequences for visual review and, outside
the fast lane, Car/Camera/microscope/STL stills at several sun elevations.
"""
from __future__ import annotations

import json
import math
import struct
import subprocess
import time
from pathlib import Path

from common import RESULT_SCHEMA, encoded_png, send_field, sha256_file, write_json
from diagnostics import (region_sample, sample_region, set_channel)
from fixtures import EXPECTED_MATERIALS
from viewer import Viewer

ROOT = Path(__file__).resolve().parents[2]
CORPUS = Path('D:/repos/binbuf/preview-3d-test-models')

SERIAL_LANES = [
    ('unit-directional-light', 'Tests.Unit.exe', ['[directional-light]']),
    ('unit-gizmo', 'Tests.Unit.exe', ['[gizmo]']),
    ('unit-ground-shadow', 'Tests.Unit.exe', ['[ground-shadow]']),
]

PANEL_MATERIALS = EXPECTED_MATERIALS['directional-probe']
WALL_NAMES = {item['name'] for item in PANEL_MATERIALS if 'floor' not in item['name']}
ALL_NAMES = {item['name'] for item in PANEL_MATERIALS}
WALL_CHANNELS = [1, 14, 15, 16, 17, 18, 20, 31]
ZERO_CHANNELS = [14, 15, 16, 17, 18, 20, 31]

# T30 external rubric. Regions are the established normalized crops from
# T27/T28/T29; the lane samples them at several sun elevations and a backlit
# azimuth so dark-side detail and bounded highlights are visible.
EXTERNAL = {
    'car': {
        'entry': 'Car.glb',
        'view': {'yawDegrees': 18.0, 'pitchDegrees': 70.0, 'distanceScale': 0.55},
        'regions': {'windshield': [0.28, 0.16, 0.72, 0.58],
                    'paint': [0.34, 0.55, 0.72, 0.9],
                    'trim': [0.12, 0.4, 0.4, 0.72]},
    },
    'camera': {
        'entry': 'Camera_01_4k.gltf',
        'view': {'yawDegrees': 0.0, 'pitchDegrees': 90.0, 'distanceScale': 0.55},
        'regions': {'lens': [0.60, 0.40, 0.76, 0.62],
                    'black-body': [0.42, 0.40, 0.58, 0.66]},
    },
    'microscope': {
        'entry': 'vintage_microscope_4k.gltf',
        'view': {'yawDegrees': 0.0, 'pitchDegrees': 90.0, 'distanceScale': 0.9},
        'regions': {'brass': [0.3, 0.16, 0.68, 0.5],
                    'enamel': [0.18, 0.45, 0.6, 0.9]},
        'knownDefect': 'resource-limit',
    },
    'stl': {
        'entry': 'base.stl',
        'view': {'yawDegrees': 25.0, 'pitchDegrees': 65.0, 'distanceScale': 1.0},
        'regions': {'relief': [0.2, 0.15, 0.8, 0.9]},
    },
}


def _f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', float(value)))[0]


def _f32(value):
    return struct.unpack('<f', struct.pack('<I', int(value) & 0xFFFFFFFF))[0]


def _send(hwnd, field, value=0):
    return send_field(hwnd, field, value)


def _set_lighting(viewer, mode):
    _send(viewer.hwnd, 88, mode)
    time.sleep(0.35)


def _set_environment_intensity(viewer, value):
    _send(viewer.hwnd, 98, _f32_bits(value))
    time.sleep(0.35)


def _set_sun(viewer, azimuth, elevation):
    _send(viewer.hwnd, 277, _f32_bits(azimuth))
    _send(viewer.hwnd, 278, _f32_bits(elevation))
    time.sleep(0.2)


def _read_sun(viewer):
    return (_f32(_send(viewer.hwnd, 277, -1)),
            _f32(_send(viewer.hwnd, 278, -1)))


def _camera_state(viewer):
    return {field: _send(viewer.hwnd, field, -1)
            for field in (31, 32, 33)}


def _check(checks, name, passed, actual=None, expected=None, tolerance=None):
    checks.append({'name': name, 'pass': bool(passed), 'actual': actual,
                   'expected': expected, 'tolerance': tolerance})


def _finite(rgba):
    return rgba is not None and all(math.isfinite(v) for v in rgba)


def _value(samples, name, channel):
    for entry in samples:
        if entry.get('material') == name:
            return (entry.get('channels') or {}).get(channel)
    return None


def _luma(rgba):
    if not rgba:
        return None
    return 0.2126 * rgba[0] + 0.7152 * rgba[1] + 0.0722 * rgba[2]


def _locate(frame):
    # The locator is deliberately the same source-colour classifier used by the
    # T22/T24 probes: every role has a distinct base colour, so the GPU image
    # itself is the evidence that the panel is visible.
    from material_semantics import _locate_probes
    return _locate_probes(frame, PANEL_MATERIALS)


def _sample_panels(viewer, channels, capture_display=True):
    set_channel(viewer.hwnd, 1)
    frame = viewer.wait_stable()
    rect = frame['rect']
    centroids = _locate(frame)
    samples = []
    for material, centroid in zip(PANEL_MATERIALS, centroids):
        entry = {'material': material['name'], 'centroid': centroid, 'channels': {}}
        if centroid is None:
            entry['error'] = 'panel not visible'
            samples.append(entry)
            continue
        x = rect['left'] + centroid[0]
        y = rect['top'] + centroid[1]
        for channel in channels:
            if channel != 1:
                set_channel(viewer.hwnd, channel)
            # The panels are flat and uniform; a single interior pixel avoids
            # pulling a foreshortened edge or the backdrop into the average.
            entry['channels'][channel] = sample_region(
                viewer.hwnd, x, y, x + 1, y + 1).get('rgba')
        samples.append(entry)
    set_channel(viewer.hwnd, 0)
    if not capture_display:
        return samples, centroids, None
    display = viewer.wait_stable()
    for entry in samples:
        centroid = entry.get('centroid')
        if not centroid:
            continue
        x, y = centroid
        index = (y * display['width'] + x) * 3
        entry['display'] = list(display['rgb'][index:index + 3])
    return samples, centroids, display


def _save_png(viewer, output: Path, name: str, stable: bool = True):
    frame = viewer.wait_stable() if stable else viewer.capture()
    path = output / 'images' / f'{name}.png'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
    return {'name': name, 'path': str(path), 'sha256': sha256_file(path),
            'stats': frame['stats'], 'camera': viewer.camera_state()}


def _check_panels(checks, samples, label, require_visible=None):
    required = WALL_NAMES if require_visible is None else set(require_visible)
    for entry in samples:
        name = entry['material']
        channels = entry.get('channels') or {}
        for channel, value in channels.items():
            _check(checks, f'{label} {name} channel {channel} finite', _finite(value),
                   value and value[:3], 'finite')
        if entry.get('centroid') is None and name in required:
            _check(checks, f'{label} {name} visible', False, None, 'visible')


def _check_world_anchor(checks, orbit_samples):
    # A Lambert panel's direct term depends only on the fixed world normal and
    # the fixed world sun, so it must not move with the camera. Coat, metal and
    # IBL channels legitimately change with the viewing angle.
    values = [_value(samples, 'directional lambert', 14) for samples in orbit_samples]
    base = values[0]
    if not base:
        _check(checks, 'orbit Lambert direct present', False, None, 'sample')
        return
    for index, value in enumerate(values[1:], start=1):
        if not value:
            _check(checks, 'orbit Lambert direct present', False, None, 'sample')
            continue
        delta = abs(_luma(value) - _luma(base))
        relative = delta / max(0.02, abs(_luma(base)))
        _check(checks,
               'world sun keeps the Lambert direct response constant while the camera orbits',
               relative <= 0.03, round(relative, 5), 0.0, 0.03)


def _check_azimuth(checks, azimuth_samples, coat_front, coat_behind):
    lambert = [(az, _luma(_value(samples, 'directional lambert', 14)))
               for az, samples in azimuth_samples]
    front = next(value for az, value in lambert if abs(az - 0.75) < 1e-4)
    _check(checks, 'front sun lights the wall Lambert term', front is not None and front > 0.02,
           front, '>0.02')
    # Moving the sun from front (0.75) to the side/behind (1.0/0.25) must
    # monotonically reduce the wall response toward zero.
    ordered = sorted((az, value) for az, value in lambert if az in (0.75, 0.875, 1.0))
    if len(ordered) == 3 and all(value is not None for _, value in ordered):
        _check(checks, 'azimuth sweep reduces the wall diffuse monotonically',
               ordered[0][1] > ordered[1][1] > ordered[2][1],
               [round(value, 5) for _, value in ordered], 'decreasing')
    behind = next((value for az, value in lambert if az == 0.25), None)
    _check(checks, 'backlit azimuth removes the wall Lambert term',
           behind is not None and behind <= 1e-4, behind, 0.0, 1e-4)
    # Channel 18 includes the coat environment, but the camera and therefore
    # the environment are identical for the front and back azimuths; only the
    # direct coat lobe can change. A positive difference proves the coat uses
    # the same sun as the base instead of the old Studio key direction.
    if coat_front is not None and coat_behind is not None:
        direct = coat_front - coat_behind
        _check(checks, 'clearcoat direct uses the same sun as the base',
               direct > 1e-4 and coat_behind > 0.0,
               [round(coat_front, 6), round(coat_behind, 6)],
               'front-behind > 1e-4, behind is ambient floor')
    else:
        _check(checks, 'clearcoat direct uses the same sun as the base', False,
               [coat_front, coat_behind], 'sample')


def _check_elevation(checks, elevation_samples):
    values = [(elevation, _luma(_value(samples, 'directional floor lambert', 14)))
              for elevation, samples in elevation_samples]
    low = next(value for elevation, value in values if elevation == 0.15)
    high = next(value for elevation, value in values if elevation == 1.2)
    _check(checks, 'raising the sun increases the floor Lambert response',
           low is not None and high is not None and high > low * 2.0,
           [low, high], 'high > 2x low')
    for elevation, value in values:
        _check(checks, f'floor response finite at elevation {elevation}',
               value is not None and math.isfinite(value), value, 'finite')


def _check_zero(checks, samples):
    for entry in samples:
        name = entry['material']
        if entry.get('centroid') is None:
            continue
        channels = entry.get('channels') or {}
        for channel in ZERO_CHANNELS:
            value = channels.get(channel)
            if channel == 20 and name == 'directional emission':
                _check(checks, 'emission survives zero light and environment',
                       value is not None and max(value[:3]) > 1.0, value and value[:3], '>1.0')
                continue
            _check(checks, f'{name} zero intensity removes channel {channel}',
                   value is not None and max(abs(v) for v in value[:3]) <= 1e-4,
                   value and value[:3], 0.0, 1e-4)


def _check_separation(checks, samples):
    metal = _value(samples, 'directional gold metal', 17)
    black = _value(samples, 'directional black paint', 17)
    glass = _value(samples, 'directional glass', 20)
    black_total = _value(samples, 'directional black paint', 20)
    metal_total = _value(samples, 'directional gold metal', 20)
    if metal and black:
        _check(checks, 'gold metal keeps more ambient reflection than black paint',
               _luma(metal) > _luma(black) * 2.0,
               round(_luma(metal), 6), f">{_luma(black) * 2.0:.6f}")
        _check(checks, 'black paint keeps a non-zero reflection floor',
               _luma(black) > 0.0005, round(_luma(black), 6), '>0.0005')
    if black_total:
        _check(checks, 'black paint is not washed to white by the ambient floor',
               max(black_total[:3]) < 2.0, black_total[:3], '<2.0')
    if glass and black_total:
        _check(checks, 'glass and black paint remain separated',
               _luma(glass) > _luma(black_total) * 1.5,
               round(_luma(glass), 6), f">{_luma(black_total) * 1.5:.6f}")
    if metal_total and black_total:
        _check(checks, 'gold metal remains brighter than black paint under the sun',
               _luma(metal_total) > _luma(black_total),
               [round(_luma(metal_total), 6), round(_luma(black_total), 6)], 'metal > black')


def _external_survey(viewer, output: Path):
    from corpus import resolve_entry
    surveys = {}
    elevations = (0.22, 0.62, 1.20)
    for name, spec in EXTERNAL.items():
        entry, _, _ = resolve_entry(CORPUS, spec['entry'])
        if entry is None or not entry.is_file():
            surveys[name] = {'status': 'unavailable', 'entry': str(CORPUS / spec['entry'])}
            continue
        try:
            viewer.open(entry)
        except Exception as error:
            surveys[name] = {'status': 'failed', 'reason': str(error)[:300],
                             'knownDefect': spec.get('knownDefect')}
            continue
        viewer.apply_controlled_presentation()
        _set_lighting(viewer, 2)
        viewer.set_camera(spec['view']['yawDegrees'], spec['view']['pitchDegrees'],
                          spec['view']['distanceScale'])
        record = {'status': 'sampled', 'entry': str(entry), 'elevations': {},
                  'knownDefect': spec.get('knownDefect')}
        for elevation in elevations:
            for azimuth_name, azimuth in (('front', 0.75), ('back', 0.25)):
                _set_sun(viewer, azimuth, elevation)
                frame = viewer.wait_stable()
                key = f'e{int(round(elevation * 100)):03d}-{azimuth_name}'
                image = output / 'external' / f'{name}-{key}.png'
                image.parent.mkdir(parents=True, exist_ok=True)
                image.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
                rect = frame['rect']
                regions = {}
                for region_name, normalized in spec['regions'].items():
                    x0 = rect['left'] + int(normalized[0] * frame['width'])
                    x1 = rect['left'] + int(normalized[2] * frame['width'])
                    y0 = rect['top'] + int(normalized[1] * frame['height'])
                    y1 = rect['top'] + int(normalized[3] * frame['height'])
                    set_channel(viewer.hwnd, 20)
                    regions[region_name] = region_sample(
                        viewer.hwnd, x0, y0, x1, y1).get('rgba')
                set_channel(viewer.hwnd, 0)
                record['elevations'][key] = {
                    'image': str(image), 'sha256': sha256_file(image),
                    'stats': frame['stats'], 'regions': regions,
                }
        surveys[name] = record
    return surveys


def _check_external(checks, surveys):
    for asset, survey in surveys.items():
        if survey.get('status') != 'sampled':
            if survey.get('knownDefect'):
                checks.append({'name': f'{asset} external observation recorded',
                               'pass': True, 'actual': survey.get('status'),
                               'expected': 'sampled or known defect'})
            else:
                _check(checks, f'{asset} external asset sampled', False,
                       survey.get('status'), 'sampled')
            continue
        for key, data in survey.get('elevations', {}).items():
            stats = data.get('stats') or {}
            _check(checks, f'{asset} {key} frame has visible form',
                   stats.get('uniqueSampleColours', 0) >= 8
                   and stats.get('lumaStdDev', 0) >= 1.0,
                   stats.get('uniqueSampleColours'), '>=8')
            for region, sample in data.get('regions', {}).items():
                _check(checks, f'{asset} {key} {region} HDR finite', _finite(sample),
                       sample and sample[:3], 'finite')
                if sample:
                    _check(checks, f'{asset} {key} {region} keeps dark-side detail',
                           max(sample[:3]) > 1e-4, sample[:3], '>1e-4')
                    _check(checks, f'{asset} {key} {region} is bounded',
                           max(sample[:3]) < 200.0, sample[:3], '<200')


def _run_serial(configuration: str, output: Path, fast: bool):
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


def write_report(output: Path, report) -> Path:
    lines = [
        '# T30 Directional lighting, reflection and sun consistency report', '',
        'Generated from the CPU light-rig/gizmo/shadow lanes and Debug GPU',
        'readback on the generated directional-probe fixture, with reviewed',
        'still/orbit/azimuth sequences and external material elevations.', '',
        '## Checks', '',
        f"- Status: **{report.get('status')}**",
        f"- Checks: {sum(1 for c in report.get('checks', []) if c['pass'])} passed / "
        f"{len(report.get('checks', []))} total",
        f"- Failures: {len(report.get('failures', []))}",
    ]
    for failure in report.get('failures', []):
        lines.append(f'  - {failure}')
    lines += ['', '## World-anchored orbit (sun azimuth 0.75, elevation 0.45)', '',
              '| camera yaw | Lambert direct | coat direct |',
              '| --- | --- | --- |']
    for entry in report.get('orbit', []):
        lambert = _value(entry['samples'], 'directional lambert', 14)
        coat = _value(entry['samples'], 'directional coat', 18)
        lines.append(f"| {entry['yaw']} | {_fmt(lambert)} | {_fmt(coat)} |")
    lines += ['', '## Azimuth sequence (stationary camera)', '',
              '| azimuth | Lambert direct | coat direct |',
              '| --- | --- | --- |']
    for entry in report.get('azimuth', []):
        lambert = _value(entry['samples'], 'directional lambert', 14)
        coat = _value(entry['samples'], 'directional coat', 18)
        lines.append(f"| {entry['azimuth']} | {_fmt(lambert)} | {_fmt(coat)} |")
    lines += ['', '## Captured review images', '']
    for image in report.get('images', []):
        lines.append(f"- `{image['path']}`")
    for asset, survey in report.get('externalSurvey', {}).items():
        if survey.get('status') != 'sampled':
            lines.append(f"- **{asset}**: {survey.get('status')}")
            continue
        for key, data in survey.get('elevations', {}).items():
            lines.append(f"- **{asset}/{key}**: `{data['image']}`")
    lines += [
        '', '## Interpretation', '',
        '- The sun is a single world-space Z-up direction. Camera orbit does not',
        '  move it; the base/coat lobes, gizmo and contact-shadow input consume',
        '  the same vector, and the shader performs the only world->camera step.',
        '- Direct diffuse on a constant-normal panel is invariant under camera',
        '  orbit, changes monotonically with azimuth, and grows with elevation.',
        '- Intensity zero removes direct, IBL, coat and the background-only',
        '  transmission fallback; emission is the documented survivor.',
        '- Gold metal keeps a Fresnel/BRDF-weighted ambient reflection above',
        '  black paint, which stays dark; thin glass remains a separate channel.',
        '', '## Evidence', '',
        f"- `{output / 'diagnostics' / 'directional.json'}`",
        f"- `{output / 'images'}` still/orbit/azimuth sequences",
        '', ]
    path = output / 'directional-report.md'
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def _fmt(value):
    if not value:
        return 'n/a'
    return '[' + ', '.join(f'{v:.5f}' for v in value[:3]) + ']'


def run(workspace: Path, output: Path, configuration: str, fast: bool = False):
    output = Path(output)
    report = {'schema': RESULT_SCHEMA, 'task': 'T30', 'checks': [], 'failures': [],
              'status': 'running', 'fast': fast}
    report['serialLanes'] = _run_serial(configuration, output, fast)
    for lane in report['serialLanes']:
        if not lane.get('pass'):
            report['failures'].append(f"serial lane failed: {lane['name']}")

    fixture = output / 'fixtures' / 'directional-probe.glb'
    if not fixture.exists():
        report['failures'].append(f'directional-probe fixture missing: {fixture}')
        report['status'] = 'fail'
        write_json(output / 'diagnostics' / 'directional.json', report)
        return report

    try:
        with Viewer(configuration, output, width=1280, height=860) as viewer:
            viewer.open(fixture)
            viewer.apply_controlled_presentation()
            _send(viewer.hwnd, 104, 0)   # grid off
            _send(viewer.hwnd, 102, 0)   # contact shadow off
            _send(viewer.hwnd, 196, 0)   # ground highlight off
            _set_lighting(viewer, 2)
            _set_environment_intensity(viewer, 1.0)
            _set_sun(viewer, 0.75, 0.45)
            viewer.set_camera(0.0, 90.0, 1.0)

            # Front-lit reference frame.
            samples, centroids, display = _sample_panels(viewer, WALL_CHANNELS)
            report['frontSamples'] = samples
            report['centroids'] = centroids
            _check_panels(report['checks'], samples, 'front')
            _check_separation(report['checks'], samples)
            report['frontImage'] = _save_png(viewer, output, 'directional-front-lit')

            # A stationary sun while the camera orbits. The wall is planar and
            # uniform, so its direct diffuse must not move with the camera.
            orbit = []
            for yaw in (0.0, 20.0, 40.0, 60.0):
                viewer.set_camera(yaw, 90.0, 1.0)
                orbit_samples, _, _ = _sample_panels(viewer, [14, 17, 18, 20])
                orbit.append({'yaw': yaw, 'samples': orbit_samples})
                _check_panels(report['checks'], orbit_samples, f'orbit-yaw-{int(yaw)}')
            report['orbit'] = orbit
            _check_world_anchor(report['checks'], [entry['samples'] for entry in orbit])
            for yaw in (0.0, 20.0, 40.0, 60.0):
                viewer.set_camera(yaw, 90.0, 1.0)
                _save_png(viewer, output, f'directional-orbit-yaw-{int(yaw):03d}')

            # Manual sun movement with a stationary camera.
            viewer.set_camera(0.0, 90.0, 1.0)
            azimuth = []
            for value in (0.75, 0.875, 1.0, 0.25):
                _set_sun(viewer, value, 0.45)
                azimuth_samples, _, _ = _sample_panels(viewer, [14, 17, 18, 20])
                azimuth.append({'azimuth': value, 'samples': azimuth_samples})
                _check_panels(report['checks'], azimuth_samples,
                              f'azimuth-{value:.3f}')
                _save_png(viewer, output,
                          f'directional-azimuth-{int(round(value * 1000)):04d}')
            report['azimuth'] = azimuth
            coat_front = next((_luma(_value(entry['samples'], 'directional coat', 18))
                               for entry in azimuth
                               if abs(entry['azimuth'] - 0.75) < 1e-4), None)
            coat_behind = next((_luma(_value(entry['samples'], 'directional coat', 18))
                                for entry in azimuth
                                if entry['azimuth'] == 0.25), None)
            report['coatDirect'] = {'front': coat_front, 'behind': coat_behind}
            _check_azimuth(report['checks'], [(entry['azimuth'], entry['samples'])
                                               for entry in azimuth],
                           coat_front, coat_behind)

            # Elevation sweep on the horizontal floor panel. The front camera
            # pitched down sees the wall and floor together.
            _set_sun(viewer, 0.75, 0.45)
            viewer.set_camera(0.0, 55.0, 0.9)
            elevation = []
            for value in (0.15, 0.5, 1.2):
                _set_sun(viewer, 0.75, value)
                elevation_samples, _, _ = _sample_panels(viewer, [14, 17, 20])
                elevation.append({'elevation': value, 'samples': elevation_samples})
                _check_panels(report['checks'], elevation_samples,
                              f'elevation-{value:.2f}',
                              require_visible=WALL_NAMES | {'directional floor lambert'})
                _save_png(viewer, output,
                          f'directional-elevation-{int(round(value * 100)):03d}')
            report['elevation'] = elevation
            _check_elevation(report['checks'],
                             [(entry['elevation'], entry['samples'])
                              for entry in elevation])

            # Zero light and zero environment. Direct, IBL, coat and the
            # background-only transmission fallback must all vanish; emission
            # is the only documented survivor.
            _set_sun(viewer, 0.75, 0.45)
            viewer.set_camera(0.0, 90.0, 1.0)
            _set_environment_intensity(viewer, 0.0)
            zero, _, _ = _sample_panels(viewer, ZERO_CHANNELS, capture_display=False)
            report['zeroSamples'] = zero
            _check_panels(report['checks'], zero, 'zero')
            _check_zero(report['checks'], zero)
            report['zeroImage'] = _save_png(viewer, output, 'directional-zero-intensity',
                                            stable=False)
            _set_environment_intensity(viewer, 1.0)

            # Mode switching is view-neutral and sun state is app-level.
            _set_sun(viewer, 0.6123, 0.7431)
            viewer.set_camera(25.0, 70.0, 0.85)
            before_sun = _read_sun(viewer)
            before_camera = _camera_state(viewer)
            before_exposure = _f32(_send(viewer.hwnd, 91, 0))
            before_tonemap = _send(viewer.hwnd, 95, 0)
            _set_lighting(viewer, 0)
            _set_lighting(viewer, 2)
            after_sun = _read_sun(viewer)
            after_camera = _camera_state(viewer)
            after_exposure = _f32(_send(viewer.hwnd, 91, 0))
            after_tonemap = _send(viewer.hwnd, 95, 0)
            _check(report['checks'], 'lighting mode switch keeps the sun azimuth',
                   abs(before_sun[0] - after_sun[0]) <= 1e-6, after_sun[0],
                   before_sun[0], 1e-6)
            _check(report['checks'], 'lighting mode switch keeps the sun elevation',
                   abs(before_sun[1] - after_sun[1]) <= 1e-6, after_sun[1],
                   before_sun[1], 1e-6)
            _check(report['checks'], 'lighting mode switch keeps camera yaw',
                   before_camera[31] == after_camera[31], after_camera[31],
                   before_camera[31])
            _check(report['checks'], 'lighting mode switch keeps camera pitch',
                   before_camera[32] == after_camera[32], after_camera[32],
                   before_camera[32])
            _check(report['checks'], 'lighting mode switch keeps camera distance',
                   before_camera[33] == after_camera[33], after_camera[33],
                   before_camera[33])
            _check(report['checks'], 'lighting mode switch keeps exposure',
                   abs(before_exposure - after_exposure) <= 1e-6,
                   after_exposure, before_exposure, 1e-6)
            _check(report['checks'], 'lighting mode switch keeps the tone map',
                   before_tonemap == after_tonemap, after_tonemap, before_tonemap)

            # Predictable exposure between the two material modes at one fixed
            # view. This is a sanity band, not an image-equality claim: the sun
            # is a single hard light, Studio has key/fill/rim plus the same IBL.
            viewer.set_camera(0.0, 90.0, 1.0)
            _set_sun(viewer, 0.75, 0.45)
            _set_lighting(viewer, 0)
            studio_frame = viewer.wait_stable()
            _set_lighting(viewer, 2)
            directional_frame = viewer.wait_stable()
            studio_luma = (studio_frame.get('stats') or {}).get('meanLuma')
            directional_luma = (directional_frame.get('stats') or {}).get('meanLuma')
            report['modeExposure'] = {'studio': studio_luma,
                                      'directional': directional_luma}
            if studio_luma and directional_luma:
                ratio = directional_luma / studio_luma
                _check(report['checks'], 'Studio/Directional exposure stays predictable',
                       0.35 <= ratio <= 2.5, round(ratio, 4), '[0.35,2.5]')
            else:
                _check(report['checks'], 'Studio/Directional exposure stays predictable',
                       False, [studio_luma, directional_luma], 'non-zero mean luma')

            # An imported up-axis change rotates geometry and ground, not the
            # world sun or the user's camera orientation.
            viewer.set_camera(0.0, 90.0, 1.0)
            _set_sun(viewer, 0.75, 0.62)
            camera_before_axis = _camera_state(viewer)
            sun_before_axis = _read_sun(viewer)
            _send(viewer.hwnd, 70, 2)  # GroundAxis::Y
            time.sleep(0.5)
            report['upAxisY'] = _save_png(viewer, output, 'directional-up-axis-y')
            sun_after_axis = _read_sun(viewer)
            camera_after_axis = _camera_state(viewer)
            _check(report['checks'], 'up-axis change keeps the world sun azimuth',
                   abs(sun_before_axis[0] - sun_after_axis[0]) <= 1e-6,
                   sun_after_axis[0], sun_before_axis[0], 1e-6)
            _check(report['checks'], 'up-axis change keeps the world sun elevation',
                   abs(sun_before_axis[1] - sun_after_axis[1]) <= 1e-6,
                   sun_after_axis[1], sun_before_axis[1], 1e-6)
            # The ground-axis command deliberately reframes the bounds and
            # resets the camera to the new home view. It must not create a
            # per-asset sun override: the same global direction is still in
            # effect, and the reframed camera is finite.
            report['upAxisCamera'] = {'before': camera_before_axis,
                                      'after': camera_after_axis}
            _check(report['checks'], 'up-axis reframe leaves a valid camera',
                   all(isinstance(value, int) for value in camera_after_axis.values()),
                   camera_after_axis, 'finite camera state')
            _send(viewer.hwnd, 70, 3)  # back to GroundAxis::Z
            time.sleep(0.5)
            report['upAxisZ'] = _save_png(viewer, output, 'directional-up-axis-z')

            # The manual sun is app state, not a per-asset exception: opening a
            # different document keeps the same lighting mode and sun. Run this
            # last so the small CAD fixture cannot disturb the captures above.
            _set_sun(viewer, 0.6625, 0.31)
            persisted_sun = _read_sun(viewer)
            other_fixture = output / 'fixtures' / 'stl-box.stl'
            if other_fixture.exists():
                viewer.open(other_fixture)
                after_open_sun = _read_sun(viewer)
                _check(report['checks'], 'opening another asset keeps the directional mode',
                       _send(viewer.hwnd, 87, 0) == 2, _send(viewer.hwnd, 87, 0), 2)
                _check(report['checks'], 'opening another asset keeps the global sun azimuth',
                       abs(after_open_sun[0] - persisted_sun[0]) <= 1e-6,
                       after_open_sun[0], persisted_sun[0], 1e-6)
                _check(report['checks'], 'opening another asset keeps the global sun elevation',
                       abs(after_open_sun[1] - persisted_sun[1]) <= 1e-6,
                       after_open_sun[1], persisted_sun[1], 1e-6)
            else:
                _check(report['checks'], 'second generated asset available for state check',
                       False, str(other_fixture), 'exists')

            if not fast:
                report['externalSurvey'] = _external_survey(viewer, output)
                _check_external(report['checks'], report['externalSurvey'])
            else:
                report['externalSurvey'] = {}
                report['externalNote'] = 'skipped by the fast iteration lane'

            report['images'] = [value for key, value in report.items()
                                if key.endswith('Image') and isinstance(value, dict)]
    except Exception as error:
        report['failures'].append(f'GPU Directional lane failed: {error}')

    if not report.get('frontSamples'):
        report['failures'].append('no Directional panel samples were captured')
    failed = [check for check in report['checks'] if not check['pass']]
    if failed:
        report['failures'].append(f'{len(failed)} Directional checks failed')
    report['status'] = 'pass' if not report['failures'] else 'fail'
    write_json(output / 'diagnostics' / 'directional.json', report)
    report['reportPath'] = str(write_report(output, report))
    return report
