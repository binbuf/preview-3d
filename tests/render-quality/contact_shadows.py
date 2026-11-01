"""T31 geometry-derived ground and contact shadow verification.

The CPU lane pins the fitted light frustum, texel stabilization, camera-relative
matrix and caster policy. The GPU lane drives the generated
``contact-shadow-probe`` fixture and compares the same camera with the contact
shadow on and off in Studio, Clay and Directional:

* changed ground pixels prove a visible contact cue;
* connected shadow regions prove disconnected casters stay disconnected instead
  of becoming one bounds ellipse;
* an enclosed bright hole inside a shadow region proves a suspended ring/strap
  loop preserves empty space;
* a weaker shadow region proves clear transmission does not cast a solid black
  silhouette;
* bounded changed-pixel coverage proves the floor is not broadly darkened.

It also checks the independent Ground highlight/grid flags, exposure and mode
independence, point/line-only content (no invented ellipse), resize,
generation change, the documented analytic fallback and device-loss rebuild.
External Car/Camera/STL same-camera pairs are captured for visual review.
"""
from __future__ import annotations

import json
import math
import os
import struct
import subprocess
import time
from pathlib import Path

from common import RESULT_SCHEMA, encoded_png, send_field, sha256_file, write_json
from viewer import Viewer

ROOT = Path(__file__).resolve().parents[2]
CORPUS = Path('D:/repos/binbuf/preview-3d-test-models')
FAST = bool(os.environ.get('RENDER_QUALITY_FAST'))

SERIAL_LANES = [
    ('unit-ground-shadow', 'Tests.Unit.exe', ['[ground-shadow]']),
    ('unit-directional-light', 'Tests.Unit.exe', ['[directional-light]']),
]

EXTERNAL = {
    'car': {
        'entry': 'Car.glb',
        'view': {'yawDegrees': 60.0, 'pitchDegrees': 35.0, 'distanceScale': 1.3},
        'regions': {'paint': [0.30, 0.45, 0.75, 0.92],
                    'front-tire': [0.12, 0.62, 0.38, 0.96],
                    'rear-tire': [0.62, 0.62, 0.90, 0.96]},
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
        'regions': {'relief': [0.2, 0.15, 0.8, 0.9],
                    'contact': [0.1, 0.55, 0.9, 0.98]},
    },
}


def _f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', float(value)))[0]


def _send(hwnd, field, value=0):
    return send_field(hwnd, field, value)


def _check(checks, name, passed, actual=None, expected=None, tolerance=None):
    checks.append({'name': name, 'pass': bool(passed), 'actual': actual,
                   'expected': expected, 'tolerance': tolerance})


def _set_lighting(viewer, mode):
    _send(viewer.hwnd, 88, mode)
    time.sleep(0.35)


def _set_sun(viewer, azimuth, elevation):
    _send(viewer.hwnd, 277, _f32_bits(azimuth))
    _send(viewer.hwnd, 278, _f32_bits(elevation))
    time.sleep(0.2)


def _set_shadow(viewer, enabled):
    _send(viewer.hwnd, 102, 1 if enabled else 0)
    time.sleep(0.3)


def _set_grid(viewer, enabled):
    _send(viewer.hwnd, 104, 1 if enabled else 0)
    time.sleep(0.2)


def _set_highlight(viewer, enabled):
    _send(viewer.hwnd, 196, 1 if enabled else 0)
    time.sleep(0.3)


def _set_exposure(viewer, value):
    _send(viewer.hwnd, 92, _f32_bits(value))
    time.sleep(0.3)


def _shadow_state(viewer):
    bytes_low = _send(viewer.hwnd, 281) & 0xFFFFFFFF
    bytes_high = _send(viewer.hwnd, 282) & 0xFFFFFFFF
    return {
        'mode': _send(viewer.hwnd, 279) & 0xFFFFFFFF,
        'fallbackReason': _send(viewer.hwnd, 280) & 0xFFFFFFFF,
        'allocationBytes': bytes_low | (bytes_high << 32),
        'casterCount': _send(viewer.hwnd, 283) & 0xFFFFFFFF,
        'mapSize': _send(viewer.hwnd, 284) & 0xFFFFFFFF,
        'passMicroseconds': _send(viewer.hwnd, 285) & 0xFFFFFFFF,
    }


