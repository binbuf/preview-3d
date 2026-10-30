"""T20 render-quality verification wrapper.

Usage:
    python tests/render-quality/verify.py --task T20 --configuration Debug \
        --output TestResults/render-quality/T20

The wrapper builds the changed Debug viewer, runs the negative capture-contract
self-tests, captures generated fixtures and the restricted local corpus, renders
the installed F3D reference conditions, and compares generated cases against
accepted regression references. It never creates or promotes a reference.

Bootstrap:
    python tests/render-quality/verify.py --task T20 --configuration Debug \
        --output TestResults/render-quality/T20 --capture-only

``--capture-only`` is an explicit evidence-collection mode, not a passing
verification lane: it skips regression comparison and exits nonzero on capture
failure. Promotion is a separate command (compare.py).
"""
from __future__ import annotations

import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import math
from pathlib import Path
import platform
import struct
import subprocess
import sys
import time

from common import (ROOT, RESULT_SCHEMA, decode_png, encoded_png, send_field,
                    sha256_file, write_json)
import compare
import corpus
import diagnostics
import env
import fixtures
import selftest
from viewer import LoadFailed, Viewer, ViewerError, set_dpi_awareness

PROFILE_DIR = Path(__file__).resolve().parent / 'profiles'
MSBUILD = Path('C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe')
F3D = Path('C:/Program Files/F3D/bin/f3d.exe')
F3D_PRESET = Path('C:/Program Files/F3D/share/f3d/configs/config.d/05_all.json')


def profile_for(task: str):
    path = PROFILE_DIR / f'{task}.json'
    if not path.exists():
        raise SystemExit(f'unknown or unimplemented profile for task {task}: {path}')
    profile = json.loads(path.read_text(encoding='utf-8'))
    if profile.get('task') != task:
        raise SystemExit(f'profile {path} declares task {profile.get("task")}, expected {task}')
    return profile


def build(configuration, output: Path, enabled=True):
    if not enabled:
        return {'command': None, 'skipped': True, 'pass': True}
    if not MSBUILD.exists():
        return {'command': str(MSBUILD), 'skipped': False, 'pass': False,
                'reason': 'MSBuild not found'}
    command = [str(MSBUILD), 'Preview3D.slnx', '/t:Preview3D',
               f'/p:Configuration={configuration}', '/p:Platform=x64', '/m', '/v:minimal']
    output.mkdir(parents=True, exist_ok=True)
    log_path = output / 'build.log'
    started = time.monotonic()
    with log_path.open('w', encoding='utf-8') as log:
        completed = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    return {
        'command': ' '.join(command), 'exitCode': completed.returncode,
        'seconds': round(time.monotonic() - started, 2), 'log': str(log_path),
        'pass': completed.returncode == 0, 'skipped': False,
    }


def case_id(lane, asset, mode, view):
    return f'{lane}-{asset}-{mode}-{view}'


def build_cases(profile, inventory_record):
    cases = []
    regions = profile.get('regions', {})
    modes = {item['id']: item for item in profile['modes']}
    views = profile['views']
    generated = profile['lanes']['generated']
    for fixture in generated['fixtures']:
        name = fixture['id']
        cases.append({
            'id': f'generated-{name}-default', 'lane': 'generated', 'asset': name,
            'mode': None, 'view': 'default', 'regression': False,
            'image': f'generated-{name}-default.png',
            'regions': regions.get(name, {}),
            'note': 'First-open/default presentation; not numerically compared because '
                    'it intentionally includes the product default framing.',
        })
        for mode in fixture['modes']:
            for view in fixture['views']:
                cases.append({
                    'id': case_id('generated', name, mode, view), 'lane': 'generated',
                    'asset': name, 'mode': mode, 'view': view,
                    'regression': bool(generated.get('regression')),
                    'image': f'{case_id("generated", name, mode, view)}.png',
                    'regions': regions.get(name, {}),
                })
    if not inventory_record.get('available'):
        return cases
    for asset in profile['lanes']['external']['assets']:
        name = asset['id']
        seed = inventory_record['seedAssets'].get(name)
        if not seed or not seed.get('available'):
            continue
        cases.append({
            'id': f'external-{name}-default', 'lane': 'external', 'asset': name,
            'mode': None, 'view': 'default', 'regression': False,
            'image': f'external-{name}-default.png',
            'regions': regions.get(name, {}),
            'knownDefect': asset.get('knownDefect'),
            'note': 'First-open/default presentation; observed only.',
        })
        for mode in asset['modes']:
            for view in asset['views']:
                cases.append({
                    'id': case_id('external', name, mode, view), 'lane': 'external',
                    'asset': name, 'mode': mode, 'view': view, 'regression': False,
                    'image': f'{case_id("external", name, mode, view)}.png',
                    'regions': regions.get(name, {}),
                    'knownDefect': asset.get('knownDefect'),
                })
        for closeup in asset.get('closeups', []):
            for mode in asset['modes']:
                view = closeup
                cases.append({
                    'id': case_id('external', name, mode, view), 'lane': 'external',
                    'asset': name, 'mode': mode, 'view': view, 'regression': False,
                    'image': f'{case_id("external", name, mode, view)}.png',
                    'regions': regions.get(name, {}),
                    'knownDefect': asset.get('knownDefect'),
                })
        if asset.get('orbit'):
            for step in profile['orbit']:
                cases.append({
                    'id': f'external-{name}-studio-orbit-{step["id"]}',
                    'lane': 'external', 'asset': name, 'mode': 'studio',
                    'view': f'orbit-{step["id"]}', 'regression': False,
                    'image': f'external-{name}-studio-orbit-{step["id"]}.png',
                    'regions': regions.get(name, {}), 'knownDefect': asset.get('knownDefect'),
                })
    return cases


