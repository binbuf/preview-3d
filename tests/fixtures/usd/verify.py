"""Verify and optionally materialize the immutable USD qualification corpus."""

import argparse
import base64
import hashlib
import json
from pathlib import Path


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


def derive(operation: str, data: bytes) -> bytes:
    if operation == 'empty':
        return b''
    if operation == 'truncate-half':
        return data[:len(data) // 2]
    if operation == 'usdc-allocation-pressure':
        mutated = bytearray(data)
        for offset, expected, replacement in (
            (753, 27, 241), (1198, 210, 186),
            (1491, 58, 0), (1492, 116, 0), (1493, 114, 0), (1494, 97, 0),
            (2416, 0, 8), (2570, 222, 206), (2833, 54, 50),
        ):
            if mutated[offset] != expected:
                raise ValueError(f'unexpected USDC byte at {offset}')
            mutated[offset] = replacement
        return bytes(mutated)
    if operation == 'truncate-tail':
        return data[:-8]
    if operation == 'unsafe-archive-path':
        if data.count(b'cube.usdc') != 2:
            raise ValueError('expected local and central cube.usdc names')
        return data.replace(b'cube.usdc', b'../x.usdc')
    if operation == 'invalid-axis':
        return replace_once(data, b'upAxis = "X"', b'upAxis = "Q"')
    if operation == 'nonfinite-point':
        return replace_once(data, b'(-1, -1, 0)', b'(inf, -1, 0)')
    if operation == 'unsupported-subdivision':
        return replace_once(data, b'subdivisionScheme = "none"',
                            b'subdivisionScheme = "catmullClark"')
    if operation == 'unsafe-reference':
        return replace_once(data, b'@compat-ref.usda@', b'@../outside.usda@')
    if operation == 'remote-reference':
        return replace_once(data, b'@compat-ref.usda@', b'@https://invalid/x@')
    if operation == 'recursive-sublayer':
        return replace_once(data, b'@compat-sub.usda@', b'@compat-composition.usda@')
    if operation == 'dependency-pressure':
        references = b', '.join(b'@compat-ref.usda@' for _ in range(65))
        return replace_once(data, b'subLayers = [@compat-sub.usda@]',
                            b'subLayers = [' + references + b']')
    raise ValueError(f'unknown operation: {operation}')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        help='materialize decoded and derived files here')
    parser.add_argument('--print-derived-hashes', action='store_true')
    args = parser.parse_args()
    manifest = json.loads((HERE / 'manifest.json').read_text(encoding='utf-8'))
    assert manifest['schema'] == 1
    source_dir = (HERE / manifest['sourceDirectory']).resolve()
    decoded = {}
    for entry in manifest['entries']:
        payload = source_bytes(source_dir, entry['path'])
        assert len(payload) == entry['bytes'], entry['path']
        assert digest(payload) == entry['sha256'], entry['path']
        decoded[entry['path']] = payload
        if args.output:
            target = args.output / 'valid' / entry.get(
                'decodedPath', entry['path'].removesuffix('.base64'))
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
    if args.print_derived_hashes:
        print(json.dumps(derived_hashes, indent=2, sort_keys=True))
    print(f"verified {len(manifest['entries'])} sources and "
          f"{len(manifest['derived'])} derived cases")


if __name__ == '__main__':
    main()