def _save(viewer, output: Path, name: str):
    frame = viewer.wait_stable()
    path = output / 'images' / f'{name}.png'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
    return {'name': name, 'path': str(path), 'sha256': sha256_file(path),
            'stats': frame['stats'], 'camera': viewer.camera_state(),
            'shadow': _shadow_state(viewer), 'performance': viewer.performance()}


def _capture(viewer, output: Path, name: str, stable: bool = True):
    frame = viewer.wait_stable() if stable else viewer.capture()
    path = output / 'images' / f'{name}.png'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
    return {'frame': frame, 'path': str(path), 'sha256': sha256_file(path)}


def _shadow_pair(viewer, output: Path, name: str, stable: bool = True):
    _set_shadow(viewer, True)
    on = _capture(viewer, output, f'{name}-shadow-on', stable=stable)
    state_on = _shadow_state(viewer)
    _set_shadow(viewer, False)
    off = _capture(viewer, output, f'{name}-shadow-off', stable=stable)
    return {'on': on, 'off': off, 'stateOn': state_on}


def _analyse_pair(pair, threshold=5):
    on = pair['on']['frame']
    off = pair['off']['frame']
    if (on['width'], on['height']) != (off['width'], off['height']):
        return {'error': 'capture extent changed'}
    width, height = on['width'], on['height']
    a, b = on['rgb'], off['rgb']
    count = width * height
    deltas = bytearray(count)
    changed_pixels = 0
    sum_diff = 0
    max_diff = 0
    sum_on = 0
    sum_off = 0
    for pixel in range(count):
        base = pixel * 3
        delta = max(abs(a[base] - b[base]), abs(a[base + 1] - b[base + 1]),
                    abs(a[base + 2] - b[base + 2]))
        sum_on += a[base] + a[base + 1] + a[base + 2]
        sum_off += b[base] + b[base + 1] + b[base + 2]
        if delta > threshold:
            deltas[pixel] = delta
            changed_pixels += 1
            sum_diff += delta
            max_diff = max(max_diff, delta)
    components = _components(deltas, width, height)
    holes = _holes(deltas, width, height, components)
    means = sorted((item['meanDiff'] for item in components), reverse=True)
    return {
        'width': width, 'height': height, 'pixels': count,
        'changedPixels': changed_pixels,
        'changedFraction': round(changed_pixels / count, 5),
        'meanDiff': round(sum_diff / max(1, changed_pixels), 2),
        'maxDiff': max_diff,
        'meanLumaOn': round(sum_on / (3.0 * count), 2),
        'meanLumaOff': round(sum_off / (3.0 * count), 2),
        'componentCount': len(components),
        'components': [{k: v for k, v in item.items() if k != 'pixels'}
                       for item in components[:12]],
        'componentMeanDiffs': [round(value, 2) for value in means],
        'holes': holes[:8],
    }


def _components(deltas, width, height, minimum=24):
    visited = bytearray(len(deltas))
    result = []
    for start in range(len(deltas)):
        if not deltas[start] or visited[start]:
            continue
        stack = [start]
        visited[start] = 1
        area = 0
        total = 0
        min_x = width
        min_y = height
        max_x = 0
        max_y = 0
        while stack:
            index = stack.pop()
            area += 1
            total += deltas[index]
            x = index % width
            y = index // width
            min_x = min(min_x, x); max_x = max(max_x, x)
            min_y = min(min_y, y); max_y = max(max_y, y)
            for offset_y in (-1, 0, 1):
                ny = y + offset_y
                if ny < 0 or ny >= height:
                    continue
                row = ny * width
                for offset_x in (-1, 0, 1):
                    nx = x + offset_x
                    if nx < 0 or nx >= width or (offset_x == 0 and offset_y == 0):
                        continue
                    neighbour = row + nx
                    if deltas[neighbour] and not visited[neighbour]:
                        visited[neighbour] = 1
                        stack.append(neighbour)
        if area >= minimum:
            result.append({'area': area, 'bbox': [min_x, min_y, max_x, max_y],
                           'meanDiff': round(total / area, 2), 'pixels': None})
    result.sort(key=lambda item: item['area'], reverse=True)
    return result


