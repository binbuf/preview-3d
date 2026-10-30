"""T16 ground contact shadow and AO screenshot evidence.

Reuses the T14/T15 app-smoke capture lane and drives the developer
``--app-smoke`` control surface:

  101 query shadow, 102 set shadow, 103 query grid, 104 set grid

It captures a CAD/STL part with the shadow on/off and with the grid on/off, a
sculpt/scan preset, a transparent asset on the checkerboard, and a point cloud
whose preset disables the shadow. Goldens use the same downsampled RGB MAE and
global SSIM tolerances as T15 and record frame/GPU/private-byte performance.

Usage:
    python tests/app-smoke/ground-shadow.py --output TestResults/ground-shadow
    python tests/app-smoke/ground-shadow.py --output TestResults/ground-shadow --update-golden
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import importlib.util
import json
import math
from pathlib import Path
import struct
import subprocess
import time

from run import ROOT, CopyData, find_window, send, user
from studio import (capture, downsampled, mae, png, sphere_fixture,
                    wait_for_render, wait_for_window)

MAE_TOLERANCE = 0.06
SSIM_TOLERANCE = 0.90
# The grid strokes are deliberately subtle; 0.0005 still rejects an unchanged
# frame while accepting the real one-pixel-line difference.
CHANGED_THRESHOLD = 0.0005

# Reuse T15's transparent-sphere fixture without executing its main().
_spec = importlib.util.spec_from_file_location(
    'studio_presets', Path(__file__).with_name('studio-presets.py'))
_studio_presets = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_studio_presets)
transparent_sphere_fixture = _studio_presets.transparent_sphere_fixture


def ssim(a, b):
    def gray(samples):
        return [0.2126 * r + 0.7152 * g + 0.0722 * b for r, g, b in samples]
    x, y = gray(a), gray(b)
    n = len(x)
    mean_x = sum(x) / n
    mean_y = sum(y) / n
    var_x = sum((v - mean_x) ** 2 for v in x) / n
    var_y = sum((v - mean_y) ** 2 for v in y) / n
    cov = sum((u - mean_x) * (v - mean_y) for u, v in zip(x, y)) / n
    c1 = (0.01 * 255) ** 2
    c2 = (0.03 * 255) ** 2
    return ((2 * mean_x * mean_y + c1) * (2 * cov + c2)) / (
        (mean_x ** 2 + mean_y ** 2 + c1) * (var_x + var_y + c2))


def stl_box_fixture(path):
    """Binary STL box: a CAD-preset fixture that must sit on the ground."""
    corners = [
        (-1.0, -1.0, 0.0), (1.0, -1.0, 0.0), (1.0, 1.0, 0.0), (-1.0, 1.0, 0.0),
        (-1.0, -1.0, 2.0), (1.0, -1.0, 2.0), (1.0, 1.0, 2.0), (-1.0, 1.0, 2.0),
    ]
    faces = [
        (0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7),
        (0, 1, 5), (0, 5, 4), (1, 2, 6), (1, 6, 5),
        (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7),
    ]
    data = bytearray(b'\0' * 80)
    data += struct.pack('<I', len(faces))
    for a, b, c in faces:
        data += struct.pack('<3f', 0.0, 0.0, 0.0)
        for index in (a, b, c):
            data += struct.pack('<3f', *corners[index])
        data += struct.pack('<H', 0)
    path.write_bytes(data)
    return path


def ply_points_fixture(path):
    """A small deterministic point cloud; the Point cloud preset must not shadow."""
    lines = ['ply', 'format ascii 1.0', 'element vertex 2048',
             'property float x', 'property float y', 'property float z',
             'end_header']
    for i in range(2048):
        ring = i % 64
        level = i // 64
        angle = ring * (2.0 * math.pi / 64.0)
        radius = 1.0 + 0.12 * math.sin(level * 0.7)
        z = 0.2 + level * 0.05
        lines.append(f'{radius * math.cos(angle):.6f} {radius * math.sin(angle):.6f} {z:.6f}')
    path.write_text('\n'.join(lines) + '\n')
    return path


def query(hwnd, field):
    return send(hwnd, 0x8000 + 104, field, 0) & 0xFFFFFFFF


def query_f64(hwnd, field):
    return struct.unpack('<d', struct.pack('<Q', query_u64(hwnd, field)))[0]


def query_u64(hwnd, field):
    return send(hwnd, 0x8000 + 104, field, 0) & 0xFFFFFFFFFFFFFFFF


def set_value(hwnd, field, value):
    return send(hwnd, 0x8000 + 104, field, value) & 0xFFFFFFFF


def open_fixture(hwnd, app, path):
    text = C.create_unicode_buffer(str(Path(path).resolve()))
    data = CopyData(104, C.sizeof(text), C.cast(text, C.c_void_p))
    generation = send(hwnd, 0x4A, 0, C.addressof(data))
    if not generation:
        raise RuntimeError('viewer rejected the fixture')
    until = time.monotonic() + 30
    while time.monotonic() < until:
        if app.poll() is not None:
            raise RuntimeError(f'viewer exited {app.returncode}')
        if send(hwnd, 0x8000 + 104, 0, 0) == 3 and send(hwnd, 0x8000 + 104, 4, 0) == generation:
            break
        time.sleep(0.05)
    else:
        raise TimeoutError('fixture never became Ready')
    wait_for_render(hwnd)
    time.sleep(0.6)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configuration', choices=['Debug', 'Release'], default='Debug')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--update-golden', action='store_true')
    args = parser.parse_args()
    if not user.SetProcessDpiAwarenessContext(W.HANDLE(-4)):
        raise C.WinError(C.get_last_error())

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cad = stl_box_fixture(output / 'cad-box.stl')
    sculpt = sphere_fixture(output / 'sculpt-spheres.glb')
    transparent = transparent_sphere_fixture(output / 'transparent-sphere.glb')
    points = ply_points_fixture(output / 'point-cloud.ply')
    exe = ROOT / f'x64/{args.configuration}/Preview3D.exe'
    if not exe.exists():
        raise SystemExit(f'built viewer not found: {exe}')

    app = subprocess.Popen([str(exe), '--app-smoke'], stderr=subprocess.PIPE)
    report = {'configuration': args.configuration, 'screenshots': {}, 'mae': {},
              'ssim': {}, 'golden': {}, 'states': {},
              'tolerances': {'mae': MAE_TOLERANCE, 'ssim': SSIM_TOLERANCE},
              'failure': None}
    try:
        hwnd = wait_for_window(app)
        user.SetWindowPos(hwnd, None, 30, 30, 1280, 860, 0x14)
        time.sleep(0.4)

        def shot(name):
            w, h, pixels = capture(hwnd)
            (output / name).write_bytes(png(w, h, pixels))
            report['screenshots'][name] = [w, h]
            return downsampled(w, h, pixels)

        captures = {}
        open_fixture(hwnd, app, cad)
        set_value(hwnd, 104, 1)  # grid on
        set_value(hwnd, 90, 1)   # Studio CAD preset
        set_value(hwnd, 102, 1)  # shadow on
        time.sleep(0.45)
        report['states']['cad'] = {'shadow': query(hwnd, 101), 'grid': query(hwnd, 103),
                                   'background': query(hwnd, 93)}
        if report['states']['cad'] != {'shadow': 1, 'grid': 1, 'background': 0}:
            raise AssertionError(f"CAD defaults were wrong: {report['states']['cad']}")
        captures['cad-shadow-on.png'] = shot('cad-shadow-on.png')
        captures['cad-grid-on.png'] = shot('cad-grid-on.png')

        set_value(hwnd, 102, 0)  # shadow off
        time.sleep(0.45)
        if query(hwnd, 101) != 0 or query(hwnd, 103) != 1:
            raise AssertionError('shadow toggle changed the grid')
        captures['cad-shadow-off.png'] = shot('cad-shadow-off.png')

        set_value(hwnd, 102, 1)  # shadow back on, grid off
        set_value(hwnd, 104, 0)
        time.sleep(0.45)
        if query(hwnd, 101) != 1 or query(hwnd, 103) != 0:
            raise AssertionError('grid toggle changed the shadow')
        captures['cad-grid-off-shadow-on.png'] = shot('cad-grid-off-shadow-on.png')

        open_fixture(hwnd, app, sculpt)
        set_value(hwnd, 90, 2)  # Scan / Sculpt
        time.sleep(0.45)
        report['states']['sculpt'] = {'shadow': query(hwnd, 101), 'grid': query(hwnd, 103)}
        if report['states']['sculpt']['shadow'] != 1:
            raise AssertionError('Scan/Sculpt should enable the contact shadow')
        captures['sculpt-shadow.png'] = shot('sculpt-shadow.png')

        open_fixture(hwnd, app, transparent)
        set_value(hwnd, 90, 3)  # Transparent -> checkerboard
        time.sleep(0.45)
        report['states']['transparent'] = {
            'shadow': query(hwnd, 101), 'background': query(hwnd, 93)}
        if report['states']['transparent'] != {'shadow': 0, 'background': 3}:
            raise AssertionError(
                f"transparent defaults were wrong: {report['states']['transparent']}")
        captures['transparent-checker.png'] = shot('transparent-checker.png')

        open_fixture(hwnd, app, points)
        set_value(hwnd, 90, 4)  # Point cloud
        time.sleep(0.45)
        report['states']['point-cloud'] = {'shadow': query(hwnd, 101)}
        if report['states']['point-cloud']['shadow'] != 0:
            raise AssertionError('Point cloud preset must disable the contact shadow')
        captures['point-cloud-no-shadow.png'] = shot('point-cloud-no-shadow.png')

        names = list(captures)
        for i in range(len(names)):
            for j in range(i + 1, len(names)):
                key = f'{names[i]}-vs-{names[j]}'
                report['mae'][key] = mae(captures[names[i]], captures[names[j]])
                report['ssim'][key] = ssim(captures[names[i]], captures[names[j]])

        for key in ('cad-shadow-on.png-vs-cad-shadow-off.png',
                    'cad-grid-on.png-vs-cad-grid-off-shadow-on.png'):
            if report['mae'].get(key, 0.0) < CHANGED_THRESHOLD:
                raise AssertionError(f'{key} did not change the image')

        golden_path = output / 'ground-shadow-golden.json'
        if args.update_golden or not golden_path.exists():
            golden_path.write_text(json.dumps(captures))
            report['goldenWritten'] = True
        else:
            golden = json.loads(golden_path.read_text())
            for name, samples in captures.items():
                if name not in golden:
                    raise AssertionError(f'missing golden for {name}')
                report['golden'][name] = {
                    'mae': mae(samples, golden[name]),
                    'ssim': ssim(samples, golden[name]),
                }
                if report['golden'][name]['mae'] > MAE_TOLERANCE:
                    raise AssertionError(
                        f'{name} MAE {report["golden"][name]["mae"]:.4f} > {MAE_TOLERANCE}')
                if report['golden'][name]['ssim'] < SSIM_TOLERANCE:
                    raise AssertionError(
                        f'{name} SSIM {report["golden"][name]["ssim"]:.4f} < {SSIM_TOLERANCE}')

        report['performance'] = {
            'mean_frame_ms': query_f64(hwnd, 99),
            'p95_frame_ms': query_f64(hwnd, 100),
            'accounted_gpu_bytes': query_u64(hwnd, 55),
            'private_bytes': query_u64(hwnd, 77),
            'presented_frames': query_u64(hwnd, 7),
        }
    except Exception as error:
        report['failure'] = str(error)
    finally:
        if app.poll() is None:
            send(hwnd, 0x10) if 'hwnd' in locals() and hwnd else None
            try:
                app.wait(timeout=8)
            except subprocess.TimeoutExpired:
                app.kill()
                app.wait(timeout=8)
        report['diagnostics'] = app.stderr.read().decode('utf-8', errors='replace')[:4096]
        app.stderr.close()
    (output / 'ground-shadow-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if report['failure']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
