"""T14 Studio IBL evidence: a material-sphere grid under Studio and Directional.

The fixture is generated here (no binary check-in): six UV spheres with white,
black and red dielectrics, rough/polished metals and a glass-like dielectric.
The script drives the developer `--app-smoke` control surface to open the
fixture and select Studio/Clay/Directional, captures the real viewer with
PrintWindow(PW_RENDERFULLCONTENT), orbits the camera once, and records a
tolerant downsampled MAE against a per-machine golden on later runs.

Usage:
    python tests/app-smoke/studio.py --output TestResults/studio-ibl
    python tests/app-smoke/studio.py --output TestResults/studio-ibl --update-golden
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
import zlib

from run import ROOT, CopyData, find_window, send, user

gdi = C.WinDLL('gdi32', use_last_error=True)

user.GetClientRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
user.ClientToScreen.argtypes = [W.HWND, C.POINTER(W.POINT)]
user.PrintWindow.argtypes = [W.HWND, W.HDC, W.UINT]
user.GetDC.argtypes = [W.HWND]
user.GetDC.restype = W.HDC
user.ReleaseDC.argtypes = [W.HWND, W.HDC]
user.SetWindowPos.argtypes = [W.HWND, W.HWND, C.c_int, C.c_int, C.c_int, C.c_int, W.UINT]
user.GetDpiForWindow.argtypes = [W.HWND]

gdi.CreateCompatibleDC.argtypes = [W.HDC]
gdi.CreateCompatibleDC.restype = W.HDC
gdi.CreateCompatibleBitmap.argtypes = [W.HDC, C.c_int, C.c_int]
gdi.CreateCompatibleBitmap.restype = W.HBITMAP
gdi.SelectObject.argtypes = [W.HDC, W.HGDIOBJ]
gdi.SelectObject.restype = W.HGDIOBJ
gdi.DeleteObject.argtypes = [W.HGDIOBJ]
gdi.DeleteDC.argtypes = [W.HDC]
gdi.GetDIBits.argtypes = [W.HDC, W.HBITMAP, W.UINT, W.UINT, C.c_void_p,
                          C.POINTER(C.c_void_p), W.UINT]


class BITMAPINFOHEADER(C.Structure):
    _fields_ = [('biSize', W.DWORD), ('biWidth', W.LONG), ('biHeight', W.LONG),
                ('biPlanes', W.WORD), ('biBitCount', W.WORD),
                ('biCompression', W.DWORD), ('biSizeImage', W.DWORD),
                ('biXPelsPerMeter', W.LONG), ('biYPelsPerMeter', W.LONG),
                ('biClrUsed', W.DWORD), ('biClrImportant', W.DWORD)]


def png(width, height, rgb):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    return (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def capture(hwnd):
    rect = W.RECT()
    if not user.GetClientRect(hwnd, C.byref(rect)):
        raise C.WinError(C.get_last_error())
    width, height = rect.right, rect.bottom
    screen = user.GetDC(hwnd)
    memory = gdi.CreateCompatibleDC(screen)
    bitmap = gdi.CreateCompatibleBitmap(screen, width, height)
    old = gdi.SelectObject(memory, bitmap)
    try:
        if not user.PrintWindow(hwnd, memory, 2):  # PW_RENDERFULLCONTENT
            raise RuntimeError('PrintWindow failed')
        header = BITMAPINFOHEADER()
        header.biSize = C.sizeof(header)
        header.biWidth = width
        header.biHeight = -height  # top-down
        header.biPlanes = 1
        header.biBitCount = 32
        buffer = C.create_string_buffer(width * height * 4)
        if not gdi.GetDIBits(memory, bitmap, 0, height, buffer,
                             C.cast(C.byref(header), C.POINTER(C.c_void_p)), 0):
            raise RuntimeError('GetDIBits failed')
        raw = buffer.raw
        rgb = bytearray(width * height * 3)
        for i in range(width * height):
            rgb[i * 3 + 0] = raw[i * 4 + 2]
            rgb[i * 3 + 1] = raw[i * 4 + 1]
            rgb[i * 3 + 2] = raw[i * 4 + 0]
        return width, height, bytes(rgb)
    finally:
        gdi.SelectObject(memory, old)
        gdi.DeleteObject(bitmap)
        gdi.DeleteDC(memory)
        user.ReleaseDC(hwnd, screen)


def downsampled(width, height, rgb, size=64):
    samples = []
    for gy in range(size):
        y = min(height - 1, (gy * height) // size)
        for gx in range(size):
            x = min(width - 1, (gx * width) // size)
            i = (y * width + x) * 3
            samples.append((rgb[i], rgb[i + 1], rgb[i + 2]))
    return samples


def mae(a, b):
    return sum(abs(x - y) for pa, pb in zip(a, b) for x, y in zip(pa, pb)) / (len(a) * 3) / 255.0


def orbit(hwnd, pixels=260):
    rect = W.RECT()
    user.GetClientRect(hwnd, C.byref(rect))
    top = round(52 * (user.GetDpiForWindow(hwnd) / 96.0))
    bottom = round(44 * (user.GetDpiForWindow(hwnd) / 96.0))
    y = top + (rect.bottom - top - bottom) // 2
    x = rect.right // 2
    send(hwnd, 0x201, 1, (y << 16) | x)
    for step in range(1, 9):
        send(hwnd, 0x200, 1, (y << 16) | (x + pixels * step // 8))
        time.sleep(0.02)
    send(hwnd, 0x202, 0, (y << 16) | (x + pixels))


def uv_sphere(segments=24, rings=16):
    positions, normals, uvs, indices = [], [], [], []
    for r in range(rings + 1):
        v = r / rings
        theta = v * math.pi
        for s in range(segments + 1):
            u = s / segments
            phi = u * 2 * math.pi
            x = math.sin(theta) * math.cos(phi)
            y = math.sin(theta) * math.sin(phi)
            z = math.cos(theta)
            positions.append((x, y, z))
            normals.append((x, y, z))
            uvs.append((u, 1.0 - v))
    for r in range(rings):
        for s in range(segments):
            a = r * (segments + 1) + s
            b = a + segments + 1
            indices += [a, b, a + 1, a + 1, b, b + 1]
    return positions, normals, uvs, indices


def sphere_fixture(path):
    materials = [
        {'name': 'white dielectric', 'pbrMetallicRoughness': {'baseColorFactor': [0.92, 0.92, 0.92, 1], 'metallicFactor': 0, 'roughnessFactor': 0.45}},
        {'name': 'black dielectric', 'pbrMetallicRoughness': {'baseColorFactor': [0.02, 0.02, 0.02, 1], 'metallicFactor': 0, 'roughnessFactor': 0.32}},
        {'name': 'red dielectric', 'pbrMetallicRoughness': {'baseColorFactor': [0.72, 0.04, 0.03, 1], 'metallicFactor': 0, 'roughnessFactor': 0.40}},
        {'name': 'rough gold metal', 'pbrMetallicRoughness': {'baseColorFactor': [1.0, 0.72, 0.28, 1], 'metallicFactor': 1, 'roughnessFactor': 0.38}},
        {'name': 'polished metal', 'pbrMetallicRoughness': {'baseColorFactor': [0.82, 0.85, 0.90, 1], 'metallicFactor': 1, 'roughnessFactor': 0.06}},
        {'name': 'glass dielectric', 'alphaMode': 'BLEND', 'pbrMetallicRoughness': {'baseColorFactor': [0.55, 0.72, 0.82, 0.35], 'metallicFactor': 0, 'roughnessFactor': 0.05}},
    ]
    positions, normals, uvs, indices = uv_sphere()
    locations = [(-2.2, 1.1), (0.0, 1.1), (2.2, 1.1), (-2.2, -1.1), (0.0, -1.1), (2.2, -1.1)]
    chunks, views, accessors, primitives = [], [], [], []
    offset = 0

    def add_view(data, target):
        nonlocal offset
        while offset % 4:
            chunks.append(b'\0')
            offset += 1
        start = offset
        chunks.append(data)
        offset += len(data)
        views.append({'buffer': 0, 'byteOffset': start, 'byteLength': len(data), 'target': target})
        return len(views) - 1

    for index, (x, y) in enumerate(locations):
        scale, z = 0.82, 0.82
        pos_bytes = b''.join(struct.pack('<3f', px * scale + x, py * scale + y, pz * scale + z)
                             for px, py, pz in positions)
        nrm_bytes = b''.join(struct.pack('<3f', *n) for n in normals)
        uv_bytes = b''.join(struct.pack('<2f', *t) for t in uvs)
        idx_bytes = b''.join(struct.pack('<I', i) for i in indices)
        pv = add_view(pos_bytes, 34962)
        nv = add_view(nrm_bytes, 34962)
        tv = add_view(uv_bytes, 34962)
        iv = add_view(idx_bytes, 34963)
        base = len(accessors)
        accessors += [
            {'bufferView': pv, 'componentType': 5126, 'count': len(positions), 'type': 'VEC3', 'min': [-1, -1, -1], 'max': [1, 1, 1]},
            {'bufferView': nv, 'componentType': 5126, 'count': len(normals), 'type': 'VEC3'},
            {'bufferView': tv, 'componentType': 5126, 'count': len(uvs), 'type': 'VEC2'},
            {'bufferView': iv, 'componentType': 5125, 'count': len(indices), 'type': 'SCALAR'},
        ]
        primitives.append({'attributes': {'POSITION': base, 'NORMAL': base + 1, 'TEXCOORD_0': base + 2},
                           'indices': base + 3, 'material': index})
    binary = b''.join(chunks)
    binary += b'\0' * ((-len(binary)) % 4)
    document = {
        'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
        'nodes': [{'mesh': 0}],
        'meshes': [{'primitives': primitives}],
        'materials': materials,
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


def wait_for_window(app, timeout=30):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if app.poll() is not None:
            raise RuntimeError(f'viewer exited {app.returncode}')
        hwnd = find_window(app.pid)
        if hwnd:
            return hwnd
        time.sleep(0.05)
    raise TimeoutError('viewer window')


def wait_for_render(hwnd, timeout=30):
    """Waits until the viewport has real geometry, not just the loading card."""
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        width, height, rgb = capture(hwnd)
        bright = 0
        total = 0
        for y in range(height // 6, height * 4 // 5, 4):
            for x in range(width // 5, width * 4 // 5, 4):
                i = (y * width + x) * 3
                if max(rgb[i], rgb[i + 1], rgb[i + 2]) > 110:
                    bright += 1
                total += 1
        if bright > total * 0.02:
            return width, height, rgb
        time.sleep(0.15)
    raise TimeoutError('model never became visible')


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
    fixture = sphere_fixture(output / 'studio-spheres.glb')
    exe = ROOT / f'x64/{args.configuration}/Preview3D.exe'
    if not exe.exists():
        raise SystemExit(f'built viewer not found: {exe}')

    app = subprocess.Popen([str(exe), '--app-smoke'], stderr=subprocess.PIPE)
    report = {'configuration': args.configuration, 'fixture': str(fixture),
              'screenshots': {}, 'mae': {}, 'failure': None}
    try:
        hwnd = wait_for_window(app)
        user.SetWindowPos(hwnd, None, 30, 30, 1280, 860, 0x14)
        time.sleep(0.4)

        def query(field, lparam=0):
            return send(hwnd, 0x8000 + 104, field, lparam)

        text = C.create_unicode_buffer(str(fixture.resolve()))
        data = CopyData(104, C.sizeof(text), C.cast(text, C.c_void_p))
        generation = send(hwnd, 0x4A, 0, C.addressof(data))
        if not generation:
            raise RuntimeError('viewer rejected the studio fixture')

        until = time.monotonic() + 30
        while time.monotonic() < until:
            if app.poll() is not None:
                raise RuntimeError(f'viewer exited {app.returncode}')
            if query(0) == 3 and query(4) == generation:
                break
            time.sleep(0.05)
        else:
            raise TimeoutError('studio fixture never became Ready')
        wait_for_render(hwnd)
        time.sleep(0.8)

        def shot(name):
            w, h, pixels = capture(hwnd)
            (output / name).write_bytes(png(w, h, pixels))
            report['screenshots'][name] = [w, h]
            return downsampled(w, h, pixels)

        def set_lighting(mode):
            query(88, mode)
            time.sleep(0.5)

        studio = shot('studio.png')
        set_lighting(2)  # Directional
        directional = shot('directional.png')
        set_lighting(1)  # Clay
        clay = shot('clay.png')
        set_lighting(0)  # back to Studio
        orbit(hwnd)
        time.sleep(0.7)
        orbited = shot('studio-orbited.png')

        report['mae']['studio-vs-directional'] = mae(studio, directional)
        report['mae']['studio-vs-clay'] = mae(studio, clay)
        report['mae']['studio-vs-orbited'] = mae(studio, orbited)
        # Distinct lighting modes and a visible reflection shift are required;
        # identical captures would mean the mode switch or the orbit did nothing.
        if report['mae']['studio-vs-directional'] < 0.005:
            raise AssertionError('Directional mode did not change the image')
        if report['mae']['studio-vs-clay'] < 0.005:
            raise AssertionError('Clay mode did not change the image')
        if report['mae']['studio-vs-orbited'] < 0.002:
            raise AssertionError('camera orbit did not move the rendering')

        golden = output / 'studio-golden.json'
        if args.update_golden or not golden.exists():
            golden.write_text(json.dumps(studio))
            report['goldenWritten'] = True
        else:
            report['studioGoldenMae'] = mae(studio, json.loads(golden.read_text()))
            # Tolerant: driver/OS capture differences must not fail the lane,
            # but a material or environment regression will.
            if report['studioGoldenMae'] > 0.06:
                raise AssertionError(f'Studio golden MAE {report["studioGoldenMae"]:.4f} > 0.06')
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
    (output / 'studio-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if report['failure']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
