"""T26 bounded order-independent transparency verification.

The lane proves the product path is weighted blended OIT, not a sorted
fallback: a permuted draw/primitive order is pixel-identical under OIT while
the recorded sorted fallback changes. It also exercises intersecting triangles
inside one primitive, nested front/back glass, alpha decals, cutouts, emissive
blends, nearest-visible picking, resize/device-loss/low-memory behaviour and
the Car/Camera paired visual cases when the local corpus is available.
"""
from __future__ import annotations

import json
import struct
import subprocess
import time
from pathlib import Path

from common import (RESULT_SCHEMA, crop, encoded_png, image_stats, mae,
                    sha256_file, write_json)
from diagnostics import (_display_rgb, channel_supported, material_records,
                         sample_region, set_channel)
from fixtures import EXPECTED_LAYERED_GLASS
from viewer import Viewer

ROOT = Path(__file__).resolve().parents[2]
CORPUS_ROOT = Path('D:/repos/binbuf/preview-3d-test-models')

SERIAL_LANES = [
    ('unit-layered-transparency', 'Tests.Unit.exe', ['[layered-transparency]']),
    ('import-wire-validator', 'Tests.ImportIsolation.exe', ['[shared-section-validator]']),
    ('import-hostile-worker', 'Tests.ImportIsolation.exe', ['[hostile-worker]']),
]