def _holes(deltas, width, height, components, minimum=10):
    """Enclosed unchanged regions inside a shadow component's bounding box."""
    holes = []
    for component in components[:6]:
        min_x, min_y, max_x, max_y = component['bbox']
        box_width = max_x - min_x + 1
        box_height = max_y - min_y + 1
        local = bytearray(box_width * box_height)
        for y in range(box_height):
            source = (min_y + y) * width + min_x
            local[y * box_width:(y + 1) * box_width] = deltas[source:source + box_width]
        visited = bytearray(len(local))
        stack = []
        for x in range(box_width):
            for y in (0, box_height - 1):
                index = y * box_width + x
                if not local[index] and not visited[index]:
                    visited[index] = 1; stack.append(index)
        for y in range(box_height):
            for x in (0, box_width - 1):
                index = y * box_width + x
                if not local[index] and not visited[index]:
                    visited[index] = 1; stack.append(index)
        while stack:
            index = stack.pop()
            x = index % box_width
            y = index // box_width
            for offset_y in (-1, 0, 1):
                ny = y + offset_y
                if ny < 0 or ny >= box_height:
                    continue
                for offset_x in (-1, 0, 1):
                    nx = x + offset_x
                    if nx < 0 or nx >= box_width:
                        continue
                    neighbour = ny * box_width + nx
                    if not local[neighbour] and not visited[neighbour]:
                        visited[neighbour] = 1; stack.append(neighbour)
        enclosed = sum(1 for index in range(len(local))
                       if not local[index] and not visited[index])
        if enclosed >= minimum:
            holes.append({'componentArea': component['area'], 'area': enclosed,
                          'bbox': component['bbox']})
    return holes


def _check_pair(checks, label, metrics):
    if metrics.get('error'):
        _check(checks, f'{label} pair comparable', False, metrics['error'], 'same extent')
        return
    _check(checks, f'{label} shadow changes the ground visibly',
           metrics['changedPixels'] >= 0.002 * metrics['pixels'],
           metrics['changedPixels'], '>=0.2% of viewport')
    _check(checks, f'{label} shadow is not a broad floor sweep',
           metrics['changedFraction'] <= 0.35,
           metrics['changedFraction'], '<=0.35')
    _check(checks, f'{label} shadow darkening is bounded and visible',
           5.0 <= metrics['meanDiff'] <= 200.0,
           metrics['meanDiff'], '[5,200]')
    _check(checks, f'{label} disconnected casters stay disconnected',
           metrics['componentCount'] >= 3, metrics['componentCount'], '>=3')
    _check(checks, f'{label} a strap-loop/cutout hole survives',
           len(metrics['holes']) >= 1, metrics['holes'], '>=1 enclosed hole')
    means = metrics.get('componentMeanDiffs') or []
    if len(means) >= 2:
        _check(checks, f'{label} clear glass casts a weaker shadow than opaque geometry',
               means[-1] <= 0.7 * max(1.0, means[0]),
               [means[0], means[-1]], 'weakest <= 0.7 * strongest')
    else:
        _check(checks, f'{label} clear glass casts a weaker shadow than opaque geometry',
               False, means, 'two components')


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


def _external_survey(viewer, output: Path):
    from corpus import resolve_entry
    # Reopening the largest external assets after the generated lane can exceed
    # the default settle window on a cold detail cache. The broker's generation
    # deadline still bounds the load; this only lets the legitimate cache warm
    # without turning a slow load into a false shadow failure.
    viewer.settle_timeout = max(viewer.settle_timeout, 240.0)
    surveys = {}
    for name, spec in EXTERNAL.items():
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
        _set_grid(viewer, False)
        _set_highlight(viewer, False)
        viewer.set_camera(spec['view']['yawDegrees'], spec['view']['pitchDegrees'],
                          spec['view']['distanceScale'])
        record = {'status': 'sampled', 'entry': str(entry), 'modes': {}}
        for mode_name, mode in (('studio', 0), ('clay', 1), ('directional', 2)):
            _set_lighting(viewer, mode)
            if mode == 2:
                _set_sun(viewer, 0.86, 0.78)
            pair = _shadow_pair(viewer, output, f'external-{name}-{mode_name}')
            metrics = _analyse_pair(pair)
            record['modes'][mode_name] = {
                'on': {'path': pair['on']['path'], 'sha256': pair['on']['sha256'],
                       'stats': pair['on']['frame']['stats']},
                'off': {'path': pair['off']['path'], 'sha256': pair['off']['sha256']},
                'metrics': metrics, 'state': pair['stateOn'],
            }
        surveys[name] = record
    return surveys


