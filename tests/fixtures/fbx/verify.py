"""Verify and optionally materialize the immutable FBX qualification corpus."""

import argparse
import base64
import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def source_bytes(source_dir: Path, name: str) -> bytes:
    data = (source_dir / name).read_bytes()
    return base64.b64decode(data) if name.endswith('.base64') else data


def replace_once(data: bytes, old: bytes, new: bytes) -> bytes:
    if data.count(old) != 1:
        raise ValueError(f'expected exactly one occurrence of {old!r}')
    return data.replace(old, new, 1)


def external_texture(data: bytes, path: bytes) -> bytes:
    content = data.index(b'\t\tContent: ,')
    end = data.index(b'\n\t}', content)
    data = data[:content] + data[end:]
    return data.replace(b'textures\\tiny_clouds.png', path).replace(
        b'D:/Dev/clean/ufbx/data/textures/tiny_clouds.png', path)


def derive(operation: str, data: bytes) -> bytes:
    if operation == 'empty':
        return b''
    if operation == 'truncate-half':
        return data[:len(data) // 2]
    if operation == 'malformed-header':
        return b'Kaydara FBX Binary  \\x00\\x1a\\x00broken'
    if operation == 'nonfinite-position':
        return replace_once(data, b'a: -0.5,-0.5,0.5', b'a: 1.#INF,-0.5,0.5')
    if operation == 'adversarial-depth':
        data = replace_once(data, b'Definitions:  {\n\tVersion: 100\n\tCount: 9',
                            b'Definitions:  {\n\tVersion: 100\n\tCount: 269')
        data = replace_once(data, b'\tObjectType: "Model" {\n\t\tCount: 3',
                            b'\tObjectType: "Model" {\n\t\tCount: 263')
        models, connections, parent = bytearray(), bytearray(), 0
        for index in range(260):
            identity = 8_000_000 + index
            models += (f'\tModel: {identity}, "Model::deep{index}", "Null" {{\n'
                       '\t\tVersion: 232\n\t}\n').encode()
            connections += f'\tC: "OO",{identity},{parent}\n'.encode()
            parent = identity
        data = replace_once(data, b'\n}\n\n; Object connections',
                            b'\n' + models + b'}\n\n; Object connections')
        return replace_once(data, b'\n}\n;Takes section',
                            b'\n' + connections + b'}\n;Takes section')
    if operation == 'allocation-pressure':
        return replace_once(data, b'\t\tVertices: *24 {', b'\t\tVertices: *12000000 {')
    if operation == 'external-approved':
        return external_texture(data, b'textures\\gray.png')
    if operation == 'external-missing':
        return external_texture(data, b'textures\\missing.png')
    if operation == 'external-unsafe':
        return external_texture(data, b'..\\..\\secret.png')
    raise ValueError(f'unknown operation: {operation}')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        help='materialize decoded and derived .fbx files here')
    parser.add_argument('--print-derived-hashes', action='store_true')
    args = parser.parse_args()
    manifest = json.loads((HERE / 'manifest.json').read_text(encoding='utf-8'))
    source_dir = (HERE / manifest['sourceDirectory']).resolve()
    decoded = {}
    for entry in manifest['entries']:
        payload = source_bytes(source_dir, entry['path'])
        assert len(payload) == entry['bytes'], entry['path']
        assert digest(payload) == entry['sha256'], entry['path']
        decoded[entry['path']] = payload
        if args.output:
            name = entry['path'].removesuffix('.base64')
            target = args.output / 'valid' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(payload)
    derived_hashes = {}
    for entry in manifest['derived']:
        payload = derive(entry['operation'], decoded[entry['base']])
        actual = digest(payload)
        derived_hashes[entry['path']] = actual
        if entry['sha256'] != 'TO_BE_RECORDED':
            assert actual == entry['sha256'], entry['path']
        if args.output:
            target = args.output / 'derived' / entry['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(payload)
    if args.output:
        sidecar = args.output / 'derived' / 'textures' / 'gray.png'
        sidecar.parent.mkdir(parents=True, exist_ok=True)
        sidecar.write_bytes((ROOT / 'interactive-viewer/test-assets/textures/gray.png').read_bytes())
    if args.print_derived_hashes:
        print(json.dumps(derived_hashes, indent=2, sort_keys=True))
    print(f"verified {len(manifest['entries'])} sources and {len(manifest['derived'])} derived cases")


if __name__ == '__main__':
    main()