# Display-space targets for locating roles from diagnostic channel 1. The
# cutout surface is base factor × authored texture; everything else is factor.
LOCATOR_BASE_COLOR = {
    'nested yellow': [0.92, 0.8, 0.12],
    'paint opaque': [0.22, 0.23, 0.25],
    'alpha decal': [0.85, 0.1, 0.6],
    # The authored 8-bit sRGB texture decodes to these linear values.
    'alpha cutout': [0.0212, 0.7156, 0.8148],
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


def _capture(viewer, path: Path):
    frame = viewer.wait_stable()
    path.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
    frame['path'] = str(path)
    frame['sha256'] = sha256_file(path)
    return frame


def _downsample(frame):
    from common import downsampled
    return downsampled(frame['width'], frame['height'], frame['rgb'])


def _stats(viewer):
    return {
        'algorithm': int(viewer_query(viewer, 243)),
        'fallbackReason': int(viewer_query(viewer, 244)),
        'layerLimit': int(viewer_query(viewer, 245)),
        'transparentDraws': int(viewer_query(viewer, 246)),
        'oitTargetBytes': int(viewer_query(viewer, 247)) | (int(viewer_query(viewer, 250)) << 32),
        'resolveUsedOit': bool(viewer_query(viewer, 252)),
        'accountedGpuBytes': viewer.performance()['accountedGpuBytes'],
        'meanFrameMs': viewer.performance()['meanFrameMs'],
        'p95FrameMs': viewer.performance()['p95FrameMs'],
    }


def viewer_query(viewer, field):
    from common import query
    return query(viewer.hwnd, field)


def _locate(frame, targets):
    """Return per-name bounding box, pixel count and a representative pixel.

    The representative pixel is the top-left covered pixel; it stays outside a
    centred decal when locating the paint beneath it.
    """
    converted = [(name, _display_rgb(color[:3])) for name, color in targets.items()]
    result = {name: {'box': None, 'count': 0, 'point': None} for name in targets}
    width, height, rgb = frame['width'], frame['height'], frame['rgb']
    for y in range(height):
        row = y * width
        for x in range(width):
            i = (row + x) * 3
            pixel = (rgb[i], rgb[i + 1], rgb[i + 2])
            best, best_distance = None, 1e9
            for name, target in converted:
                distance = sum((pixel[channel] - target[channel]) ** 2 for channel in range(3))
                if distance < best_distance:
                    best, best_distance = name, distance
            if best is None or best_distance > 45 * 45:
                continue
            entry = result[best]
            box = entry['box']
            if box is None:
                entry['box'] = [x, y, x, y]
            else:
                box[0] = min(box[0], x); box[1] = min(box[1], y)
                box[2] = max(box[2], x); box[3] = max(box[3], y)
            entry['count'] += 1
            if entry['point'] is None or (x, y) < entry['point']:
                entry['point'] = (x, y)
    for entry in result.values():
        if entry['count'] < 120:
            entry['box'] = None
            entry['point'] = None
    return result


def _box_contains(box, point):
    return (box is not None and point is not None
            and box[0] <= point[0] <= box[2] and box[1] <= point[1] <= box[3])


def _interior_point(frame, box, linear_color):
    """A covered pixel near the box centre, avoiding an anti-aliased edge."""
    if box is None:
        return None
    target = _display_rgb(linear_color[:3])
    width, rgb = frame['width'], frame['rgb']
    cx = (box[0] + box[2]) // 2
    cy = (box[1] + box[3]) // 2
    best, best_distance = None, 1e18
    for y in range(box[1], box[3] + 1):
        for x in range(box[0], box[2] + 1):
            i = (y * width + x) * 3
            pixel = (rgb[i], rgb[i + 1], rgb[i + 2])
            distance = sum((pixel[c] - target[c]) ** 2 for c in range(3))
            if distance > 30 * 30:
                continue
            center_distance = (x - cx) ** 2 + (y - cy) ** 2
            if center_distance < best_distance:
                best, best_distance = (x, y), center_distance
    return best


def _find_paint_hole(frame, paint_linear, cutout_box):
    """A paint pixel inside the cutout box is an alpha-cutout hole."""
    if not paint_linear or not cutout_box:
        return None
    target = _display_rgb(paint_linear[:3])
    width, rgb = frame['width'], frame['rgb']
    for y in range(cutout_box[1], cutout_box[3] + 1):
        for x in range(cutout_box[0], cutout_box[2] + 1):
            i = (y * width + x) * 3
            pixel = (rgb[i], rgb[i + 1], rgb[i + 2])
            if sum((pixel[c] - target[c]) ** 2 for c in range(3)) < 20 * 20:
                return (x, y)
    return None


def _mean_luma(width, height, rgb):
    if not rgb:
        return 0.0
    return sum(rgb[i] for i in range(0, len(rgb), 3)) / (len(rgb) // 3)


def _content_box(frame):
    """Bounding box of bright/colourful model pixels, excluding the backdrop."""
    width, height, rgb = frame['width'], frame['height'], frame['rgb']
    x0, y0, x1, y1 = width, height, -1, -1
    for y in range(0, height, 2):
        for x in range(0, width, 2):
            i = (y * width + x) * 3
            r, g, b = rgb[i], rgb[i + 1], rgb[i + 2]
            maximum = max(r, g, b)
            minimum = min(r, g, b)
            if maximum < 58:
                continue
            if maximum < 115 and maximum - minimum < 20:
                continue
            x0 = min(x0, x); y0 = min(y0, y)
            x1 = max(x1, x); y1 = max(y1, y)
    if x1 <= x0 or y1 <= y0:
        return [0.0, 0.0, 1.0, 1.0]
    margin_x = max(4, (x1 - x0) // 12)
    margin_y = max(4, (y1 - y0) // 12)
    left = max(0, x0 - margin_x) / width
    top = max(0, y0 - margin_y) / height
    right = min(width, x1 + margin_x) / width
    bottom = min(height, y1 + margin_y) / height
    return [left, top, right, bottom]


def _subset_fixture(source: Path, target: Path, node_name: str):
    data = source.read_bytes()
    at, document, binary = 12, None, None
    while at + 8 <= len(data):
        length, kind = struct.unpack_from('<II', data, at)
        at += 8
        chunk = data[at:at + length]
        at += length
        if kind == 0x4E4F534A:
            document = json.loads(chunk.decode('utf-8'))
        elif kind == 0x004E4942:
            binary = chunk
    if document is None:
        raise ValueError('fixture is not a GLB')
    index = next((i for i, node in enumerate(document.get('nodes', []))
                  if node.get('name') == node_name), None)
    if index is None:
        raise ValueError(f'fixture has no node {node_name}')
    document['scenes'][0]['nodes'] = [index]
    text = json.dumps(document, sort_keys=True, separators=(',', ':')).encode()
    text += b' ' * ((-len(text)) % 4)
    binary = (binary or b'') + b'\0' * ((-len(binary or b'')) % 4)
    payload = (struct.pack('<III', 0x46546C67, 2, 28 + len(text) + len(binary))
               + struct.pack('<II', len(text), 0x4E4F534A) + text
               + struct.pack('<II', len(binary), 0x004E4942) + binary)
    target.write_bytes(payload)
    return target


def _pick_any(viewer, frame, points):
    for x, y in points:
        picked = viewer.pick_at(x, y)
        if picked['hit'] and picked['id']:
            return picked
    return {'hit': False, 'id': 0, 'requests': 0}


def _pick_census(viewer, step: int = 25):
    """Census exact pick ids over the physical viewport.

    Scanning removes any dependence on the capture/DPI click mapping while
    still proving the pick target is the nearest-visible surface: ids that are
    hidden behind another surface must not appear at all.
    """
    from common import query, send_field
    left = viewer_query(viewer, 256) & 0xFFFF
    top = (viewer_query(viewer, 256) >> 16) & 0xFFFF
    right = viewer_query(viewer, 257) & 0xFFFF
    bottom = (viewer_query(viewer, 257) >> 16) & 0xFFFF
    width = max(1, right - left)
    height = max(1, bottom - top)
    x0 = left + width * 3 // 10
    x1 = left + width * 3 // 4
    y0 = top + height * 3 // 10
    y1 = top + height * 4 // 5
    census = {}
    for y in range(y0, max(y0 + 1, y1), step):
        for x in range(x0, max(x0 + 1, x1), step):
            before = query(viewer.hwnd, 254)
            send_field(viewer.hwnd, 258, (x & 0xFFFF) | ((y & 0xFFFF) << 16))
            deadline = time.monotonic() + 1.0
            while time.monotonic() < deadline and query(viewer.hwnd, 254) <= before:
                time.sleep(0.01)
            picked = query(viewer.hwnd, 251)
            if picked:
                census[picked] = census.get(picked, 0) + 1
    return census


def _expected_pick_id(records, base_color):
    target = [round(v, 3) for v in base_color]
    record = next((item for item in records
                   if [round(v, 3) for v in item.get('baseColor', [])] == target), None)
    return None if record is None else int(record['chunkId']) & 0xFFFF


def _check(check_list, name, passed, actual=None, expected=None):
    check_list.append({'name': name, 'pass': bool(passed), 'actual': actual,
                       'expected': expected})


def _run_generated(workspace: Path, output: Path, configuration: str, fast: bool):
    checks = []
    evidence = {}
    fixture = output / 'fixtures' / 'layered-glass.glb'
    permuted = output / 'fixtures' / 'layered-glass-permuted.glb'
    images = output / 'images'
    images.mkdir(parents=True, exist_ok=True)
    if not fixture.exists() or not permuted.exists():
        return checks, {'error': 'layered-glass fixtures missing'}

    with Viewer(configuration, output, width=1280, height=900, settle_timeout=120) as viewer:
        generation, load_seconds, _ = viewer.open(fixture)
        viewer.apply_controlled_presentation()
        viewer.set_camera(0.0, 90.0, 2.5)
        stats = _stats(viewer)
        evidence['stats'] = stats
        evidence['loadSeconds'] = round(load_seconds, 3)
        evidence['generation'] = generation
        _check(checks, 'OIT is the effective product algorithm', stats['algorithm'] == 1, stats)
        _check(checks, 'OIT has no fallback reason', stats['fallbackReason'] == 0,
               stats['fallbackReason'], 0)
        _check(checks, 'supported layer limit is recorded', stats['layerLimit'] >= 8,
               stats['layerLimit'], '>=8')
        _check(checks, 'transparent draws are non-empty', stats['transparentDraws'] >= 9,
               stats['transparentDraws'], '>=9')
        _check(checks, 'OIT targets consume bounded GPU memory', stats['oitTargetBytes'] > 0,
               stats['oitTargetBytes'], '>0')
        _check(checks, 'resolve consumed the OIT composite', stats['resolveUsedOit'], stats)
        _check(checks, 'OIT target bytes are accounted in the GPU budget',
               stats['oitTargetBytes'] <= stats['accountedGpuBytes'],
               [stats['oitTargetBytes'], stats['accountedGpuBytes']], 'target <= accounted')

        base = _capture(viewer, images / 'layered-oit.png')
        base_samples = _downsample(base)
        from common import send_field
        send_field(viewer.hwnd, 249, 1)
        perm = _capture(viewer, images / 'layered-oit-permuted.png')
        perm_delta = mae(base_samples, _downsample(perm))
        evidence['oitPermutationMae'] = perm_delta
        _check(checks, 'OIT is invariant to transparent draw order', perm_delta < 0.0005,
               round(perm_delta, 8), '<0.0005')
        send_field(viewer.hwnd, 249, 0)

        # Recorded sorted fallback must not be the usual high-quality path.
        send_field(viewer.hwnd, 248, 1)
        viewer.wait_stable()
        fallback_stats = _stats(viewer)
        evidence['fallbackStats'] = fallback_stats
        _check(checks, 'forced fallback is recorded, not silent',
               fallback_stats['algorithm'] == 0 and fallback_stats['fallbackReason'] == 2,
               fallback_stats)
        fallback = _capture(viewer, images / 'layered-sorted-fallback.png')
        fallback_samples = _downsample(fallback)
        send_field(viewer.hwnd, 249, 1)
        fallback_perm = _capture(viewer, images / 'layered-sorted-fallback-permuted.png')
        fallback_delta = mae(fallback_samples, _downsample(fallback_perm))
        evidence['fallbackPermutationMae'] = fallback_delta
        _check(checks, 'old sorted path is genuinely order-dependent',
               fallback_delta > 0.0001, round(fallback_delta, 8), '>0.0001')
        _check(checks, 'OIT is less order-sensitive than the sorted fallback',
               perm_delta * 10 < fallback_delta, [perm_delta, fallback_delta],
               'oit delta much smaller')
        send_field(viewer.hwnd, 249, 0)
        send_field(viewer.hwnd, 248, 0)
        viewer.wait_stable()

        # Small orbit sequence: identical under order permutation at every view.
        orbit = []
        for yaw, pitch in [(-8.0, 88.0), (0.0, 90.0), (8.0, 92.0)]:
            viewer.set_camera(yaw, pitch, 2.5)
            first = _capture(viewer, images / f'layered-orbit-{int(yaw)}-oit.png')
            send_field(viewer.hwnd, 249, 1)
            second = _capture(viewer, images / f'layered-orbit-{int(yaw)}-permuted.png')
            send_field(viewer.hwnd, 249, 0)
            delta = mae(_downsample(first), _downsample(second))
            orbit.append({'yaw': yaw, 'pitch': pitch, 'mae': delta,
                          'uniqueColours': first['stats']['uniqueSampleColours']})
            _check(checks, f'orbit {int(yaw)}° has no order-dependent popping',
                   delta < 0.0005, round(delta, 8), '<0.0005')
            _check(checks, f'orbit {int(yaw)}° is non-blank',
                   first['stats']['uniqueSampleColours'] >= 12, first['stats'])
        evidence['orbit'] = orbit
        viewer.set_camera(0.0, 90.0, 2.5)

        # Material roles and authored alpha modes.
        records = material_records(viewer.hwnd)
        evidence['materialRecords'] = len(records)
        matched = {}
        for expected in EXPECTED_LAYERED_GLASS:
            target = [round(v, 3) for v in expected['baseColor'][:4]]
            record = next((item for item in records
                           if [round(v, 3) for v in item.get('baseColor', [0, 0, 0, 0])] == target),
                          None)
            matched[expected['name']] = record is not None
            _check(checks, f"{expected['name']} material record present", record is not None,
                   record is not None, True)
            if record is not None:
                _check(checks, f"{expected['name']} alpha mode",
                       record['alphaMode'] == expected['alphaMode'],
                       record['alphaMode'], expected['alphaMode'])
        cutout = next((item for item in records
                       if [round(v, 3) for v in item.get('baseColor', [])] == [1.0, 1.0, 1.0, 1.0]
                       and item['alphaMode'] == 1), None)
        _check(checks, 'alpha cutout uses a base-color coverage map',
               cutout is not None and bool(cutout.get('mapMask', 0) & 1),
               cutout.get('mapMask') if cutout else None, 'bit 0 set')
        emissive = next((item for item in records
                         if [round(v, 3) for v in item.get('baseColor', [])]
                         == [0.12, 0.1, 0.08, 0.35]), None)
        _check(checks, 'emissive blended material keeps its emission',
               emissive is not None and emissive.get('emissiveStrength', 0) > 1.0,
               emissive.get('emissiveStrength') if emissive else None, '>1')

        # Locate roles from the isolated base-colour channel.
        set_channel(viewer.hwnd, 1)
        located_frame = viewer.wait_stable()
        located = _locate(located_frame, LOCATOR_BASE_COLOR)
        evidence['located'] = {name: entry['box'] for name, entry in located.items()}
        boxes = [entry['box'] for entry in located.values() if entry['box']]
        if boxes:
            x0 = max(0, min(box[0] for box in boxes) - 15)
            y0 = max(0, min(box[1] for box in boxes) - 15)
            x1 = min(located_frame['width'], max(box[2] for box in boxes) + 15)
            y1 = min(located_frame['height'], max(box[3] for box in boxes) + 15)
            content = [x0 / located_frame['width'], y0 / located_frame['height'],
                       x1 / located_frame['width'], y1 / located_frame['height']]
            for label, frame in (('oit', base), ('oit-permuted', perm),
                                 ('fallback', fallback),
                                 ('fallback-permuted', fallback_perm)):
                cw, ch, crgb = crop(frame['width'], frame['height'], frame['rgb'], content)
                (images / f'layered-content-{label}.png').write_bytes(
                    encoded_png(cw, ch, crgb))
            evidence['contentCrop'] = content
        for name in ('alpha decal', 'paint opaque', 'alpha cutout', 'nested yellow'):
            _check(checks, f'{name} visible in base-colour channel',
                   located.get(name, {}).get('point') is not None,
                   located.get(name, {}), 'located')
        decal_point = _interior_point(located_frame,
            located.get('alpha decal', {}).get('box'),
            LOCATOR_BASE_COLOR['alpha decal'])
        paint_point = located.get('paint opaque', {}).get('point')
        cutout_point = _interior_point(located_frame,
            located.get('alpha cutout', {}).get('box'),
            LOCATOR_BASE_COLOR['alpha cutout'])
        cutout_box = located.get('alpha cutout', {}).get('box')
        paint_box = located.get('paint opaque', {}).get('box')
        hole_point = _find_paint_hole(located_frame, LOCATOR_BASE_COLOR['paint opaque'],
                                      cutout_box)
        _check(checks, 'alpha cutout exposes paint through its holes', hole_point is not None,
               hole_point, 'paint pixel inside cutout box')
        set_channel(viewer.hwnd, 19)
        emissive_frame = viewer.wait_stable()
        emissive_located = _locate(emissive_frame, {'emissive blend': [4.05, 0.81, 0.225]})
        emissive_point = _interior_point(emissive_frame,
            emissive_located.get('emissive blend', {}).get('box'), [4.05, 0.81, 0.225])
        _check(checks, 'emissive blend visible in the emission channel',
               emissive_point is not None, emissive_located.get('emissive blend'), 'located')

        # Numeric evidence on the effective-input channels.
        def sample(point, channel):
            if point is None:
                return None
            rect = located_frame['rect']
            x = rect['left'] + point[0]
            y = rect['top'] + point[1]
            set_channel(viewer.hwnd, channel)
            return sample_region(viewer.hwnd, x - 1, y - 1, x + 1, y + 1).get('rgba')

        coverage = sample(decal_point, 9)
        evidence['decalCoverage'] = coverage
        _check(checks, 'decal keeps its authored low coverage',
               coverage is not None and 0.2 < coverage[0] < 0.42,
               coverage[0] if coverage else None, '~0.3')
        nested_point = _interior_point(located_frame,
            located.get('nested yellow', {}).get('box'), LOCATOR_BASE_COLOR['nested yellow'])
        nested_coverage = sample(nested_point, 9)
        _check(checks, 'nested front glass keeps its authored coverage',
               nested_coverage is not None and 0.3 < nested_coverage[0] < 0.5,
               nested_coverage[0] if nested_coverage else None, '~0.4')
        transmission = sample(decal_point, 10)
        _check(checks, 'coverage and transmission stay separate inputs',
               transmission is not None and transmission[0] < 0.01,
               transmission, 'transmission 0')
        composite = sample(decal_point, 22)
        evidence['decalCompositeInputs'] = composite
        _check(checks, 'diagnostic channel exposes coverage/reflection/transmission',
               composite is not None and len(composite) == 4
               and abs(composite[0] - 0.3) < 0.08 and composite[2] < 0.01,
               composite, '(alpha~0.3, glassReflectance, transmission~0)')
        emission = sample(emissive_point, 19)
        evidence['emissiveChannel'] = emission
        _check(checks, 'emissive blend contributes linear HDR emission',
               emission is not None and emission[0] > 0.5,
               emission[:3] if emission else None, '>0.5 linear red')

        # Nearest-visible picking is verified by scanning exact integer ids over
        # the physical viewport. Hidden surfaces must not appear at all; this is
        # independent of the capture/DPI click mapping.
        set_channel(viewer.hwnd, 0)
        viewer.wait_stable()
        def pick_id(color, alpha_mode=None):
            target = [round(v, 3) for v in color]
            for item in records:
                if [round(v, 3) for v in item.get('baseColor', [])] == target:
                    if alpha_mode is None or item.get('alphaMode') == alpha_mode:
                        return int(item['chunkId']) & 0xFFFF
            return None

        decal_id = pick_id([0.85, 0.1, 0.6, 0.3])
        paint_id = pick_id([0.22, 0.23, 0.25, 1.0])
        cutout_id = pick_id([1.0, 1.0, 1.0, 1.0], 1)
        emissive_id = pick_id([0.12, 0.1, 0.08, 0.35])
        nested_front_id = pick_id([0.92, 0.8, 0.12, 0.4])
        nested_back_ids = [pick_id([0.9, 0.25, 0.12, 0.4]),
                           pick_id([0.12, 0.72, 0.25, 0.4]),
                           pick_id([0.12, 0.3, 0.92, 0.4])]
        hidden_id = pick_id([0.2, 0.4, 0.95, 0.5])
        census = _pick_census(viewer)
        evidence['pickCensus'] = census
        _check(checks, 'pick census contains exact integer ids',
               census and all(isinstance(key, int) and key != 0 for key in census),
               census, 'non-zero integer ids')
        _check(checks, 'nearest low-coverage decal wins over paint',
               decal_id in census and paint_id in census and decal_id != paint_id,
               [decal_id, paint_id, census.get(decal_id), census.get(paint_id)],
               'both visible, distinct ids')
        _check(checks, 'cutout covered pixels and holes both resolve exactly',
               cutout_id in census and paint_id in census,
               [cutout_id, paint_id, census.get(cutout_id), census.get(paint_id)],
               'cutout and paint ids')
        _check(checks, 'hidden glass behind opaque paint is never picked',
               hidden_id is not None and hidden_id not in census,
               [hidden_id, hidden_id in census], 'hidden id absent')
        _check(checks, 'nested front glass is nearest; back layers are hidden',
               nested_front_id in census
               and all(value not in census for value in nested_back_ids if value),
               [nested_front_id, nested_back_ids, census],
               'front id present, back ids absent')
        _check(checks, 'emissive blended geometry is pickable by identity',
               emissive_id in census, [emissive_id, census.get(emissive_id)],
               'emissive id present')

        # The census is deterministic under OIT permutation and the fallback.
        send_field(viewer.hwnd, 249, 1)
        permuted_census = _pick_census(viewer)
        send_field(viewer.hwnd, 249, 0)
        send_field(viewer.hwnd, 248, 1)
        viewer.wait_stable()
        fallback_census = _pick_census(viewer)
        send_field(viewer.hwnd, 248, 0)
        viewer.wait_stable()
        evidence['pickCensusPermuted'] = permuted_census
        evidence['pickCensusFallback'] = fallback_census
        required_ids = [value for value in
                        (decal_id, paint_id, cutout_id, emissive_id, nested_front_id)
                        if value]
        _check(checks, 'picking is deterministic under draw-order permutation',
               all(key in permuted_census for key in required_ids)
               and (hidden_id is None or hidden_id not in permuted_census),
               permuted_census, required_ids)
        _check(checks, 'picking is deterministic on the sorted fallback',
               all(key in fallback_census for key in required_ids)
               and (hidden_id is None or hidden_id not in fallback_census),
               fallback_census, required_ids)

        # Resize / cancellation / device loss / low-memory fallback.
        before_bytes = _stats(viewer)['oitTargetBytes']
        viewer.resize(900, 700)
        after_resize_bytes = _stats(viewer)['oitTargetBytes']
        resized = _capture(viewer, images / 'layered-after-resize.png')
        viewer.resize(1280, 900)
        restored_bytes = _stats(viewer)['oitTargetBytes']
        evidence['resize'] = {'before': before_bytes, 'small': after_resize_bytes,
                              'restored': restored_bytes,
                              'uniqueColours': resized['stats']['uniqueSampleColours']}
        _check(checks, 'resize rebuilds bounded OIT targets',
               after_resize_bytes > 0 and restored_bytes > 0
               and after_resize_bytes != before_bytes,
               evidence['resize'], 'target bytes track extent')
        _check(checks, 'resized frame still renders transparency',
               resized['stats']['uniqueSampleColours'] >= 10, resized['stats'])

        # Supersede generation: request the permuted twin immediately after the
        # base fixture. The final model must be the second generation and keep
        # OIT active.
        first_generation = viewer.request_open(fixture)
        second_generation = viewer.request_open(permuted)
        final_generation, _, _ = viewer.wait_open(second_generation)
        evidence['cancelledGeneration'] = {
            'first': first_generation, 'second': second_generation,
            'final': final_generation,
        }
        _check(checks, 'superseded import keeps the final generation drawable',
               final_generation == second_generation and _stats(viewer)['algorithm'] == 1,
               evidence['cancelledGeneration'], second_generation)

        from common import send_field as send
        send(viewer.hwnd, 65, 0)
        deadline = time.monotonic() + 90.0
        recovered = False
        while time.monotonic() < deadline:
            time.sleep(0.5)
            state = viewer_query(viewer, 0) - 1
            if state == 2 and _stats(viewer)['algorithm'] == 1 \
                    and _stats(viewer)['oitTargetBytes'] > 0:
                recovered = True
                break
        reset_frame = _capture(viewer, images / 'layered-after-device-reset.png')
        evidence['deviceReset'] = {'recovered': recovered,
                                   'stats': reset_frame['stats']}
        _check(checks, 'device loss rebuilds OIT and the model',
               recovered and reset_frame['stats']['uniqueSampleColours'] >= 10,
               evidence['deviceReset'])

    # Low-memory allocation failure is a real fallback branch, exercised by the
    # Debug-only startup switch.
    low_memory = {'status': 'not-run'}
    try:
        environment = dict(__import__('os').environ)
        environment['PREVIEW3D_TEST_OIT_ALLOCATION_FAILURE'] = '1'
        with Viewer(configuration, output, width=1000, height=800,
                    environment=environment) as viewer:
            viewer.open(output / 'fixtures' / 'layered-glass.glb')
            viewer.apply_controlled_presentation()
            viewer.set_camera(0.0, 90.0, 2.5)
            frame = _capture(viewer, images / 'layered-low-memory-fallback.png')
            state = _stats(viewer)
            low_memory = {'status': 'ran', 'algorithm': state['algorithm'],
                          'reason': state['fallbackReason'],
                          'targetBytes': state['oitTargetBytes'],
                          'stats': frame['stats']}
            _check(checks, 'OIT allocation failure selects the recorded fallback',
                   state['algorithm'] == 0 and state['fallbackReason'] == 1,
                   state, 'sorted fallback / allocation reason')
            _check(checks, 'low-memory fallback still renders geometry',
                   frame['stats']['uniqueSampleColours'] >= 10, frame['stats'])
    except Exception as error:
        low_memory = {'status': 'error', 'error': str(error)}
        _check(checks, 'low-memory fallback viewer ran', False, str(error), 'ran')
    evidence['lowMemory'] = low_memory

    return checks, evidence


def _external_cases(output: Path, configuration: str):
    """Car/Camera paired cases when the restricted local corpus is available."""
    checks = []
    evidence = {'status': 'unavailable'}
    if not CORPUS_ROOT.exists():
        return checks, evidence
    import corpus
    inventory = corpus.inventory(CORPUS_ROOT)
    assets = inventory.get('seedAssets', {})
    car = assets.get('car') or {}
    camera = assets.get('camera') or {}
    if not car.get('available') or not camera.get('available'):
        return checks, evidence
    images = output / 'images'
    images.mkdir(parents=True, exist_ok=True)
    evidence = {'status': 'ran', 'car': {}, 'camera': {}}

    with Viewer(configuration, output, width=1280, height=900, settle_timeout=180) as viewer:
        from common import send_field
        viewer.open(Path(car['entry']))
        viewer.apply_controlled_presentation()
        car_results = []
        for yaw, pitch, name in [(0.0, 90.0, 'front'), (30.0, 60.0, 'oblique')]:
            viewer.set_camera(yaw, pitch, 1.0)
            base = _capture(viewer, images / f'car-{name}-oit.png')
            base_samples = _downsample(base)
            send_field(viewer.hwnd, 249, 1)
            permuted = _capture(viewer, images / f'car-{name}-oit-permuted.png')
            permuted_delta = mae(base_samples, _downsample(permuted))
            send_field(viewer.hwnd, 249, 0)
            send_field(viewer.hwnd, 248, 1)
            fallback = _capture(viewer, images / f'car-{name}-sorted-fallback.png')
            fallback_delta = mae(base_samples, _downsample(fallback))
            send_field(viewer.hwnd, 248, 0)
            viewer.wait_stable()
            # Content-adaptive crop: the evidence window position/DPI varies
            # between lanes, so fixed normalized regions can miss the model.
            content = _content_box(base)
            windshield = [0.08, 0.0, 0.95, 0.55]
            cw, ch, crgb = crop(base['width'], base['height'], base['rgb'], content)
            cfw, cfh, cfrgb = crop(fallback['width'], fallback['height'],
                                   fallback['rgb'], content)
            (images / f'car-{name}-content-oit.png').write_bytes(
                encoded_png(cw, ch, crgb))
            (images / f'car-{name}-content-fallback.png').write_bytes(
                encoded_png(cfw, cfh, cfrgb))
            ow, oh, orgb = crop(cw, ch, crgb, windshield)
            fw, fh, frgb = crop(cfw, cfh, cfrgb, windshield)
            (images / f'car-{name}-windshield-oit.png').write_bytes(
                encoded_png(ow, oh, orgb))
            (images / f'car-{name}-windshield-fallback.png').write_bytes(
                encoded_png(fw, fh, frgb))
            result = {'view': name, 'permutationMae': permuted_delta,
                      'fallbackMae': fallback_delta,
                      'windshieldOitLuma': round(_mean_luma(ow, oh, orgb), 2),
                      'windshieldFallbackLuma': round(_mean_luma(fw, fh, frgb), 2)}
            car_results.append(result)
            _check(checks, f'Car {name} windshield has no order popping',
                   permuted_delta < 0.0005, round(permuted_delta, 8), '<0.0005')
            _check(checks, f'Car {name} OIT preserves windshield exposure',
                   abs(result['windshieldOitLuma'] - result['windshieldFallbackLuma']) < 0.5,
                   result, '<0.5 luma')
        evidence['car'] = car_results

        viewer.open(Path(camera['entry']))
        viewer.apply_controlled_presentation()
        viewer.set_camera(0.0, 90.0, 1.0)
        base = _capture(viewer, images / 'camera-lens-oit.png')
        send_field(viewer.hwnd, 249, 1)
        permuted = _capture(viewer, images / 'camera-lens-oit-permuted.png')
        permuted_delta = mae(_downsample(base), _downsample(permuted))
        send_field(viewer.hwnd, 249, 0)
        send_field(viewer.hwnd, 248, 1)
        fallback = _capture(viewer, images / 'camera-lens-sorted-fallback.png')
        send_field(viewer.hwnd, 248, 0)
        viewer.wait_stable()
        content = _content_box(base)
        lens = [0.3, 0.25, 0.95, 0.95]
        cw, ch, crgb = crop(base['width'], base['height'], base['rgb'], content)
        cfw, cfh, cfrgb = crop(fallback['width'], fallback['height'],
                               fallback['rgb'], content)
        (images / 'camera-content-oit.png').write_bytes(encoded_png(cw, ch, crgb))
        (images / 'camera-content-fallback.png').write_bytes(
            encoded_png(cfw, cfh, cfrgb))
        ow, oh, orgb = crop(cw, ch, crgb, lens)
        fw, fh, frgb = crop(cfw, cfh, cfrgb, lens)
        (images / 'camera-lens-crop-oit.png').write_bytes(encoded_png(ow, oh, orgb))
        (images / 'camera-lens-crop-fallback.png').write_bytes(encoded_png(fw, fh, frgb))
        camera_result = {
            'permutationMae': permuted_delta,
            'lensOitLuma': round(_mean_luma(ow, oh, orgb), 2),
            'lensFallbackLuma': round(_mean_luma(fw, fh, frgb), 2),
        }
        evidence['camera'] = camera_result
        _check(checks, 'Camera lens is stable under order permutation',
               permuted_delta < 0.0005, round(permuted_delta, 8), '<0.0005')
        _check(checks, 'Camera lens behaviour is preserved from the sorted path',
               abs(camera_result['lensOitLuma'] - camera_result['lensFallbackLuma']) < 0.5,
               camera_result, '<0.5 luma')
    return checks, evidence


def write_report(output: Path, report):
    lines = [
        '# T26 bounded order-independent transparency report',
        '',
        'Algorithm: weighted blended OIT (McGuire-Bavoil family) in linear HDR,',
        'resolved before tone mapping. Two fixed RGBA16F targets (weighted colour',
        'and revealage) and one transparent geometry pass. No per-pixel linked',
        'list, peel count or unbounded layer allocation.',
        '',
        f"- Status: **{report.get('status')}**",
        f"- Checks: {sum(1 for c in report.get('checks', []) if c['pass'])} passed / "
        f"{len(report.get('checks', []))} total",
        f"- Failures: {len(report.get('failures', []))}",
    ]
    for failure in report.get('failures', []):
        lines.append(f'  - {failure}')
    evidence = report.get('generated', {})
    stats = evidence.get('stats', {})
    lines += [
        '', '## Bounds and fallback', '',
        f"- Supported validated overlap range: {stats.get('layerLimit', 'n/a')} layers.",
        f"- OIT target bytes: {stats.get('oitTargetBytes', 'n/a')} (accounted GPU bytes "
        f"{stats.get('accountedGpuBytes', 'n/a')}).",
        f"- Transparent draws: {stats.get('transparentDraws', 'n/a')}.",
        f"- Mean/p95 frame ms in this capture: {stats.get('meanFrameMs', 'n/a')} / "
        f"{stats.get('p95FrameMs', 'n/a')}.",
        '- The sorted fallback is selected only when the OIT targets cannot be',
        '  allocated or a Debug test forces it; it is recorded in diagnostics',
        '  fields 243/244 and is never the normal high-quality path.',
        '- Beyond the validated range the resolve remains order-independent but',
        '  is a weighted average, not an exact front-to-back composite.',
        '', '## Evidence', '',
        f"- OIT permutation MAE: {evidence.get('oitPermutationMae', 'n/a')}",
        f"- Sorted fallback permutation MAE: {evidence.get('fallbackPermutationMae', 'n/a')}",
        f"- Low-memory fallback: {evidence.get('lowMemory', {})}",
        f"- Resize: {evidence.get('resize', {})}",
        f"- Device reset: {evidence.get('deviceReset', {})}",
        f"- Car: {report.get('external', {}).get('car', 'unavailable')}",
        f"- Camera: {report.get('external', {}).get('camera', 'unavailable')}",
        f"- Images: `{output / 'images'}`",
        '', '## Interpretation', '',
        '- OIT is order-independent within floating-point noise; the deliberately',
        '  permuted fixture and the runtime reversed draw order both resolve the',
        '  same image. The sorted fallback changes under the same permutation,',
        '  which is the regression the old implementation failed.',
        '- Transmission/reflection inputs remain separate material terms and',
        '  diagnostic channel 22 still exposes coverage, reflectance and',
        '  transmission. OIT does not add refraction; T27 owns the transmitted',
        '  scene sample, tint and rough refraction.',
        '- Selection writes an exact integer id with depth, never an averaged',
        '  id. The nearest visible fragment wins; alpha cutouts and low-coverage',
        '  decals are tested explicitly.',
        '',
    ]
    path = output / 'layered-transparency-report.md'
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def run(workspace: Path, output: Path, configuration: str, fast: bool = False):
    output = Path(output)
    report = {'schema': RESULT_SCHEMA, 'task': 'T26', 'checks': [], 'failures': [],
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
        report['failures'].append(f'generated transparency lane failed: {error}')
        report['generated'] = {'error': str(error)}

    if not fast:
        try:
            external_checks, external = _external_cases(output, configuration)
            report['checks'].extend(external_checks)
            report['external'] = external
        except Exception as error:
            report['failures'].append(f'external paired case lane failed: {error}')
            report['external'] = {'status': 'error', 'error': str(error)}
    else:
        report['external'] = {'status': 'skipped', 'reason': 'fast iteration lane'}

    failed = [check for check in report['checks'] if not check['pass']]
    if failed:
        report['failures'].append(f'{len(failed)} transparency checks failed')
    report['status'] = 'pass' if not report['failures'] else 'fail'
    write_json(output / 'diagnostics/layered-transparency.json', report)
    report['reportPath'] = str(write_report(output, report))
    return report
