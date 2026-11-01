"""T15 presentation evidence: one capture per Studio preset plus a transparent asset.

The script reuses the T14 app-smoke window-capture lane (PrintWindow +
downsampled RGB MAE) and drives the developer `--app-smoke` control surface to
select Studio PBR / CAD / Scan-Sculpt / Transparent / Point cloud. Each preset
gets a stable per-machine golden with an explicit MAE and global SSIM tolerance;
the transparent asset is captured with the checkerboard background so alpha
compositing is visible.

Usage:
    python tests/app-smoke/studio-presets.py --output TestResults/studio-presets
    python tests/app-smoke/studio-presets.py --output TestResults/studio-presets --update-golden
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import math
from pathlib import Path
import struct
import subprocess
import time

from run import ROOT, CopyData, find_window, send, user
from studio import (capture, downsampled, mae, png, uv_sphere, wait_for_render,
                    wait_for_window)

PRESET_NAMES = ['studio-pbr', 'studio-cad', 'scan-sculpt', 'transparent', 'point-cloud']
MAE_TOLERANCE = 0.06
SSIM_TOLERANCE = 0.90


def ssim(a, b):
    """Global SSIM over the downsampled RGB samples (presentation, not QA)."""
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


def transparent_sphere_fixture(path):
    """One large alpha-blended sphere; the checkerboard shows through it."""
    positions, normals, uvs, indices = uv_sphere()
    pos_bytes = b''.join(struct.pack('<3f', *p) for p in positions)
    nrm_bytes = b''.join(struct.pack('<3f', *n) for n in normals)
    uv_bytes = b''.join(struct.pack('<2f', *t) for t in uvs)
    idx_bytes = b''.join(struct.pack('<I', i) for i in indices)
    views, accessors, chunks = [], [], []
    offset = 0
    for data, target in ((pos_bytes, 34962), (nrm_bytes, 34962), (uv_bytes, 34962),
                         (idx_bytes, 34963)):
        while offset % 4:
            chunks.append(b'\0')
            offset += 1
        start = offset
        chunks.append(data)
        offset += len(data)
        views.append({'buffer': 0, 'byteOffset': start, 'byteLength': len(data), 'target': target})
    accessors = [
        {'bufferView': 0, 'componentType': 5126, 'count': len(positions), 'type': 'VEC3',
         'min': [-1, -1, -1], 'max': [1, 1, 1]},
        {'bufferView': 1, 'componentType': 5126, 'count': len(normals), 'type': 'VEC3'},
        {'bufferView': 2, 'componentType': 5126, 'count': len(uvs), 'type': 'VEC2'},
        {'bufferView': 3, 'componentType': 5125, 'count': len(indices), 'type': 'SCALAR'},
    ]
    binary = b''.join(chunks)
    binary += b'\0' * ((-len(binary)) % 4)
    document = {
        'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
        'nodes': [{'mesh': 0, 'scale': [1.6, 1.6, 1.6]}],
        'meshes': [{'primitives': [{
            'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2},
            'indices': 3, 'material': 0}]}],
        'materials': [{'name': 'transparent dielectric', 'alphaMode': 'BLEND',
                       'pbrMetallicRoughness': {
                           'baseColorFactor': [0.55, 0.78, 0.92, 0.28],
                           'metallicFactor': 0.0, 'roughnessFactor': 0.08}}],
        'accessors': accessors, 'bufferViews': views,
        'buffers': [{'byteLength': len(binary)}],
    }
    text = json.dumps(document, sort_keys=True, separators=(',', ':')).encode()
    text += b' ' * ((-len(text)) % 4)
    data = struct.pack('<III', 0x46546c67, 2, 28 + len(text) + len(binary))
    data += struct.pack('<II', len(text), 0x4e4f534a) + text
    data += struct.pack('<II', len(binary), 0x004e4942) + binary
    path.write_bytes(data)
    return path


def f32_bits(value):
    bits = C.c_uint32()
    C.memmove(C.byref(bits), C.byref(C.c_float(value)), 4)
    return bits.value


def query_bits(hwnd, field):
    return send(hwnd, 0x8000 + 104, field, 0) & 0xFFFFFFFF


def query_f32(hwnd, field):
    return struct.unpack('<f', struct.pack('<I', query_bits(hwnd, field)))[0]


def query_f64(hwnd, field):
    return struct.unpack('<d', struct.pack('<Q', query_u64(hwnd, field)))[0]


def query_u64(hwnd, field):
    return send(hwnd, 0x8000 + 104, field, 0) & 0xFFFFFFFFFFFFFFFF


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
    # Reuse the richer six-sphere fixture for the preset sweep, and generate one
    # dedicated transparent sphere for the alpha/checkerboard evidence.
    from studio import sphere_fixture
    spheres = sphere_fixture(output / 'studio-spheres.glb')
    transparent = transparent_sphere_fixture(output / 'transparent-sphere.glb')
    exe = ROOT / f'x64/{args.configuration}/Preview3D.exe'
    if not exe.exists():
        raise SystemExit(f'built viewer not found: {exe}')

    app = subprocess.Popen([str(exe), '--app-smoke'], stderr=subprocess.PIPE)
    report = {'configuration': args.configuration, 'screenshots': {}, 'mae': {},
              'ssim': {}, 'golden': {}, 'tolerances': {
                  'mae': MAE_TOLERANCE, 'ssim': SSIM_TOLERANCE}, 'failure': None}
    try:
        hwnd = wait_for_window(app)
        user.SetWindowPos(hwnd, None, 30, 30, 1280, 860, 0x14)
        time.sleep(0.4)

        def shot(name):
            w, h, pixels = capture(hwnd)
            (output / name).write_bytes(png(w, h, pixels))
            report['screenshots'][name] = [w, h]
            return downsampled(w, h, pixels)

        open_fixture(hwnd, app, spheres)
        captures = {}
        for preset, name in enumerate(PRESET_NAMES):
            send(hwnd, 0x8000 + 104, 90, preset)
            time.sleep(0.45)
            captures[name] = shot(f'preset-{name}.png')

        open_fixture(hwnd, app, transparent)
        send(hwnd, 0x8000 + 104, 90, 3)  # Transparent preset -> checkerboard
        time.sleep(0.45)
        captures['transparent-asset'] = shot('transparent-asset.png')

        names = list(captures)
        for i in range(len(names)):
            for j in range(i + 1, len(names)):
                key = f'{names[i]}-vs-{names[j]}'
                report['mae'][key] = mae(captures[names[i]], captures[names[j]])
                report['ssim'][key] = ssim(captures[names[i]], captures[names[j]])

        # Presets must not collapse into the same image.
        for key in ('studio-pbr-vs-studio-cad', 'studio-pbr-vs-scan-sculpt',
                    'studio-pbr-vs-transparent', 'studio-pbr-vs-point-cloud'):
            if report['mae'].get(key, 1.0) < 0.002:
                raise AssertionError(f'{key} did not change the image')

        golden_path = output / 'studio-presets-golden.json'
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
    (output / 'studio-presets-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if report['failure']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
