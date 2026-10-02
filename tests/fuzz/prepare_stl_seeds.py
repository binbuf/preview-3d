"""Materialize bounded libFuzzer seeds for the STL fast-path target.

Fixtures are generated here rather than committed (tests/fixtures/README.md).
The seeds are deterministic, small, and deliberately cover both dialects and
the header/normalization edge cases the adapter must fail closed on. The
preparer refuses a non-empty output directory because libFuzzer mutates its
corpus in place.
"""

import argparse
import struct
from pathlib import Path


MAGIC = 0x5A4C5453  # "STLZ"
ADAPTER = 0
ADAPTER_BINARY_ONLY = 1
DETECTION = 2
FACET_PRIMITIVES = 3


def envelope(domain: int, payload: bytes, flags: int = 0) -> bytes:
    return struct.pack('<IBBH', MAGIC, domain, flags, 0) + payload


def facet(normal, v0, v1, v2, attr: int = 0) -> bytes:
    values = (*normal, *v0, *v1, *v2)
    return struct.pack('<12fH', *values, attr)


def binary_stl(header: bytes, facets: list[bytes], count: int | None = None) -> bytes:
    blob = bytearray(header[:80].ljust(80, b'\x00'))
    blob += struct.pack('<I', len(facets) if count is None else count)
    for record in facets:
        blob += record
    return bytes(blob)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists() and any(args.output.iterdir()):
        raise SystemExit(f'refusing non-empty seed directory: {args.output}; use a new path')
    args.output.mkdir(parents=True, exist_ok=True)

    ascii_triangle = (
        b'solid unit\n'
        b'  facet normal 0 0 1\n'
        b'    outer loop\n'
        b'      vertex 0 0 0\n'
        b'      vertex 1 0 0\n'
        b'      vertex 0 1 0\n'
        b'    endloop\n'
        b'  endfacet\n'
        b'endsolid unit\n'
    )
    ascii_empty = b'solid empty\nendsolid empty\n'
    ascii_bad_number = (
        b'solid bad\nfacet normal 0 0 1\nouter loop\n'
        b'vertex nan 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nendsolid bad\n'
    )

    good = facet((0.0, 0.0, 1.0), (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (1.0, 1.0, 0.0))
    degenerate = facet((0.0, 0.0, 1.0), (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), (1.0, 1.0, 0.0))
    non_finite = facet((float('nan'), 0.0, 1.0), (0.0, 0.0, 0.0), (1.0, 0.0, 0.0),
                       (0.0, 1.0, 0.0))
    binary_good = binary_stl(b'Binary STL generated seed', [good])
    binary_solid_name = binary_stl(b'solid-but-binary', [good, good])
    binary_multi = binary_stl(b'Binary STL generated seed', [good, degenerate, non_finite])

    seeds: list[tuple[str, bytes]] = [
        ('ascii-triangle.adapter.seed', envelope(ADAPTER, ascii_triangle)),
        ('ascii-empty.adapter.seed', envelope(ADAPTER, ascii_empty)),
        ('ascii-bad-number.adapter.seed', envelope(ADAPTER, ascii_bad_number)),
        ('ascii-triangle.binary-only.seed',
         envelope(ADAPTER_BINARY_ONLY, ascii_triangle, flags=1)),
        ('binary-good.adapter.seed', envelope(ADAPTER, binary_good)),
        ('binary-solid-name.adapter.seed', envelope(ADAPTER, binary_solid_name)),
        ('binary-multi.adapter.seed', envelope(ADAPTER, binary_multi)),
        ('binary-truncated.adapter.seed', envelope(ADAPTER, binary_good[:-10])),
        # Declared count far beyond Tier A: must close ResourceLimit, no alloc.
        ('binary-count-max.adapter.seed',
         envelope(ADAPTER, binary_stl(b'count-max', [], count=0xFFFFFFFF))),
        ('detect-solid-prefix.seed', envelope(DETECTION, b'solid named thing')),
        ('detect-binary-prefix.seed', envelope(DETECTION, binary_good)),
        ('facets-alignment.seed',
         envelope(FACET_PRIMITIVES, good + degenerate + non_finite + b'\x00' * 50)),
    ]
    for name, blob in seeds:
        (args.output / name).write_bytes(blob)
    print(f'materialized {len(seeds)} STL fuzz seeds in {args.output}')


if __name__ == '__main__':
    main()