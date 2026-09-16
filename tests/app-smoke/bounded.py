"""TSK-205 real-app split imports, bounded queues, cancel and valid reopen."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import importlib.util
import json
from pathlib import Path
import subprocess
import time
from run import ROOT, user, CopyData, send, find_window, private_bytes, processes

spec = importlib.util.spec_from_file_location('recipes', ROOT/'tests/fixtures/generate.py')
recipes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recipes)


def run(exe, directory, large):
    app = subprocess.Popen([str(exe), '--app-smoke'])
    report = {'failure': None, 'events': [], 'viewerPeakPrivateBytes': 0,
              'workerPeakPrivateBytes': 0, 'peakQueueBytes': 0, 'maxUiMs': 0}
    hwnd = 0
    workers = set()
    def query(field):
        at = time.perf_counter()
        value = send(hwnd, 0x8000+104, field)
        report['maxUiMs'] = max(report['maxUiMs'], (time.perf_counter()-at)*1000)
        return value
    def sample():
        report['viewerPeakPrivateBytes'] = max(report['viewerPeakPrivateBytes'], private_bytes(app.pid))
        total = 0
        for pid, (parent, name) in processes().items():
            if parent == app.pid and name == 'Preview3DImportWorker.exe':
                workers.add(pid)
                total += private_bytes(pid)
        report['workerPeakPrivateBytes'] = max(report['workerPeakPrivateBytes'], total)
        report['peakQueueBytes'] = max(report['peakQueueBytes'], query(6))
    def wait(predicate, label, timeout=120):
        until = time.monotonic()+timeout
        while time.monotonic() < until:
            if app.poll() is not None:
                raise RuntimeError(f'app exited during {label}')
            if predicate():
                return
            if hwnd:
                sample()
            time.sleep(0.02)
        raise TimeoutError(label)
    def open_file(path):
        text = C.create_unicode_buffer(str(path.resolve()))
        data = CopyData(104, C.sizeof(text), C.cast(text, C.c_void_p))
        generation = send(hwnd, 0x4a, 0, C.addressof(data))
        assert generation
        return generation
    try:
        wait(lambda: find_window(app.pid), 'window')
        hwnd = find_window(app.pid)
        wait(lambda: query(2), 'background')
        report['baselineViewerPrivateBytes'] = private_bytes(app.pid)
        for name, triangles, points in [('split.glb', 2000000, 0), ('split.stl', 2000000, 0),
                                        ('mesh-le.ply', 2000000, 0), ('mesh-be.ply', 2000000, 0),
                                        ('points-le.ply', 0, 8000000), ('points-be.ply', 0, 8000000)]:
            at = time.perf_counter()
            generation = open_file(directory/name)
            wait(lambda: query(4) == generation and query(47) == generation, 'first split geometry')
            event = {'fixture': name, 'firstGeometryMs': (time.perf_counter()-at)*1000,
                     'firstTriangles': query(14), 'firstPoints': query(15), 'firstState': query(0)}
            assert event['firstTriangles'] < triangles or event['firstPoints'] < points
            assert event['firstState'] == 2
            wait(lambda: query(0) in (3, 4), 'complete split import')
            assert query(0) == 3, f'error code {query(41)}'
            assert query(14) == triangles and query(15) == points and query(16)
            assert query(28) == 0, 'UI retained whole normalized geometry'
            assert query(8) > 1
            event.update({'readyMs': (time.perf_counter()-at)*1000, 'chunks': query(8)})
            report['events'].append(event)
            sample()
        if large:
            for name in ['A-large-glb.glb', 'A-large-stl.stl', 'A-large-ply-mesh-le.ply', 'A-large-ply-points-be.ply']:
                at = time.perf_counter()
                generation = open_file(large/name)
                wait(lambda: query(4) == generation or query(0) == 4, 'large first geometry', timeout=600)
                assert query(4) == generation and query(0) == 2, f'large source failed {query(41)}'
                event = {'fixture': name, 'firstGeometryMs': (time.perf_counter()-at)*1000,
                         'firstTriangles': query(14), 'firstPoints': query(15)}
                send(hwnd, 0x100, 0x1b)
                assert query(0) == 5 and not query(16)
                report['events'].append(event)
                sample()
        generation = open_file(ROOT/'interactive-viewer/test-assets/tri_tight.glb')
        wait(lambda: query(4) == generation and query(0) == 3, 'valid reopen')
        assert query(14) == 1
        assert report['peakQueueBytes'] <= 128*1024*1024
        assert report['workerPeakPrivateBytes'] < 512*1024*1024
        assert (report['viewerPeakPrivateBytes']-report['baselineViewerPrivateBytes']
                +report['workerPeakPrivateBytes']) <= 1536*1024*1024
        send(hwnd, 0x10)
        app.wait(timeout=15)
        assert app.returncode == 0
        until = time.monotonic()+15
        while any(name == 'Preview3DImportWorker.exe' and (parent == app.pid or pid in workers)
                  for pid, (parent, name) in processes().items()):
            if time.monotonic() >= until:
                raise TimeoutError('worker cleanup')
            time.sleep(0.02)
        report['exitCode'] = app.returncode
        report['survivingWorkers'] = 0
    except Exception as error:
        report['failure'] = f'{type(error).__name__}: {error}'
    finally:
        if app.poll() is None:
            app.kill()
            app.wait(timeout=15)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configuration', choices=['Debug', 'Release'], default='Debug')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--large-fixtures', type=Path)
    args = parser.parse_args()
    user.SetProcessDpiAwarenessContext(W.HANDLE(-4))
    directory = args.output.parent/'bounded-fixtures'
    directory.mkdir(parents=True, exist_ok=True)
    recipes.glb(directory/'split.glb', 2000000)
    recipes.stl(directory/'split.stl', 2000000)
    for big, suffix in [(False, 'le'), (True, 'be')]:
        recipes.ply(directory/f'mesh-{suffix}.ply', 2000000, False, '>' if big else '<')
        recipes.ply(directory/f'points-{suffix}.ply', 8000000, True, '>' if big else '<')
    hashes = {path.name: recipes.digest(path) for path in directory.iterdir()}
    exe = ROOT/f'x64/{args.configuration}/Preview3D.exe'
    report = {'configuration': args.configuration, 'exeSha256': recipes.digest(exe),
              'workerExeSha256': recipes.digest(exe.with_name('Preview3DImportWorker.exe')),
              'fixtureSha256': hashes, 'run': run(exe, directory, args.large_fixtures)}
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))
    if report['run']['failure']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
