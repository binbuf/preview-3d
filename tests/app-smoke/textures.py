"""TSK-203: deterministic raster fixtures, visible low-mip refinement and recovery."""
import argparse
import copy
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time
import zlib

from run import ROOT, CopyData, find_window, processes, send, user, W


def png(width, height):
    def chunk(kind, data):
        return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data))
    rows = b''.join(b'\0'+bytes((128, 128, 128, 255))*width for _ in range(height))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))+chunk(b'IDAT', zlib.compress(rows, 9))+chunk(b'IEND', b'')


def quad_glb(image):
    """A single-sided textured quad whose front faces home camera +Z.

    Deliberately NOT doubleSided: the D3D12 back-culling convention must keep
    authored counter-clockwise front faces (glTF) visible. A regression here
    makes the whole quad invisible and un-pickable.
    """
    positions = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    uvs = [(0, 1), (1, 1), (1, 0), (0, 0)]
    indices = [0, 1, 2, 0, 2, 3]
    pos_bytes = b''.join(struct.pack('<3f', *p) for p in positions)
    uv_bytes = b''.join(struct.pack('<2f', *t) for t in uvs)
    idx_bytes = b''.join(struct.pack('<I', i) for i in indices)
    geometry = pos_bytes+uv_bytes+idx_bytes
    doc = {
        'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
        'nodes': [{'mesh': 0}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'TEXCOORD_0': 1},
                                    'indices': 2, 'material': 0}]}],
        'materials': [{'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}}}],
        'textures': [{'source': 0}],
        'images': [{'bufferView': 3, 'mimeType': 'image/png'}],
        'accessors': [
            {'bufferView': 0, 'componentType': 5126, 'count': 4, 'type': 'VEC3'},
            {'bufferView': 1, 'componentType': 5126, 'count': 4, 'type': 'VEC2'},
            {'bufferView': 2, 'componentType': 5125, 'count': 6, 'type': 'SCALAR'}],
        'bufferViews': [
            {'buffer': 0, 'byteOffset': 0, 'byteLength': len(pos_bytes)},
            {'buffer': 0, 'byteOffset': len(pos_bytes), 'byteLength': len(uv_bytes)},
            {'buffer': 0, 'byteOffset': len(pos_bytes)+len(uv_bytes), 'byteLength': len(idx_bytes)},
            {'buffer': 0, 'byteOffset': len(geometry), 'byteLength': len(image)}],
        'buffers': [{'byteLength': len(geometry)+len(image)}]}
    binary = geometry+image
    binary += b'\0'*((-len(binary)) % 4)
    text = json.dumps(doc, sort_keys=True, separators=(',', ':')).encode()
    text += b' '*((-len(text)) % 4)
    data = struct.pack('<III', 0x46546c67, 2, 28+len(text)+len(binary))
    data += struct.pack('<II', len(text), 0x4e4f534a)+text+struct.pack('<II', len(binary), 0x004e4942)+binary
    return data


def fixtures(directory):
    seed = (ROOT/'interactive-viewer/test-assets/basisu_textured_triangle.glb').read_bytes()
    size = struct.unpack_from('<I', seed, 12)[0]
    document = json.loads(seed[20:20+size])
    at = 28+size
    geometry = seed[at:at+72]
    hashes = {}
    for name, image in [('raster-256.glb', png(256, 256)), ('raster-capped.glb', png(4096, 512)), ('raster-corrupt.glb', b'corrupt')]:
        doc = copy.deepcopy(document)
        doc.pop('extensionsUsed', None)
        doc.pop('extensionsRequired', None)
        doc['textures'] = [{'source': 0}]
        doc['images'] = [{'bufferView': 3, 'mimeType': 'image/png'}]
        doc['bufferViews'][3] = {'buffer': 0, 'byteOffset': 72, 'byteLength': len(image)}
        binary = geometry+image
        doc['buffers'] = [{'byteLength': len(binary)}]
        binary += b'\0'*((-len(binary)) % 4)
        text = json.dumps(doc, sort_keys=True, separators=(',', ':')).encode()
        text += b' '*((-len(text)) % 4)
        data = struct.pack('<III', 0x46546c67, 2, 28+len(text)+len(binary))
        data += struct.pack('<II', len(text), 0x4e4f534a)+text+struct.pack('<II', len(binary), 0x004e4942)+binary
        directory.mkdir(parents=True, exist_ok=True)
        (directory/name).write_bytes(data)
        hashes[name] = hashlib.sha256(data).hexdigest()
    quad = quad_glb(png(256, 256))
    (directory/'raster-quad-single-sided.glb').write_bytes(quad)
    hashes['raster-quad-single-sided.glb'] = hashlib.sha256(quad).hexdigest()
    return hashes


def run(exe, directory):
    app = subprocess.Popen([str(exe), '--texture-mip-smoke'], stderr=subprocess.PIPE)
    report = {'failure': None, 'scenes': [], 'maxUiMs': 0}
    hwnd = 0
    def query(field):
        at = time.perf_counter()
        result = send(hwnd, 0x8000+104, field)
        report['maxUiMs'] = max(report['maxUiMs'], (time.perf_counter()-at)*1000)
        return result
    def wait(predicate, label):
        until = time.monotonic()+25
        while time.monotonic() < until:
            if app.poll() is not None:
                raise RuntimeError(f'exit {app.returncode}: {label}')
            value = predicate()
            if value:
                return value
            if hwnd and query(0) == 4:
                raise RuntimeError(f'{label}: import failed')
            time.sleep(0.01)
        raise TimeoutError(label)
    def open_file(path):
        text = C.create_unicode_buffer(str(path.resolve()))
        data = CopyData(104, C.sizeof(text), C.cast(text, C.c_void_p))
        return send(hwnd, 0x4a, 0, C.addressof(data))
    def click(packed):
        point = W.POINT(packed & 65535, packed >> 16)
        user.ClientToScreen.argtypes = [W.HWND, C.POINTER(W.POINT)]
        user.ClientToScreen(hwnd, C.byref(point))
        user.SetCursorPos(point.x, point.y)
        time.sleep(0.03)
        send(hwnd, 0x201, 1, packed)
        send(hwnd, 0x202, 0, packed)
    try:
        hwnd = wait(lambda: find_window(app.pid), 'window')
        wait(lambda: query(2), 'background')
        report['startupMaxUiMs'] = report['maxUiMs']
        report['maxUiMs'] = 0
        for name, expected in [('raster-256.glb', 256), ('raster-capped.glb', 2048)]:
            generation = open_file(directory/name)
            wait(lambda: query(4) == generation and query(37) == 64, 'low mip Present')
            assert query(0) == 2 and query(11) == 1 and query(38) == 1
            frames = query(7)
            scene = {'name': name, 'lowExtent': query(37), 'lowMips': query(39)}
            send(hwnd, 0x20a, 120 << 16)
            wait(lambda: query(7) > frames, 'input Present while refining')
            wait(lambda: query(0) == 3 and query(37) == expected, 'full chain Ready')
            assert query(38) == 1 and query(11) == 1, 'refinement retained duplicate live textures'
            scene.update(fullExtent=query(37), fullMips=query(39), textureCount=query(38))
            report['scenes'].append(scene)
        # A single-sided textured quad must remain visible under back-face
        # culling, so its projected bounds center must GPU-pick. An inverted
        # front-face convention makes the whole quad invisible and un-pickable.
        generation = open_file(directory/'raster-quad-single-sided.glb')
        wait(lambda: query(4) == generation and query(0) == 3 and query(37) == 256 and query(11) == 1,
             'single-sided textured quad Ready')
        click(query(29))
        wait(lambda: query(22) == 1, 'single-sided textured quad GPU pick')
        report['singleSidedFrontFacePick'] = True
        generation = open_file(directory/'raster-corrupt.glb')
        wait(lambda: query(4) == generation and query(0) == 3, 'optional fallback Ready')
        assert query(37) == 2 and query(40) > 0 and query(11) == 1
        report['fallbackWarningLength'] = query(40)
        # Replace/cancel with a refinement and then recover through a valid open.
        open_file(directory/'raster-256.glb')
        send(hwnd, 0x100, 0x1b)
        generation = open_file(ROOT/'interactive-viewer/test-assets/tri_tight.glb')
        wait(lambda: query(4) == generation and query(0) == 3, 'valid reopen')
        assert query(38) == 0 and query(40) == 0
        report['debugAvailable'] = bool(query(36))
        report['debugErrors'] = query(35)
        assert report['debugErrors'] == 0
        report['peakQueueBytes'] = query(6)
        assert report['peakQueueBytes'] <= 128*1024*1024
        open_file(directory/'raster-256.glb')
        send(hwnd, 0x10)
        app.wait(timeout=10)
        assert app.returncode == 0
        until = time.monotonic()+5
        while time.monotonic() < until and any(parent == app.pid for parent, name in processes().values() if name == 'Preview3DImportWorker.exe'):
            time.sleep(0.05)
        assert not any(parent == app.pid for parent, name in processes().values() if name == 'Preview3DImportWorker.exe')
    except Exception as error:
        report['failure'] = str(error)
        if hwnd and app.poll() is None:
            try:
                report['stateAtFailure'] = {str(field): query(field) for field in [0,1,4,8,11,37,38,39,40]}
            except Exception:
                report['exitCodeAtFailure'] = app.poll()
    finally:
        if app.poll() is None:
            app.kill()
            app.wait(timeout=10)
        report['diagnostics'] = app.stderr.read().decode('utf-8', errors='replace')[:4096]
        app.stderr.close()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configuration', choices=['Debug', 'Release'], default='Debug')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not user.SetProcessDpiAwarenessContext(W.HANDLE(-4)):
        raise C.WinError(C.get_last_error())
    output = args.output.resolve()
    directory = output.parent/'texture-fixtures'
    hashes = fixtures(directory)
    assert fixtures(directory) == hashes
    exe = ROOT/f'x64/{args.configuration}/Preview3D.exe'
    report = {'configuration': args.configuration, 'fixtureSha256': hashes,
              'exeSha256': hashlib.sha256(exe.read_bytes()).hexdigest(), 'run': run(exe, directory)}
    output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))
    if report['run']['failure']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
