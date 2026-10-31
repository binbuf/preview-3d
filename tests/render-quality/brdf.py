"""T24 BRDF, environment integration and clearcoat layering verification.

Runs the CPU reference lanes (split-sum LUT, solid-angle SH irradiance, GGX
prefilter) and then drives the Debug viewer's numeric HDR/diagnostic channels
on the generated ``brdf-probe`` fixture in Studio, Clay and Directional. It
also records Car/Camera nonregression observations in every lighting mode.

The CPU checks live in ``Tests.Unit.exe`` and compare the shipped bake against
independent uniform-quadrature references, not against a copy of the production
formula. The GPU checks read raw linear values from the scene target; the
display transform and tone-curve agreement are checked against the captured
8-bit frame.
"""
from __future__ import annotations

import json
import math
import struct
import subprocess
import time
from pathlib import Path

from common import RESULT_SCHEMA, encoded_png, sha256_file, write_json
from diagnostics import (_display_rgb, _neutral_tone_map, _save_channel_images,
                         _srgb_encode, material_records, region_sample,
                         sample_region, set_channel)
from fixtures import EXPECTED_MATERIALS
from material_semantics import _locate_probes, _check_material_records
from viewer import Viewer

ROOT = Path(__file__).resolve().parents[2]
CORPUS = Path('D:/repos/binbuf/preview-3d-test-models')
T21_REPORT = ROOT / 'TestResults/render-quality/T21/diagnostics/diagnostics.json'

SERIAL_LANES = [
    ('unit-brdf-ibl', 'Tests.Unit.exe', ['[brdf]']),
    ('unit-material-lobes', 'Tests.Unit.exe', ['[material-lobes]']),
    ('unit-studio-ibl', 'Tests.Unit.exe', ['[studio-ibl]']),
    ('import-gltf-specular', 'Tests.ImportIsolation.exe', ['[gltf-extensions]']),
]

CHANNELS_ALL = [1, 6, 7, 9, 11, 12, 16, 17, 18, 19, 20, 21, 22, 23]
CHANNELS_IBL = [16, 17, 21, 22]
CHANNELS_ZERO = [16, 17, 18, 19, 20]

EXTERNAL = {
    'car': {
        'entry': 'Car.glb',
        'view': {'yawDegrees': 18.0, 'pitchDegrees': 70.0, 'distanceScale': 0.48},
        'regions': {'windshield': [0.28, 0.16, 0.72, 0.58],
                    'paint': [0.34, 0.55, 0.72, 0.9]},
    },
    'camera': {
        'entry': 'Camera_01_4k.gltf',
        'view': {'yawDegrees': 0.0, 'pitchDegrees': 90.0, 'distanceScale': 0.42},
        'regions': {'lens': [0.60, 0.40, 0.76, 0.62],
                    'black-body': [0.42, 0.40, 0.58, 0.66]},
    },
}


def _f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', float(value)))[0]


def _send(hwnd, field, value=0):
    from common import send_field
    return send_field(hwnd, field, value)


def _set_lighting(viewer, mode):
    _send(viewer.hwnd, 88, mode)
    time.sleep(0.35)


def _set_environment_intensity(viewer, value):
    _send(viewer.hwnd, 98, _f32_bits(value))
    time.sleep(0.35)


def _pixel(frame, centroid):
    if centroid is None:
        return None
    x, y = centroid
    width, height = frame['width'], frame['height']
    if not (0 <= x < width and 0 <= y < height):
        return None
    index = (y * width + x) * 3
    return [frame['rgb'][index], frame['rgb'][index + 1], frame['rgb'][index + 2]]


def _sample_probes(viewer, channels):
    expected = EXPECTED_MATERIALS['brdf-probe']
    set_channel(viewer.hwnd, 1)
    locator = viewer.capture()
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
    display = viewer.wait_stable()
    for entry in samples:
        entry['display'] = _pixel(display, entry.get('centroid'))
    return samples, centroids, display


def _value(samples, name, channel):
    for entry in samples:
        if entry.get('material') == name:
            return (entry.get('channels') or {}).get(channel)
    return None


def _luma(rgba):
    if not rgba:
        return None
    return 0.2126 * rgba[0] + 0.7152 * rgba[1] + 0.0722 * rgba[2]


