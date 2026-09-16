"""Deterministic, bounded-memory Tier A corpus. No downloads or random seeds."""
import argparse
import ctypes
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import zlib

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / 'interactive-viewer/test-assets'
COLORS = [[1, 0, 0, 1], [0, 1, 0, 1], [0, 0, 1, 1], [1, 1, 1, 1]]


def canonical(value):
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + '\n').encode('utf-8')


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def repeat(out, record, count):
    # At most 1 MiB of private generation scratch even for 4 GiB files.
    block_count = max(1, (1024 * 1024) // len(record))
    block = record * min(count, block_count)
    while count >= block_count:
        out.write(block)
        count -= block_count
    out.write(record * count)


def triangle(component):
    x, y, z = [(10 if component & (1 << bit) else 0) for bit in range(3)]
    return [(x, y, z), (x + 1, y, z), (x, y + 1, z + 1)]


def png():
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    pixels = bytes([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255])
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(b'\0' + pixels[:8] + b'\0' + pixels[8:], 9))
            + chunk(b'IEND', b''))


def glb(path, count, nodes=8, target=0):
    assert count % 8 == 0
    per = count // 8
    views, accessors, meshes = [], [], []
    offset = 0
    for component in range(8):
        size = per * 3 * 12
        views.append({'buffer': 0, 'byteOffset': offset, 'byteLength': size})
        accessors.append({'bufferView': len(views)-1, 'componentType': 5126, 'count': per*3,
                          'type': 'VEC3', 'min': list(triangle(component)[0]),
                          'max': [max(p[i] for p in triangle(component)) for i in range(3)]})
        offset += size
        views.append({'buffer': 0, 'byteOffset': offset, 'byteLength': per*12})
        accessors.append({'bufferView': len(views)-1, 'componentType': 5121, 'normalized': True,
                          'count': per*3, 'type': 'VEC4'})
        offset += per*12
        views.append({'buffer': 0, 'byteOffset': offset, 'byteLength': per*12})
        accessors.append({'bufferView': len(views)-1, 'componentType': 5125, 'count': per*3, 'type': 'SCALAR'})
        offset += per*12
        meshes.append({'primitives': [{'attributes': {'POSITION': component*3, 'COLOR_0': component*3+1},
                                       'indices': component*3+2, 'material': component % 4}]})
    image = png()
    views.append({'buffer': 0, 'byteOffset': offset, 'byteLength': len(image)})
    length = (offset + len(image) + 3) & ~3
    materials = [{'pbrMetallicRoughness': {'baseColorFactor': color, 'metallicFactor': 0.25,
                                        'roughnessFactor': 0.75, 'baseColorTexture': {'index': 0}},
                  'doubleSided': True} for color in COLORS]
    doc = {'asset': {'version': '2.0'}, 'buffers': [{'byteLength': length}],
           'bufferViews': views, 'accessors': accessors, 'meshes': meshes, 'materials': materials,
           'images': [{'bufferView': len(views)-1, 'mimeType': 'image/png'}],
           'textures': [{'source': 0}], 'nodes': [{'mesh': i} if i < 8 else {} for i in range(nodes)],
           'scenes': [{'nodes': list(range(nodes))}], 'scene': 0}
    payload_length = length
    for _ in range(4):
        doc['buffers'][0]['byteLength'] = length
        raw = json.dumps(doc, sort_keys=True, separators=(',', ':')).encode()
        raw += b' ' * (-len(raw) % 4)
        updated = max(payload_length, (target - 28 - len(raw) + 3) & ~3) if target else length
        if updated == length:
            break
        length = updated
    assert doc['buffers'][0]['byteLength'] == length
    total = 28 + len(raw) + length
    assert total < 2**32, 'GLB uint32 length overflow'
    with path.open('wb') as out:
        out.write(struct.pack('<5I', 0x46546c67, 2, total, len(raw), 0x4e4f534a))
        out.write(raw)
        out.write(struct.pack('<2I', length, 0x004e4942))
        for component in range(8):
            repeat(out, b''.join(struct.pack('<3f', *p) for p in triangle(component)), per)
            repeat(out, bytes(int(v*255) for v in COLORS[component % 4]) * 3, per)
            # Index values vary; keep the temporary array bounded.
            for start in range(0, per*3, 65536):
                n = min(65536, per*3-start)
                out.write(struct.pack('<' + 'I'*n, *range(start, start+n)))
        out.write(image)
        repeat(out, b'\0', total-out.tell())


