"""Verify the immutable STEP/STP qualification corpus.

The committed `.stp` files are the durable artifact; the XDE writer embeds a
timestamp, so they are never regenerated implicitly. This script only hashes
the committed fixtures and re-derives the adversarial cases in memory, checking
each pinned SHA-256. Use `--output` to materialize the derived cases for a
manual run and `--print-derived-hashes` to record new ones.
"""

import argparse
import hashlib
import json
import re
from pathlib import Path


HERE = Path(__file__).resolve().parent


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def replace_once(data: bytes, old: bytes, new: bytes) -> bytes:
    if data.count(old) != 1:
        raise ValueError(f'expected exactly one occurrence of {old!r}')
    return data.replace(old, new, 1)


def insert_before_data_end(data: bytes, record: bytes) -> bytes:
    data_start = data.index(b'DATA;')
    endsec = data.index(b'ENDSEC;', data_start)
    return data[:endsec] + record + data[endsec:]


def derive(operation: str, data: bytes) -> bytes:
    if operation == 'empty':
        return b''
    if operation == 'truncate-half':
        return data[:len(data) // 2]
    if operation == 'truncate-terminator':
        marker = b'END-ISO-10303-21;'
        index = data.rfind(marker)
        if index < 0:
            raise ValueError('missing Part-21 terminator')
        return data[:index]
    if operation == 'nul-append':
        return data + b'\x00'
    if operation == 'utf16-bom':
        return b'\xff\xfe' + data
    if operation == 'zip-signature':
        return b'PK\x03\x04' + data
    if operation == 'not-part21':
        return data.replace(b'ISO-10303-21;', b'ISO-10303-22;', 1)
    if operation == 'external-file-population':
        declaration = b"\nFILE_POPULATION(('AP214'),('x'),($),$);\n"
        return replace_once(data, b'HEADER;\n', b'HEADER;\n' + declaration)
    if operation == 'external-document-relative':
        declaration = b"\nDOCUMENT_FILE('sub/other.stp',('AUTOMOTIVE_DESIGN'),'x',#1);\n"
        return replace_once(data, b'HEADER;\n', b'HEADER;\n' + declaration)
    if operation == 'duplicate-entity':
        match = re.search(rb'#\d+\s*=[^;]*;', data)
        if not match:
            raise ValueError('no entity record to duplicate')
        record = match.group(0)
        return insert_before_data_end(data, record + b'\n')
    if operation == 'unterminated-string':
        return insert_before_data_end(data, b"#999999=PRODUCT('oops;\n")
    if operation == 'unterminated-comment':
        return insert_before_data_end(data, b'/* oops\n')
    raise ValueError(f'unknown operation: {operation}')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        help='materialize the derived adversarial cases here')
    parser.add_argument('--print-derived-hashes', action='store_true')
    parser.add_argument('--require-manual-corpus', action='store_true',
                        help='fail if the local manual large-file corpus is absent')
    args = parser.parse_args()
    manifest = json.loads((HERE / 'manifest.json').read_text(encoding='utf-8'))
    assert manifest['schema'] == 1
    source_dir = (HERE / manifest['sourceDirectory']).resolve()

    decoded = {}
    for entry in manifest['entries']:
        payload = (source_dir / entry['path']).read_bytes()
        assert len(payload) == entry['bytes'], entry['path']
        assert digest(payload) == entry['sha256'], entry['path']
        decoded[entry['path']] = payload

    derived_hashes = {}
    for entry in manifest['derived']:
        payload = derive(entry['operation'], decoded[entry['base']])
        actual = digest(payload)
        derived_hashes[entry['path']] = actual
        if entry['sha256'] != 'TO_BE_RECORDED':
            assert actual == entry['sha256'], entry['path']
        if args.output:
            target = args.output / entry['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(payload)
    if args.print_derived_hashes:
        print(json.dumps(derived_hashes, indent=2, sort_keys=True))

    manual_checked = 0
    for entry in manifest.get('manualCorpus', []):
        source = (HERE.parents[2] / entry['path']).resolve()
        if not source.is_file():
            if args.require_manual_corpus:
                raise SystemExit(f'manual corpus missing: {source}')
            continue
        assert source.stat().st_size == entry['bytes'], entry['path']
        assert digest(source.read_bytes()) == entry['sha256'], entry['path']
        manual_checked += 1

    print(f"verified {len(manifest['entries'])} sources, "
          f"{len(manifest['derived'])} derived cases, "
          f"{manual_checked} manual corpus item(s)")


if __name__ == '__main__':
    main()
