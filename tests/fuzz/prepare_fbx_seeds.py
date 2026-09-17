"""Materialize bounded libFuzzer envelopes from the immutable FBX corpus."""

import argparse
import base64
import json
from pathlib import Path
import struct


MAGIC = 0x5A584246
EVALUATE = 1 << 2
PROTOCOL = 1 << 0


def envelope(primary: bytes, flags: int = EVALUATE, path: bytes = b'', sidecar: bytes = b'') -> bytes:
    return struct.pack('<IBBHI', MAGIC, flags, 0, len(path), len(primary)) + primary + path + sidecar


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    manifest_path = root / 'tests/fixtures/fbx/manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    sources = (manifest_path.parent / manifest['sourceDirectory']).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    selected = {
        'cube-binary.fbx.base64', 'hierarchy-instances-pivots-ascii.fbx',
        'combined-skin-blend-ascii.fbx', 'embedded-png-ascii.fbx',
        'nurbs-only-ascii.fbx', 'nested-hierarchy-binary.fbx.base64',
    }
    count = 0
    for entry in manifest['entries']:
        if entry['path'] not in selected:
            continue
        payload = (sources / entry['path']).read_bytes()
        if entry['path'].endswith('.base64'):
            payload = base64.b64decode(payload)
        name = entry['path'].removesuffix('.base64') + '.seed'
        (args.output / name).write_bytes(envelope(payload))
        count += 1
    # Header-sized and malformed protocol records drive the copy/validate path.
    (args.output / 'protocol-empty.seed').write_bytes(envelope(b'', PROTOCOL))
    (args.output / 'protocol-header.seed').write_bytes(
        envelope(struct.pack('<IIQQIIQ48x', 0x50334457, 10, 0, 88, 0, 0, 0), PROTOCOL))
    print(f'materialized {count + 2} FBX/protocol fuzz seeds in {args.output}')


if __name__ == '__main__':
    main()
