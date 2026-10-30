"""Negative contract tests: prove the harness rejects the failure modes T20 lists."""
from __future__ import annotations

import json
from pathlib import Path
import tempfile

from common import crop, encoded_png, image_stats, write_json
from compare import camera_contract, capture_contract, compare_frames, lane_contract
from corpus import external_lane_status


def gradient(width, height, offset=0):
    rgb = bytearray(width * height * 3)
    for y in range(height):
        for x in range(width):
            i = (y * width + x) * 3
            rgb[i] = (x * 255 // max(1, width - 1) + offset) % 256
            rgb[i + 1] = (y * 255 // max(1, height - 1) + offset) % 256
            rgb[i + 2] = ((x + y) * 255 // max(1, width + height - 2) + offset) % 256
    return bytes(rgb)


def run(output: Path):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    width, height = 160, 100
    reference = gradient(width, height)
    reference_path = output / 'reference.png'
    reference_path.write_bytes(encoded_png(width, height, reference))
    results = []

    def record(name, failure, detail):
        results.append({'scenario': name, 'expected': 'nonzero/failure',
                        'observedFailure': bool(failure), 'detail': detail,
                        'pass': bool(failure)})

    # Deliberately damaged window/lens region.
    damaged = bytearray(reference)
    x0, y0, x1, y1 = 40, 20, 120, 70
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = (y * width + x) * 3
            damaged[i:i + 3] = bytes((230, 20, 20))
    comparison = compare_frames(
        {'width': width, 'height': height, 'rgb': bytes(damaged)},
        reference_path, regions={'lens': [0.25, 0.2, 0.75, 0.7]},
        thresholds={'regionMae': 0.02, 'globalMae': 0.01})
    record('damaged-window-lens-region', not comparison['pass'], comparison['reasons'])

    blank = bytes([18, 18, 18]) * (width * height)
    contract = capture_contract({
        'width': width, 'height': height, 'settled': True, 'complete': True,
        'generationMatched': True, 'stats': image_stats(width, height, blank)})
    record('blank-capture', not contract['pass'], contract['reasons'])

    stale = capture_contract({
        'width': width, 'height': height, 'settled': False, 'complete': False,
        'generationMatched': False, 'stats': image_stats(width, height, reference)})
    record('stale-proxy-only-capture', not stale['pass'], stale['reasons'])

    changed = camera_contract(
        {'yawDegrees': 30, 'pitchDegrees': -22, 'distanceScale': 1.0,
         'projection': 'perspective', 'viewport': [1280, 772]},
        {'yawDegrees': 0, 'pitchDegrees': 0, 'distanceScale': 1.0,
         'projection': 'perspective', 'viewport': [1280, 772]})
    record('changed-camera', not changed['pass'], changed['reasons'])

    missing_corpus = external_lane_status({
        'available': False, 'reason': 'external corpus root is not present'})
    record('missing-corpus-entry', missing_corpus['status'] != 'available', missing_corpus)

    # Ignored lane: a required lane with an empty selection must fail.
    ignored = lane_contract(['generated'], {})
    record('ignored-empty-lane', not ignored['pass'], ignored['failures'])

    # Missing accepted reference path.
    from compare import verify_case
    with tempfile.TemporaryDirectory() as temp:
        missing = verify_case({'id': 'selftest-missing-reference',
                               'image': 'does-not-exist.png'}, Path(temp))
    record('missing-reference', not missing['pass'], missing['reasons'])

    # Accepted reference exists but the candidate image is absent.
    with tempfile.TemporaryDirectory() as temp:
        missing_image = verify_case({
            'id': 'generated-sphere-grid-studio-front',
            'image': 'generated-sphere-grid-studio-front.png'}, Path(temp))
    record('missing-image', not missing_image['pass'], missing_image['reasons'])

    report = {'schema': 1, 'scenarios': results,
              'pass': all(item['pass'] for item in results)}
    write_json(output / 'contract-selftest.json', report)
    return report