def camera_for(profile, case):
    if case['view'] == 'default':
        return None
    if case['view'].startswith('orbit-'):
        step_id = case['view'].split('orbit-', 1)[1]
        return next((step for step in profile['orbit'] if step['id'] == step_id), None)
    return profile['views'].get(case['view'])


def capture_cases(profile, inventory_record, output: Path, configuration):
    cases = build_cases(profile, inventory_record)
    if not cases:
        raise SystemExit('empty test selection: no capture cases were registered')
    image_dir = output / 'images'
    image_dir.mkdir(parents=True, exist_ok=True)
    records = {}
    failures = []
    generated_root = output / 'fixtures'

    def save(viewer, case, lighting=None, camera=None, settled=True, complete=True,
             generation_matched=True, failure=None):
        if case['view'] != 'default':
            viewer.apply_controlled_presentation()
        if lighting is not None:
            send_field(viewer.hwnd, 88, lighting)
            time.sleep(0.35)
        if camera is not None:
            viewer.set_camera(camera['yawDegrees'], camera['pitchDegrees'],
                              camera['distanceScale'])
        else:
            send_field(viewer.hwnd, 36, 0)
            time.sleep(0.3)
        frame = viewer.wait_stable() if settled else viewer.capture()
        path = image_dir / case['image']
        path.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
        record = {
            'id': case['id'], 'lane': case['lane'], 'asset': case['asset'],
            'mode': case['mode'], 'view': case['view'],
            'image': path.name, 'path': str(path),
            'width': frame['width'], 'height': frame['height'],
            'client': frame['client'], 'dpi': frame['dpi'],
            'dpiScale': frame['dpiScale'], 'viewportCrop': frame['rect'],
            'stats': frame['stats'], 'sha256': sha256_file(path),
            'settled': bool(settled and not failure),
            'complete': bool(complete and not failure),
            'generationMatched': bool(generation_matched and not failure),
            'camera': viewer.camera_state(), 'model': viewer.model_state(),
            'presentation': viewer.presentation_state(),
            'performance': viewer.performance(),
            'regions': case.get('regions', {}),
            'knownDefect': case.get('knownDefect'),
            'failure': failure,
            'capturedAt': time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime()),
        }
        records[case['id']] = record
        return record

    with Viewer(configuration, output,
                width=profile['viewport']['width'],
                height=profile['viewport']['height']) as viewer:
        required = sum(profile.get('requiredCapabilities', {}).values())
        available = viewer.capabilities()
        if required and (available & required) != required:
            raise ViewerError(
                f'required capture capabilities missing: wanted 0x{required:x}, '
                f'viewer reports 0x{available:x}')
        # Generated fixtures are captured first: they must work without the
        # external corpus and fail the lane on their own if broken.
        generated = fixtures.generate(generated_root)
        for fixture in profile['lanes']['generated']['fixtures']:
            name = fixture['id']
            path = Path(generated[name]['path'])
            try:
                generation, load_seconds, _ = viewer.open(path)
            except LoadFailed as error:
                failures.append(f'generated fixture {name} failed to load: {error}')
                continue
            default_case = next(item for item in cases
                                if item['id'] == f'generated-{name}-default')
            default_record = save(viewer, default_case)
            default_record['loadSeconds'] = round(load_seconds, 3)
            for case in [item for item in cases if item['lane'] == 'generated'
                         and item['asset'] == name and item['view'] != 'default']:
                mode = profile['modes']
                lighting = next(item['lighting'] for item in mode if item['id'] == case['mode'])
                record = save(viewer, case, lighting=lighting,
                              camera=camera_for(profile, case))
                record['generation'] = generation
                record['generationMatched'] = viewer.model_state()['generation'] == generation

        # External corpus: the external qualification lane reports unavailable
        # assets as incomplete. It never turns a skip into success.
        if inventory_record.get('available'):
            for asset in profile['lanes']['external']['assets']:
                name = asset['id']
                seed = inventory_record['seedAssets'].get(name)
                if not seed or not seed.get('available'):
                    continue
                try:
                    generation, load_seconds, _ = viewer.open(Path(seed['entry']))
                except LoadFailed as error:
                    # Keep the observed partial image, but never call it a
                    # complete load. The microscope ResourceLimit is the known
                    # case; any other failure is still recorded honestly.
                    frame = viewer.capture()
                    path = image_dir / f'external-{name}-failed-load.png'
                    path.write_bytes(encoded_png(
                        frame['width'], frame['height'], frame['rgb']))
                    record = {
                        'id': f'external-{name}-failed-load', 'lane': 'external',
                        'asset': name, 'mode': None, 'view': 'failed-load',
                        'image': path.name, 'path': str(path),
                        'width': frame['width'], 'height': frame['height'],
                        'client': frame['client'], 'dpi': frame['dpi'],
                        'dpiScale': frame['dpiScale'], 'viewportCrop': frame['rect'],
                        'stats': frame['stats'], 'sha256': sha256_file(path),
                        'settled': False, 'complete': False, 'generationMatched': False,
                        'camera': viewer.camera_state(), 'model': viewer.model_state(),
                        'presentation': viewer.presentation_state(),
                        'performance': viewer.performance(), 'regions': {},
                        'knownDefect': asset.get('knownDefect'),
                        'failure': {'message': str(error), 'code': getattr(error, 'code', None),
                                    'stage': getattr(error, 'stage', None),
                                    'state': getattr(error, 'state', None)},
                        'capturedAt': time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime()),
                    }
                    records[record['id']] = record
                    if not asset.get('knownDefect'):
                        failures.append(f'external asset {name} failed to load: {error}')
                    continue
                default_case = next(item for item in cases
                                    if item['id'] == f'external-{name}-default')
                default_record = save(viewer, default_case)
                default_record['loadSeconds'] = round(load_seconds, 3)
                for case in [item for item in cases if item['lane'] == 'external'
                             and item['asset'] == name and item['view'] != 'default']:
                    if case['view'] == 'default' or case['id'].endswith('-failed-load'):
                        continue
                    lighting = next(item['lighting'] for item in profile['modes']
                                    if item['id'] == case['mode'])
                    record = save(viewer, case, lighting=lighting,
                                  camera=camera_for(profile, case))
                    record['generation'] = generation
                    record['generationMatched'] = viewer.model_state()['generation'] == generation

    write_json(output / 'captures.json', {'schema': 1, 'captures': records})
    write_json(image_dir / 'captures.json', {'schema': 1, 'captures': records})
    return cases, records, failures