def stl(path, count):
    with path.open('wb') as out:
        out.write(b'Preview3D deterministic corpus v1'.ljust(80, b'\0'))
        out.write(struct.pack('<I', count))
        for c in range(8):
            record = struct.pack('<12fH', 0, -math.sqrt(0.5), math.sqrt(0.5),
                                 *(v for p in triangle(c) for v in p), 0)
            repeat(out, record, count//8)


def ply(path, count, points=False, endian='<'):
    vertices = count if points else count*3
    encoding = 'little' if endian == '<' else 'big'
    header = (f'ply\nformat binary_{encoding}_endian 1.0\nelement vertex {vertices}\n'
              'property float x\nproperty float y\nproperty float z\n'
              'property uchar red\nproperty uchar green\nproperty uchar blue\n')
    if points:
        # Unknown scalars exercise bounded skipping; 48 bytes per point.
        header += ''.join(f'property float padding{i}\n' for i in range(8)) + 'property uchar padding8\n'
    else:
        header += f'element face {count}\nproperty list uchar uint vertex_indices\n'
    with path.open('wb') as out:
        out.write((header + 'end_header\n').encode())
        for c in range(8):
            positions = triangle(c)
            # Cycle the three extrema in every component, including point clouds.
            records = [struct.pack(endian+'3f', *p) + bytes(int(v*255) for v in COLORS[c % 4][:3])
                       + (bytes(33) if points else b'') for p in positions]
            n = vertices//8
            repeat(out, b''.join(records), n//3)
            out.write(b''.join(records[:n % 3]))
        if not points:
            for start in range(0, count, 16384):
                # Reverse source order: nonlocal face references are intentional.
                out.write(b''.join(struct.pack(endian+'B3I', 3, i*3, i*3+1, i*3+2)
                                   for i in range(count-1-start, max(-1, count-1-start-16384), -1)))


def meshopt(path):
    # Fixture encoding only. Read ABI from the pinned installed meshoptimizer.h.
    dll = ROOT / 'vcpkg_installed/x64-windows/x64-windows/bin/meshoptimizer.dll'
    lib = ctypes.CDLL(str(dll))
    bound = lib.meshopt_encodeVertexBufferBound
    bound.argtypes = [ctypes.c_size_t, ctypes.c_size_t]
    bound.restype = ctypes.c_size_t
    encode = lib.meshopt_encodeVertexBuffer
    encode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t]
    encode.restype = ctypes.c_size_t
    raw = struct.pack('<9f', 0, 0, 0, 1, 0, 0, 0, 1, 0)
    output = ctypes.create_string_buffer(bound(3, 12))
    size = encode(output, len(output), raw, 3, 12)
    assert size
    encoded = output.raw[:size]
    doc = {'asset': {'version': '2.0'}, 'extensionsUsed': ['EXT_meshopt_compression'],
           'extensionsRequired': ['EXT_meshopt_compression'],
           'buffers': [{'byteLength': len(encoded)}, {'byteLength': 36}],
           'bufferViews': [{'buffer': 1, 'byteLength': 36, 'byteStride': 12,
                            'extensions': {'EXT_meshopt_compression': {'buffer': 0, 'byteOffset': 0,
                             'byteLength': len(encoded), 'byteStride': 12, 'count': 3, 'mode': 'ATTRIBUTES', 'filter': 'NONE'}}}],
           'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3', 'min': [0,0,0], 'max': [1,1,0]}],
           'meshes': [{'primitives': [{'attributes': {'POSITION': 0}}]}],
           'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}
    raw_json = json.dumps(doc, sort_keys=True, separators=(',', ':')).encode()
    raw_json += b' ' * (-len(raw_json) % 4)
    encoded += bytes(-len(encoded) % 4)
    path.write_bytes(struct.pack('<5I', 0x46546c67, 2, 28+len(raw_json)+len(encoded), len(raw_json), 0x4e4f534a)
                     + raw_json + struct.pack('<2I', len(encoded), 0x004e4942) + encoded)


