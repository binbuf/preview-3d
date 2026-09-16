"""TSK-206 real-app document handoff, coarse/full suppression and eviction."""
import argparse
import ctypes as C
import importlib.util
import json
from pathlib import Path
import subprocess
import time
import traceback
from run import ROOT, CopyData, find_window, processes, send, user, W, private_bytes

spec = importlib.util.spec_from_file_location('coarse_fixtures', ROOT/'tests/fixtures/coarse.py')
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)


def run(exe, directory):
    app = subprocess.Popen([str(exe), '--coarse-proxy-smoke'])
    report = {'failure': None, 'events': [], 'maxUiMs': 0, 'peakQueueBytes': 0,
              'viewerPeakPrivateBytes': 0, 'workerPeakPrivateBytes': 0}
    hwnd = 0
    workers = set()
    def query(field, value=0):
        at = time.perf_counter()
        result = send(hwnd, 0x8000+104, field, value)
        report['maxUiMs'] = max(report['maxUiMs'], (time.perf_counter()-at)*1000)
        return result
    def sample():
        report['peakQueueBytes'] = max(report['peakQueueBytes'], query(6))
        report['viewerPeakPrivateBytes'] = max(report['viewerPeakPrivateBytes'], private_bytes(app.pid))
        total = 0
        for pid, (parent, name) in processes().items():
            if parent == app.pid and name == 'Preview3DImportWorker.exe':
                workers.add(pid)
                total += private_bytes(pid)
        report['workerPeakPrivateBytes'] = max(report['workerPeakPrivateBytes'], total)
    def wait(predicate, label, timeout=90):
        until = time.monotonic()+timeout
        while time.monotonic() < until:
            if app.poll() is not None:
                raise RuntimeError(f'exit {app.returncode}: {label}')
            if predicate():
                return
            if hwnd:
                sample()
            time.sleep(0.01)
        raise TimeoutError(label)
    def open_file(path):
        text = C.create_unicode_buffer(str(path.resolve()))
        data = CopyData(104, C.sizeof(text), C.cast(text, C.c_void_p))
        result = send(hwnd, 0x4a, 0, C.addressof(data))
        assert result
        return result
    try:
        wait(lambda: find_window(app.pid), 'window')
        hwnd = find_window(app.pid)
        wait(lambda: query(2), 'background')
        baseline = open_file(ROOT/'interactive-viewer/test-assets/tri_tight.glb')
        wait(lambda: query(4) == baseline and query(0) == 3, 'baseline')
        cancelled = open_file(directory/'glb.glb')
        wait(lambda: query(53) > 0, 'validated scan before handoff')
        assert query(51) != cancelled and query(4) == baseline
        send(hwnd, 0x100, 0x1b)
        wait(lambda: query(0) == 3, 'cancel restoring prior Ready document')
        assert query(4) == baseline and query(47) == baseline and query(14) == 1
        report['events'].append({'action': 'cancel-before-handoff', 'preservedGeneration': baseline})
        damaged = directory/'malformed.ply'
        data = bytearray((directory/'mesh-le.ply').read_bytes())
        # A truncated final list is malformed; out-of-range indices instead
        # exercise the supported invalid-face warning/drop policy.
        data[len(data)-13] = 4
        damaged.write_bytes(data)
        failed = open_file(damaged)
        wait(lambda: query(0) == 4, 'late scan failure')
        assert query(4) == baseline and query(47) == baseline and query(14) == 1
        report['events'].append({'action': 'failure-before-handoff', 'errorCode': query(41), 'preservedGeneration': baseline})
        for name in ['glb.glb', 'stl-reordered.stl', 'mesh-le-reordered.ply', 'mesh-be.ply',
                     'points-le-reordered.ply', 'points-be.ply', 'Draw-heavy.glb']:
            at = time.perf_counter()
            previous = query(4)
            source = ROOT/'interactive-viewer/test-assets/corpus'/name if name=='Draw-heavy.glb' else directory/name
            expected = 2048 if name=='Draw-heavy.glb' else 100000
            generation = open_file(source)
            assert query(4)==previous and query(51)!=generation, 'replacement displaced the prior model before coarse completion'
            wait(lambda: query(53) > 0, 'scan')
            if query(51)!=generation:
                assert query(4)==previous, 'scan-only publication displaced the prior model'
            wait(lambda: query(4) == generation and query(47) == generation, 'complete proxy handoff')
            assert query(51) == generation and query(16)
            assert query(0) == 2, 'proxy handoff must still be Loading/refining'
            assert query(48) > 0 and query(49) < query(48)
            event = {'fixture': name, 'proxyHandoffMs': (time.perf_counter()-at)*1000,
                     'coarseChunks': query(48), 'fineAtHandoff': query(49), 'verifiedBounds': bool(query(16)),
                     'coarseAllocationBytes': query(54)}
            assert 0 < query(54) <= 64*1024*1024
            wait(lambda: query(0) in (3, 4), 'fine completion')
            assert query(0) == 3, f'error {query(41)}'
            assert query(50) == query(48) and query(49) == query(48)
            assert query(8) == query(49), 'duplicate coarse surfaces were drawn'
            assert query(28) == 0 and query(48) > 0
            if name.startswith('points'):
                assert query(15) == expected
            else:
                assert query(14) == expected
            query(52)
            wait(lambda: query(49) == 0 and query(50) == 0, 'fence-safe fine eviction')
            assert query(8) == query(48) and query(14)+query(15) == expected
            event.update({'visibleAfterEviction': query(8), 'fineRetainedAfterEviction': query(49)})
            report['events'].append(event)
            sample()
        assert query(35) == 0, 'D3D12 debug errors'
        assert report['peakQueueBytes'] <= 128*1024*1024
        send(hwnd, 0x10)
        app.wait(timeout=15)
        assert app.returncode == 0
        wait_until = time.monotonic()+15
        while any(name == 'Preview3DImportWorker.exe' and (parent == app.pid or pid in workers)
                  for pid, (parent, name) in processes().items()):
            if time.monotonic() > wait_until:
                raise TimeoutError('worker cleanup')
            time.sleep(0.02)
        report['exitCode'] = app.returncode
        report['survivingWorkers'] = 0
    except Exception as error:
        report['failure'] = f'{type(error).__name__}: {error}'
        report['failureTraceback'] = traceback.format_exc()
    finally:
        if app.poll() is None:
            app.kill()
            app.wait(timeout=15)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configuration', choices=['Debug', 'Release'], default='Debug')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    user.SetProcessDpiAwarenessContext(W.HANDLE(-4))
    directory = args.output.parent/'coarse-app-fixtures'
    manifest = fixtures.generate(directory)
    exe = ROOT/f'x64/{args.configuration}/Preview3D.exe'
    report = {'configuration': args.configuration, 'exeSha256': fixtures.recipes.digest(exe),
              'workerExeSha256': fixtures.recipes.digest(exe.with_name('Preview3DImportWorker.exe')),
              'fixtures': manifest, 'run': run(exe, directory)}
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report['run'], indent=2))
    if report['run']['failure']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
