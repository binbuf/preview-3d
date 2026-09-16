"""TSK-207 live budget drop/restoration, proxy survival and pinned detail replay."""
import argparse
import ctypes as C
import json
import math
from pathlib import Path
import subprocess
import time
import traceback
from run import ROOT, user, W, CopyData, send, find_window, processes, private_bytes
from bounded import recipes
from textures import fixtures as texture_fixtures


def run(exe, paths, uma):
    app = subprocess.Popen([str(exe), '--uma-budget-smoke' if uma else '--app-smoke'])
    hwnd = 0
    workers = set()
    report = {'failure': None, 'events': [], 'requestedUmaDevice': uma,
              'peakQueueBytes': 0, 'peakOutstandingRequests': 0, 'peakViewerPrivateBytes': 0}

    def query(field, value=0):
        return send(hwnd, 0x8000+104, field, value)

    def sample():
        report['peakQueueBytes'] = max(report['peakQueueBytes'], query(6))
        report['peakOutstandingRequests'] = max(report['peakOutstandingRequests'], query(61))
        report['peakViewerPrivateBytes'] = max(report['peakViewerPrivateBytes'], private_bytes(app.pid))
        assert query(61) <= 32
        for pid, (parent, name) in processes().items():
            if parent == app.pid and name == 'Preview3DImportWorker.exe':
                workers.add(pid)

    def wait(predicate, label, timeout=180, allow_closed=False):
        until = time.monotonic()+timeout
        while time.monotonic() < until:
            if app.poll() is not None and not allow_closed:
                raise RuntimeError(f'app exit {app.returncode}: {label}')
            if predicate():
                return
            if hwnd and app.poll() is None:
                sample()
            time.sleep(0.02)
        raise TimeoutError(label)

    def open_file(path):
        text = C.create_unicode_buffer(str(path.resolve()))
        data = CopyData(104, C.sizeof(text), C.cast(text, C.c_void_p))
        return send(hwnd, 0x4a, 0, C.addressof(data))

    try:
        wait(lambda: find_window(app.pid), 'window')
        hwnd = find_window(app.pid)
        wait(lambda: query(2), 'background')
        baseline_bytes = query(55)
        report['actualUma'] = bool(query(64))
        assert report['actualUma'] == uma, 'the requested physical memory architecture was not selected'
        for path in paths:
            query(62, 96)
            generation = open_file(path)
            wait(lambda: query(51) == generation and query(4) == generation, 'complete proxy')
            wait(lambda: query(0) in (3, 4), 'initial scan/fine terminal')
            assert query(0) == 3, f'import failure {query(41)}'
            assert query(16) and query(48) and query(49), 'usable verified coarse/fine set'
            original_fine = query(49)
            original_counts = (query(14), query(15))
            original_coarse = query(48)
            evictions_before = query(58)
            # Retain targets and the immutable coarse reserve, leave <one fine chunk.
            low_mib = max(24, math.ceil((baseline_bytes+query(54))/1024/1024)+2)
            query(62, low_mib)
            wait(lambda: query(58)>evictions_before and query(49)<original_fine, 'automatic fine eviction')
            wait(lambda: query(55)<=query(56) and query(57)==0, 'fence retirement restores cap')
            assert query(48) == original_coarse and query(16)
            assert (query(14), query(15)) == original_counts
            assert query(8) == query(48)-query(50)+query(49), 'coarse/full coverage gap or duplicate'
            fine_low = query(49)
            requests_before = query(59)
            # Exercise a real camera change while pressure remains active.
            send(hwnd, 0x100, 0xBB)
            query(62, 192)
            wait(lambda: query(59)>requests_before and query(49)>fine_low, 'pinned source detail recovery')
            wait(lambda: query(55)<=query(56), 'restored budget cap')
            assert query(0) == 3 and query(48) == original_coarse
            assert (query(14), query(15)) == original_counts
            assert query(35) == 0, 'D3D12 validation errors'
            report['events'].append({'fixture': path.name, 'generation': generation,
                'sourceCounts': original_counts, 'coarseChunks': original_coarse,
                'fineBeforeDrop': original_fine, 'fineUnderPressure': fine_low,
                'fineRecovered': query(49), 'lowTargetMiB': low_mib,
                'accountedGpuBytes': query(55), 'targetGpuBytes': query(56),
                'evictions': query(58)-evictions_before, 'detailRequests': query(59)-requests_before})
        assert report['peakQueueBytes'] <= 128*1024*1024
        # Texture retirement must restore the retained mip tail as well as geometry.
        texture_directory = ROOT/'TestResults/tsk207-texture-fixtures'
        report['textureFixtureSha256'] = texture_fixtures(texture_directory)
        query(62, 192)
        generation = open_file(texture_directory/'raster-capped.glb')
        wait(lambda: query(0) == 3 and query(4) == generation and query(37) == 2048,
             'full texture before pressure')
        texture_counts = (query(14), query(15))
        texture_low_mib = math.ceil((baseline_bytes+query(54))/1024/1024)+1
        query(62, texture_low_mib)
        wait(lambda: query(37) == 64, 'texture fallback after budget drop')
        wait(lambda: query(55)<=query(56) and query(57)==0, 'texture fence retirement')
        assert query(0) == 3 and query(16) and query(48)
        assert (query(14), query(15)) == texture_counts
        assert query(35) == 0, 'D3D12 texture retirement errors'
        report['texturePressure'] = {'generation': generation, 'fullWidth': 2048,
            'fallbackWidth': query(37), 'lowTargetMiB': texture_low_mib,
            'accountedGpuBytes': query(55), 'targetGpuBytes': query(56)}
        # The reserved set failing admission produces a recoverable resource card.
        query(62, 1)
        open_file(ROOT/'interactive-viewer/test-assets/tri_tight.glb')
        wait(lambda: query(0) == 4, 'controlled reserved-set error')
        assert query(41) != 0
        query(62, 192)
        open_file(ROOT/'interactive-viewer/test-assets/tri_tight.glb')
        wait(lambda: query(0) == 3 and query(14) == 1, 'valid reopen after resource error')
        send(hwnd, 0x10)
        app.wait(timeout=15)
        assert app.returncode == 0
        wait(lambda: not any(name == 'Preview3DImportWorker.exe' and (parent == app.pid or pid in workers)
             for pid, (parent, name) in processes().items()), 'worker cleanup', 15, allow_closed=True)
        report['exitCode'] = 0
        report['survivingWorkers'] = 0
    except Exception as error:
        report['failure'] = f'{type(error).__name__}: {error}'
        report['traceback'] = traceback.format_exc()
    finally:
        if app.poll() is None:
            app.kill()
            app.wait(timeout=15)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configuration', choices=['Debug', 'Release'], default='Debug')
    parser.add_argument('--fixtures', type=Path, default=ROOT/'TestResults/bounded-fixtures')
    parser.add_argument('--large-fixture', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--adapter', choices=['both', 'discrete', 'uma'], default='both')
    parser.add_argument('--fixture', action='append', choices=['Pressure.glb','split.glb','split.stl',
        'mesh-le.ply','mesh-be.ply','points-le.ply','points-be.ply'], help='repeat to select a subset; default: all')
    args = parser.parse_args()
    user.SetProcessDpiAwarenessContext(W.HANDLE(-4))
    args.fixtures.mkdir(parents=True, exist_ok=True)
    generators = {
        'Pressure.glb': lambda path: recipes.glb(path, 100000),
        'split.glb': lambda path: recipes.glb(path, 2000000),
        'split.stl': lambda path: recipes.stl(path, 2000000),
        'mesh-le.ply': lambda path: recipes.ply(path, 2000000, False, '<'),
        'mesh-be.ply': lambda path: recipes.ply(path, 2000000, False, '>'),
        'points-le.ply': lambda path: recipes.ply(path, 8000000, True, '<'),
        'points-be.ply': lambda path: recipes.ply(path, 8000000, True, '>'),
    }
    names = list(dict.fromkeys(args.fixture or generators))
    for name in names:
        path=args.fixtures/name
        if not path.exists():
            generators[name](path)
    paths = [args.fixtures/name for name in names]
    if args.large_fixture:
        paths.append(args.large_fixture)
    exe=ROOT/f'x64/{args.configuration}/Preview3D.exe'
    report = {'configuration': args.configuration, 'exeSha256': recipes.digest(exe),
        'workerExeSha256': recipes.digest(exe.with_name('Preview3DImportWorker.exe')),
        'fixtureSha256': {path.name: recipes.digest(path) for path in paths},
        'runs': [run(exe, paths, uma) for uma in ([False, True] if args.adapter=='both' else [args.adapter=='uma'])]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))
    if any(run['failure'] for run in report['runs']):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
