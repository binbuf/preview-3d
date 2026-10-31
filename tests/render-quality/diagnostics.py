"""T21 material and light-contribution diagnostics.

The viewer exposes raw linear inspection channels and numeric HDR samples
through its Debug-only test-control messages (WM_APP+104 fields 124-194). This
module drives those messages, records the numbers, compares the viewer's
validated wire snapshot with source material values read here (never in the
viewer), and writes the evidence that the T21 cause-by-region report cites.
"""
from __future__ import annotations

import json
import math
import struct
import time
from pathlib import Path

from common import RESULT_SCHEMA, encoded_png, sha256_file, write_json
from viewer import Viewer, ViewerError

# Channel id -> stable name. Keep in sync with the Debug switch in
# interactive-viewer/src/app/D3D12ViewerPath.cpp.
CHANNELS = {
    1: 'base-color', 2: 'geometric-normal', 3: 'shading-normal', 4: 'uv0',
    5: 'uv1-unsupported', 6: 'metallic', 7: 'roughness', 8: 'occlusion',
    9: 'coverage-alpha', 10: 'transmission', 11: 'ior-f0', 12: 'clearcoat',
    13: 'sheen', 14: 'direct-diffuse', 15: 'direct-specular',
    16: 'environment-diffuse', 17: 'environment-specular', 18: 'coat',
    19: 'emission', 20: 'pre-tone-map-hdr', 21: 'environment-brdf-scale',
    22: 'transmission-composite', 23: 'display-calibration',
}
CONTRIBUTION_CHANNELS = [14, 15, 16, 17, 18, 19, 20]

FLOAT_FIELDS = {
    149: ('baseColor', 0), 150: ('baseColor', 1), 151: ('baseColor', 2),
    152: ('baseColor', 3), 153: ('metallic', None), 154: ('roughness', None),
    155: ('alphaCutoff', None), 156: ('emissive', 0), 157: ('emissive', 1),
    158: ('emissive', 2), 159: ('emissiveStrength', None),
    160: ('clearcoat', None), 161: ('clearcoatRoughness', None),
    162: ('occlusionStrength', None), 163: ('sheenRoughness', None),
    164: ('sheen', 0), 165: ('sheen', 1), 166: ('sheen', 2),
    167: ('specularFactor', None), 168: ('ior', None),
    169: ('specularColor', 0), 170: ('specularColor', 1), 171: ('specularColor', 2),
    172: ('transmission', None), 173: ('uvOffset', 0), 174: ('uvOffset', 1),
    175: ('uvScale', 0), 176: ('uvScale', 1), 177: ('uvRotation', None),
}


def _u32(hwnd, field, value=0):
    return int(_send(hwnd, field, value)) & 0xFFFFFFFF

def _send(hwnd, field, value=0):
    from common import send_field
    return send_field(hwnd, field, value)


def _f32(hwnd, field):
    return struct.unpack('<f', struct.pack('<I', _u32(hwnd, field)))[0]


def _set_sample(hwnd, x0, y0, x1, y1):
    _send(hwnd, 126, (x0 & 0xFFFF) | ((y0 & 0xFFFF) << 16))
    _send(hwnd, 127, (x1 & 0xFFFF) | ((y1 & 0xFFFF) << 16))


def sample_region(hwnd, x0, y0, x1, y1, timeout=5.0):
    _set_sample(hwnd, x0, y0, x1, y1)
    _send(hwnd, 128, 0)
    deadline = time.monotonic() + timeout
    status = 0
    while time.monotonic() < deadline:
        status = _send(hwnd, 129, 0) & 0xFFFFFFFF
        if status in (2, 3):
            break
        time.sleep(0.05)
    if status != 2:
        return {'status': status, 'rgba': None}
    rgba = [_f32(hwnd, field) for field in (130, 131, 132, 133)]
    return {'status': status, 'rgba': rgba}


