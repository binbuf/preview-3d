"""TSK-206 deterministic component/source reorder corpus; bounded generation."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location('recipes', Path(__file__).with_name('generate.py'))
recipes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recipes)


def generate(directory):
    directory.mkdir(parents=True, exist_ok=True)
    entries = []
    for fmt in ['glb', 'stl', 'mesh-le', 'mesh-be', 'points-le', 'points-be']:
        suffix = fmt if fmt in ['glb', 'stl'] else 'ply'
        original = directory / f'{fmt}.{suffix}'
        reordered = directory / f'{fmt}-reordered.{suffix}'
        if fmt == 'glb':
            recipes.glb(original, 100000)
            data = original.read_bytes()
            length = struct.unpack_from('<I', data, 12)[0]
            doc = json.loads(data[20:20+length])
            doc['scenes'][0]['nodes'] = [7, 3, 5, 1, 6, 2, 4, 0]
            raw = json.dumps(doc, sort_keys=True, separators=(',', ':')).encode()
            raw += b' ' * (-len(raw) % 4)
            tail = data[20+length:]
            reordered.write_bytes(struct.pack('<5I', 0x46546c67, 2, 20+len(raw)+len(tail), len(raw), 0x4e4f534a) + raw + tail)
        elif fmt == 'stl':
            recipes.stl(original, 100000)
            # Reverse component blocks, preserving valid facets and all boundaries.
            data = original.read_bytes()
            block = 12500*50
            reordered.write_bytes(data[:84] + b''.join(data[84+i*block:84+(i+1)*block] for i in reversed(range(8))))
        else:
            points = fmt.startswith('points')
            endian = '>' if fmt.endswith('be') else '<'
            recipes.ply(original, 100000, points, endian)
            data = original.read_bytes()
            at = data.index(b'end_header\n') + len(b'end_header\n')
            if points:
                block = 12500*48
                reordered.write_bytes(data[:at] + b''.join(data[at+i*block:at+(i+1)*block] for i in reversed(range(8))))
            else:
                face_at = at+300000*15
                faces = data[face_at:]
                reordered.write_bytes(data[:face_at]+b''.join(faces[i:i+13] for i in range(len(faces)-13, -1, -13)))
        for path in [original, reordered]:
            entries.append({'path': path.name, 'sha256': recipes.digest(path), 'bytes': path.stat().st_size,
                            'expected': {'validPrimitives': 100000, 'firstMinComponents': 8,
                                         'completeMinComponents': 8, 'coarseMaxPrimitives': 5000,
                                         'gpuGeometryMaxBytes': 64*1024*1024,
                                         'bounds': [[0, 0, 0], [11, 11, 11]]}})
    manifest = {'recipe': 'coarse.py v1; 100000 primitives; eight separated components; no random seed', 'entries': entries}
    (directory/'manifest.json').write_bytes(recipes.canonical(manifest))
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    generate(parser.parse_args().output)
