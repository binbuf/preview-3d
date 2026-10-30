"""Image comparison, capture-contract checks and baseline promotion."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil

from common import (ROOT, crop, decode_png, downsampled, encoded_png, image_stats,
                    mae, read_json, sha256_bytes, sha256_file, ssim, write_json)

ACCEPTED = ROOT / 'tests' / 'render-quality' / 'baselines' / 'accepted'
OBSERVED = ROOT / 'tests' / 'render-quality' / 'baselines' / 'observed'

DEFAULT_THRESHOLDS = {
    'globalMae': 0.06,
    'globalSsim': 0.90,
    'regionMae': 0.09,
    'minLumaStdDev': 1.0,
    'minUniqueSampleColours': 8,
    'cameraYawDegrees': 0.5,
    'cameraPitchDegrees': 0.5,
    'cameraDistanceScale': 0.01,
}


def load_image(path: Path):
    width, height, rgb = decode_png(Path(path).read_bytes())
    return width, height, rgb, downsampled(width, height, rgb)


def compare_frames(candidate, reference, regions=None, thresholds=None):
    thresholds = {**DEFAULT_THRESHOLDS, **(thresholds or {})}
    width, height, rgb = candidate['width'], candidate['height'], candidate['rgb']
    ref_width, ref_height, ref_rgb, ref_samples = load_image(reference)
    result = {'reference': str(reference), 'pass': True, 'reasons': []}
    if (width, height) != (ref_width, ref_height):
        result['pass'] = False
        result['reasons'].append(
            f'viewport changed: {width}x{height} != {ref_width}x{ref_height}')
        return result
    samples = downsampled(width, height, rgb)
    result['globalMae'] = round(mae(samples, ref_samples), 6)
    result['globalSsim'] = round(ssim(samples, ref_samples), 6)
    if result['globalMae'] > thresholds['globalMae']:
        result['pass'] = False
        result['reasons'].append(
            f'global MAE {result["globalMae"]:.4f} > {thresholds["globalMae"]}')
    if result['globalSsim'] < thresholds['globalSsim']:
        result['pass'] = False
        result['reasons'].append(
            f'global SSIM {result["globalSsim"]:.4f} < {thresholds["globalSsim"]}')
    result['regions'] = {}
    for name, rect in (regions or {}).items():
        cw, ch, crgb = crop(width, height, rgb, rect)
        rw, rh, rrgb = crop(ref_width, ref_height, ref_rgb, rect)
        if (cw, ch) != (rw, rh):
            result['pass'] = False
            result['reasons'].append(f'region {name} geometry changed')
            continue
        value = mae(downsampled(cw, ch, crgb), downsampled(rw, rh, rrgb))
        result['regions'][name] = {'mae': round(value, 6), 'rect': rect}
        if value > thresholds['regionMae']:
            result['pass'] = False
            result['reasons'].append(
                f'region {name} MAE {value:.4f} > {thresholds["regionMae"]}')
    return result


def capture_contract(capture, thresholds=None):
    """Reject blank, stale/proxy-only, unsettled or malformed captures."""
    thresholds = {**DEFAULT_THRESHOLDS, **(thresholds or {})}
    reasons = []
    stats = capture.get('stats') or {}
    if not capture.get('settled'):
        reasons.append('frames were not settled')
    if not capture.get('complete'):
        reasons.append('model was not a complete load (proxy-only or terminal failure)')
    if not capture.get('generationMatched'):
        reasons.append('capture generation does not match the accepted load')
    if stats.get('lumaStdDev', 0) < thresholds['minLumaStdDev']:
        reasons.append(f'capture is blank (luma std dev {stats.get("lumaStdDev")})')
    if stats.get('uniqueSampleColours', 0) < thresholds['minUniqueSampleColours']:
        reasons.append('capture is a flat/placeholder image')
    if capture.get('width', 0) < 64 or capture.get('height', 0) < 64:
        reasons.append('capture viewport is too small')
    return {'pass': not reasons, 'reasons': reasons}


def camera_contract(camera, reference, thresholds=None):
    thresholds = {**DEFAULT_THRESHOLDS, **(thresholds or {})}
    reasons = []
    for key, tolerance in (('yawDegrees', 'cameraYawDegrees'),
                           ('pitchDegrees', 'cameraPitchDegrees'),
                           ('distanceScale', 'cameraDistanceScale')):
        if abs(float(camera.get(key, 0)) - float(reference.get(key, 0))) > thresholds[tolerance]:
            reasons.append(f'{key} changed: {camera.get(key)} != {reference.get(key)}')
    if camera.get('projection') != reference.get('projection'):
        reasons.append('projection changed')
    if list(camera.get('viewport', [])) != list(reference.get('viewport', [])):
        reasons.append('physical viewport pixels changed')
    return {'pass': not reasons, 'reasons': reasons}


def lane_contract(required_lanes, selected_lanes):
    """A required lane with no registered cases fails instead of passing empty."""
    failures = [f'required lane "{lane}" produced no cases'
                for lane in required_lanes if selected_lanes.get(lane, 0) == 0]
    return {'pass': not failures, 'failures': failures,
            'required': list(required_lanes), 'selected': dict(selected_lanes)}


def accepted_record(case_id):
    path = ACCEPTED / 'accepted.json'
    if not path.exists():
        return None
    manifest = read_json(path)
    return manifest.get('cases', {}).get(case_id)


def verify_case(case, capture_dir: Path):
    """Compare one capture against its required accepted reference."""
    case_id = case['id']
    record = accepted_record(case_id)
    if record is None:
        return {'case': case_id, 'status': 'missing-reference',
                'pass': False, 'reasons': ['no accepted regression reference']}
    reference = ACCEPTED / record['image']
    if not reference.exists():
        return {'case': case_id, 'status': 'missing-reference',
                'pass': False, 'reasons': [f'missing image {reference.name}']}
    candidate_path = Path(capture_dir) / case['image']
    if not candidate_path.exists():
        return {'case': case_id, 'status': 'missing-image',
                'pass': False, 'reasons': [f'missing capture {candidate_path.name}']}
    candidate = capture_case(candidate_path, case, capture_meta(capture_dir, case))
    contract = capture_contract(candidate, case.get('thresholds'))
    candidate_camera = {
        **(candidate.get('camera') or {}),
        'viewport': [candidate.get('width'), candidate.get('height')],
    }
    reference_camera = {
        **(record.get('camera') or {}),
        'viewport': record.get('viewport'),
    }
    camera = camera_contract(candidate_camera, reference_camera,
                             case.get('thresholds'))
    width, height, rgb, _ = load_image(candidate_path)
    comparison = compare_frames(
        {'width': width, 'height': height, 'rgb': rgb}, reference,
        record.get('regions') or case.get('regions'), case.get('thresholds'))
    passed = contract['pass'] and camera['pass'] and comparison['pass']
    return {
        'case': case_id, 'status': 'pass' if passed else 'fail', 'pass': passed,
        'reasons': contract['reasons'] + camera['reasons'] + comparison['reasons'],
        'contract': contract, 'camera': camera, 'comparison': comparison,
    }


def capture_case(path: Path, case, meta=None):
    width, height, rgb = decode_png(Path(path).read_bytes())
    result = {
        'width': width, 'height': height, 'rgb': rgb,
        'stats': image_stats(width, height, rgb),
        'path': str(path),
    }
    result.update(meta or {})
    return result


def capture_meta(capture_dir: Path, case):
    meta_path = Path(capture_dir) / 'captures.json'
    if not meta_path.exists():
        return {}
    manifest = read_json(meta_path)
    return manifest.get('captures', {}).get(case['id'], {})


def promote(args):
    """Explicit baseline promotion: candidate -> immutable accepted reference."""
    candidate = Path(args.candidate)
    if not candidate.exists():
        raise SystemExit(f'candidate not found: {candidate}')
    width, height, rgb = decode_png(candidate.read_bytes())
    meta = {}
    if args.captures:
        meta = read_json(Path(args.captures)).get('captures', {}).get(args.case, {})
    accepted = ACCEPTED
    accepted.mkdir(parents=True, exist_ok=True)
    (accepted / 'promotions').mkdir(exist_ok=True)
    target = accepted / f'{args.case}.png'
    before = accepted / f'{args.case}.png'
    promotion_dir = accepted / 'promotions'
    promotion = {
        'case': args.case,
        'reason': args.reason,
        'reviewer': args.reviewer,
        'review': args.review,
        'candidate': str(candidate),
        'candidateSha256': sha256_file(candidate),
        'beforeSha256': sha256_file(before) if before.exists() else None,
        'image': target.name,
        'camera': meta.get('camera'),
        'viewport': [width, height],
        'regions': meta.get('regions', {}),
        'stats': image_stats(width, height, rgb),
        'crops': {},
    }
    candidate_copy = promotion_dir / f'{args.case}-candidate.png'
    shutil.copyfile(candidate, candidate_copy)
    promotion['candidateCopy'] = str(candidate_copy.relative_to(ROOT))
    for name, rect in (meta.get('regions') or {}).items():
        cw, ch, crgb = crop(width, height, rgb, rect)
        crop_path = promotion_dir / f'{args.case}-crop-{name}.png'
        crop_path.write_bytes(encoded_png(cw, ch, crgb))
        promotion['crops'][name] = str(crop_path.relative_to(ROOT))
    if before.exists():
        diff = promotion_dir / f'{args.case}-before.png'
        shutil.copyfile(before, diff)
        promotion['before'] = str(diff.relative_to(ROOT))
        bw, bh, brgb = decode_png(before.read_bytes())
        if (bw, bh) == (width, height):
            delta = bytearray(width * height * 3)
            for i in range(0, len(delta), 3):
                for channel in range(3):
                    value = abs(rgb[i + channel] - brgb[i + channel])
                    delta[i + channel] = min(255, value * 3)
            diff_path = promotion_dir / f'{args.case}-diff.png'
            diff_path.write_bytes(encoded_png(width, height, bytes(delta)))
            promotion['diff'] = str(diff_path.relative_to(ROOT))
    target.write_bytes(candidate.read_bytes())
    manifest_path = accepted / 'accepted.json'
    manifest = read_json(manifest_path) if manifest_path.exists() else {
        'schema': 1, 'cases': {}, 'note': 'Accepted regression references. '
        'Ordinary verification never writes this file; use promote.py explicitly.'}
    manifest['cases'][args.case] = {
        'image': target.name,
        'sha256': sha256_file(target),
        'camera': meta.get('camera'),
        'viewport': [width, height],
        'regions': meta.get('regions', {}),
        'thresholds': json.loads(args.thresholds) if args.thresholds else None,
        'promotion': str((accepted / 'promotions' / f'{args.case}.json').relative_to(ROOT)),
    }
    write_json(manifest_path, manifest)
    write_json(accepted / 'promotions' / f'{args.case}.json', promotion)
    print(json.dumps(promotion, indent=2))
    return promotion


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', required=True)
    parser.add_argument('--candidate', required=True)
    parser.add_argument('--captures', help='captures.json produced by the runner')
    parser.add_argument('--reason', required=True)
    parser.add_argument('--reviewer', required=True)
    parser.add_argument('--review', required=True,
                        help='path or identifier of the image-review record')
    parser.add_argument('--thresholds', help='JSON object overriding tolerances')
    promote(parser.parse_args())


if __name__ == '__main__':
    main()
