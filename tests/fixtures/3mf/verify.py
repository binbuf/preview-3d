"""Verify and optionally materialize the frozen 3MF-007 qualification corpus."""

import argparse
import base64
import hashlib
import io
import json
from pathlib import Path
import struct
import zipfile


HERE = Path(__file__).resolve().parent


def source_bytes(source_dir: Path, name: str) -> bytes:
    data = (source_dir / name).read_bytes()
    return base64.b64decode(b''.join(data.split()), validate=True) if name.endswith('.base64') else data


def rewrite_package(source: bytes, transform) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(io.BytesIO(source)) as original, zipfile.ZipFile(output, 'w') as target:
        for entry in original.infolist():
            payload = original.read(entry.filename)
            if entry.filename.lower().endswith('.model'):
                payload = transform(payload)
            if entry.filename == '[Content_Types].xml' and transform.__name__ == 'add_private_metadata':
                marker = b'</Types>'
                assert payload.count(marker) == 1
                payload = payload.replace(marker,
                    b'<Default Extension="config" ContentType="application/xml"/>' + marker, 1)
            info = zipfile.ZipInfo(entry.filename, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED if entry.is_dir() else zipfile.ZIP_DEFLATED
            target.writestr(info, payload)
        if transform.__name__ == 'add_private_metadata':
            info = zipfile.ZipInfo('Metadata/model_settings.config', (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            target.writestr(info, b'<config><plate><plater_id>1</plater_id></plate></config>')
    return output.getvalue()


def add_private_metadata(model: bytes) -> bytes:
    return model


def add_unknown_required(model: bytes) -> bytes:
    marker = b'<model '
    assert model.count(marker) == 1
    return model.replace(marker, marker +
                         b'requiredextensions="evil" xmlns:evil="http://example.invalid/3mf/evil" ', 1)


def make_slice_optional(model: bytes) -> bytes:
    required = b'requiredextensions="s p"'
    assert model.count(required) == 1
    return model.replace(required, b'requiredextensions="p"', 1)


def derive(operation: str, source: bytes) -> bytes:
    if operation == 'empty':
        return b''
    if operation == 'truncate-tail':
        return source[:-11]
    if operation == 'bad-local-signature':
        assert source[:4] == b'PK\x03\x04'
        return b'BAD!' + source[4:]
    if operation == 'bad-central-offset':
        data = bytearray(source)
        eocd = data.rfind(b'PK\x05\x06')
        assert eocd >= 0 and eocd + 22 == len(data)
        struct.pack_into('<I', data, eocd + 16, 0xfffffff0)
        return bytes(data)
    if operation == 'unsafe-central-path':
        data = bytearray(source)
        central = data.find(b'PK\x01\x02')
        assert central >= 0
        name_start = central + 46
        assert data[name_start:name_start + 3] == b'3D/'
        data[name_start:name_start + 3] = b'../'
        return bytes(data)
    if operation == 'private-metadata':
        return rewrite_package(source, add_private_metadata)
    if operation == 'unknown-required':
        return rewrite_package(source, add_unknown_required)
    if operation == 'slice-optional':
        return rewrite_package(source, make_slice_optional)
    raise ValueError(f'unknown operation: {operation}')


def verify(entry: dict, data: bytes) -> None:
    assert len(data) == entry['bytes'], entry['path']
    assert hashlib.sha256(data).hexdigest() == entry['sha256'], entry['path']


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='materialize decoded and derived .3mf files')
    args = parser.parse_args()
    manifest = json.loads((HERE / 'manifest.json').read_text(encoding='utf-8'))
    assert manifest['schema'] == 1 and manifest['corpus'] == '3MF-007'
    source_dir = (HERE / manifest['sourceDirectory']).resolve()
    decoded = {}
    for entry in manifest['entries']:
        data = source_bytes(source_dir, entry['path'])
        verify(entry, data)
        decoded[entry['path']] = data
        if args.output:
            destination = args.output / 'valid' / entry['path'].removesuffix('.base64')
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
    assert decoded['static-production.3mf.base64'] == derive(
        'slice-optional', decoded['production-boxes.3mf.base64'])
    for entry in manifest['derived']:
        data = derive(entry['operation'], decoded[entry['base']])
        verify(entry, data)
        if args.output:
            destination = args.output / 'derived' / entry['path']
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
    print(f"verified {len(manifest['entries'])} sources and {len(manifest['derived'])} derived cases")


if __name__ == '__main__':
    main()
