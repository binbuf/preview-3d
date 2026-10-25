"""Materialize bounded libFuzzer seeds for the PLY fast-path target.

Fixtures are generated here rather than committed (tests/fixtures/README.md).
The seeds cover ASCII, binary little-endian and binary big-endian meshes and
point clouds plus header and scalar edge cases. The preparer refuses a
non-empty output directory because libFuzzer mutates its corpus in place.
"""

import argparse
import struct
from pathlib import Path


MAGIC = 0x5A594C50  # "PLYZ"
ADAPTER_BINARY = 0
ADAPTER_ASCII = 1
HEADER = 2
SCALAR_PRIMITIVES = 3


def envelope(domain: int, payload: bytes, flags: int = 0) -> bytes:
    return struct.pack('<IBBH', MAGIC, domain, flags, 0) + payload


def ascii_header(format_name: str) -> bytes:
    return (
        f'ply\nformat {format_name} 1.0\ncomment generated seed\n'
        'element vertex 3\n'
        'property float x\nproperty float y\nproperty float z\n'
        'property uchar red\nproperty uchar green\nproperty uchar blue\n'
        'element face 1\n'
        'property list uchar int vertex_indices\n'
        'end_header\n'
    ).encode('ascii')


ASCII_MESH = ascii_header('ascii') + (
    b'0 0 0 255 0 0\n'
    b'1 0 0 0 255 0\n'
    b'0 1 0 0 0 255\n'
    b'3 0 1 2\n'
)

ASCII_POINTS = (
    'ply\nformat ascii 1.0\nelement vertex 4\n'
    'property float x\nproperty float y\nproperty float z\n'
    'element unknown 1\nproperty float mystery\nend_header\n'
    '0 0 0\n1 1 1\n2 4 8\n-3 9 -27\n42.0\n'
).encode('ascii')


def binary_body(endian: str) -> bytes:
    vertices = b''.join(
        struct.pack(endian + '3f3B', *position, *color)
        for position, color in (
            ((0.0, 0.0, 0.0), (255, 0, 0)),
            ((1.0, 0.0, 0.0), (0, 255, 0)),
            ((0.0, 1.0, 0.0), (0, 0, 255)),
        )
    )
    face = struct.pack(endian + 'B3i', 3, 0, 1, 2)
    return vertices + face


def binary_header(format_name: str) -> bytes:
    return (
        f'ply\nformat {format_name} 1.0\n'
        'element vertex 3\n'
        'property float x\nproperty float y\nproperty float z\n'
        'property uchar red\nproperty uchar green\nproperty uchar blue\n'
        'element face 1\n'
        'property list uchar int vertex_indices\n'
        'end_header\n'
    ).encode('ascii')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists() and any(args.output.iterdir()):
        raise SystemExit(f'refusing non-empty seed directory: {args.output}; use a new path')
    args.output.mkdir(parents=True, exist_ok=True)

    binary_le = binary_header('binary_little_endian') + binary_body('<')
    binary_be = binary_header('binary_big_endian') + binary_body('>')
    # Declared count far beyond Tier A: must close ResourceLimit, no allocation.
    huge = (
        'ply\nformat binary_little_endian 1.0\nelement vertex 4000000000\n'
        'property float x\nproperty float y\nproperty float z\nend_header\n'
    ).encode('ascii')

    seeds: list[tuple[str, bytes]] = [
        ('ascii-mesh.adapter.seed', envelope(ADAPTER_ASCII, ASCII_MESH)),
        ('ascii-points-unknown.adapter.seed', envelope(ADAPTER_ASCII, ASCII_POINTS)),
        ('binary-le-mesh.adapter.seed', envelope(ADAPTER_BINARY, binary_le)),
        ('binary-be-mesh.adapter.seed', envelope(ADAPTER_BINARY, binary_be)),
        ('binary-le-mesh.ascii-domain.seed', envelope(ADAPTER_ASCII, binary_le)),
        ('binary-le-truncated.adapter.seed', envelope(ADAPTER_BINARY, binary_le[:-5])),
        ('binary-count-huge.adapter.seed', envelope(ADAPTER_BINARY, huge)),
        ('header-endianness.seed', envelope(HEADER, binary_be)),
        ('header-missing-end.seed',
         envelope(HEADER, b'ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\n')),
        ('header-bad-format.seed',
         envelope(HEADER, b'ply\nformat ascii 2.0\nend_header\n')),
        ('header-unknown-keyword.seed',
         envelope(HEADER, b'ply\nformat ascii 1.0\nwibble\nend_header\n')),
        ('scalar-arbitrary.seed', envelope(SCALAR_PRIMITIVES, bytes(range(256)) * 4)),
    ]
    for name, blob in seeds:
        (args.output / name).write_bytes(blob)
    print(f'materialized {len(seeds)} PLY fuzz seeds in {args.output}')


if __name__ == '__main__':
    main()