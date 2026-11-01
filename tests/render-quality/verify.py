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

``--fast`` is an iteration lane: it skips the negative contract self-tests, the
external corpus, F3D references and the review-record gate, and treats lanes as
optional. It still builds, generates fixtures, captures their views and runs the
task's focused lane, so use it between edits and run the full command once for
acceptance evidence.
"""
from __future__ import annotations

import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import math
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys
import time

from common import (ROOT, RESULT_SCHEMA, decode_png, encoded_png, send_field,
                    sha256_file, write_json)
import anti_aliasing
import brdf
import clay_mode
import compare
import contact_shadows
import corpus
import diagnostics
import directional
import env
import fixtures
import full_corpus
import layered_transparency
import material_semantics
import scene_transmission
import screen_space_ao
import selftest
import studio_lighting
import texture_budget
import textured_lobes
import appearance
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


def build_import_tests(configuration, output: Path, enabled=True):
    """Build the task's unit/import lane binaries before running them.

    The ordinary build target is the viewer only; T23's acceptance also depends
    on the broker/worker accounting and detail-lifecycle tests, so the import
    suite must be current rather than a stale artefact.
    """
    if not enabled:
        return {'command': None, 'skipped': True, 'pass': True}
    project = ROOT / 'tests' / 'import-isolation' / 'Tests.ImportIsolation.vcxproj'
    if not MSBUILD.exists():
        return {'command': str(MSBUILD), 'skipped': False, 'pass': False,
                'reason': 'MSBuild not found'}
    # The project derives asset/worker paths from $(SolutionDir); a direct
    # project build would otherwise bake the project directory into them.
    command = [str(MSBUILD), str(project), f'/p:Configuration={configuration}',
               '/p:Platform=x64', f'/p:SolutionDir={ROOT}\\', '/m', '/v:minimal']
    output.mkdir(parents=True, exist_ok=True)
    log_path = output / 'import-tests-build.log'
    started = time.monotonic()
    with log_path.open('w', encoding='utf-8') as log:
        completed = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    return {
        'command': ' '.join(command), 'exitCode': completed.returncode,
        'seconds': round(time.monotonic() - started, 2), 'log': str(log_path),
        'pass': completed.returncode == 0, 'skipped': False,
    }


def build_unit_tests(configuration, output: Path, enabled=True):
    """Build the CPU reference/contract suite the task's lane runs.

    T24's acceptance includes independent numeric BRDF/environment references
    in Tests.Unit, so a stale test binary is not evidence. The project derives
    include paths from the repository root, so build it through the solution's
    directory layout.
    """
    if not enabled:
        return {'command': None, 'skipped': True, 'pass': True}
    project = ROOT / 'tests' / 'unit' / 'Tests.Unit.vcxproj'
    if not MSBUILD.exists():
        return {'command': str(MSBUILD), 'skipped': False, 'pass': False,
                'reason': 'MSBuild not found'}
    command = [str(MSBUILD), str(project), f'/p:Configuration={configuration}',
               '/p:Platform=x64', f'/p:SolutionDir={ROOT}\\', '/m', '/v:minimal']
    output.mkdir(parents=True, exist_ok=True)
    log_path = output / 'unit-tests-build.log'
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


def seed_for(inventory_record, asset):
    """Resolve a profile asset to its local-corpus record.

    T34 profiles use ``rootName`` (the actual corpus entry) so the checked-in
    profile carries no local path. Earlier profiles use the seed alias.
    """
    root_name = asset.get('rootName')
    if root_name:
        record = inventory_record.get('rootAssets', {}).get(root_name)
        if record:
            return record
    return inventory_record.get('seedAssets', {}).get(asset['id'])


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
        seed = seed_for(inventory_record, asset)
        if not seed or not seed.get('available'):
            continue
        cases.append({
            'id': f'external-{name}-default', 'lane': 'external', 'asset': name,
            'mode': None, 'view': 'default', 'regression': False,
            'image': f'external-{name}-default.png',
            'regions': regions.get(name, {}),
            'knownDefect': asset.get('knownDefect'),
            'family': asset.get('family') or seed.get('family'),
            'rootName': asset.get('rootName') or seed.get('rootName'),
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
                    'family': asset.get('family') or seed.get('family'),
                    'rootName': asset.get('rootName') or seed.get('rootName'),
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
                    'family': asset.get('family') or seed.get('family'),
                    'rootName': asset.get('rootName') or seed.get('rootName'),
                })
        if asset.get('orbit'):
            for step in profile['orbit']:
                cases.append({
                    'id': f'external-{name}-studio-orbit-{step["id"]}',
                    'lane': 'external', 'asset': name, 'mode': 'studio',
                    'view': f'orbit-{step["id"]}', 'regression': False,
                    'image': f'external-{name}-studio-orbit-{step["id"]}.png',
                    'regions': regions.get(name, {}), 'knownDefect': asset.get('knownDefect'),
                    'family': asset.get('family') or seed.get('family'),
                    'rootName': asset.get('rootName') or seed.get('rootName'),
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

        # External corpus: every root asset is accounted for. A failed open or
        # an asset above the local matrix's size cap becomes a typed disposition,
        # never a silent success skip.
        if inventory_record.get('available'):
            for asset in profile['lanes']['external']['assets']:
                name = asset['id']
                seed = seed_for(inventory_record, asset)
                common = {
                    'id': f'external-{name}-disposition', 'lane': 'external',
                    'asset': name, 'mode': None, 'view': 'disposition',
                    'regions': {}, 'knownDefect': asset.get('knownDefect'),
                    'family': asset.get('family') or (seed or {}).get('family'),
                    'rootName': asset.get('rootName') or (seed or {}).get('rootName'),
                    'entry': (seed or {}).get('entry'),
                    'entrySha256': (seed or {}).get('entrySha256'),
                    'entryBytes': (seed or {}).get('entryBytes'),
                    'capturedAt': time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime()),
                }
                if not seed or not seed.get('available'):
                    record = dict(common, disposition='unavailable',
                                  reason='root entry could not be resolved')
                    records[record['id']] = record
                    failures.append(
                        f'external asset {name} unavailable: {record["reason"]}')
                    continue
                cap = asset.get('maxEntryBytes')
                if cap and (seed.get('entryBytes') or 0) > cap:
                    record = dict(common, disposition='resource-budget',
                                  reason=f'entry {seed["entryBytes"]} bytes exceeds '
                                         f'the local render-matrix cap {cap}',
                                  capBytes=cap)
                    records[record['id']] = record
                    continue
                try:
                    generation, load_seconds, _ = viewer.open(Path(seed['entry']))
                except LoadFailed as error:
                    # Keep the observed partial image, but never call it a
                    # complete load. A typed disposition is evidence; it is not
                    # a successful image skip.
                    try:
                        frame = viewer.capture()
                        path = image_dir / f'external-{name}-failed-load.png'
                        path.write_bytes(encoded_png(
                            frame['width'], frame['height'], frame['rgb']))
                        image_fields = {
                            'image': path.name, 'path': str(path),
                            'width': frame['width'], 'height': frame['height'],
                            'client': frame['client'], 'dpi': frame['dpi'],
                            'dpiScale': frame['dpiScale'],
                            'viewportCrop': frame['rect'], 'stats': frame['stats'],
                            'sha256': sha256_file(path),
                        }
                    except Exception:
                        image_fields = {}
                    record = dict(
                        common, id=f'external-{name}-failed-load',
                        view='failed-load', settled=False, complete=False,
                        generationMatched=False,
                        failure={
                            'message': str(error)[:500],
                            'code': getattr(error, 'code', None),
                            'stage': getattr(error, 'stage', None),
                            'state': getattr(error, 'state', None),
                        },
                        disposition='open-failure', **image_fields)
                    records[record['id']] = record
                    if not asset.get('knownDefect') and not asset.get(
                            'allowFailureDisposition'):
                        failures.append(
                            f'external asset {name} failed to load: {error}')
                    continue
                default_case = next(item for item in cases
                                    if item['id'] == f'external-{name}-default')
                try:
                    default_record = save(viewer, default_case)
                except Exception as error:  # noqa: BLE001
                    record = dict(common, id=f'external-{name}-capture-failure',
                                  view='capture-failure',
                                  disposition='capture-failure',
                                  failure={'message': str(error)[:500]})
                    records[record['id']] = record
                    failures.append(
                        f'external asset {name} default capture failed: {error}')
                    continue
                default_record.update({
                    'loadSeconds': round(load_seconds, 3),
                    'family': common['family'],
                    'rootName': common['rootName'],
                    'entry': common['entry'],
                    'entrySha256': common['entrySha256'],
                    'entryBytes': common['entryBytes'],
                    'importState': viewer.import_state(),
                    'passes': viewer.pass_state(),
                })
                compatibility = viewer.import_state()
                default_record['degradation'] = {
                    'compatibilityRecords': compatibility['compatibilityRecords'],
                    'optionalFeatureWarnings': compatibility['optionalFeatureWarnings'],
                    'textureWarnings': compatibility['textureWarnings'],
                    'warningTextLength': compatibility['warningTextLength'],
                }
                for case in [item for item in cases if item['lane'] == 'external'
                             and item['asset'] == name and item['view'] != 'default']:
                    if case['view'] == 'default' or case['id'].endswith('-failed-load'):
                        continue
                    lighting = next(item['lighting'] for item in profile['modes']
                                    if item['id'] == case['mode'])
                    try:
                        record = save(viewer, case, lighting=lighting,
                                      camera=camera_for(profile, case))
                    except Exception as error:  # noqa: BLE001
                        records[f'{case["id"]}-failure'] = dict(
                            common, id=f'{case["id"]}-failure',
                            mode=case['mode'], view=case['view'],
                            disposition='capture-failure',
                            failure={'message': str(error)[:500]})
                        failures.append(
                            f'external asset {name} capture {case["id"]} failed: '
                            f'{error}')
                        continue
                    record['generation'] = generation
                    record['generationMatched'] = viewer.model_state()['generation'] == generation
                    record['family'] = common['family']
                    record['rootName'] = common['rootName']
                    record['importState'] = viewer.import_state()
                    record['passes'] = viewer.pass_state()

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
    parser.add_argument('--fast', action='store_true',
                        help='Iteration subset: skip contract self-tests, external corpus and '
                             'F3D references. Not acceptance evidence')
    parser.add_argument('--stall-seconds', type=float, default=300.0,
                        help='Kill the viewer and fail the lane when it makes no progress '
                             'for this many seconds (0 disables the watchdog)')
    args = parser.parse_args()
    if args.fast:
        # Focused lanes added by later tasks can skip their slow external
        # sampling without changing their signatures.
        os.environ['RENDER_QUALITY_FAST'] = '1'
    os.environ['RENDER_QUALITY_STALL_SECONDS'] = str(args.stall_seconds)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    set_dpi_awareness()

    report = {
        'schema': RESULT_SCHEMA, 'task': args.task, 'configuration': args.configuration,
        'generatedAt': time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime()),
        'commands': [], 'failures': [], 'status': 'running',
        'captureOnly': args.capture_only, 'fast': args.fast,
    }
    if args.fast:
        report['note'] = ('Fast iteration lane: contract self-tests, external corpus, '
                          'F3D references and review gating are skipped. It is not '
                          'acceptance evidence; run the full command before finishing.')
    report['commands'].append(' '.join(sys.argv))

    profile = profile_for(args.task)
    if args.fast:
        # Keep the generated/fixture lane and the task's focused lane; drop the
        # expensive external/reference work and relax required-lane accounting.
        for lane in profile.get('lanes', {}).values():
            lane['required'] = False
        profile.setdefault('lanes', {}).setdefault('external', {})['assets'] = []
        profile['lanes'].setdefault('reference', {}).setdefault(
            'f3d', {}).update({'assets': [], 'configurations': []})
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

    # T23's import/accounting checks are compiled artefacts, not source-string
    # checks: build the suite before the lane runs.
    if args.task == 'T23' and not args.capture_only:
        import_build = build_import_tests(args.configuration, output, enabled=not args.no_build)
        report['importTestBuild'] = import_build
        if import_build.get('command'):
            report['commands'].append(import_build['command'])
        if not import_build['pass']:
            report['failures'].append(
                f'import test build failed (see {import_build.get("log")})')

    if args.fast:
        contract = {'pass': True, 'skipped': True, 'reason': 'fast iteration lane'}
    else:
        contract = selftest.run(output / 'contract-selftest')
    report['contractSelfTest'] = contract
    if not contract['pass']:
        report['failures'].append('capture-contract negative self-tests did not all fail as expected')

    if args.fast:
        inventory_record = {'available': False, 'assets': [],
                            'note': 'external corpus skipped by the fast iteration lane'}
        report['corpus'] = inventory_record
        report['externalQualification'] = {
            'status': 'skipped', 'reason': 'fast iteration lane'}
    else:
        inventory_record = corpus.inventory(Path(profile['lanes']['external']['root']))
        write_json(output / 'corpus-inventory.json', inventory_record)
        report['corpus'] = inventory_record
        report['externalQualification'] = corpus.external_lane_status(inventory_record)
    if report['externalQualification']['status'] != 'available' and not args.fast:
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
    if args.task == 'T21a' and not args.capture_only:
        fixture_id = profile['lanes']['generated']['fixtures'][0]['id']
        fixture_path = Path(generated_files[fixture_id]['path'])
        appearance_report = appearance.run(args.configuration, output, fixture_path,
                                           inventory_record)
        report['appearance'] = appearance_report
        if appearance_report.get('failures'):
            report['failures'].extend(
                f'appearance: {failure}' for failure in appearance_report['failures'])
    if args.task == 'T22' and not args.capture_only:
        # Serial unit/import/hostile/cache lanes plus the GPU material-semantic
        # readback and channel-image lane. It is a separate viewer session from
        # the capture lane so a failed semantic check cannot be hidden by a
        # successful screenshot.
        semantic_report = material_semantics.run(
            ROOT, output, args.configuration, fast=args.fast)
        report['materialSemantics'] = semantic_report
        if semantic_report.get('failures'):
            report['failures'].extend(
                f'material-semantics: {failure}'
                for failure in semantic_report['failures'])
        report['causeReport'] = semantic_report.get('reportPath')
    if args.task in ('T24', 'T25', 'T26', 'T27', 'T28', 'T29', 'T30', 'T31', 'T32', 'T33', 'T34') and not args.capture_only:
        unit_build = build_unit_tests(args.configuration, output, enabled=not args.no_build)
        report['unitTestBuild'] = unit_build
        if unit_build.get('command'):
            report['commands'].append(unit_build['command'])
        if not unit_build['pass']:
            report['failures'].append(
                f'unit test build failed (see {unit_build.get("log")})')
        # The shared MaterialDielectricF0/F90 helper and the T25 lobe-map import
        # contract are exercised by the importer suite, so build it for the
        # full lane. T26 also runs the shared validator/hostile import lanes.
        if not args.fast:
            import_build = build_import_tests(args.configuration, output,
                                              enabled=not args.no_build)
            report['importTestBuild'] = import_build
            if import_build.get('command'):
                report['commands'].append(import_build['command'])
            if not import_build['pass']:
                report['failures'].append(
                    f'import test build failed (see {import_build.get("log")})')

    if args.task == 'T23' and not args.capture_only:
        # Serial accounting/import lanes plus a Debug viewer readiness session:
        # reused-image count, the microscope hold past its former late-limit
        # window, and Camera/Car non-regression. The holder remains honest: a
        # late terminal error is a failure, not a successful capture.
        budget_report = texture_budget.run(ROOT, output, args.configuration, fast=args.fast)
        report['textureBudget'] = budget_report
        if budget_report.get('failures'):
            report['failures'].extend(
                f'texture-budget: {failure}' for failure in budget_report['failures'])
        report['causeReport'] = budget_report.get('reportPath')
    if args.task == 'T24' and not args.capture_only:
        # CPU reference lanes plus a Debug GPU numeric/readback session and
        # Car/Camera nonregression captures in all three modes.
        brdf_report = brdf.run(ROOT, output, args.configuration, fast=args.fast)
        report['brdf'] = brdf_report
        if brdf_report.get('failures'):
            report['failures'].extend(
                f'brdf: {failure}' for failure in brdf_report['failures'])
        report['causeReport'] = brdf_report.get('reportPath')
    if args.task == 'T25' and not args.capture_only:
        # Textured coat/specular/sheen/transmission lanes: serial
        # unit/import/wire/hostile checks plus the GPU left/right map-variation
        # session on the generated camera-facing probe fixture.
        lobe_report = textured_lobes.run(ROOT, output, args.configuration, fast=args.fast)
        report['texturedLobes'] = lobe_report
        if lobe_report.get('failures'):
            report['failures'].extend(
                f'textured-lobes: {failure}' for failure in lobe_report['failures'])
        report['causeReport'] = lobe_report.get('reportPath')
    if args.task == 'T26' and not args.capture_only:
        # Serial unit/import/wire/hostile checks plus the generated
        # layered-glass order-permutation, depth/picking and lifecycle lane.
        transparency_report = layered_transparency.run(
            ROOT, output, args.configuration, fast=args.fast)
        report['layeredTransparency'] = transparency_report
        if transparency_report.get('failures'):
            report['failures'].extend(
                f'layered-transparency: {failure}'
                for failure in transparency_report['failures'])
        report['causeReport'] = transparency_report.get('reportPath')
    if args.task == 'T27' and not args.capture_only:
        # Serial unit/import checks plus the generated coverage/tint/roughness
        # numeric lane, overlap/edge cases, lifecycle and the Car/Camera/Suit
        # glass rubric.
        transmission_report = scene_transmission.run(
            ROOT, output, args.configuration, fast=args.fast)
        report['sceneTransmission'] = transmission_report
        if transmission_report.get('failures'):
            report['failures'].extend(
                f'scene-transmission: {failure}'
                for failure in transmission_report['failures'])
        report['causeReport'] = transmission_report.get('reportPath')

    if args.task == 'T28' and not args.capture_only:
        # Studio environment radiometry, contribution-based presentation
        # selection, exposure/background independence and the first-open
        # framing review. The generated fixture carries every critical material
        # family; Car/Camera/Suit provide external nonregression regions.
        lighting_report = studio_lighting.run(
            ROOT, output, args.configuration, fast=args.fast)
        report['studioLighting'] = lighting_report
        if lighting_report.get('failures'):
            report['failures'].extend(
                f'studio-lighting: {failure}'
                for failure in lighting_report['failures'])
        report['causeReport'] = lighting_report.get('reportPath')
    if args.task == 'T30' and not args.capture_only:        # One documented world-anchored sun for the base/coat lobes, gizmo and
        # contact-shadow input; stationary-camera azimuth/elevation sweeps;
        # stationary-sun camera orbit; intensity-zero and up-axis state; and
        # Car/Camera/microscope/STL stills at several sun elevations.
        directional_report = directional.run(
            ROOT, output, args.configuration, fast=args.fast)
        report['directionalLighting'] = directional_report
        if directional_report.get('failures'):
            report['failures'].extend(
                f'directional: {failure}'
                for failure in directional_report['failures'])
        report['causeReport'] = directional_report.get('reportPath')
    if args.task == 'T29' and not args.capture_only:
        # Effective Clay draw/depth state, MASK-cutout policy, neutral matte
        # shading, transparent-interior occlusion, mirrored/small/mixed
        # primitives, picking and the mode round-trip. Car/Camera/microscope/
        # STL and STEP crops come from the profile's capture lane.
        clay_report = clay_mode.run(
            ROOT, output, args.configuration, fast=args.fast)
        report['clayMode'] = clay_report
        if clay_report.get('failures'):
            report['failures'].extend(
                f'clay-mode: {failure}' for failure in clay_report['failures'])
        report['causeReport'] = clay_report.get('reportPath')
    if args.task == 'T32' and not args.capture_only:
        # CPU calibration/budget lanes plus the GPU depth-reconstruction,
        # AO-only mask, on/off pair, scale/projection/motion and lifecycle
        # lane. External Car/Camera/STL pairs are captured in a fresh session.
        ao_report = screen_space_ao.run(
            ROOT, output, args.configuration, fast=args.fast)
        report['screenSpaceAo'] = ao_report
        if ao_report.get('failures'):
            report['failures'].extend(
                f'screen-space-ao: {failure}' for failure in ao_report['failures'])
        report['causeReport'] = ao_report.get('reportPath')
    if args.task == 'T33' and not args.capture_only:
        # CPU/import policy lanes plus same-camera AA pairs, the SSAA
        # reference, picking identity, UI text, orbit, physical sizes and
        # resize/device-loss lifecycle on the generated edge fixture.
        aa_report = anti_aliasing.run(
            ROOT, output, args.configuration, fast=args.fast)
        report['antiAliasing'] = aa_report
        if aa_report.get('failures'):
            report['failures'].extend(
                f'anti-aliasing: {failure}' for failure in aa_report['failures'])
        report['causeReport'] = aa_report.get('reportPath')
    if args.task == 'T31' and not args.capture_only:
        # CPU fitted-frustum/texel-bias/caster-policy lanes plus same-camera
        # shadow on/off GPU pairs in Studio, Clay and Directional; holes,
        # disconnected casters, bounded transmission attenuation, independent
        # highlight/grid, fallback, resize, generation and device loss.
        shadow_report = contact_shadows.run(
            ROOT, output, args.configuration, fast=args.fast)
        report['contactShadows'] = shadow_report
        if shadow_report.get('failures'):
            report['failures'].extend(
                f'contact-shadows: {failure}'
                for failure in shadow_report['failures'])
        report['causeReport'] = shadow_report.get('reportPath')
    if args.task == 'T34' and not args.capture_only:
        # T34 never trusts the generic capture loop alone: it audits every
        # local-corpus root entry, captures portable FBX/USD/3MF/OBJ/STEP
        # fixtures, exercises the render lifecycle, checks Debug-only controls
        # and the Release dependency closure, and summarises measured cost.
        if not args.fast:
            release_build = build('Release', output / 'release-build',
                                  enabled=not args.no_build)
            report['releaseBuild'] = release_build
            if release_build.get('command'):
                report['commands'].append(release_build['command'])
            if not release_build['pass']:
                report['failures'].append(
                    f'Release build failed (see {release_build.get("log")})')
        corpus_report = full_corpus.run(
            ROOT, output, args.configuration, fast=args.fast,
            profile=profile, inventory=inventory_record)
        report['fullCorpus'] = corpus_report
        if corpus_report.get('failures'):
            report['failures'].extend(
                f'full-corpus: {failure}' for failure in corpus_report['failures'])
        report['causeReport'] = corpus_report.get('reportPath')
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
        'f3d': ({'status': 'skipped', 'reason': 'fast iteration lane'}
                if args.fast else render_f3d(profile, inventory_record, output)),
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
    if not args.capture_only and not args.fast and not review_ok:
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