def _finite(rgba):
    return rgba is not None and all(math.isfinite(v) for v in rgba)


def _check(checks, name, passed, actual=None, expected=None, tolerance=None):
    checks.append({'name': name, 'pass': bool(passed), 'actual': actual,
                   'expected': expected, 'tolerance': tolerance})


def _check_basic_channels(checks, samples):
    for entry in samples:
        name = entry['material']
        channels = entry.get('channels') or {}
        for channel in CHANNELS_ALL:
            value = channels.get(channel)
            _check(checks, f'{name} channel {channel} finite', _finite(value),
                   value and value[:3], 'finite')
        metallic = channels.get(6)
        roughness = channels.get(7)
        if metallic is not None and roughness is not None:
            _check(checks, f'{name} metallic in range', 0.0 <= metallic[0] <= 1.0,
                   metallic[0], '[0,1]')
            _check(checks, f'{name} roughness in range', 0.0 <= roughness[0] <= 1.0,
                   roughness[0], '[0,1]')


def _check_material_values(checks, samples):
    expected = {item['name']: item for item in EXPECTED_MATERIALS['brdf-probe']}
    for entry in samples:
        name = entry['material']
        channels = entry.get('channels') or {}
        spec = expected[name]
        metallic = (channels.get(6) or [None])[0]
        roughness = (channels.get(7) or [None])[0]
        if metallic is not None:
            _check(checks, f'{name} metallic matches source',
                   abs(metallic - spec['metallic']) <= 0.02, metallic,
                   spec['metallic'], 0.02)
        if roughness is not None:
            _check(checks, f'{name} roughness matches source',
                   abs(roughness - spec['roughness']) <= 0.03, roughness,
                   spec['roughness'], 0.03)
        clearcoat = (channels.get(12) or [None])[0]
        if clearcoat is not None:
            _check(checks, f'{name} clearcoat matches source',
                   abs(clearcoat - spec.get('clearcoat', 0.0)) <= 0.02, clearcoat,
                   spec.get('clearcoat', 0.0), 0.02)
        ior_f0 = channels.get(11)
        if ior_f0 and 'ior' in spec:
            ratio = (spec['ior'] - 1.0) / (spec['ior'] + 1.0)
            _check(checks, f'{name} IOR encoded',
                   abs(ior_f0[0] - spec['ior'] * 0.25) <= 0.03, ior_f0[0],
                   spec['ior'] * 0.25, 0.03)
            _check(checks, f'{name} dielectric F0',
                   abs(ior_f0[1] - ratio * ratio) <= 0.02, ior_f0[1],
                   ratio * ratio, 0.02)
        emission = channels.get(19)
        if 'emissive' in spec:
            expected_emission = max(spec['emissive']) * spec['emissiveStrength']
            _check(checks, f'{name} emission strength applied',
                   emission is not None and max(emission[:3]) >= expected_emission * 0.4,
                   emission and emission[:3], expected_emission)
        transmission = channels.get(22)
        if 'transmission' in spec:
            _check(checks, f'{name} transmission carried in composite',
                   transmission is not None and abs(transmission[2] - spec['transmission']) <= 0.02,
                   transmission and transmission[:3], spec['transmission'], 0.02)