def _check_external(checks, surveys):
    for asset, survey in surveys.items():
        if survey.get('status') != 'sampled':
            _check(checks, f'{asset} external observation recorded', False,
                   survey.get('status'), 'sampled')
            continue
        for mode_name, data in survey['modes'].items():
            metrics = data['metrics']
            _check(checks, f'{asset}/{mode_name} shadow pair captured',
                   metrics.get('changedPixels', 0) > 0,
                   metrics.get('changedPixels'), '>0')
            _check(checks, f'{asset}/{mode_name} shadow state is the map',
                   data['state'].get('mode') == 1, data['state'].get('mode'), 1)


def _write_report(output: Path, report) -> Path:
    lines = [
        '# T31 geometry-derived contact shadow report', '',
        'Generated from the CPU fitted-frustum/caster lanes and Debug GPU',
        'same-camera shadow on/off pairs in Studio, Clay and Directional.', '',
        '## Checks', '',
        f"- Status: **{report.get('status')}**",
        f"- Checks: {sum(1 for c in report.get('checks', []) if c['pass'])} passed / "
        f"{len(report.get('checks', []))} total",
        f"- Failures: {len(report.get('failures', []))}",
    ]
    for failure in report.get('failures', []):
        lines.append(f'  - {failure}')
    lines += ['', '## Generated shadow pairs', '',
              '| mode | changed | fraction | mean diff | components | holes |',
              '| --- | --- | --- | --- | --- | --- |']
    for mode, data in report.get('pairs', {}).items():
        metrics = data['metrics']
        lines.append(
            f"| {mode} | {metrics.get('changedPixels')} | "
            f"{metrics.get('changedFraction')} | {metrics.get('meanDiff')} | "
            f"{metrics.get('componentCount')} | {len(metrics.get('holes', []))} |")
    lines += ['', '## Captured review images', '']
    for entry in report.get('images', []):
        lines.append(f"- `{entry}`")
    for asset, survey in report.get('externalSurvey', {}).items():
        if survey.get('status') != 'sampled':
            lines.append(f"- **{asset}**: {survey.get('status')}")
            continue
        for mode_name, data in survey['modes'].items():
            lines.append(f"- **{asset}/{mode_name}**: `{data['on']['path']}` / "
                         f"`{data['off']['path']}`")
    lines += [
        '', '## Interpretation', '',
        '- Triangle geometry is rendered into a fitted 2048x2048 (1024 fallback)',
        '  orthographic light depth map. The bounds only size the frustum and the',
        '  ground quad; they are never the shadow silhouette.',
        '- The caster pass is depth-only plus one R8 attenuation value. BLEND',
        '  coverage/decal surfaces are excluded, MASK keeps its authored alpha',
        '  test, mirrored/double-sided geometry culls nothing, and transmissive',
        '  surfaces are scaled into a documented bounded range instead of',
        '  painting a solid black silhouette.',
        '- The contact shadow is premultiplied black coverage in the linear HDR',
        '  target; the Ground highlight remains a display-space backdrop layer.',
        '  Shadow, highlight and grid toggles stay independent.',
        '', '## Evidence', '',
        f"- `{output / 'diagnostics' / 'contact-shadows.json'}`",
        f"- `{output / 'images'}` same-camera on/off pairs",
        '', ]
    path = output / 'contact-shadow-report.md'
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def run(workspace: Path, output: Path, configuration: str, fast: bool = False):
    output = Path(output)
    report = {'schema': RESULT_SCHEMA, 'task': 'T31', 'checks': [], 'failures': [],
              'status': 'running', 'fast': fast}
    report['serialLanes'] = _run_serial(configuration, output)
    for lane in report['serialLanes']:
        if not lane.get('pass'):
            report['failures'].append(f"serial lane failed: {lane['name']}")

    fixture = output / 'fixtures' / 'contact-shadow-probe.glb'
    if not fixture.exists():
        report['failures'].append(f'contact-shadow-probe fixture missing: {fixture}')
        report['status'] = 'fail'
        write_json(output / 'diagnostics' / 'contact-shadows.json', report)
        return report

    images = []
    try:
        with Viewer(configuration, output, width=1280, height=860) as viewer:
            viewer.open(fixture)
            viewer.apply_controlled_presentation()
            _set_grid(viewer, False)
            _set_highlight(viewer, False)
            viewer.set_camera(25.0, 40.0, 1.05)
            report['generation'] = viewer.model_state()['generation']
            report['boundsDimensions'] = viewer.model_state()['dimensions']
            pair_results = {}
            for mode_name, mode in (('studio', 0), ('clay', 1), ('directional', 2)):
                _set_lighting(viewer, mode)
                if mode == 2:
                    # The documented Studio/Clay key direction expressed as a
                    # world sun: azimuth 0.86, elevation 0.78 is within a
                    # degree of (0.45, -0.55, 0.70). The implementation still
                    # consumes the global Directional sun; this only keeps the
                    # same-camera comparison framed like the other two modes.
                    _set_sun(viewer, 0.86, 0.78)
                pair = _shadow_pair(viewer, output, f'contact-{mode_name}')
                metrics = _analyse_pair(pair)
                pair_results[mode_name] = {
                    'on': {'path': pair['on']['path'], 'sha256': pair['on']['sha256'],
                           'stats': pair['on']['frame']['stats']},
                    'off': {'path': pair['off']['path'], 'sha256': pair['off']['sha256']},
                    'metrics': metrics, 'state': pair['stateOn'],
                }
                images.extend([pair['on']['path'], pair['off']['path']])
                _check_pair(report['checks'], mode_name, metrics)
                _check(report['checks'], f'{mode_name} uses the fitted shadow map',
                       pair['stateOn']['mode'] == 1, pair['stateOn'], 'mode 1')
                _check(report['checks'], f'{mode_name} map is bounded',
                       pair['stateOn']['mapSize'] in (2048, 1024),
                       pair['stateOn']['mapSize'], '2048 or 1024')
                _check(report['checks'], f'{mode_name} has bounded shadow targets',
                       pair['stateOn']['allocationBytes'] > 0
                       and pair['stateOn']['allocationBytes'] < 128 * 1024 * 1024,
                       pair['stateOn']['allocationBytes'], '(0,128MB)')
                _check(report['checks'], f'{mode_name} caster pass has geometry',
                       pair['stateOn']['casterCount'] >= 5,
                       pair['stateOn']['casterCount'], '>=5')
                _check(report['checks'], f'{mode_name} caster timing recorded',
                       pair['stateOn']['passMicroseconds'] >= 0,
                       pair['stateOn']['passMicroseconds'], '>=0')
            report['pairs'] = pair_results
            report['performance'] = viewer.performance()

            # T21a precedence: highlight off keeps the contact shadow (already
            # exercised above), and shadow off keeps the highlight.
            _set_lighting(viewer, 0)
            _set_shadow(viewer, False)
            _set_highlight(viewer, True)
            highlight_on = _capture(viewer, output, 'contact-highlight-on-shadow-off')
            _set_highlight(viewer, False)
            highlight_off = _capture(viewer, output, 'contact-highlight-off-shadow-off')
            highlight_metrics = _analyse_pair({'on': {'frame': highlight_on['frame']},
                                               'off': {'frame': highlight_off['frame']}})
            _check(report['checks'], 'highlight stays independent when shadows are off',
                   highlight_metrics.get('changedPixels', 0) > 1000,
                   highlight_metrics.get('changedPixels'), '>1000')
            report['highlightIndependence'] = highlight_metrics

            # Grid independence: hiding the grid never hides grounding.
            _set_shadow(viewer, True)
            _set_highlight(viewer, False)
            _set_grid(viewer, True)
            grid_pair = _shadow_pair(viewer, output, 'contact-grid-on')
            grid_metrics = _analyse_pair(grid_pair)
            _check(report['checks'], 'hidden grid does not remove grounding',
                   grid_metrics.get('changedPixels', 0) >= 0.002 * grid_metrics.get('pixels', 1),
                   grid_metrics.get('changedPixels'), '>=0.2%')
            _set_grid(viewer, False)
            report['gridIndependence'] = grid_metrics

            # Exposure/mode independence: the coverage layer cannot invert.
            exposure = {}
            for value in (0.5, 2.0):
                _set_exposure(viewer, value)
                pair = _shadow_pair(viewer, output, f'contact-exposure-{int(value*10):02d}')
                exposure[str(value)] = _analyse_pair(pair)
            counts = [entry.get('changedPixels', 0) for entry in exposure.values()]
            _check(report['checks'], 'contact shadow survives exposure changes',
                   all(count > 0.002 * entry.get('pixels', 1)
                       for count, entry in zip(counts, exposure.values())),
                   counts, '>-')
            if len(counts) == 2 and min(counts) > 0:
                ratio = max(counts) / min(counts)
                _check(report['checks'], 'contact coverage is exposure-stable',
                       0.6 <= ratio <= 1.6, round(ratio, 3), '[0.6,1.6]')
            report['exposure'] = exposure
            _set_exposure(viewer, 1.0)

            # Point/line-only content: no triangle silhouette, so no invented
            # bounds ellipse may appear.
            line_obj = output / 'fixtures' / 'line-point-scene.obj'
            if line_obj.exists():
                viewer.open(line_obj)
                viewer.apply_controlled_presentation()
                _set_grid(viewer, False)
                _set_highlight(viewer, False)
                # Line/point-only content is sparse; the canonical first-open
                # framing is stable, while a forced test camera can leave a
                # near-empty view that the stability heuristic rejects.
                time.sleep(0.6)
                _set_shadow(viewer, True)
                time.sleep(0.6)
                on = _capture(viewer, output, 'contact-line-point-shadow-on',
                              stable=False)
                state = _shadow_state(viewer)
                _set_shadow(viewer, False)
                time.sleep(0.6)
                off = _capture(viewer, output, 'contact-line-point-shadow-off',
                               stable=False)
                pair = {'on': on, 'off': off, 'stateOn': state}
                metrics = _analyse_pair(pair, threshold=2)
                _check(report['checks'], 'point/line-only content draws no ellipse',
                       metrics.get('changedPixels', 0) == 0,
                       metrics.get('changedPixels'), 0)
                _check(report['checks'], 'point/line-only content reports no caster',
                       state['mode'] == 0 and state['casterCount'] == 0,
                       state, 'mode 0, caster 0')
                report['linePoint'] = {'metrics': metrics, 'state': state}
            else:
                _check(report['checks'], 'point/line fixture available', False,
                       str(line_obj), 'exists')

            # Generation change: opening another asset then the contact fixture
            # rebuilds the map from the current drawable set.
            viewer.open(fixture)
            viewer.apply_controlled_presentation()
            _set_grid(viewer, False)
            _set_highlight(viewer, False)
            viewer.set_camera(25.0, 40.0, 1.05)
            second_generation = viewer.model_state()['generation']
            state = _shadow_state(viewer)
            _check(report['checks'], 'generation change rebuilds the shadow casters',
                   state['mode'] == 1 and state['casterCount'] >= 5,
                   {'generation': second_generation, 'state': state},
                   'map with casters')

            # Resize is viewport-only: the fitted map is not tied to the scene
            # target extent, and the next frame still samples it.
            before_resize_state = _shadow_state(viewer)
            viewer.resize(900, 700)
            resized = _capture(viewer, output, 'contact-after-resize')
            resized_state = _shadow_state(viewer)
            _check(report['checks'], 'resize keeps the fitted shadow map',
                   resized_state['mode'] == 1 and resized_state['mapSize'] in (2048, 1024),
                   resized_state, 'mode 1, bounded map')
            _check(report['checks'], 'resize keeps the same shadow allocation',
                   resized_state['allocationBytes'] == before_resize_state['allocationBytes'],
                   [before_resize_state['allocationBytes'],
                    resized_state['allocationBytes']], 'unchanged')
            _check(report['checks'], 'resized frame still renders the model',
                   resized['frame']['stats']['uniqueSampleColours'] >= 8,
                   resized['frame']['stats']['uniqueSampleColours'], '>=8')
            report['resize'] = {'state': resized_state,
                                'stats': resized['frame']['stats']}
            viewer.resize(1280, 860)

            # A superseded import must not leave its casters in the fitted map:
            # the depth pass always re-renders the currently published drawable
            # set. Request the contact fixture and the STL box back-to-back so
            # only the second generation can publish, then verify the map.
            stl_fixture = output / 'fixtures' / 'stl-box.stl'
            if stl_fixture.exists():
                first_request = viewer.request_open(fixture)
                second_request = viewer.request_open(stl_fixture)
                final_generation, _, _ = viewer.wait_open(second_request)
                cancelled_state = _shadow_state(viewer)
                _check(report['checks'], 'superseded import rebuilds from the published model',
                       final_generation == second_request
                       and cancelled_state['mode'] == 1
                       and cancelled_state['casterCount'] >= 1,
                       {'first': first_request, 'second': second_request,
                        'final': final_generation, 'state': cancelled_state},
                       'second generation with casters')
                report['cancelledGeneration'] = {
                    'first': first_request, 'second': second_request,
                    'final': final_generation, 'state': cancelled_state}
                viewer.open(fixture)
                viewer.apply_controlled_presentation()
                _set_grid(viewer, False)
                _set_highlight(viewer, False)
                viewer.set_camera(25.0, 40.0, 1.05)
            else:
                _check(report['checks'], 'STL fixture available for supersession', False,
                       str(stl_fixture), 'exists')

            # Documented analytic fallback (Debug force): mode 2, still a
            # visible contact cue, then back to the fitted map.
            _send(viewer.hwnd, 286, 1)
            time.sleep(0.4)
            fallback_state = _shadow_state(viewer)
            fallback_visible = _analyse_pair(
                _shadow_pair(viewer, output, 'contact-fallback'))
            _send(viewer.hwnd, 286, 0)
            _set_shadow(viewer, True)
            time.sleep(0.4)
            restored_state = _shadow_state(viewer)
            _check(report['checks'], 'forced low-quality fallback is recorded and visible',
                   fallback_state['mode'] == 2
                   and fallback_state['fallbackReason'] == 2
                   and fallback_visible.get('changedPixels', 0) > 1000,
                   {'state': fallback_state,
                    'changed': fallback_visible.get('changedPixels')},
                   'mode 2, visible')
            _check(report['checks'], 'fitted map returns after the fallback',
                   restored_state['mode'] == 1, restored_state, 'mode 1')
            report['fallback'] = {'forced': fallback_state,
                                  'restored': restored_state,
                                  'metrics': fallback_visible}

            # Device loss rebuilds the shadow targets on the new device.
            _set_shadow(viewer, True)
            viewer.wait_stable()
            before_recovery = _send(viewer.hwnd, 66) & 0xFFFFFFFF
            _send(viewer.hwnd, 65, 0)
            deadline = time.monotonic() + 90.0
            recovered = False
            while time.monotonic() < deadline:
                time.sleep(0.5)
                state_now = (_send(viewer.hwnd, 0) & 0xFFFFFFFF) - 1
                current = _shadow_state(viewer)
                if (state_now == 2 and current['mode'] == 1
                        and current['allocationBytes'] > 0):
                    recovered = True
                    break
            after_recovery = _send(viewer.hwnd, 66) & 0xFFFFFFFF
            reset = _capture(viewer, output, 'contact-after-device-reset')
            report['deviceReset'] = {
                'recovered': recovered,
                'recoveryCountBefore': before_recovery,
                'recoveryCountAfter': after_recovery,
                'state': _shadow_state(viewer),
                'stats': reset['frame']['stats'],
            }
            _check(report['checks'], 'device loss rebuilds the fitted shadow map',
                   recovered and reset['frame']['stats']['uniqueSampleColours'] >= 8,
                   report['deviceReset'], 'recovered and drawable')

            report['images'] = images
    except Exception as error:
        report['failures'].append(f'GPU contact-shadow lane failed: {error}')

    # External evidence runs in a fresh viewer process. Reopening the largest
    # assets after a long generated/readback session can leave the detail cache
    # unwilling to settle; a clean session proves the shadow path, not a stale
    # residency state from earlier evidence work.
    if not fast:
        try:
            with Viewer(configuration, output, width=1280, height=860) as ext_viewer:
                report['externalSurvey'] = _external_survey(ext_viewer, output)
            _check_external(report['checks'], report['externalSurvey'])
        except Exception as error:
            report['externalSurvey'] = {'status': 'failed', 'reason': str(error)}
            _check(report['checks'], 'external shadow survey completed', False,
                   str(error), 'sampled')
    else:
        report['externalSurvey'] = {}
        report['externalNote'] = 'skipped by the fast iteration lane'

    if not report.get('pairs'):
        report['failures'].append('no generated contact-shadow pairs were captured')
    failed = [check for check in report['checks'] if not check['pass']]
    if failed:
        report['failures'].append(f'{len(failed)} contact-shadow checks failed')
    report['status'] = 'pass' if not report['failures'] else 'fail'
    write_json(output / 'diagnostics' / 'contact-shadows.json', report)
    report['reportPath'] = str(_write_report(output, report))
    return report