def generate(output, lane, tier):
    output.mkdir(parents=True, exist_ok=True)
    entries = []

    def add(name, expected, recipe):
        path = output / name
        entries.append({'id': path.stem, 'path': name, 'bytes': path.stat().st_size,
                        'sha256': digest(path), 'expected': expected, 'recipe': recipe})

    tiers = ['small'] if lane == 'routine' else ([tier] if tier != 'all' else ['small', 'medium', 'large'])
    for t in tiers:
        count = 240 if lane == 'routine' else {'small': 100000, 'medium': 5000000, 'large': 60000000}[t]
        for fmt in ['glb', 'stl', 'ply-mesh-le', 'ply-mesh-be', 'ply-points-le', 'ply-points-be']:
            suffix = 'ply' if fmt.startswith('ply') else fmt
            name = f'A-{t}-{fmt}.{suffix}'
            points = 'points' in fmt
            if fmt == 'glb':
                target = 0 if lane == 'routine' else {'small': 8*2**20, 'medium': 350*2**20, 'large': 2**32-65536}[t]
                glb(output/name, count, 2000 if t == 'medium' else 8, target)
            elif fmt == 'stl':
                stl(output/name, count)
            else:
                ply(output/name, count, points, '>' if fmt.endswith('be') else '<')
            expected = {'triangles': 0 if points else count, 'points': count if points else 0,
                        'sourceVertices': count if points else count*3, 'bounds': [[0,0,0],[11,11,11]],
                        'components': 8, 'materials': 4 if fmt == 'glb' else 0,
                        'vertexColors': 'RGBA8 four groups' if fmt == 'glb' else ('RGB8 four groups' if suffix == 'ply' else 'none'),
                        'textures': [{'width': 2, 'height': 2, 'rgba8': [255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255]}] if fmt == 'glb' else [],
                        'materialFactors': {'baseColors': COLORS, 'metallic': 0.25, 'roughness': 0.75, 'doubleSided': True} if fmt == 'glb' else None,
                        'proxy': {'firstMinComponents': 8, 'firstMinPrimitives': 8,
                                  'completeMinComponents': 8, 'boundsVerified': True,
                                  'coarseMaxPrimitives': max(1, min(2000000, count//20)),
                                  'thresholdApplies': 'large proxy qualification after TSK-206'}}
            add(name, expected, {'algorithm': fmt, 'primitives': count, 'tier': t, 'scaled': lane == 'routine'})

    if lane == 'routine' or tier in ['small', 'medium', 'all']:
        count = 240 if lane == 'routine' else (100000 if tier == 'small' else 5000000)
        glb(output/'Pressure.glb', count)
        add('Pressure.glb', {'triangles': count, 'bounds': [[0,0,0],[11,11,11]], 'materials': 4,
                             'pressure': 'DXGI reduction injected by TSK-207/302; fixture alone does not lower the budget'},
            {'algorithm': 'glb', 'primitives': count})
        shutil.copyfile(ASSETS/'tri_external.bin', output/'approved.bin')
        original = json.loads((ASSETS/'tri_external.gltf').read_text())
        for label, uri in [('approved', 'approved.bin'), ('missing', 'missing.bin'), ('traversal', '../outside.bin'),
                           ('encoded-traversal', '%2e%2e/outside.bin'), ('absolute', 'C:/outside.bin'),
                           ('network', 'https://example.invalid/mesh.bin'), ('unc', '//server/share/mesh.bin'),
                           ('ads', 'approved.bin:stream')]:
            original['buffers'][0]['uri'] = uri
            name = f'sidecar-{label}.gltf'
            (output/name).write_bytes(canonical(original))
            add(name, {'triangles': 1, 'bounds': [[0,0,0],[1,1,0]], 'policy': 'accept' if label == 'approved' else 'reject'},
                {'algorithm': 'external glTF URI mutation', 'uri': uri})
        add('approved.bin', {'role': 'broker-approved local sidecar'}, {'algorithm': 'copy tri_external.bin'})
        # Sparse/nonlocal accessor fixture: zero base, reverse-index sparse values.
        sparse = json.loads((ASSETS/'tri_external.gltf').read_text())
        binary = bytes([2,1,0,0]) + struct.pack('<9f', 0,1,0, 1,0,0, 0,0,0)
        sparse['buffers'] = [{'uri': 'sparse.bin', 'byteLength': len(binary)}]
        sparse['bufferViews'] = [{'buffer':0,'byteOffset':0,'byteLength':3}, {'buffer':0,'byteOffset':4,'byteLength':36}]
        sparse['accessors'] = [{'componentType':5126,'count':3,'type':'VEC3', 'min':[0,0,0],'max':[1,1,0],
                                'sparse': {'count':3,'indices':{'bufferView':0,'componentType':5121}, 'values':{'bufferView':1}}}]
        sparse['meshes'][0]['primitives'][0] = {'attributes': {'POSITION':0}}
        # glTF sparse indices must be increasing: this variant is intentionally malformed.
        (output/'sparse-invalid.gltf').write_bytes(canonical(sparse))
        (output/'sparse.bin').write_bytes(binary)
        add('sparse-invalid.gltf', {'policy':'reject', 'reason':'non-increasing sparse indices'}, {'algorithm':'sparse reverse-index mutation'})
        add('sparse.bin', {'role':'sparse sidecar'}, {'algorithm':'explicit 40-byte sparse data'})
        sparse['buffers'][0]['uri'] = 'sparse-valid.bin'
        valid_binary = bytes([0,1,2,0]) + struct.pack('<9f', 0,0,0, 1,0,0, 0,1,0)
        (output/'sparse-valid.bin').write_bytes(valid_binary)
        (output/'sparse-valid.gltf').write_bytes(canonical(sparse))
        add('sparse-valid.gltf', {'policy':'accept','triangles':1,'bounds':[[0,0,0],[1,1,0]]}, {'algorithm':'zero-base sparse accessor with increasing indices; non-indexed triangle'})
        add('sparse-valid.bin', {'role':'sparse sidecar'}, {'algorithm':'explicit 40-byte valid sparse data'})
        # Many real instances; geometry payload stays small.
        source = (ASSETS/'tri_tight.glb').read_bytes()
        json_len = struct.unpack_from('<I', source, 12)[0]
        doc = json.loads(source[20:20+json_len])
        primitive = doc['meshes'][0]['primitives'][0]
        doc['meshes'] = [{'primitives':[dict(primitive,material=i)]} for i in range(128)]
        doc['materials'] = [{'pbrMetallicRoughness':{'baseColorFactor':[(i%16)/15,((i//16)%8)/7,0.5,1],
                            'metallicFactor':0,'roughnessFactor':0.75}} for i in range(128)]
        doc['nodes'] = [{'mesh':i%128, 'translation':[i % 32, i//32, 0]} for i in range(2048)]
        doc['scenes'] = [{'nodes':list(range(2048))}]
        raw = json.dumps(doc, sort_keys=True, separators=(',', ':')).encode()
        raw += b' ' * (-len(raw) % 4)
        tail = source[20+json_len:]
        (output/'Draw-heavy.glb').write_bytes(struct.pack('<5I',0x46546c67,2,20+len(raw)+len(tail),len(raw),0x4e4f534a)+raw+tail)
        add('Draw-heavy.glb', {'uniquePayloadTriangles':1,'sourceTriangles':128,'triangles':2048,'instances':2048,
                              'materials':128,'bounds':[[0,0,0],[32,64,0]],'policy':'accept after TSK-205 lifts the current 1024-chunk catalog cap'},
            {'algorithm':'32x64 translated instances; 128 meshes/material groups share tri_tight.glb accessors'})
        for name in ['draco_triangle.glb','draco_position_only.glb','basisu_textured_triangle.glb','basisu_corrupt_ktx2.glb']:
            shutil.copyfile(ASSETS/name, output/name)
            add(name, {'triangles':1, 'bounds':[[0,0,0],[1,1,0]],
                       'feature': 'Draco' if 'draco' in name else 'KTX2/Basis',
                       'texture': {'width':8,'height':8,'encoding':'BC7 or RGBA8 fallback'} if 'textured' in name else None,
                       'policy':'geometry with optional texture fallback' if 'corrupt' in name else 'accept'},
                {'algorithm':'frozen encoded seed', 'source':name, 'regeneration':'interactive-viewer/tools/build-gen-glbs-draco.ps1 or build-gen-glbs-ktx2.ps1; new encoder output requires explicit manifest review'})
        meshopt(output/'meshopt.glb')
        add('meshopt.glb', {'triangles':1,'bounds':[[0,0,0],[1,1,0]],'feature':'EXT_meshopt_compression','policy':'accept'},
            {'algorithm':'pinned meshoptimizer vertex encoder', 'vertices':3, 'stride':12})
        # Valid RIFF WebP lossless 1x1 transparent seed, no external codec required.
        import base64
        webp = base64.b64decode('UklGRhoAAABXRUJQVlA4TA0AAAAvAAAAEAcQERGIiP4HAA==')
        (output/'sample.webp').write_bytes(webp)
        textured = json.loads((ASSETS/'tri_external.gltf').read_text())
        textured['buffers'][0]['uri'] = 'approved.bin'
        textured['extensionsUsed'] = ['EXT_texture_webp']
        textured['extensionsRequired'] = ['EXT_texture_webp']
        textured['images'] = [{'uri':'sample.webp','mimeType':'image/webp'}]
        textured['textures'] = [{'extensions':{'EXT_texture_webp':{'source':0}}}]
        textured['materials'] = [{'pbrMetallicRoughness':{'baseColorTexture':{'index':0}}}]
        textured['meshes'][0]['primitives'][0]['material'] = 0
        (output/'webp.gltf').write_bytes(canonical(textured))
        add('sample.webp', {'width':1,'height':1,'role':'WebP sidecar'}, {'algorithm':'frozen lossless WebP seed'})
        add('webp.gltf', {'triangles':1,'feature':'EXT_texture_webp','policy':'accept'}, {'algorithm':'WebP required texture glTF'})
        malformed = {'truncated.glb': b'glTF', 'over-limit.stl': bytes(80)+struct.pack('<I',0xffffffff),
                     'truncated.stl': bytes(80)+struct.pack('<I',8),
                     'over-limit.ply': b'ply\nformat binary_little_endian 1.0\nelement vertex 18446744073709551615\nproperty float x\nproperty float y\nproperty float z\nend_header\n',
                     'empty.ply': b'ply\nformat binary_little_endian 1.0\nelement vertex 0\nproperty float x\nproperty float y\nproperty float z\nend_header\n'}
        for name, data in malformed.items():
            (output/name).write_bytes(data)
            add(name, {'policy':'reject','reason':name.split('.')[0]}, {'algorithm':'explicit malformed header'})

    if lane == 'qualification' and tier in ['large','all']:
        # Nonlocal traversal of position/color/index views in an approximately
        # 2 GiB brokered BIN, without retaining the BIN in the generator heap.
        temporary = output/'adversarial-generation.glb'
        glb(temporary,35791392)
        with temporary.open('rb') as source:
            header = source.read(20)
            json_length = struct.unpack_from('<I',header,12)[0]
            doc = json.loads(source.read(json_length))
            bin_length,kind = struct.unpack('<2I',source.read(8))
            assert kind == 0x004e4942
            with (output/'A-adversarial-layout.bin').open('wb') as target:
                shutil.copyfileobj(source,target,1024*1024)
        temporary.unlink()
        doc['buffers'][0]['uri'] = 'A-adversarial-layout.bin'
        doc['meshes'].reverse()
        (output/'A-adversarial-layout.gltf').write_bytes(canonical(doc))
        add('A-adversarial-layout.bin', {'role':'approximately 2 GiB nonlocal sidecar','bytes':bin_length},
            {'algorithm':'GLB BIN externalization with 1 MiB copy windows','triangles':35791392})
        add('A-adversarial-layout.gltf', {'triangles':35791392,'bounds':[[0,0,0],[11,11,11]],'components':8},
            {'algorithm':'reverse mesh traversal against ascending BIN views','sparseCompanion':'routine sparse-valid.gltf and sparse-invalid.gltf'})

    manifest = {'schema':1,'recipeVersion':1,'lane':lane,'tier':tier,'entries':entries}
    (output/'manifest.json').write_bytes(canonical(manifest))
    # Generated, immutable projection for Catch2 (JSON remains the full contract).
    if lane == 'routine':
        lines = ['// Generated by tests/fixtures/generate.py; do not edit.', '#pragma once',
                 'namespace fixture_manifest {',
                 'struct Entry { const wchar_t* path; const char* sha256; unsigned triangles; unsigned points; };',
                 'inline constexpr Entry geometry[] = {']
        for entry in entries[:6]:
            exp = entry['expected']
            lines.append(f'    {{ L"corpus/{entry["path"]}", "{entry["sha256"]}", {exp["triangles"]}, {exp["points"]} }},')
        lines.extend(['};','struct File { const wchar_t* path; const char* sha256; };', 'inline constexpr File files[] = {'])
        for entry in entries:
            lines.append(f'    {{ L"corpus/{entry["path"]}", "{entry["sha256"]}" }},')
        lines.append(f'    {{ L"corpus/manifest.json", "{digest(output/"manifest.json")}" }},')
        lines.extend(['};','} // namespace fixture_manifest',''])
        (output/'Expectations.h').write_bytes('\n'.join(lines).encode())
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--lane', choices=['routine','qualification'], default='routine')
    parser.add_argument('--tier', choices=['small','medium','large','all'], default='small')
    parser.add_argument('--compare', type=Path, help='Fail unless complete manifest matches this immutable manifest')
    args = parser.parse_args()
    manifest = generate(args.output, args.lane, args.tier)
    if args.compare and canonical(manifest) != args.compare.read_bytes():
        raise SystemExit('Fixture manifest differs; inspect bytes/metadata/recipe before updating the checked-in manifest')
    print(f'{len(manifest["entries"])} fixtures; manifest SHA-256 {digest(args.output/"manifest.json")}')


if __name__ == '__main__':
    main()