def render_f3d(profile, inventory_record, output: Path):
    reference = output / 'reference'
    reference.mkdir(parents=True, exist_ok=True)
    lane = {'status': 'available', 'renders': [], 'commands': []}
    if not F3D.exists():
        return {'status': 'incomplete', 'reason': 'installed F3D not found', 'renders': []}
    if not F3D_PRESET.exists():
        return {'status': 'incomplete', 'reason': 'shipped 05_all.json preset not found',
                'renders': []}
    assets = inventory_record.get('seedAssets', {})
    for alias in profile['lanes']['reference']['f3d']['assets']:
        seed = assets.get(alias)
        if not seed or not seed.get('available'):
            lane['status'] = 'incomplete'
            lane['reason'] = f'missing reference asset {alias}'
            continue
        for configuration in profile['lanes']['reference']['f3d']['configurations']:
            if configuration == 'shipped-preset':
                command = [str(F3D), seed['entry'], '--config', str(F3D_PRESET)]
            else:
                # Recorded controlled condition: no configuration file, explicit
                # up axis and view angle, default neutral material rendering.
                command = [str(F3D), seed['entry'], '--no-config', '--up', '+Z',
                           '--camera-view-angle', '45']
            image = reference / f'f3d-{alias}-{configuration}.png'
            command += ['--output', str(image), '--resolution', '1280,900']
            started = time.monotonic()
            try:
                completed = subprocess.run(command, capture_output=True, text=True,
                                           timeout=240,
                                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                ok = completed.returncode == 0 and image.exists()
                error = None if ok else ((completed.stderr or completed.stdout)[-2000:])
            except subprocess.TimeoutExpired:
                ok, error = False, 'F3D render timed out'
            size = None
            if image.exists():
                try:
                    width, height, _ = decode_png(image.read_bytes())
                    size = [width, height]
                except Exception as decode_error:
                    ok, error, size = False, str(decode_error), None
            render = {
                'asset': alias, 'configuration': configuration,
                'command': ' '.join(command), 'image': str(image),
                'width': size[0] if size else None, 'height': size[1] if size else None,
                'sha256': sha256_file(image) if image.exists() else None,
                'seconds': round(time.monotonic() - started, 2),
                'pass': bool(ok), 'error': error,
            }
            lane['renders'].append(render)
            lane['commands'].append(' '.join(command))
            if not ok:
                lane['status'] = 'fail'
    return lane


def khronos_route():
    return {
        'status': 'incomplete',
        'route': 'Render tests/render-quality generated glass-reference.glb with the '
                 'Khronos glTF Sample Viewer (KHR_materials_transmission + volume) and '
                 'compare glass regions side by side.',
        'binary': None,
        'reason': 'No Khronos sample viewer is installed on this workstation; F3D 3.5.0 '
                  'gray Camera lens is recorded as a defect, not an accepted target.',
        'providerSettingsChanged': False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--task', required=True)
    parser.add_argument('--configuration', choices=['Debug', 'Release'], default='Debug')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--no-build', action='store_true')
    parser.add_argument('--capture-only', action='store_true',
                        help='Collect candidate images for review/promotion; not a passing lane')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    set_dpi_awareness()

    report = {
        'schema': RESULT_SCHEMA, 'task': args.task, 'configuration': args.configuration,
        'generatedAt': time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime()),
        'commands': [], 'failures': [], 'status': 'running',
        'captureOnly': args.capture_only,
    }
    report['commands'].append(' '.join(sys.argv))

    profile = profile_for(args.task)
    report['profile'] = str(PROFILE_DIR / f'{args.task}.json')
    build_result = build(args.configuration, output, enabled=not args.no_build)
    report['build'] = build_result
    if build_result.get('command'):
        report['commands'].append(build_result['command'])
    if not build_result['pass']:
        report['failures'].append(f'build failed (see {build_result.get("log")})')

    # T21 must prove the inspection surface is absent from a shipping binary.
    # Build Release explicitly and record the exact command; a missing Release
    # binary makes the rejection check fail rather than silently pass.
    if args.task == 'T21' and not args.capture_only:
        release_build = build('Release', output / 'release-build', enabled=not args.no_build)
        report['releaseBuild'] = release_build
        if release_build.get('command'):
            report['commands'].append(release_build['command'])
        if not release_build['pass']:
            report['failures'].append(
                f'Release build failed (see {release_build.get("log")})')

    contract = selftest.run(output / 'contract-selftest')
    report['contractSelfTest'] = contract
    if not contract['pass']:
        report['failures'].append('capture-contract negative self-tests did not all fail as expected')

    inventory_record = corpus.inventory(Path(profile['lanes']['external']['root']))
    write_json(output / 'corpus-inventory.json', inventory_record)
    report['corpus'] = inventory_record
    report['externalQualification'] = corpus.external_lane_status(inventory_record)
    if report['externalQualification']['status'] != 'available':
        report['externalQualification']['note'] = (
            'External qualification is incomplete; generated fixtures still run and are '
            'the only portable passing lane.')

    executable = ROOT / f'x64/{args.configuration}/Preview3D.exe'
    report['environment'] = env.environment(executable)

    generated_files = fixtures.generate(output / 'fixtures')
    report['fixtures'] = generated_files
    report['fixtures']['provenance'] = {
        'origin': 'tests/render-quality/fixtures.py',
        'redistribution': 'Generated in-repository; redistributable with the test tree.',
        'externalCorpus': 'Restricted local corpus; not committed and not required by the '
                          'generated lane.',
        'f3dScreenshots': 'Rendered locally with installed F3D 3.5.0; local evidence only.',
    }

    cases = []
    captures = {}
    capture_failures = []
    try:
        cases, captures, capture_failures = capture_cases(
            profile, inventory_record, output, args.configuration)
    except Exception as error:
        capture_failures.append(f'capture lane failed: {error}')
        report['failures'].append(f'capture lane failed: {error}')
    report['failures'].extend(capture_failures)
    report['cases'] = cases
    report['captures'] = captures

    if args.task == 'T21' and not args.capture_only:
        diagnostic_report = diagnostics.run(
            ROOT, output, ROOT / f'x64/Release/Preview3D.exe')
        report['diagnostics'] = diagnostic_report
        if diagnostic_report.get('failures'):
            report['failures'].extend(
                f'diagnostics: {failure}' for failure in diagnostic_report['failures'])
        report['causeReport'] = str(diagnostics.write_cause_report(output, diagnostic_report))

    required_lanes = [name for name, lane in profile['lanes'].items() if lane.get('required')]
    selected_lanes = {}
    for case in cases:
        selected_lanes.setdefault(case['lane'], 0)
        selected_lanes[case['lane']] += 1
    report['laneSelection'] = selected_lanes
    lane_result = compare.lane_contract(required_lanes, selected_lanes)
    report['laneContract'] = lane_result
    if not lane_result['pass']:
        report['failures'].extend(lane_result['failures'])

    report['reference'] = {
        'f3d': render_f3d(profile, inventory_record, output),
        'khronos': khronos_route(),
    }
    if report['reference']['f3d'].get('status') == 'fail':
        report['failures'].append('F3D reference lane failed')

    comparisons = []
    regression_selected = any(case.get('regression') for case in cases)
    if not args.capture_only:
        for case in cases:
            if not case.get('regression'):
                continue
            result = compare.verify_case(case, output / 'images')
            comparisons.append(result)
            if not result['pass']:
                report['failures'].append(
                    f'regression {case["id"]}: {"; ".join(result["reasons"])}')
        if not comparisons and regression_selected:
            report['failures'].append('required regression lane produced no comparisons')
    report['comparisons'] = comparisons

    report['knownDefects'] = profile.get('knownDefects', [])
    report['observedBaselines'] = {
        'status': 'observed' if args.capture_only else 'observed-and-compared',
        'images': str(output / 'images'),
        'manifest': str(output / 'captures.json'),
        'note': 'Observed baselines are evidence of the current rendering, including known '
                'defects. They are not accepted quality targets.',
    }
    review_path = ROOT / 'tests' / 'render-quality' / 'review' / f'{args.task}-review.json'
    review_ok = review_path.exists()
    if review_ok:
        try:
            review_data = json.loads(review_path.read_text(encoding='utf-8'))
            review_ok = bool(review_data.get('imagesInspected')) and bool(
                review_data.get('reviewer', {}).get('identity'))
        except Exception:
            review_ok = False
    report['review'] = {
        'status': 'recorded' if review_ok else 'needs-visual-review',
        'record': str(review_path),
        'reviewer': 'image-capable agent (local Symphony vision disabled; file images read '
                    'through the agent image tool)',
        'providerSettingsChanged': False,
    }
    if not args.capture_only and not review_ok:
        report['failures'].append('image review record missing or empty')
    if args.capture_only:
        report['status'] = 'capture-only'
        passed = not capture_failures and contract['pass'] and build_result['pass']
    else:
        passed = not report['failures']
        report['status'] = 'pass' if passed else 'fail'
    write_json(output / 'report.json', report)
    print(json.dumps({
        'status': report['status'], 'failures': report['failures'],
        'captures': len(captures), 'comparisons': len(comparisons),
        'externalQualification': report['externalQualification']['status'],
        'reference': report['reference']['f3d'].get('status'),
        'report': str(output / 'report.json'),
    }, indent=2))
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()