def region_sample(hwnd, x0, y0, x1, y1):
    """Average a region through bounded <=64x64 HDR readback tiles."""
    width = max(1, x1 - x0)
    height = max(1, y1 - y0)
    columns = max(1, min(3, width // 16))
    rows = max(1, min(3, height // 16))
    sums = [0.0] * 4
    count = 0
    for row in range(rows):
        for column in range(columns):
            tx0 = x0 + width * column // columns
            ty0 = y0 + height * row // rows
            tx1 = x0 + width * (column + 1) // columns
            ty1 = y0 + height * (row + 1) // rows
            sample = sample_region(hwnd, tx0, ty0, tx1, ty1)
            if not sample.get('rgba'):
                continue
            for channel in range(4):
                sums[channel] += sample['rgba'][channel]
            count += 1
    if not count:
        return {'status': 3, 'rgba': None}
    return {'status': 2, 'rgba': [value / count for value in sums],
            'tiles': count}


def channel_supported(hwnd):
    mask = _u32(hwnd, 125)
    return {channel: bool(mask & (1 << channel)) for channel in CHANNELS}


def set_channel(hwnd, channel):
    _send(hwnd, 124, channel)
    _send(hwnd, 106, 0)  # force present
    # Keep the signal path deterministic without depending on frame timing.
    time.sleep(0.12)
    return _u32(hwnd, 124, -1)


def material_records(hwnd):
    count = _u32(hwnd, 134)
    records = []
    for index in range(count):
        _send(hwnd, 135, index)
        record = {
            'chunkId': _u32(hwnd, 136), 'materialChunkId': _u32(hwnd, 137),
            'sourceMeshId': _u32(hwnd, 138), 'sourceNodeId': _u32(hwnd, 139),
            'instanceId': _u32(hwnd, 140), 'vertexCount': _u32(hwnd, 141),
            'indexCount': _u32(hwnd, 142), 'bucket': _u32(hwnd, 143),
            'depthWrite': bool(_u32(hwnd, 144)), 'cullMode': _u32(hwnd, 145),
            'hasMaterial': bool(_u32(hwnd, 146)), 'alphaMode': _u32(hwnd, 147),
            'flags': _u32(hwnd, 148),
            'normalScale': _f32(hwnd, 201),
            'slotFlags': [_u32(hwnd, 202 + slot) for slot in range(5)],
            'slotUvOffset': [[_f32(hwnd, 207 + slot), _f32(hwnd, 211 + slot)]
                             for slot in range(4)],
            'slotUvScale': [[_f32(hwnd, 215 + slot), _f32(hwnd, 219 + slot)]
                            for slot in range(4)],
            'slotUvRotation': [_f32(hwnd, 223 + slot) for slot in range(4)],
            # Field 178 packs slots 0..3 into one word; slot 4 has its own
            # field because the published diagnostics contract is append-only.
            'textureIndicesPacked': _u32(hwnd, 178),
            'textureIndices': [(_u32(hwnd, 178) >> (slot * 8)) & 0xFF for slot in range(4)]
                              + [_u32(hwnd, 227)],
            'textureChunkIds': [_u32(hwnd, 179 + slot) for slot in range(4)]
                               + [_u32(hwnd, 228)],
            'textureFormats': [_u32(hwnd, 183 + slot) for slot in range(4)]
                              + [_u32(hwnd, 229)],
            'textureColorSpaces': [_u32(hwnd, 187 + slot) for slot in range(4)]
                                  + [_u32(hwnd, 230)],
            'mapMask': _u32(hwnd, 191),
            'occlusionBound': bool(_u32(hwnd, 191) & (1 << 4)),
            'points': bool(_u32(hwnd, 192)), 'lines': bool(_u32(hwnd, 193)),
            'completeVertex': bool(_u32(hwnd, 194)),
        }
        for field, (name, component) in FLOAT_FIELDS.items():
            value = _f32(hwnd, field)
            if component is None:
                record[name] = value
            else:
                record.setdefault(name, [0.0, 0.0, 0.0, 0.0] if name == 'baseColor' else [0.0, 0.0, 0.0])
                record[name][component] = value
        records.append(record)
    return records


def gltf_source_metadata(path: Path):
    """Bounded source-side material identity. Runs only in test tooling."""
    path = Path(path)
    data = path.read_bytes()
    try:
        if data[:4] == b'glTF':
            at = 12
            document = None
            while at + 8 <= len(data):
                length, kind = struct.unpack_from('<II', data, at)
                at += 8
                if kind == 0x4E4F534A:  # JSON
                    document = json.loads(data[at:at + min(length, 16 * 1024 * 1024)].decode('utf-8'))
                    break
                at += length
            if document is None:
                return {'status': 'unreadable', 'reason': 'no JSON chunk'}
        else:
            document = json.loads(data[:16 * 1024 * 1024].decode('utf-8'))
    except Exception as error:  # bounded, reported rather than fatal
        return {'status': 'unreadable', 'reason': str(error)[:200]}
    materials = []
    for index, material in enumerate(document.get('materials', []) or []):
        pbr = material.get('pbrMetallicRoughness', {}) or {}
        extensions = material.get('extensions', {}) or {}
        materials.append({
            'index': index, 'name': material.get('name'),
            'baseColorFactor': pbr.get('baseColorFactor', [1.0, 1.0, 1.0, 1.0]),
            'metallicFactor': pbr.get('metallicFactor', 1.0),
            'roughnessFactor': pbr.get('roughnessFactor', 1.0),
            'alphaMode': material.get('alphaMode', 'OPAQUE'),
            'doubleSided': bool(material.get('doubleSided', False)),
            'extensions': sorted(extensions.keys()),
            'hasBaseColorTexture': 'baseColorTexture' in pbr,
            'hasMetallicRoughnessTexture': 'metallicRoughnessTexture' in pbr,
            'hasNormalTexture': 'normalTexture' in material,
            'hasOcclusionTexture': 'occlusionTexture' in material,
            'hasEmissiveTexture': 'emissiveTexture' in material,
        })
    primitives = []
    for mesh_index, mesh in enumerate(document.get('meshes', []) or []):
        for primitive_index, primitive in enumerate(mesh.get('primitives', []) or []):
            primitives.append({'mesh': mesh_index, 'primitive': primitive_index,
                               'material': primitive.get('material')})
    return {'status': 'read', 'materials': materials, 'primitives': primitives}


def _match(expected, records, tolerance=1e-3):
    for record in records:
        if any(abs(a - b) > tolerance for a, b in
               zip(record.get('baseColor', [0, 0, 0, 0]), expected['baseColor'])):
            continue
        if abs(record.get('metallic', -1) - expected['metallic']) > tolerance:
            continue
        if abs(record.get('roughness', -1) - expected['roughness']) > tolerance:
            continue
        return record
    return None


def _neutral_tone_map(color):
    color = [max(channel, 0.0) for channel in color]
    start = 0.8 - 0.04
    desaturation = 0.15
    minimum = min(color)
    offset = minimum - 6.25 * minimum * minimum if minimum < 0.08 else 0.04
    color = [channel - offset for channel in color]
    peak = max(color)
    if peak >= start:
        d = 1.0 - start
        new_peak = 1.0 - d * d / (peak + d - start)
        color = [channel * new_peak / max(peak, 1e-6) for channel in color]
        grey = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0)
        color = [channel + (new_peak - channel) * grey for channel in color]
    return [min(1.0, max(0.0, channel)) for channel in color]


def _srgb_encode(color):
    return [12.92 * channel if channel <= 0.0031308
            else 1.055 * (channel ** (1.0 / 2.4)) - 0.055 for channel in color]


def _display_rgb(linear):
    return [round(255.0 * channel) for channel in _srgb_encode(_neutral_tone_map(linear))]


def _centroids(width, height, rgb, colors):
    """Centroid of the pixels closest to each expected display base colour.

    The base-colour channel is linear; the display applies the same neutral
    tone curve and sRGB transfer as the product. Matching the resulting 8-bit
    colour is independent of framing and exposure and avoids hue shifts from
    the per-channel transfer.
    """
    targets = [_display_rgb(color[:3]) for color in colors]
    sums = [[0, 0, 0] for _ in colors]
    for y in range(height):
        row = y * width
        for x in range(width):
            i = (row + x) * 3
            pixel = (rgb[i], rgb[i + 1], rgb[i + 2])
            best, best_distance = -1, 1e9
            for index, target in enumerate(targets):
                distance = sum((pixel[channel] - target[channel]) ** 2 for channel in range(3))
                if distance < best_distance:
                    best, best_distance = index, distance
            if best < 0 or best_distance > 70 * 70:
                continue
            sums[best][0] += x
            sums[best][1] += y
            sums[best][2] += 1
    centroids = []
    for sx, sy, count in sums:
        centroids.append((sx // count, sy // count) if count >= 100 else None)
    return centroids


def _save_channel_images(viewer, output: Path, channels):
    images = {}
    for channel in channels:
        set_channel(viewer.hwnd, channel)
        time.sleep(0.35)
        frame = viewer.capture()
        path = output / 'channels' / f'channel-{channel:02d}-{CHANNELS[channel]}.png'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
        images[channel] = {
            'name': CHANNELS[channel], 'image': str(path), 'sha256': sha256_file(path),
            'width': frame['width'], 'height': frame['height'],
        }
    return images


def _probe_patches(viewer, expected, output: Path):
    """Locate the probe spheres and read numeric HDR channel values."""
    set_channel(viewer.hwnd, 1)
    frame = viewer.wait_stable()
    centroids = _centroids(frame['width'], frame['height'], frame['rgb'],
                           [material['baseColor'] for material in expected])
    rect = frame['rect']
    results = []
    for expected_material, centroid in zip(expected, centroids):
        entry = {'material': expected_material['name'], 'centroid': centroid, 'channels': {}}
        if centroid is None:
            entry['error'] = 'probe sphere was not visible'
            results.append(entry)
            continue
        x = rect['left'] + centroid[0]
        y = rect['top'] + centroid[1]
        for channel in [1, 5, 6, 7, 9, 10, 11, 12, 13, 20] + CONTRIBUTION_CHANNELS:
            set_channel(viewer.hwnd, channel)
            sample = sample_region(viewer.hwnd, x - 4, y - 4, x + 4, y + 4)
            entry['channels'][channel] = sample.get('rgba')
        results.append(entry)
    return results, rect


def _value(patch, channel):
    rgba = patch['channels'].get(channel)
    return rgba if rgba else None


def _check_probe(patches, expected):
    checks = []

    def check(name, actual, expected_value, tolerance, detail=None):
        ok = actual is not None and abs(actual - expected_value) <= tolerance
        checks.append({'name': name, 'pass': bool(ok), 'actual': actual,
                       'expected': expected_value, 'tolerance': tolerance,
                       'detail': detail})

    for patch, material in zip(patches, expected):
        name = material['name']
        base = _value(patch, 1)
        for channel, component, label in ((0, 0, 'r'), (1, 1, 'g'), (2, 2, 'b')):
            check(f'{name} base-color.{label}', base[channel] if base else None,
                  material['baseColor'][channel], 0.03)
        metallic = _value(patch, 6)
        check(f'{name} metallic', metallic[0] if metallic else None,
              material['metallic'], 0.03)
        roughness = _value(patch, 7)
        check(f'{name} roughness', roughness[0] if roughness else None,
              material['roughness'], 0.04)
        uv1 = _value(patch, 5)
        check(f'{name} uv1 unsupported marker',
              uv1[0] if uv1 else None, 1.0, 0.03, 'unsupported channel marker')
        if 'clearcoat' in material:
            clearcoat = _value(patch, 12)
            check(f'{name} clearcoat', clearcoat[0] if clearcoat else None,
                  material['clearcoat'], 0.03)
        if 'transmission' in material:
            transmission = _value(patch, 10)
            check(f'{name} transmission', transmission[0] if transmission else None,
                  material['transmission'], 0.03)
        if 'ior' in material:
            ior_f0 = _value(patch, 11)
            ratio = (material['ior'] - 1.0) / (material['ior'] + 1.0)
            f0 = ratio * ratio
            check(f'{name} ior-encoded', ior_f0[0] if ior_f0 else None,
                  material['ior'] * 0.25, 0.03)
            check(f'{name} ior-f0', ior_f0[1] if ior_f0 else None, f0, 0.01)
        if material.get('alphaMode') == 2 and 'transmission' not in material:
            alpha = _value(patch, 9)
            check(f'{name} coverage-alpha', alpha[0] if alpha else None,
                  material['baseColor'][3], 0.04)
        # The pre-tone-map HDR view must equal the sum of its isolated lobes.
        lobes = [_value(patch, channel) for channel in CONTRIBUTION_CHANNELS[:-1]]
        hdr = _value(patch, 20)
        if hdr and all(lobes):
            for component in range(3):
                total = sum(lobe[component] for lobe in lobes)
                check(f'{name} hdr-sum.component{component}', hdr[component],
                      total, 0.02)
    return checks


# Region survey: which contribution dominates each named region. Normalised
# rectangles match the T20 profile so the captured studio images and these
# numeric samples describe the same area.
SURVEY = {
    'car': {
        'entry': 'Car.glb',
        'regions': {'windshield': [0.28, 0.16, 0.72, 0.58],
                    'paint': [0.34, 0.55, 0.72, 0.9]},
        'camera': {'yawDegrees': 18.0, 'pitchDegrees': 70.0, 'distanceScale': 0.48},
    },
    'camera': {
        'entry': 'Camera_01_4k.gltf',
        'regions': {'lens': [0.60, 0.40, 0.76, 0.62],
                    'black-body': [0.42, 0.40, 0.58, 0.66]},
        'camera': {'yawDegrees': 0.0, 'pitchDegrees': 90.0, 'distanceScale': 0.42},
    },
    'suit': {
        'entry': 'Advanced Crew Escape Suit.glb',
        'regions': {'torso': [0.32, 0.28, 0.68, 0.72]},
        'camera': {'yawDegrees': 0.0, 'pitchDegrees': 90.0, 'distanceScale': 0.8},
    },
}


def _region_survey(viewer, name, spec, output: Path):
    from corpus import resolve_entry
    entry, _, _ = resolve_entry(Path('D:/repos/binbuf/preview-3d-test-models'), spec['entry'])
    if entry is None or not entry.is_file():
        return {'asset': name, 'status': 'unavailable', 'reason': 'seed asset not present'}
    try:
        viewer.open(entry)
    except Exception as error:
        return {'asset': name, 'status': 'failed', 'reason': str(error)[:300]}
    viewer.apply_controlled_presentation()
    camera = spec['camera']
    viewer.set_camera(camera['yawDegrees'], camera['pitchDegrees'], camera['distanceScale'])
    set_channel(viewer.hwnd, 20)
    frame = viewer.wait_stable()
    rect = frame['rect']
    regions = {}
    for region_name, normalized in spec['regions'].items():
        x0 = rect['left'] + int(normalized[0] * frame['width'])
        x1 = rect['left'] + int(normalized[2] * frame['width'])
        y0 = rect['top'] + int(normalized[1] * frame['height'])
        y1 = rect['top'] + int(normalized[3] * frame['height'])
        samples = {}
        for channel in [1, 9, 10] + CONTRIBUTION_CHANNELS:
            set_channel(viewer.hwnd, channel)
            samples[CHANNELS[channel]] = region_sample(viewer.hwnd, x0, y0, x1, y1)
        contributions = {CHANNELS[channel]: samples.get(CHANNELS[channel], {}).get('rgba')
                         for channel in CONTRIBUTION_CHANNELS[:-1]}
        ranking = []
        for label, rgba in contributions.items():
            if not rgba:
                continue
            magnitude = sum(rgba[:3]) / 3.0
            ranking.append({'contribution': label, 'meanRgb': rgba[:3],
                            'magnitude': round(magnitude, 6)})
        ranking.sort(key=lambda item: item['magnitude'], reverse=True)
        regions[region_name] = {
            'rect': [x0, y0, x1, y1],
            'samples': samples,
            'dominant': ranking[0]['contribution'] if ranking else None,
            'ranking': ranking,
        }
    return {'asset': name, 'status': 'sampled', 'entry': str(entry), 'regions': regions}


def write_cause_report(output: Path, report) -> Path:
    """Short evidence report: what dominates each region and what T22-T27 own."""
    output = Path(output)
    lines = [
        '# T21 material and light-contribution report',
        '',
        'Generated from the Debug viewer GPU readback and the captured evidence images.',
        'Dominance is a numeric observation from isolated channels; it is not by itself',
        'proof of a root cause where no isolated A/B comparison was run.',
        '',
        '## Checks',
        '',
        f"- Diagnostic status: **{report.get('status')}**",
        f"- Checks: {sum(1 for c in report.get('checks', []) if c['pass'])} passed / "
        f"{len(report.get('checks', []))} total",
        f"- Failures: {len(report.get('failures', []))}",
    ]
    for failure in report.get('failures', []):
        lines.append(f"  - {failure}")
    lines += ['', '## Numeric probe patches (linear HDR readback)', '']
    for patch in report.get('probePatches', []):
        channels = patch.get('channels', {})
        def fmt(channel):
            rgba = channels.get(channel)
            return 'n/a' if not rgba else '[' + ', '.join(f'{v:.4f}' for v in rgba[:3]) + ']'
        lines.append(f"- **{patch['material']}**: base {fmt(1)}, metallic {fmt(6)}, "
                     f"roughness {fmt(7)}, alpha {fmt(9)}, transmission {fmt(10)}, "
                     f"clearcoat {fmt(12)}, HDR {fmt(20)}")
    lines += ['', '## Contribution dominance by region', '']
    for asset, survey in report.get('regionSurvey', {}).items():
        if survey.get('status') != 'sampled':
            lines.append(f"- **{asset}**: {survey.get('status')} — {survey.get('reason', '')}")
            continue
        for region, data in survey.get('regions', {}).items():
            ranking = ', '.join(f"{item['contribution']} {item['magnitude']:.4f}"
                                for item in data.get('ranking', [])[:3])
            lines.append(f"- **{asset}/{region}**: dominant `{data.get('dominant')}`. "
                         f"Top: {ranking or 'no data'}")
    lines += [
        '',
        '## Hand-off to T22-T27',
        '',
        '- T22: authored AO is still ORM-red unless a map is bound; verify the independent',
        '  occlusion texture and strength path with an isolated comparison.',
        '- T24: compare direct/environment diffuse/specular and coat channels before changing',
        '  exposure or light intensity; the probe fixture is the isolated reference.',
        '- T25: texture-driven clearcoat/sheen/specular/transmission are still warning-only;',
        '  the unbound-slot identities are recorded in diagnostics.json.',
        '- T26/T27: use the Car windshield/side-window and Camera lens region dominance plus',
        '  the captured channel images to separate coverage alpha, transmission and ordering.',
        '- T29: Clay suppresses lobes but retains authored draw buckets; verify against the',
        '  material record bucket/depth-write fields rather than the final colour alone.',
        '',
        '## Evidence',
        '',
        '- `diagnostics/diagnostics.json` — numeric checks, material records, source metadata.',
        f"- `diagnostics/channels/` — {len(report.get('channelImages', {}))} diagnostic views.",
        '- `images/` — profile studio captures for Car, Camera, Suit and the probe grid.',
        '- `captures.json` — capture conditions, camera/presentation state and hashes.',
        '',
    ]
    path = output / 'cause-report.md'
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def run(workspace: Path, output: Path, release_exe: Path):
    output = Path(output)
    diagnostic_dir = output / 'diagnostics'
    diagnostic_dir.mkdir(parents=True, exist_ok=True)
    report = {'schema': RESULT_SCHEMA, 'task': 'T21', 'checks': [],
              'failures': [], 'status': 'running'}
    fixture_root = output / 'fixtures'
    from fixtures import EXPECTED_MATERIALS

    probe_path = fixture_root / 'material-probe.glb'
    if not probe_path.exists():
        report['failures'].append(f'material probe fixture missing: {probe_path}')
        report['status'] = 'fail'
        write_json(diagnostic_dir / 'diagnostics.json', report)
        return report

    # --- Source metadata is read in test tooling, never in the viewer. ---
    report['sourceMaterials'] = {
        'material-probe': gltf_source_metadata(probe_path),
        'external': {},
    }
    corpus = Path('D:/repos/binbuf/preview-3d-test-models')
    from corpus import resolve_entry
    for name, spec in SURVEY.items():
        source, _, _ = resolve_entry(corpus, spec['entry'])
        if source is not None and source.is_file():
            report['sourceMaterials']['external'][name] = gltf_source_metadata(source)
        else:
            report['sourceMaterials']['external'][name] = {
                'status': 'unavailable', 'entry': str(corpus / spec['entry'])}

    with Viewer('Debug', output, width=1280, height=860) as viewer:
        viewer.open(probe_path)
        viewer.apply_controlled_presentation()
        viewer.set_camera(0.0, 90.0, 1.35)
        report['channelSupport'] = channel_supported(viewer.hwnd)
        report['channelImages'] = _save_channel_images(
            viewer, diagnostic_dir, list(CHANNELS.keys()))

        set_channel(viewer.hwnd, 0)
        viewer.set_camera(0.0, 90.0, 1.35)
        patches, rect = _probe_patches(viewer, EXPECTED_MATERIALS['material-probe'], diagnostic_dir)
        report['probeRect'] = rect
        report['probePatches'] = patches
        report['checks'].extend(_check_probe(
            patches, EXPECTED_MATERIALS['material-probe']))

        records = material_records(viewer.hwnd)
        report['materialRecords'] = records
        identity = []
        for expected in EXPECTED_MATERIALS['material-probe']:
            record = _match(expected, records)
            ok = record is not None
            identity.append({'name': expected['name'], 'matched': ok,
                             'materialChunkId': record and record.get('materialChunkId'),
                             'bucket': record and record.get('bucket'),
                             'depthWrite': record and record.get('depthWrite'),
                             'cullMode': record and record.get('cullMode'),
                             'expectedBucket': expected['bucket']})
            if ok:
                for field, expected_value, tolerance in (
                        ('bucket', expected['bucket'], 0),
                        ('alphaMode', expected['alphaMode'], 0),
                        ('clearcoat', expected.get('clearcoat', 0.0), 1e-3),
                        ('transmission', expected.get('transmission', 0.0), 1e-3),
                        ('ior', expected.get('ior', 1.5), 1e-3),
                        ('emissiveStrength', expected.get('emissiveStrength', 1.0), 1e-3)):
                    if field not in expected and field in ('clearcoat', 'transmission', 'ior'):
                        continue
                    actual = record.get(field)
                    if isinstance(actual, bool):
                        actual = int(actual)
                    good = actual is not None and abs(float(actual) - float(expected_value)) <= tolerance
                    report['checks'].append({
                        'name': f"{expected['name']} {field}", 'pass': bool(good),
                        'actual': actual, 'expected': expected_value,
                        'tolerance': tolerance})
            else:
                report['failures'].append(f"material identity not matched: {expected['name']}")
        report['materialIdentity'] = identity

        # --- Which contributions dominate the named external regions? ---
        report['regionSurvey'] = {}
        for name, spec in SURVEY.items():
            try:
                report['regionSurvey'][name] = _region_survey(viewer, name, spec, output)
            except Exception as error:
                report['regionSurvey'][name] = {
                    'asset': name, 'status': 'failed', 'reason': str(error)[:300]}

    failed = [check for check in report['checks'] if not check['pass']]
    if failed:
        report['failures'].append(f'{len(failed)} diagnostic identity/patch checks failed')
    # Unsupported channel must be advertised as unsupported and marked.
    if report['channelSupport'].get(5):
        report['failures'].append('UV1 channel 5 was advertised as supported')

    # --- Release test-control rejection. ---
    report['releaseRejection'] = _release_rejection(release_exe, diagnostic_dir)
    if not report['releaseRejection'].get('pass'):
        report['failures'].append('Release test-control rejection failed')

    report['status'] = 'pass' if not report['failures'] else 'fail'
    write_json(diagnostic_dir / 'diagnostics.json', report)
    return report


def _release_rejection(release_exe: Path, output: Path):
    import subprocess

    from common import send, wait_for_window
    result = {'exe': str(release_exe), 'pass': False, 'responses': {}}
    if not Path(release_exe).exists():
        result['reason'] = 'Release viewer executable is not built'
        return result
    process = subprocess.Popen([str(release_exe)])
    hwnd = 0
    try:
        hwnd = wait_for_window(process)
        if not hwnd:
            result['reason'] = 'Release viewer window did not appear'
            return result
        time.sleep(0.5)
        for field, value in ((0, 0), (124, 6), (125, 0), (134, 0), (129, 0)):
            try:
                result['responses'][str(field)] = int(
                    send(hwnd, 0x8000 + 104, field, value)) & 0xFFFFFFFF
            except Exception as error:
                result['responses'][str(field)] = f'timeout: {error}'
        # A Release build has no test-control handler at all: every field must
        # be zero (DefWindowProc), including the new T21 inspection fields.
        result['pass'] = all(value == 0 for value in result['responses'].values())
        if not result['pass']:
            result['reason'] = 'Release build answered a test-control message'
        write_json(Path(output) / 'release-rejection.json', result)
    finally:
        try:
            if hwnd:
                send(hwnd, 0x10)
        except Exception:
            pass
        try:
            process.wait(timeout=10)
        except Exception:
            process.kill()
    return result
