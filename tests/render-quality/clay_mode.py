"""T29 Clay geometry-review verification.

Two parts:

1. serial unit lanes for the material/tonemap/shadows contracts the override
   touches, then
2. a Debug GPU session on the generated ``clay-review`` fixture. It proves the
   effective draw bucket/depth state (not just a shader colour), the MASK
   cutout policy, opaque hiding of transparent interiors, neutral matte shading,
   mirrored/small/mixed-primitive visibility, picking and a
   Studio->Clay->Directional->Studio round-trip that leaves imported data,
   texture residency, exposure and camera untouched.

The profile's capture lane supplies controlled generated and external crops.
This module never promotes a baseline and never treats a missing external asset
as a pass.
"""
from __future__ import annotations

import subprocess
import time
from pathlib import Path

from common import RESULT_SCHEMA, decode_png, encoded_png, image_stats, write_json
from diagnostics import _display_rgb, material_records, set_channel
from viewer import Viewer

ROOT = Path(__file__).resolve().parents[2]

SERIAL_LANES = [
    ('unit-clay-studio-contract', 'Tests.Unit.exe', ['[studio-lighting]']),
    ('unit-clay-tonemap-presets', 'Tests.Unit.exe', ['[tonemap],[studio-presets]']),
]

# Distinct base colours let the isolated base-colour channel locate each role in
# the Studio frame before the mode is switched. Clay replaces every base colour
# with the same neutral matte, so centroids are collected while Studio is
# selected and reused at the same camera.
ROLES = [
    {'name': 'identity red metal', 'baseColor': [0.78, 0.05, 0.04]},
    {'name': 'identity blue emission', 'baseColor': [0.04, 0.06, 0.72]},
    {'name': 'identity green normal', 'baseColor': [0.05, 0.62, 0.10]},
    {'name': 'identity neutral', 'baseColor': [0.55, 0.50, 0.46]},
    {'name': 'cutout sheet', 'baseColor': [0.64, 0.64, 0.64], 'tolerance': 32},
    {'name': 'glass sheet', 'baseColor': [0.92, 0.55, 0.10]},
    {'name': 'overlap near', 'baseColor': [0.75, 0.15, 0.75]},
    {'name': 'overlap far', 'baseColor': [0.15, 0.75, 0.25]},
    {'name': 'opaque transmissive', 'baseColor': [0.90, 0.90, 0.20]},
    {'name': 'small one', 'baseColor': [0.80, 0.25, 0.20]},
    {'name': 'small two', 'baseColor': [0.20, 0.55, 0.80]},
    {'name': 'small three', 'baseColor': [0.25, 0.80, 0.35]},
    {'name': 'mirrored', 'baseColor': [0.55, 0.30, 0.85]},
]

IDENTITY = ['identity red metal', 'identity blue emission',
            'identity green normal', 'identity neutral']

EXTERNAL_CLAY_CROPS = {
    'car': ['windshield', 'paint'],
    'camera': ['lens', 'black-body'],
    'microscope': ['brass', 'enamel'],
    'stl': ['box'],
    'lid': ['box'],
    'cartridge': ['box'],
}


def _progress(output, message):
    line = f'{time.strftime("%H:%M:%S")} {message}'
    print(line, flush=True)
    try:
        path = Path(output) / 'diagnostics' / 'progress.log'
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('a', encoding='utf-8') as log:
            log.write(line + '\n')
    except OSError:
        pass


def _check(checks, name, passed, actual=None, expected=None, tolerance=None):
    checks.append({'name': name, 'pass': bool(passed), 'actual': actual,
                   'expected': expected, 'tolerance': tolerance})


def _set_lighting(viewer, mode):
    from common import send_field
    send_field(viewer.hwnd, 88, mode)
    time.sleep(0.35)


def _luma(rgb):
    return None if not rgb else 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]


def _chroma(rgb):
    return None if not rgb else max(rgb) - min(rgb)