def _check_energy(checks, samples):
    white = _value(samples, 'brdf white dielectric', 20)
    black = _value(samples, 'brdf black dielectric', 20)
    white_diffuse = _value(samples, 'brdf white dielectric', 16)
    black_diffuse = _value(samples, 'brdf black dielectric', 16)
    if white_diffuse and black_diffuse:
        _check(checks, 'white dielectric environment diffuse exceeds black',
               _luma(white_diffuse) > _luma(black_diffuse) * 2.0,
               _luma(white_diffuse), f">{_luma(black_diffuse) * 2.0:.5f}")
    if white and black:
        _check(checks, 'white dielectric is brighter than black dielectric',
               _luma(white) > _luma(black) * 2.0, _luma(white),
               f">{_luma(black) * 2.0:.5f}")

    black_specular = _value(samples, 'brdf black dielectric', 17)
    if black_specular:
        _check(checks, 'black dielectric retains an environment reflection',
               _luma(black_specular) > 0.002, _luma(black_specular), '>0.002')

    # Split-sum DFG scale read directly from the GPU. A white metal is the
    # white-furnace identity: F0 = 1 with multiple-scattering compensation = 1.
    white_metal = _value(samples, 'brdf white metal', 21)
    if white_metal:
        _check(checks, 'white metal environment BRDF is white-furnace unity',
               all(abs(v - 1.0) <= 0.06 for v in white_metal[:3]),
               white_metal[:3], 1.0, 0.06)
    rough_metal = _value(samples, 'brdf rough metal', 21)
    if white_metal and rough_metal:
        _check(checks, 'white metal scale exceeds rough gold metal',
               _luma(white_metal) > _luma(rough_metal),
               _luma(white_metal), f">{_luma(rough_metal):.5f}")
    black_scale = _value(samples, 'brdf black dielectric', 21)
    if black_scale:
        _check(checks, 'black dielectric uses its IOR Fresnel scale',
               0.02 <= black_scale[0] <= 0.14, black_scale[0], '[0.02,0.14]')
    ior15 = _value(samples, 'brdf ior 15', 21)
    ior20 = _value(samples, 'brdf ior 20', 21)
    if ior15 and ior20:
        _check(checks, 'higher IOR increases the dielectric reflection',
               ior20[0] > ior15[0] * 1.8, ior20[0],
               f">{ior15[0] * 1.8:.5f}")
    spec_zero = _value(samples, 'brdf specular zero', 21)
    if spec_zero:
        _check(checks, 'specularFactor=0 removes the dielectric reflection',
               spec_zero[0] <= 0.01, spec_zero[0], 0.0, 0.01)
    tinted = _value(samples, 'brdf specular tinted', 21)
    if tinted:
        _check(checks, 'specularColor tints the reflection',
               tinted[0] > tinted[1] > tinted[2], tinted[:3],
               'R>G>B')

    # Clearcoat layering: base specular is attenuated by the Fresnel-weighted
    # coat weight, and emission beneath the coat is attenuated too.
    coat_zero = _value(samples, 'brdf coat zero', 17)
    coat_one = _value(samples, 'brdf coat one', 17)
    coat_channel = _value(samples, 'brdf coat one', 18)
    if coat_zero and coat_one:
        _check(checks, 'clearcoat attenuates the base environment specular',
               _luma(coat_one) < _luma(coat_zero) * 0.98,
               _luma(coat_one), f"<{_luma(coat_zero) * 0.98:.5f}")
    if coat_channel:
        _check(checks, 'clearcoat contributes its own layer',
               _luma(coat_channel) > 0.001, _luma(coat_channel), '>0.001')
    coated_metal = _value(samples, 'brdf coated metal', 18)
    if coated_metal:
        _check(checks, 'coated metal has a coat lobe',
               _luma(coated_metal) > 0.001, _luma(coated_metal), '>0.001')
    emission = _value(samples, 'brdf emission', 19)
    if emission:
        _check(checks, 'high emission is not clipped by lighting',
               max(emission[:3]) > 1.0, emission[:3], '>1.0')
    coat_emission = _value(samples, 'brdf coat emission', 19)
    if emission and coat_emission:
        _check(checks, 'coat attenuates emission beneath it',
               max(coat_emission[:3]) < max(emission[:3])
               and max(coat_emission[:3]) > 1.0,
               coat_emission[:3], f"<{max(emission[:3]):.3f} and >1.0")

    transmissive = _value(samples, 'brdf transmissive', 22)
    transmissive_spec = _value(samples, 'brdf transmissive', 17)
    if transmissive:
        _check(checks, 'transmissive composite exposes the interim model',
               abs(transmissive[2] - 1.0) <= 0.02, transmissive[:3],
               'transmission 1')
        _check(checks, 'transmissive alpha carries the interface Fresnel',
               0.01 <= transmissive[0] <= 0.25, transmissive[0], '[0.01,0.25]')
        _check(checks, 'transmissive alpha matches its Fresnel reflectance',
               abs(transmissive[0] - transmissive[1]) <= 0.01,
               transmissive[:2], 'alpha == glassFresnel', 0.01)
    if transmissive_spec:
        _check(checks, 'transmissive surface still reflects the environment',
               _luma(transmissive_spec) > 0.005, _luma(transmissive_spec), '>0.005')


