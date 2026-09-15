"""Independently scan small fixture bytes against the immutable JSON contract."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib


def glb(data, expected):
    magic,version,total,json_size,kind = struct.unpack_from('<5I',data)
    assert (magic,version,total,kind)==(0x46546c67,2,len(data),0x4e4f534a)
    doc = json.loads(data[20:20+json_size])
    size,kind = struct.unpack_from('<2I',data,20+json_size)
    binary = memoryview(data)[28+json_size:]
    assert kind==0x004e4942 and size==len(binary)==doc['buffers'][0]['byteLength']
    positions,colors,triangles = [],set(),0
    for mesh in doc['meshes']:
        for primitive in mesh['primitives']:
            accessor = doc['accessors'][primitive['attributes']['POSITION']]
            view = doc['bufferViews'][accessor['bufferView']]
            offset = view.get('byteOffset',0)+accessor.get('byteOffset',0)
            positions.extend(struct.iter_unpack('<3f',binary[offset:offset+accessor['count']*12]))
            triangles += doc['accessors'][primitive['indices']]['count']//3
            accessor = doc['accessors'][primitive['attributes']['COLOR_0']]
            view = doc['bufferViews'][accessor['bufferView']]
            offset = view.get('byteOffset',0)
            colors.update(struct.iter_unpack('4B',binary[offset:offset+accessor['count']*4]))
    factors = expected['materialFactors']
    assert len(doc['materials'])==expected['materials']
    assert [m['pbrMetallicRoughness']['baseColorFactor'] for m in doc['materials']]==factors['baseColors']
    for material in doc['materials']:
        assert material['pbrMetallicRoughness']['metallicFactor']==factors['metallic']
        assert material['pbrMetallicRoughness']['roughnessFactor']==factors['roughness']
        assert material['doubleSided']==factors['doubleSided']
    assert colors=={tuple(int(c*255) for c in color) for color in factors['baseColors']}
    image = doc['images'][0]
    view = doc['bufferViews'][image['bufferView']]
    raw = binary[view['byteOffset']:view['byteOffset']+view['byteLength']]
    assert raw[:8]==b'\x89PNG\r\n\x1a\n'
    at,encoded = 8,b''
    while at<len(raw):
        n = struct.unpack_from('>I',raw,at)[0]
        kind = bytes(raw[at+4:at+8])
        chunk = bytes(raw[at+8:at+8+n])
        assert zlib.crc32(kind+chunk)==struct.unpack_from('>I',raw,at+8+n)[0]
        if kind==b'IHDR':
            width,height,depth,color,_,_,_ = struct.unpack('>IIBBBBB',chunk)
            assert (width,height,depth,color)==(2,2,8,6)
        if kind==b'IDAT': encoded += chunk
        at += n+12
    scanlines = zlib.decompress(encoded)
    assert scanlines[0]==scanlines[9]==0
    assert list(scanlines[1:9]+scanlines[10:18])==expected['textures'][0]['rgba8']
    return positions,triangles,0


def stl(data):
    count = struct.unpack_from('<I',data,80)[0]
    assert len(data)==84+count*50
    positions = []
    for record in struct.iter_unpack('<12fH',data[84:]):
        positions.extend([record[3:6],record[6:9],record[9:12]])
    return positions,count,0


def ply(data):
    header,body = data.split(b'end_header\n',1)
    lines = header.decode('ascii').splitlines()
    endian = '<' if 'little' in lines[1] else '>'
    vertices = int(next(line.split()[2] for line in lines if line.startswith('element vertex ')))
    faces = next((int(line.split()[2]) for line in lines if line.startswith('element face ')),0)
    stride = 15 if faces else 48
    positions = [struct.unpack_from(endian+'3f',body,i*stride) for i in range(vertices)]
    colors = {struct.unpack_from('3B',body,i*stride+12) for i in range(vertices)}
    assert colors=={(255,0,0),(0,255,0),(0,0,255),(255,255,255)}
    assert len(body)==vertices*stride+faces*13
    for record in struct.iter_unpack(endian+'B3I',body[vertices*stride:]):
        assert record[0]==3 and max(record[1:])<vertices
    return positions,faces,vertices if not faces else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('corpus',type=Path)
    parser.add_argument('--max-bytes',type=int,default=64*1024*1024,
                        help='Independent small scanner refuses larger inputs; use the streaming qualification pipeline for those')
    args = parser.parse_args()
    manifest = json.loads((args.corpus/'manifest.json').read_text())
    for entry in manifest['entries']:
        path = args.corpus/entry['path']
        assert path.stat().st_size==entry['bytes'],entry['path']
        with path.open('rb') as stream:
            assert hashlib.file_digest(stream,'sha256').hexdigest()==entry['sha256'],entry['path']
    for entry in manifest['entries']:
        if not entry['id'].startswith('A-') or 'sourceVertices' not in entry['expected']:
            continue
        assert entry['bytes']<=args.max_bytes, 'small metadata scanner byte cap exceeded'
        data = (args.corpus/entry['path']).read_bytes()
        expected = entry['expected']
        if entry['path'].endswith('.glb'): positions,triangles,points = glb(data,expected)
        elif entry['path'].endswith('.stl'): positions,triangles,points = stl(data)
        else: positions,triangles,points = ply(data)
        assert (triangles,points)==(expected['triangles'],expected['points'])
        assert len(positions)==expected['sourceVertices']
        bounds = [[min(p[i] for p in positions) for i in range(3)], [max(p[i] for p in positions) for i in range(3)]]
        assert bounds==expected['bounds'],entry['path']
        components = {sum((1<<i) if p[i]>=10 else 0 for i in range(3)) for p in positions}
        assert len(components)==expected['components']
        assert expected['proxy']['firstMinComponents']<=len(components)
        print(f'{entry["id"]}: {triangles} triangles, {points} points, bounds {bounds}')


if __name__=='__main__':
    main()
