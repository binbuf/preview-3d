"""Repeatable TSK-302 viewer benchmark and optional PresentMon/ETW correlation."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import platform
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def machine_metadata():
    command = (
        "$os=Get-CimInstance Win32_OperatingSystem;"
        "$cpu=Get-CimInstance Win32_Processor;"
        "$gpu=Get-CimInstance Win32_VideoController;"
        "$disk=Get-PhysicalDisk;"
        "@{os=$os.Caption;build=$os.BuildNumber;cpu=@($cpu.Name);"
        "ramBytes=$os.TotalVisibleMemorySize*1024;"
        "gpu=@($gpu|Select-Object Name,DriverVersion,CurrentRefreshRate,CurrentHorizontalResolution,CurrentVerticalResolution);"
        "disks=@($disk|Select-Object FriendlyName,MediaType,BusType);"
        "power=(powercfg /getactivescheme)}|ConvertTo-Json -Depth 5"
    )
    return json.loads(subprocess.check_output(
        ['powershell', '-NoProfile', '-Command', command], text=True, encoding='utf-8-sig'))


def parse_presentmon(path):
    """Retain every ETW-derived interval and label exclusions mechanically."""
    if not path.exists():
        return {'available': False, 'rawIntervalsMs': [], 'exclusions': {}}
    intervals, exclusions = [], {}
    with path.open(newline='', encoding='utf-8-sig') as stream:
        for row in csv.DictReader(stream):
            value = row.get('msBetweenPresents') or row.get('MsBetweenPresents')
            if not value:
                exclusions['missing_interval'] = exclusions.get('missing_interval', 0) + 1
                continue
            try:
                interval = float(value)
            except ValueError:
                exclusions['invalid_interval'] = exclusions.get('invalid_interval', 0) + 1
                continue
            reason = None
            if row.get('Dropped', '').lower() in ('1', 'true'):
                reason = 'dropped'
            elif row.get('PresentMode', '').lower() in ('', 'unknown'):
                reason = 'unclassified_present_mode'
            intervals.append({'ms': interval, 'excludedReason': reason})
            if reason:
                exclusions[reason] = exclusions.get(reason, 0) + 1
    included = [entry['ms'] for entry in intervals if entry['excludedReason'] is None]
    return {
        'available': True,
        'rawIntervals': intervals,
        'exclusions': exclusions,
        'includedMeanMs': statistics.fmean(included) if included else None,
        'includedMedianMs': statistics.median(included) if included else None,
        'includedP95Ms': sorted(included)[math.ceil(len(included) * .95) - 1] if included else None,
        'includedMaxMs': max(included) if included else None,
    }


def run_once(args, exe, fixture, index, run_dir):
    app_result = run_dir / f'app-{index}.json'
    present_csv = run_dir / f'presentmon-{index}.csv'
    present = None
    if args.presentmon:
        # PresentMon 2.x consumes the DXGI/D3D ETW providers locally. It is
        # opt-in: routine tests neither require elevation nor start ETW.
        present = subprocess.Popen([
            str(args.presentmon), '--process_name', 'Preview3D.exe',
            '--output_file', str(present_csv), '--terminate_on_proc_exit'])
        time.sleep(.5)
    command = [str(exe), f'--benchmark={fixture}', f'--benchmark-result={app_result}',
               f'--benchmark-duration-ms={args.duration_ms}', f'--benchmark-frames={args.frames}',
               '--benchmark-repeat=1', f'--benchmark-reference={args.reference}']
    if args.copy_delay:
        command.append('--coarse-proxy-smoke')
    if args.worker_budget_failure:
        command.append('--benchmark-worker-budget-failure')
    if args.occluded:
        command.append('--benchmark-occluded')
    completed = subprocess.run(command, timeout=args.timeout, check=False)
    if present:
        try:
            present.wait(timeout=5)
        except subprocess.TimeoutExpired:
            present.terminate()
            present.wait(timeout=5)
    if not app_result.exists():
        raise RuntimeError(f'viewer produced no result (exit {completed.returncode})')
    result = json.loads(app_result.read_text(encoding='utf-8'))
    result['processExitCode'] = completed.returncode
    result['etw'] = parse_presentmon(present_csv) if args.presentmon else {
        'available': False, 'reason': 'PresentMon not requested'}
    # A failing viewer status must produce a failing process result and vice
    # versa; this keeps injected failures from being lost by a wrapper.
    if (result['status'] == 'pass') != (completed.returncode == 0):
        result['status'] = 'fail'
        result.setdefault('harnessFailures', []).append('status/exit-code mismatch')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    parser.add_argument('--configuration', choices=['Debug', 'Release'], default='Release')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runs', type=int, default=5)
    parser.add_argument('--duration-ms', type=int, default=10_000)
    parser.add_argument('--frames', type=int, default=1200)
    parser.add_argument('--reference', choices=['performance', 'compatibility'], default='compatibility')
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--presentmon', type=Path,
                        help='Optional PresentMon 2.x executable for local ETW present correlation')
    parser.add_argument('--copy-delay', action='store_true', help='Inject the existing 750 ms copy delay')
    parser.add_argument('--worker-budget-failure', action='store_true',
                        help='Inject a 1 MiB general-worker Job commit limit')
    parser.add_argument('--occluded', action='store_true', help='Minimize the viewer to classify occluded Presents')
    args = parser.parse_args()
    if args.runs < 1 or args.runs > 100:
        parser.error('--runs must be 1..100')
    fixture = args.fixture.resolve()
    if not fixture.is_file():
        parser.error(f'fixture not found: {fixture}')
    exe = ROOT / 'x64' / args.configuration / 'Preview3D.exe'
    worker = exe.with_name('Preview3DImportWorker.exe')
    if not exe.is_file() or not worker.is_file():
        parser.error('build Preview3D through Preview3D.slnx first')
    if args.presentmon and not args.presentmon.is_file():
        parser.error(f'PresentMon not found: {args.presentmon}')
    run_dir = args.output.parent / (args.output.stem + '-runs')
    run_dir.mkdir(parents=True, exist_ok=True)
    runs = [run_once(args, exe, fixture, i + 1, run_dir) for i in range(args.runs)]

    def values(path):
        result = []
        for run in runs:
            value = run
            for key in path:
                value = value.get(key) if isinstance(value, dict) else None
            if isinstance(value, (int, float)) and value >= 0:
                result.append(value)
        return result

    def summary(items):
        return None if not items else {'mean': statistics.fmean(items), 'median': statistics.median(items),
            'p95': sorted(items)[math.ceil(len(items) * .95) - 1], 'maximum': max(items)}

    report = {
        'schema': 1,
        'task': 'TSK-302',
        'configuration': args.configuration,
        'reference': args.reference,
        'fixture': {'path': str(fixture), 'sha256': sha256(fixture), 'bytes': fixture.stat().st_size},
        'build': {'gitCommit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  'workingDiffSha256': hashlib.sha256(subprocess.check_output(
                      ['git', '-c', 'core.warnCRLF=false', 'diff', 'HEAD'], cwd=ROOT)).hexdigest(),
                  'viewerSha256': sha256(exe), 'workerSha256': sha256(worker),
                  'harnessSha256': sha256(Path(__file__))},
        'machine': machine_metadata(),
        'runMetadata': {'runCount': args.runs, 'durationMs': args.duration_ms, 'frameLimit': args.frames,
                        'cacheState': 'cold process; OS/driver caches uncontrolled',
                        'python': platform.python_version(), 'clock': str(time.get_clock_info('perf_counter'))},
        'failureCount': sum(run['status'] != 'pass' for run in runs),
        'summary': {
            'firstBackgroundMs': summary(values(['milestonesMs', 'firstBackground'])),
            'loadingUiMs': summary(values(['milestonesMs', 'loadingUi'])),
            'firstGeometryMs': summary(values(['milestonesMs', 'firstGeometry'])),
            'completeCoarseMs': summary(values(['milestonesMs', 'completeCoarse'])),
            'frameP95Ms': summary(values(['frames', 'p95Ms'])),
            'frameMaxMs': summary(values(['frames', 'maxMs'])),
            'inputToPresentMs': summary(values(['responsiveness', 'inputToPresentMs'])),
            'viewerPeakPrivateBytes': summary(values(['memory', 'viewerPeakPrivateBytes'])),
            'workerPeakPrivateBytes': summary(values(['memory', 'workerPeakPrivateBytes'])),
        },
        'runs': runs,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'failureCount': report['failureCount'], 'summary': report['summary']}, indent=2))
    if report['failureCount']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