def _check_mode_consistency(checks, studio, directional):
    """Studio and Directional must use the same Fresnel-weighted IBL."""
    for item in EXPECTED_MATERIALS['brdf-probe']:
        name = item['name']
        for channel, tolerance in ((16, 0.006), (17, 0.012)):
            a = _value(studio, name, channel)
            b = _value(directional, name, channel)
            if a is None or b is None:
                _check(checks, f'{name} mode IBL channel {channel} present', False,
                       None, 'sample')
                continue
            delta = max(abs(x - y) for x, y in zip(a[:3], b[:3]))
            _check(checks, f'{name} Studio/Directional IBL channel {channel} match',
                   delta <= tolerance, round(delta, 6), 0.0, tolerance)


def _check_zero_environment(checks, samples):
    for entry in samples:
        name = entry['material']
        channels = entry.get('channels') or {}
        for channel in (16, 17, 18):
            value = channels.get(channel)
            _check(checks, f'{name} env intensity 0 removes channel {channel}',
                   value is not None and max(abs(v) for v in value[:3]) <= 1e-4,
                   value and value[:3], 0.0, 1e-4)
        hdr = channels.get(20)
        if name in ('brdf emission', 'brdf coat emission'):
            _check(checks, f'{name} emission survives zero environment',
                   hdr is not None and max(hdr[:3]) > 1.0, hdr and hdr[:3], '>1.0')
        else:
            _check(checks, f'{name} zero light/environment is black',
                   hdr is not None and max(abs(v) for v in hdr[:3]) <= 1e-3,
                   hdr and hdr[:3], 0.0, 1e-3)