def _pixel(frame, point):
    if point is None:
        return None
    x, y = point
    x = max(0, min(frame['width'] - 1, int(round(x))))
    y = max(0, min(frame['height'] - 1, int(round(y))))
    index = (y * frame['width'] + x) * 3
    return [frame['rgb'][index], frame['rgb'][index + 1], frame['rgb'][index + 2]]


def _locate(frame, roles):
    """Bounding box + centroid of each role in an isolated base-colour frame."""
    targets = [(_display_rgb(role['baseColor'][:3]),
                role.get('tolerance', 44), index)
               for index, role in enumerate(roles)]
    sums = [[0, 0, 0] for _ in roles]
    boxes = [[10 ** 9, 10 ** 9, -1, -1] for _ in roles]
    width, height, rgb = frame['width'], frame['height'], frame['rgb']
    for y in range(height):
        row = y * width
        for x in range(width):
            i = (row + x) * 3
            pixel = (rgb[i], rgb[i + 1], rgb[i + 2])
            best, best_distance, best_tolerance = -1, 1e9, 0
            for target, tolerance, owner in targets:
                distance = ((pixel[0] - target[0]) ** 2 + (pixel[1] - target[1]) ** 2
                            + (pixel[2] - target[2]) ** 2)
                if distance < best_distance:
                    best, best_distance, best_tolerance = owner, distance, tolerance
            if best < 0 or best_distance > best_tolerance * best_tolerance:
                continue
            sums[best][0] += x
            sums[best][1] += y
            sums[best][2] += 1
            box = boxes[best]
            box[0] = min(box[0], x); box[1] = min(box[1], y)
            box[2] = max(box[2], x); box[3] = max(box[3], y)
    located = {}
    for role, (sx, sy, count), box in zip(roles, sums, boxes):
        located[role['name']] = {
            'count': count,
            'centroid': (sx // count, sy // count) if count >= 40 else None,
            'box': box if count >= 40 else None,
        }
    return located


def _record_summary(records):
    triangles = [r for r in records if not r['points'] and not r['lines']]
    return {
        'total': len(records),
        'triangles': len(triangles),
        'blend': sum(1 for r in triangles if r['alphaMode'] == 2),
        'mask': sum(1 for r in triangles if r['alphaMode'] == 1),
        'transmissive': sum(1 for r in records if r['authoredBucket'] == 3),
        'overridden': sum(1 for r in records if r['clayOverride']),
        'lines': sum(1 for r in records if r['lines']),
        'points': sum(1 for r in records if r['points']),
    }


def _run_serial(configuration, output, fast):
    executable_dir = ROOT / f'x64/{configuration}'
    lanes = []
    for name, executable, filters in SERIAL_LANES:
        if fast and name != 'unit-clay-studio-contract':
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


def _check_draw_state(checks, studio_records, clay_records):
    studio = _record_summary(studio_records)
    clay = _record_summary(clay_records)
    _check(checks, 'fixture exposes authored BLEND draws', studio['blend'] > 0,
           studio['blend'], '>0')
    _check(checks, 'fixture exposes a MASK cutout', studio['mask'] > 0,
           studio['mask'], '>0')
    _check(checks, 'fixture exposes OPAQUE transmissive draws',
           studio['transmissive'] > 0, studio['transmissive'], '>0')
    # Studio keeps the authored buckets; Clay rewrites every triangle to opaque
    # (MASK stays a cutout) and turns depth writes on.
    authored_ok = all(r['bucket'] == r['authoredBucket'] and not r['clayOverride']
                      for r in studio_records)
    _check(checks, 'Studio keeps authored draw buckets', authored_ok,
           [(r['bucket'], r['authoredBucket']) for r in studio_records
            if r['bucket'] != r['authoredBucket'] or r['clayOverride']][:5], 'none')
    clay_bad = [r for r in clay_records if not r['points'] and not r['lines']
                and (r['bucket'] != (1 if r['alphaMode'] == 1 else 0)
                     or not r['depthWrite'])]
    _check(checks, 'Clay makes every triangle opaque with depth writes',
           not clay_bad,
           [(r['chunkId'], r['bucket'], r['depthWrite']) for r in clay_bad][:5],
           'all triangles bucket 0 (MASK 1), depthWrite true')
    _check(checks, 'Clay overrides authored BLEND/transmission',
           clay['overridden'] == sum(1 for r in studio_records
                                     if r['authoredBucket'] in (2, 3)),
           clay['overridden'], sum(1 for r in studio_records
                                   if r['authoredBucket'] in (2, 3)))
    mask_records = [r for r in clay_records if r['alphaMode'] == 1
                    and not r['points'] and not r['lines']]
    _check(checks, 'Clay MASK cutout keeps its bucket and depth write',
           bool(mask_records) and all(r['bucket'] == 1 and r['depthWrite']
                                      for r in mask_records),
           [(r['bucket'], r['depthWrite']) for r in mask_records], '[(1, true)]')
    return {'studio': studio, 'clay': clay}


def _check_neutral(checks, display, located):
    values = []
    for name in IDENTITY:
        point = (located.get(name) or {}).get('centroid')
        rgb = _pixel(display, point)
        values.append({'name': name, 'rgb': rgb, 'luma': _luma(rgb),
                       'chroma': _chroma(rgb)})
    usable = [item for item in values if item['rgb']]
    _check(checks, 'Clay renders every authored identity card', len(usable) == 4,
           len(usable), 4)
    if len(usable) != 4:
        return values
    lumas = [item['luma'] for item in usable]
    chromas = [item['chroma'] for item in usable]
    spread = max(lumas) - min(lumas)
    _check(checks, 'Clay suppresses authored colour/metal/coat/sheen/emission',
           spread <= 4.0, spread, '<=4 display levels')
    _check(checks, 'Clay identity cards stay neutral gray',
           max(chromas) <= 3.0, max(chromas), '<=3 display levels')
    _check(checks, 'Clay neutral matte is not washed out',
           40.0 <= sum(lumas) / len(lumas) <= 235.0,
           round(sum(lumas) / len(lumas), 2), '40..235')
    return values


def _pick_normalized(viewer, nx=0.5, ny=0.5, timeout=5.0):
    """Pick through field 255 so capture/DPI coordinates cannot miss."""
    from common import query, send_field
    before = query(viewer.hwnd, 254)
    send_field(viewer.hwnd, 255,
               (int(nx * 65535) & 0xFFFF) | ((int(ny * 65535) & 0xFFFF) << 16))
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and query(viewer.hwnd, 254) <= before:
        time.sleep(0.02)
    return {'hit': bool(query(viewer.hwnd, 251)), 'id': query(viewer.hwnd, 251)}


def _check_cutout(checks, studio, clay, located, background):
    entry = located.get('cutout sheet') or {}
    box = entry.get('box')
    if not box:
        _check(checks, 'Clay cutout sheet is visible', False, None, 'box')
        return {}
    left, top, right, bottom = box
    width, height = right - left, bottom - top
    hole = (left + width * 0.5, top + height * 0.5)
    sheet_points = [(left + width * 0.13, top + height * 0.5),
                    (left + width * 0.87, top + height * 0.5),
                    (left + width * 0.5, top + height * 0.13),
                    (left + width * 0.5, top + height * 0.87)]
    studio_hole = _pixel(studio, hole)
    clay_hole = _pixel(clay, hole)
    clay_sheets = [_pixel(clay, point) for point in sheet_points]
    usable = [value for value in clay_sheets if value]
    hole_luma = _luma(clay_hole)
    sheet_luma = sum(_luma(value) for value in usable) / len(usable) if usable else None
    _check(checks, 'Studio MASK cutout already shows texture-alpha holes',
           bool(studio_hole) and bool(background)
           and _luma(studio_hole) <= _luma(background) + 12.0,
           _luma(studio_hole), f"<= {_luma(background) + 12.0 if background else None}")
    _check(checks, 'Clay preserves the modeled MASK cutout',
           sheet_luma is not None and hole_luma is not None
           and sheet_luma - hole_luma >= 25.0,
           {'hole': hole_luma, 'sheet': sheet_luma}, 'sheet - hole >= 25')
    _check(checks, 'Clay cutout hole reveals the background, not gray fill',
           bool(background) and bool(clay_hole)
           and max(abs(a - b) for a, b in zip(clay_hole, background)) <= 14,
           clay_hole, background, 14)
    return {'hole': hole, 'holeLuma': hole_luma, 'sheetLuma': sheet_luma,
            'sheetSamples': clay_sheets}


def _check_hidden_interior(checks, studio, clay, located):
    glass = (located.get('glass sheet') or {}).get('box')
    if not glass:
        _check(checks, 'Clay glass/interior roles are visible', False,
               {'glass': glass}, 'glass located')
        return {}
    left, top, right, bottom = glass
    # The sphere sits on the glass centre axis, so its projection is the glass
    # box centre even though the Studio debug base-colour channel shows the
    # transparent sheet in front of it.
    interior = (left + (right - left) * 0.5, top + (bottom - top) * 0.5)
    flat_point = (left + (right - left) * 0.30, top + (bottom - top) * 0.5)
    studio_center = _pixel(studio, interior)
    studio_flat = _pixel(studio, flat_point)
    clay_center = _pixel(clay, interior)
    clay_flat = _pixel(clay, flat_point)
    _check(checks, 'Studio shows the interior through the transmissive sheet',
           bool(studio_center) and bool(studio_flat)
           and abs(_luma(studio_center) - _luma(studio_flat)) >= 8.0,
           {'center': _luma(studio_center), 'flat': _luma(studio_flat)}, '>=8 luma')
    _check(checks, 'Clay opaque glass hides the interior mesh',
           bool(clay_center) and bool(clay_flat)
           and abs(_luma(clay_center) - _luma(clay_flat)) <= 4.0,
           {'center': _luma(clay_center), 'flat': _luma(clay_flat)}, '<=4 luma')
    return {'interior': interior, 'flatPoint': flat_point,
            'studioCenter': studio_center, 'studioFlat': studio_flat,
            'clayCenter': clay_center, 'clayFlat': clay_flat}


def _check_overlap(checks, clay, located):
    values = []
    for name in ('overlap near', 'overlap far', 'opaque transmissive'):
        rgb = _pixel(clay, (located.get(name) or {}).get('centroid'))
        values.append({'name': name, 'rgb': rgb, 'chroma': _chroma(rgb)})
    usable = [item for item in values if item['rgb']]
    _check(checks, 'Clay overlapping authored-alpha sheets are visible',
           len(usable) == 3, len(usable), 3)
    _check(checks, 'Clay overlap has no authored tint or blend seam colour',
           bool(usable) and all(item['chroma'] <= 4 for item in usable),
           max((item['chroma'] or 0) for item in usable) if usable else None,
           '<=4 display levels')
    return values


def _check_small_and_mirrored(checks, clay, located):
    small = []
    for name in ('small one', 'small two', 'small three', 'mirrored'):
        point = (located.get(name) or {}).get('centroid')
        rgb = _pixel(clay, point)
        small.append({'name': name, 'rgb': rgb, 'luma': _luma(rgb)})
    usable = [item for item in small if item['rgb']]
    _check(checks, 'Clay keeps small parts and the mirrored instance visible',
           len(usable) == 4, len(usable), 4)
    _check(checks, 'Clay small-part silhouettes are not washed-out whites',
           bool(usable) and all(20.0 <= item['luma'] <= 235.0 for item in usable),
           [round(item['luma'], 2) for item in usable if item['luma']], '20..235')
    return small


def _run_mixed_topology(configuration, output, checks):
    """Open the OBJ line/point scene and prove Clay leaves it drawable.

    glTF LINES primitives are rejected by the bounded section validator today,
    so this control is an OBJ fixture. Its triangle, LineList and PointList
    chunks share one generation; Clay must not re-bucket lines/points or make
    them disappear.
    """
    fixture = Path(output) / 'fixtures' / 'line-point-scene.obj'
    if not fixture.exists():
        _check(checks, 'mixed line/point fixture exists', False, str(fixture))
        return {}
    try:
        with Viewer(configuration, output, width=960, height=720) as viewer:
            viewer.open(fixture)
            viewer.apply_controlled_presentation()
            from common import send_field
            send_field(viewer.hwnd, 104, 0)
            _set_lighting(viewer, 0)
            studio = viewer.wait_stable()
            studio_records = material_records(viewer.hwnd)
            _set_lighting(viewer, 1)
            clay = viewer.wait_stable()
            clay_records = material_records(viewer.hwnd)
            _set_lighting(viewer, 0)
            studio_after = viewer.wait_stable()
        lines = [r for r in clay_records if r['lines']]
        points = [r for r in clay_records if r['points']]
        triangles = [r for r in clay_records if not r['lines'] and not r['points']]
        _check(checks, 'mixed scene exposes line and point chunks',
               bool(lines) and bool(points),
               {'lines': len(lines), 'points': len(points),
                'triangles': len(triangles)}, 'lines and points >0')
        _check(checks, 'Clay leaves line/point draw state unchanged',
               all(r['bucket'] == r['authoredBucket'] and r['depthWrite']
                   and not r['clayOverride'] for r in lines + points),
               [(r['bucket'], r['authoredBucket'], r['clayOverride'])
                for r in lines + points], 'unchanged')
        _check(checks, 'mixed scene Clay frame is non-blank',
               (clay['stats'].get('lumaStdDev') or 0) >= 3.0,
               clay['stats'].get('lumaStdDev'), '>=3')
        delta = _mae_bytes(studio['rgb'], studio_after['rgb'])
        _check(checks, 'mixed scene survives the Clay round-trip',
               delta <= 0.0015, round(delta, 6), '<=0.0015')
        return {'lines': len(lines), 'points': len(points),
                'triangles': len(triangles), 'roundTripMae': round(delta, 6),
                'studioRecords': studio_records, 'clayRecords': clay_records}
    except Exception as error:
        _check(checks, 'mixed line/point scene renders', False, str(error))
        return {'error': str(error)}


def _mae_bytes(left, right):
    if not left or not right or len(left) != len(right):
        return 1.0
    return sum(abs(a - b) for a, b in zip(left, right)) / (len(left) * 255.0)


def _check_roundtrip(checks, before, after, studio_before, studio_after,
                     initial, final, initial_textures, final_textures,
                     initial_camera, final_camera, picks):
    unstable = ('presentedFrames', 'detailRequests', 'requestedDetails')
    stable_initial = {key: value for key, value in initial.items()
                      if key not in unstable}
    stable_final = {key: value for key, value in final.items()
                    if key not in unstable}
    _check(checks, 'Studio->Clay->Directional->Studio leaves imported data unchanged',
           stable_initial == stable_final,
           {'changed': {key: [stable_initial.get(key), stable_final.get(key)]
                        for key in stable_initial
                        if stable_initial.get(key) != stable_final.get(key)}},
           'identical import state')
    _check(checks, 'Mode round-trip keeps texture residency',
           initial_textures == final_textures,
           {'before': initial_textures, 'after': final_textures}, 'identical')
    _check(checks, 'Mode round-trip does not change camera or exposure',
           initial_camera == final_camera and before.get('exposure') == after.get('exposure'),
           {'cameraChanged': initial_camera != final_camera,
            'exposure': [before.get('exposure'), after.get('exposure')]}, 'identical')
    delta = _mae_bytes(studio_before['rgb'], studio_after['rgb'])
    _check(checks, 'Studio pixels are stable across the mode round-trip',
           delta <= 0.0015, round(delta, 6), '<=0.0015')
    ids = [picks.get('studioBefore', {}).get('id'),
           picks.get('clay', {}).get('id'),
           picks.get('studioAfter', {}).get('id')]
    _check(checks, 'Picking survives the mode round-trip',
           all(bool(picks.get(key, {}).get('hit')) for key in
               ('studioBefore', 'clay', 'studioAfter'))
           and len(set(ids)) == 1,
           picks, 'same nonzero id in Studio, Clay and Studio')


def _external_review(output, checks):
    """Record the profile's Clay crops without inventing a visual pass.

    The controlled fixture owns the numeric verdicts. External crop existence,
    non-blank content and neutral-model chroma are measured here; the separate
    review record holds the actual visual observations. A missing external
    asset is reported incomplete, never a silent pass.
    """
    images = Path(output) / 'images'
    review = {}
    for asset, regions in EXTERNAL_CLAY_CROPS.items():
        candidates = sorted(images.glob(f'external-{asset}-clay-*.png'))
        if not candidates:
            review[asset] = {'status': 'unavailable',
                             'reason': 'no Clay capture for this external asset'}
            continue
        entries = []
        for path in candidates:
            try:
                width, height, rgb = decode_png(path.read_bytes())
                stats = image_stats(width, height, rgb)
                sample = [rgb[index] for index in range(0, len(rgb), 3 * 997)][:400]
                chroma = (sum(max(rgb[i:i + 3]) - min(rgb[i:i + 3])
                              for i in range(0, min(len(rgb), 3 * 400), 3)) / 400.0
                          if sample else 0.0)
                entries.append({'image': str(path), 'width': width, 'height': height,
                                'meanChroma': round(chroma, 3),
                                'lumaStdDev': stats.get('lumaStdDev'),
                                'uniqueSampleColours': stats.get('uniqueSampleColours'),
                                'regions': regions})
                _check(checks, f'{asset} Clay crop is non-blank',
                       (stats.get('lumaStdDev') or 0) >= 3.0
                       and (stats.get('uniqueSampleColours') or 0) >= 20,
                       {'lumaStdDev': stats.get('lumaStdDev'),
                        'colours': stats.get('uniqueSampleColours')}, 'non-blank')
            except Exception as error:
                entries.append({'image': str(path), 'error': str(error)})
                _check(checks, f'{asset} Clay crop reads', False, str(error))
        review[asset] = {'status': 'sampled', 'captures': entries}
    # The STEP assembly is an in-repository generated CAD fixture (the external
    # Voron file exceeds the bounded STEP host budget), so it is recorded from
    # the generated lane rather than the external corpus.
    step_images = sorted(images.glob('generated-step-assembly-clay-*.png'))
    if step_images:
        entries = []
        for path in step_images:
            try:
                width, height, rgb = decode_png(path.read_bytes())
                stats = image_stats(width, height, rgb)
                entries.append({'image': str(path), 'width': width, 'height': height,
                                'lumaStdDev': stats.get('lumaStdDev'),
                                'uniqueSampleColours': stats.get('uniqueSampleColours')})
                _check(checks, 'STEP assembly Clay crop is non-blank',
                       (stats.get('lumaStdDev') or 0) >= 3.0,
                       stats.get('lumaStdDev'), '>=3')
            except Exception as error:
                entries.append({'image': str(path), 'error': str(error)})
                _check(checks, 'STEP assembly Clay crop reads', False, str(error))
        review['step-assembly'] = {'status': 'sampled', 'captures': entries}
    else:
        review['step-assembly'] = {'status': 'unavailable',
                                   'reason': 'no generated STEP Clay capture'}
    return review


def run(workspace: Path, output: Path, configuration: str, fast: bool = False):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    progress = output / 'diagnostics' / 'progress.log'
    try:
        progress.unlink()
    except OSError:
        pass
    _progress(output, f'T29 Clay lane start (fast={fast})')
    report = {'schema': RESULT_SCHEMA, 'task': 'T29', 'checks': [],
              'failures': [], 'status': 'running', 'fast': fast}
    report['serialLanes'] = _run_serial(configuration, output, fast)
    for lane in report['serialLanes']:
        if not lane.get('pass'):
            report['failures'].append(f"serial lane failed: {lane['name']}")

    fixture = output / 'fixtures' / 'clay-review.glb'
    if not fixture.exists():
        report['failures'].append(f'clay-review fixture missing: {fixture}')
        report['status'] = 'fail'
        write_json(output / 'diagnostics' / 'clay-mode.json', report)
        return report

    try:
        _progress(output, 'opening viewer')
        with Viewer(configuration, output, width=1280, height=900) as viewer:
            viewer.open(fixture)
            _progress(output, 'clay fixture loaded')
            viewer.apply_controlled_presentation()
            # Isolate model form from backdrop layers while sampling.
            from common import send_field
            send_field(viewer.hwnd, 104, 0)   # grid off
            send_field(viewer.hwnd, 102, 0)   # ground shadow off
            send_field(viewer.hwnd, 196, 0)   # ground highlight off
            viewer.set_camera(0.0, 90.0, 1.2)

            _set_lighting(viewer, 0)
            studio_before = viewer.wait_stable()
            initial_state = viewer.import_state()
            initial_textures = viewer.texture_state()
            initial_camera = viewer.camera_state()
            initial_presentation = viewer.presentation_state()
            studio_records = material_records(viewer.hwnd)

            _progress(output, 'locating fixture roles')
            set_channel(viewer.hwnd, 1)
            located_frame = viewer.wait_stable()
            located = _locate(located_frame, ROLES)
            report['located'] = located
            set_channel(viewer.hwnd, 0)
            viewer.wait_stable()

            initial_pick = None
            background = _pixel(studio_before, (12, 12))

            _progress(output, 'Clay mode')
            _set_lighting(viewer, 1)
            clay = viewer.wait_stable()
            clay_records = material_records(viewer.hwnd)
            from common import query
            report['clayTransparentDraws'] = query(viewer.hwnd, 246)
            report['studioTransparentDraws'] = None
            report['clayForwardTransmission'] = query(viewer.hwnd, 263)
            report['clayRecords'] = clay_records
            report['drawState'] = _check_draw_state(
                report['checks'], studio_records, clay_records)
            _check(report['checks'], 'Clay submits no transparent draw pass',
                   report['clayTransparentDraws'] == 0,
                   report['clayTransparentDraws'], 0)
            _check(report['checks'], 'Clay submits no forward-transmission draw',
                   report['clayForwardTransmission'] == 0,
                   report['clayForwardTransmission'], 0)
            report['identity'] = _check_neutral(report['checks'], clay, located)
            report['cutout'] = _check_cutout(
                report['checks'], studio_before, clay, located, background)
            report['hiddenInterior'] = _check_hidden_interior(
                report['checks'], studio_before, clay, located)
            report['overlap'] = _check_overlap(report['checks'], clay, located)
            report['smallParts'] = _check_small_and_mirrored(
                report['checks'], clay, located)
            report['clayImagePath'] = str(
                output / 'images' / 'controlled-clay-review.png')
            (output / 'images').mkdir(parents=True, exist_ok=True)
            (output / 'images' / 'controlled-clay-review.png').write_bytes(
                encoded_png(clay['width'], clay['height'], clay['rgb']))

            _progress(output, 'Directional and Studio round-trip')
            _set_lighting(viewer, 2)
            viewer.wait_stable()
            _set_lighting(viewer, 0)
            studio_after = viewer.wait_stable()
            final_state = viewer.import_state()
            final_textures = viewer.texture_state()
            final_camera = viewer.camera_state()
            final_presentation = viewer.presentation_state()
            # Picking is checked after the image captures so a selection
            # outline cannot change the round-trip pixels.
            picks = {'studioBefore': _pick_normalized(viewer)}
            _set_lighting(viewer, 1)
            picks['clay'] = _pick_normalized(viewer)
            _set_lighting(viewer, 0)
            picks['studioAfter'] = _pick_normalized(viewer)
            _check_roundtrip(
                report['checks'], initial_presentation, final_presentation,
                studio_before, studio_after, initial_state, final_state,
                initial_textures, final_textures, initial_camera, final_camera,
                picks)
            report['roundTrip'] = {
                'initial': initial_state, 'final': final_state,
                'initialTextures': initial_textures,
                'finalTextures': final_textures,
                'initialCamera': initial_camera, 'finalCamera': final_camera,
                'initialPresentation': initial_presentation,
                'finalPresentation': final_presentation,
                'picks': picks,
            }
            report['presentationState'] = final_presentation
            report['performance'] = viewer.performance()
    except Exception as error:
        _progress(output, f'GPU Clay lane failed: {error}')
        report['failures'].append(f'GPU Clay lane failed: {error}')

    _progress(output, 'mixed line/point topology')
    report['mixedTopology'] = _run_mixed_topology(
        configuration, output, report['checks'])
    _progress(output, 'external crop record')
    report['externalReview'] = _external_review(output, report['checks'])

    if not report.get('clayRecords'):
        report['failures'].append('no Clay material records were captured')
    failed = [check for check in report['checks'] if not check['pass']]
    if failed:
        report['failures'].append(f'{len(failed)} Clay checks failed')
    report['status'] = 'pass' if not report['failures'] else 'fail'
    write_json(output / 'diagnostics' / 'clay-mode.json', report)
    report['reportPath'] = str(_write_report(output, report))
    _progress(output, f'T29 Clay lane {report["status"]} '
                       f'({len(report["checks"])} checks)')
    return report


def _write_report(output, report):
    lines = [
        '# T29 Clay geometry-review report', '',
        f"- Status: **{report.get('status')}**",
        f"- Checks: {sum(1 for c in report.get('checks', []) if c['pass'])} passed / "
        f"{len(report.get('checks', []))} total",
        f"- Failures: {len(report.get('failures', []))}", '',
        '## Effective draw state', '',
        f"- Studio records: `{report.get('drawState', {}).get('studio')}`",
        f"- Clay records: `{report.get('drawState', {}).get('clay')}`",
        f"- Clay transparent draws: `{report.get('clayTransparentDraws')}`",
        f"- Clay forward transmission draws: `{report.get('clayForwardTransmission')}`",
        '',
        '## Round-trip', '',
        f"- `{report.get('roundTrip')}`", '',
        '## External crops', '',
    ]
    for asset, survey in report.get('externalReview', {}).items():
        lines.append(f"- **{asset}**: {survey.get('status')} — "
                     f"{survey.get('reason', '')}")
        for capture in survey.get('captures', []):
            lines.append(f"  - `{capture.get('image')}` "
                         f"meanChroma={capture.get('meanChroma')} "
                         f"stdDev={capture.get('lumaStdDev')}")
    lines += ['', '## Checks', '']
    for check in report.get('checks', []):
        marker = 'PASS' if check['pass'] else 'FAIL'
        lines.append(f"- {marker} {check['name']}: `{check.get('actual')}`")
    lines += ['', '## Failures', '']
    lines += [f"- {failure}" for failure in report.get('failures', [])] or ['- none']
    path = output / 'clay-mode-report.md'
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path
