"""Materialize immutable 3MF packages as standalone libFuzzer seeds."""

import argparse
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'fixtures' / '3mf'))
from verify import HERE, derive, source_bytes, verify  # noqa: E402
import json  # noqa: E402


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists() and any(args.output.iterdir()):
        raise SystemExit(f'refusing non-empty seed directory: {args.output}')
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((HERE / 'manifest.json').read_text(encoding='utf-8'))
    source_dir = (HERE / manifest['sourceDirectory']).resolve()
    sources = {}
    count = 0
    for entry in manifest['entries']:
        data = source_bytes(source_dir, entry['path'])
        verify(entry, data)
        sources[entry['path']] = data
        (args.output / (entry['path'].removesuffix('.base64') + '.seed')).write_bytes(
            struct.pack('<IBBH', 0x5a464d33, 0, 0, 0) + data)
        count += 1
    for entry in manifest['derived']:
        data = derive(entry['operation'], sources[entry['base']])
        verify(entry, data)
        (args.output / (entry['path'] + '.seed')).write_bytes(
            struct.pack('<IBBH', 0x5a464d33, 0, 0, 0) + data)
        count += 1
    print(f'materialized {count} 3MF seeds in {args.output}')


if __name__ == '__main__':
    main()
