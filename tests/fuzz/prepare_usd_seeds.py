"""Materialize bounded multi-domain libFuzzer seeds from the USD corpus."""

import argparse
import base64
import json
from pathlib import Path
import struct


MAGIC = 0x5A465355
OBJECT_GRAPH = 0
USDZ_DIRECTORY = 1
NORMALIZED_OUTPUT = 2
CONTROL_FRAME = 3
DEPENDENCY_IDENTIFIER = 4


def envelope(domain: int, payload: bytes, flags: int = 0) -> bytes:
    return struct.pack('<IBBH', MAGIC, domain, flags, 0) + payload


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    manifest_path = root / 'tests/fixtures/usd/manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    sources = (manifest_path.parent / manifest['sourceDirectory']).resolve()
    if args.output.exists() and any(args.output.iterdir()):
        raise SystemExit(
            f'refusing non-empty seed directory: {args.output}; use a new path')
    args.output.mkdir(parents=True, exist_ok=True)
    count = 0
    for entry in manifest['entries']:
        if entry['path'] not in {
            'mesh.usda', 'static-scene.usda', 'materials.usda',
            'cube.usdc.base64', 'cube.usdz.base64',
            'compat-composition.usda', 'compat-point-instancer.usda',
        }:
            continue
        payload = (sources / entry['path']).read_bytes()
        if entry['path'].endswith('.base64'):
            payload = base64.b64decode(payload)
        domain = USDZ_DIRECTORY if entry['path'] == 'cube.usdz.base64' else OBJECT_GRAPH
        name = entry.get('decodedPath', entry['path'].removesuffix('.base64')) + '.seed'
        (args.output / name).write_bytes(envelope(domain, payload))
        count += 1

    # Mutated normalized-section and host/resolver control frames enter the
    # exact production decoders without needing a GPU or child process.
    section_header = struct.pack('<IIQQIIQ48x', 0x50334457, 10, 0, 88, 0, 0, 0)
    (args.output / 'normalized-header.seed').write_bytes(
        envelope(NORMALIZED_OUTPUT, section_header))
    openusd_request = struct.pack('<IIQQQQIIQ', 18, 48, 1, 0, 0, 0, 4096, 0, 0)
    (args.output / 'openusd-control.seed').write_bytes(
        envelope(CONTROL_FRAME, openusd_request))
    sidecar = struct.pack('<IIQII200s', 9, 216, 1, 16, 0,
                          b'compat-ref.usda'.ljust(200, b'\0'))
    (args.output / 'resolver-control.seed').write_bytes(
        envelope(CONTROL_FRAME, sidecar))
    (args.output / 'resolver-identifier.seed').write_bytes(
        envelope(DEPENDENCY_IDENTIFIER,
                 b'../outside.usda\0preview3d://layers/root.usda'))
    print(f'materialized {count + 4} USD fuzz seeds in {args.output}')


if __name__ == '__main__':
    main()