def _check_display(checks, samples, display):
    """Neutral hue retention and the documented display blend order.

    The exact CPU/GPU tone-map comparison uses the flat 0.3 diagnostic patch
    (``_check_flat_tone_map``) so it cannot be confused by a one-pixel spatial
    mismatch between the HDR readback and the captured frame.
    """
    white = next((e for e in samples if e['material'] == 'brdf white dielectric'), None)
    if white and white.get('channels', {}).get(20):
        hdr = white['channels'][20][:3]
        chroma = max(hdr) - min(hdr)
        _check(checks, 'neutral dielectric response keeps hue', chroma <= 0.10,
               round(chroma, 5), 0.0, 0.10)

    blend = next((e for e in samples if e['material'] == 'brdf coverage blend'), None)
    if blend and blend.get('channels', {}).get(20) and blend.get('display'):
        hdr = blend['channels'][20][:3]
        composite = blend.get('channels', {}).get(22) or [1.0]
        alpha = max(0.0, min(1.0, composite[0]))
        mapped = [255.0 * v for v in _srgb_encode(_neutral_tone_map(hdr))]
        # Top-centre is backdrop; the model rows sit below it. The resolve
        # composites the tone-mapped scene over the display-space backdrop.
        index = (5 * display['width'] + display['width'] // 2) * 3
        backdrop = list(display['rgb'][index:index + 3])
        expected = [round(b + (m - b) * alpha) for b, m in zip(backdrop, mapped)]
        actual = blend.get('display')
        delta = max(abs(a - b) for a, b in zip(actual, expected))
        _check(checks, 'linear scene is tone-mapped before display blend',
               delta <= 16, actual, expected, 16)


def _check_flat_tone_map(viewer, centroid, output: Path, checks):
    """CPU/GPU tone-map agreement on the flat 0.3 linear diagnostic patch."""
    set_channel(viewer.hwnd, 23)
    from common import send_field
    send_field(viewer.hwnd, 106, 0)  # force one present
    time.sleep(0.5)
    frame = viewer.capture()
    rect = frame['rect']
    x = rect['left'] + centroid[0]
    y = rect['top'] + centroid[1]
    sample = sample_region(viewer.hwnd, x, y, x + 1, y + 1)
    actual = _pixel(frame, centroid)
    expected = [round(255.0 * v) for v in _srgb_encode(_neutral_tone_map([0.3, 0.3, 0.3]))]
    if sample.get('rgba') is None or actual is None:
        _check(checks, 'flat tone-map patch sampled', False, None, expected)
        return
    hdr = sample['rgba'][:3]
    delta = max(abs(a - b) for a, b in zip(actual, expected))
    _check(checks, 'CPU/GPU neutral tone map agrees (flat patch)', delta <= 3,
           actual, expected, 3)
    _check(checks, 'flat patch reads the authored 0.3 linear value',
           all(abs(v - 0.3) <= 0.002 for v in hdr), hdr, 0.3, 0.002)
    _check(checks, 'flat neutral patch has no hue shift',
           max(actual) - min(actual) <= 1, actual, 'R==G==B')


def _external_survey(viewer, output: Path, fast: bool):
    from corpus import resolve_entry
    baseline = None
    if T21_REPORT.exists():
        try:
            baseline = json.loads(T21_REPORT.read_text(encoding='utf-8')).get('regionSurvey', {})
        except Exception:
            baseline = None
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
        record = {'status': 'sampled', 'entry': str(entry), 'modes': {}}
        for mode_name, mode in (('studio', 0), ('clay', 1), ('directional', 2)):
            viewer.apply_controlled_presentation()
            _set_lighting(viewer, mode)
            view = spec['view']
            viewer.set_camera(view['yawDegrees'], view['pitchDegrees'], view['distanceScale'])
            frame = viewer.wait_stable()
            image = output / 'external' / f'{name}-{mode_name}.png'
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
                for channel in (1, 16, 17, 18, 20):
                    set_channel(viewer.hwnd, channel)
                    samples[channel] = region_sample(viewer.hwnd, x0, y0, x1, y1).get('rgba')
                regions[region_name] = samples
            record['modes'][mode_name] = {
                'image': str(image), 'sha256': sha256_file(image),
                'stats': frame['stats'], 'regions': regions,
            }
            if baseline and name in baseline:
                before = baseline[name].get('regions', {})
                record['modes'][mode_name]['t21Baseline'] = before
        surveys[name] = record
    return surveys


def _check_external(checks, surveys):
    for asset, survey in surveys.items():
        if survey.get('status') != 'sampled':
            _check(checks, f'{asset} external asset sampled', False,
                   survey.get('status'), 'sampled')
            continue
        for mode_name, data in survey['modes'].items():
            stats = data.get('stats') or {}
            _check(checks, f'{asset} {mode_name} frame visible',
                   stats.get('uniqueSampleColours', 0) >= 8
                   and stats.get('lumaStdDev', 0) >= 1.0,
                   stats.get('uniqueSampleColours'), '>=8')
            for region, samples in data['regions'].items():
                hdr = samples.get(20)
                _check(checks, f'{asset} {mode_name} {region} HDR finite',
                       _finite(hdr), hdr and hdr[:3], 'finite')
                if hdr:
                    _check(checks, f'{asset} {mode_name} {region} is not black',
                           max(hdr[:3]) > 1e-4, hdr[:3], '>1e-4')
                    _check(checks, f'{asset} {mode_name} {region} is bounded',
                           max(hdr[:3]) < 200.0, hdr[:3], '<200')
        # Mandatory Car paint / Camera lens nonregression in Studio.
        studio = survey['modes'].get('studio', {})
        region_name = 'paint' if asset == 'car' else 'lens'
        samples = studio.get('regions', {}).get(region_name, {})
        specular = samples.get(17)
        baseline = studio.get('t21Baseline', {}).get(region_name, {}).get(
            'samples', {}).get('environment-specular', {}).get('rgba')
        if specular and baseline:
            before = sum(baseline[:3]) / 3.0
            after = sum(specular[:3]) / 3.0
            ratio = after / before if before > 1e-6 else None
            _check(checks, f'{asset} {region_name} environment specular nonregression',
                   ratio is not None and 0.2 <= ratio <= 5.0,
                   round(ratio, 4) if ratio is not None else None, '[0.2,5.0]')
        elif specular:
            _check(checks, f'{asset} {region_name} environment specular present',
                   sum(specular[:3]) / 3.0 > 0.002, specular[:3], '>0.002')


def _run_serial(configuration: str, output: Path, fast: bool = False):
    exe_dir = ROOT / f'x64/{configuration}'
    lanes = []
    for name, executable, filters in SERIAL_LANES:
        # The fast iteration lane keeps the CPU reference checks but skips the
        # import-worker suite (which the wrapper does not build in fast mode).
        if fast and name.startswith('import-'):
            lanes.append({'name': name, 'skipped': True, 'pass': True,
                          'reason': 'fast iteration lane'})
            continue
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
        '# T24 BRDF, environment and clearcoat report', '',
        'Generated from the CPU reference lanes plus Debug GPU numeric readback,',
        'captured display frames and Car/Camera observations in all three modes.', '',
        '## Checks', '',
        f"- Status: **{report.get('status')}**",
        f"- Checks: {sum(1 for c in report.get('checks', []) if c['pass'])} passed / "
        f"{len(report.get('checks', []))} total",
        f"- Failures: {len(report.get('failures', []))}",
    ]
    for failure in report.get('failures', []):
        lines.append(f'  - {failure}')
    lines += ['', '## GPU probe values (linear)', '',
              '| probe | metallic | roughness | env diffuse | env specular | coat | emission | BRDF scale |',
              '| --- | --- | --- | --- | --- | --- | --- | --- |']
    for entry in report.get('studioSamples', []):
        ch = entry.get('channels', {})
        def fmt(channel):
            value = ch.get(channel)
            return 'n/a' if not value else '[' + ', '.join(f'{v:.4f}' for v in value[:3]) + ']'
        lines.append(f"| {entry['material']} | {fmt(6)} | {fmt(7)} | {fmt(16)} | "
                     f"{fmt(17)} | {fmt(18)} | {fmt(19)} | {fmt(21)} |")
    lines += ['', '## External observations', '']
    for asset, survey in report.get('externalSurvey', {}).items():
        if survey.get('status') != 'sampled':
            lines.append(f"- **{asset}**: {survey.get('status')}"
                         f" — {survey.get('reason', '')}")
            continue
        for mode, data in survey.get('modes', {}).items():
            lines.append(f"- **{asset}/{mode}**: `{data.get('image')}`")
    lines += [
        '', '## Interpretation', '',
        '- The split-sum LUT and GGX prefilter are checked against independent',
        '  uniform-quadrature integrals; white metal is the white-furnace identity.',
        '- Studio and Directional share the same Fresnel-weighted IBL channels;',
        '  Directional no longer adds a second unweighted transmission reflection.',
        '- Clearcoat now mixes the base toward the coat by c*Fresnel and attenuates',
        '  base specular, diffuse, sheen and emission by (1-c*Fresnel).',
        '- KHR_materials_specular keeps F0 (IOR*color) and F90 (specularFactor)',
        '  independent at normal and grazing incidence.',
        '- Transmission still uses the interim alpha-carried interface Fresnel;',
        '  channel 22 exposes (alpha, glassReflectance, transmission) until T27.',
        '', '## Evidence', '',
        f"- `{output / 'diagnostics' / 'brdf.json'}`",
        f"- `{output / 'external'}` mode images",
        '', ]
    path = output / 'brdf-report.md'
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def run(workspace: Path, output: Path, configuration: str, fast: bool = False):
    output = Path(output)
    report = {'schema': RESULT_SCHEMA, 'task': 'T24', 'checks': [],
              'failures': [], 'status': 'running', 'fast': fast}
    report['serialLanes'] = _run_serial(configuration, output, fast)
    for lane in report['serialLanes']:
        if not lane.get('pass'):
            report['failures'].append(f"serial lane failed: {lane['name']}")

    fixture = output / 'fixtures' / 'brdf-probe.glb'
    if not fixture.exists():
        report['failures'].append(f'brdf-probe fixture missing: {fixture}')
        report['status'] = 'fail'
        write_json(output / 'diagnostics' / 'brdf.json', report)
        return report

    try:
        with Viewer(configuration, output, width=1280, height=860) as viewer:
            viewer.open(fixture)
            viewer.apply_controlled_presentation()
            # The numeric probes are located from the base-colour channel. The
            # display-space ground highlight and grid are bright neutral
            # surfaces that would compete with the white probes, so the numeric
            # session turns both off (the capture lane keeps the product
            # presentation).
            _send(viewer.hwnd, 104, 0)   # grid off
            _send(viewer.hwnd, 102, 0)   # contact shadow off
            _send(viewer.hwnd, 196, 0)   # ground highlight off
            viewer.set_camera(0.0, 90.0, 1.4)

            # Material identity from the validated wire snapshot.
            records = material_records(viewer.hwnd)
            report['materialRecords'] = records
            identity_checks, matched = _check_material_records(
                records, EXPECTED_MATERIALS['brdf-probe'])
            # The shared checker expects a few T22-only fields; keep only the
            # checks whose source record exists.
            report['materialChecks'] = matched
            report['checks'].extend(identity_checks)

            # Studio: full channel sweep.
            _set_lighting(viewer, 0)
            _set_environment_intensity(viewer, 1.0)
            studio, centroids, display = _sample_probes(viewer, CHANNELS_ALL)
            report['studioSamples'] = studio
            report['centroids'] = centroids
            _check_basic_channels(report['checks'], studio)
            _check_material_values(report['checks'], studio)
            _check_energy(report['checks'], studio)
            _check_display(report['checks'], studio, display)
            white = next((e for e in studio
                          if e['material'] == 'brdf white dielectric'), None)
            if white and white.get('centroid'):
                _check_flat_tone_map(viewer, white['centroid'], output,
                                     report['checks'])
            report['channelImages'] = _save_channel_images(
                viewer, output / 'diagnostics', [1, 17, 18, 19, 20, 21, 22])
            set_channel(viewer.hwnd, 0)

            # Directional: same IBL, different direct rig.
            _set_lighting(viewer, 2)
            directional, _, _ = _sample_probes(viewer, CHANNELS_IBL)
            report['directionalSamples'] = directional
            _check_mode_consistency(report['checks'], studio, directional)

            # Clay: environment retained, specular suppressed, coat absent.
            _set_lighting(viewer, 1)
            clay_samples, _, clay_display = _sample_probes(viewer, [12, 16, 17])
            report['claySamples'] = clay_samples
            clay_spec = _value(clay_samples, 'brdf white dielectric', 17)
            studio_spec = _value(studio, 'brdf white dielectric', 17)
            if clay_spec and studio_spec:
                _check(report['checks'], 'Clay reduces environment specular',
                       _luma(clay_spec) < _luma(studio_spec) * 0.5,
                       _luma(clay_spec), f"<{_luma(studio_spec) * 0.5:.5f}")
            clay_coat = _value(clay_samples, 'brdf coat one', 12)
            if clay_coat:
                _check(report['checks'], 'Clay suppresses clearcoat',
                       clay_coat[0] <= 0.02, clay_coat[0], 0.0, 0.02)

            # Zero light and zero environment. Every environment channel must
            # vanish; emission is the only surviving term.
            _set_lighting(viewer, 0)
            _set_environment_intensity(viewer, 0.0)
            zero, _, _ = _sample_probes(viewer, CHANNELS_ZERO)
            report['zeroEnvironmentSamples'] = zero
            _check_zero_environment(report['checks'], zero)
            _set_environment_intensity(viewer, 1.0)
            _set_lighting(viewer, 0)

            if not fast:
                report['externalSurvey'] = _external_survey(viewer, output, fast)
                _check_external(report['checks'], report['externalSurvey'])
            else:
                report['externalSurvey'] = {}
                report['externalNote'] = 'skipped by the fast iteration lane'
    except Exception as error:
        report['failures'].append(f'GPU BRDF lane failed: {error}')

    if not report.get('studioSamples'):
        report['failures'].append('no studio probe samples were captured')
    failed = [check for check in report['checks'] if not check['pass']]
    if failed:
        report['failures'].append(f'{len(failed)} BRDF/numeric checks failed')
    if not report.get('materialChecks'):
        report['failures'].append('no BRDF material records were matched')
    report['status'] = 'pass' if not report['failures'] else 'fail'
    write_json(output / 'diagnostics' / 'brdf.json', report)
    report['reportPath'] = str(write_report(output, report))
    return report
