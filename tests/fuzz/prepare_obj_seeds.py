"""Materialize bounded libFuzzer seeds for the OBJ/MTL fast-path target.

Fixtures are generated here rather than committed (tests/fixtures/README.md).
Each seed is a bounded envelope carrying an OBJ/MTL primary source plus at most
one in-memory virtual sidecar (the MTL reply the open-file callback may serve).
The seeds cover OBJ tokenization, MTL tokenization, missing and hostile
sidecar references, and index/normalization edge cases. The preparer refuses a
non-empty output directory because libFuzzer mutates its corpus in place.
"""

import argparse
import struct
from pathlib import Path


MAGIC = 0x5A4A424F  # "OBJZ"
MTL_PRIMARY = 1 << 0
EXTERNAL_FILES = 1 << 1
CANCEL = 1 << 3


def envelope(primary: bytes, flags: int = EXTERNAL_FILES, path: bytes = b'',
             sidecar: bytes = b'', cancel_after: int = 0) -> bytes:
    return (struct.pack('<IBBHI', MAGIC, flags, cancel_after, len(path), len(primary))
            + primary + path + sidecar)


OBJ_HAPPY = (
    b'mtllib material.mtl\n'
    b'o unit\n'
    b'v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n'
    b'vt 0 0\nvt 1 0\nvt 0 1\n'
    b'vn 0 0 1\n'
    b'usemtl red\n'
    b'f 1/1/1 2/2/1 3/3/1\n'
    b'f 1/1/1 3/3/1 4/1/1\n'
)

MTL_HAPPY = (
    b'newmtl red\n'
    b'Ka 0.1 0.1 0.1\n'
    b'Kd 1.0 0.0 0.0\n'
    b'Ks 0.5 0.5 0.5\n'
    b'Ns 32.0\n'
    b'd 1.0\n'
    b'map_Kd ../textures/evil.png\n'
    b'map_Bump -bm 0.5 bump.png\n'
    b'newmtl unreferenced\n'
    b'Kd 0 1 0\n'
)

OBJ_MALFORMED = (
    b'v 0 0 zero\n'
    b'v 1 0 0 extra tokens here\n'
    b'f 1 0 2\n'
    b'f 1 2 999999999\n'
    b'f -1 -2 -3\n'
    b'f 1\n'
    b'vn nan 0 1\n'
    b'f 1//1 2//1 3//1\n'
)

OBJ_MANY = b''.join(
    b'v %d %d %d\n' % (i, (i * 7) % 13, (i * 3) % 5) for i in range(64)
) + b'f 1 2 3\nf 3 4 5\nf 5 6 7\nf 7 8 9\n'

HOSTILE_REFERENCES = (
    b'mtllib ../../etc/passwd\n'
    b'mtllib C:\\Windows\\System32\\config\\SAM\n'
    b'mtllib \\\\server\\share\\other.mtl\n'
    b'mtllib http://example.invalid/other.mtl\n'
    b'mtllib assets\\..\\..\\escape.mtl\n'
    b'v 0 0 0\nv 1 0 0\nv 0 1 0\n'
    b'usemtl missing\n'
    b'f 1 2 3\n'
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists() and any(args.output.iterdir()):
        raise SystemExit(f'refusing non-empty seed directory: {args.output}; use a new path')
    args.output.mkdir(parents=True, exist_ok=True)

    seeds: list[tuple[str, bytes]] = [
        # OBJ + a served MTL sidecar: full OBJ/MTL tokenization and materials.
        ('obj-with-mtl.adapter.seed',
         envelope(OBJ_HAPPY, EXTERNAL_FILES, b'material.mtl', MTL_HAPPY)),
        # OBJ references an MTL but the sidecar is not served (missing).
        ('obj-missing-mtl.adapter.seed',
         envelope(OBJ_HAPPY, EXTERNAL_FILES, b'material.mtl', b'')),
        # Same OBJ with external loading disabled: no sidecar request at all.
        ('obj-no-external.seed', envelope(OBJ_HAPPY, 0, b'material.mtl', MTL_HAPPY)),
        # MTL parsed directly as the primary source.
        ('mtl-primary.seed', envelope(MTL_HAPPY, MTL_PRIMARY)),
        ('mtl-hostile-primary.seed',
         envelope(MTL_HAPPY + b'map_Kd \\\\server\\share\\x.png\n', MTL_PRIMARY)),
        # Hostile references: every path must be denied without a filesystem hit.
        ('obj-hostile-references.adapter.seed',
         envelope(HOSTILE_REFERENCES, EXTERNAL_FILES, b'material.mtl', MTL_HAPPY)),
        # Malformed OBJ indices and normals: must fail closed, never OOB.
        ('obj-malformed.adapter.seed', envelope(OBJ_MALFORMED)),
        # A wider triangulation sample.
        ('obj-many-faces.adapter.seed', envelope(OBJ_MANY)),
        # Cancellation request during load.
        ('obj-cancel.seed',
         envelope(OBJ_HAPPY, EXTERNAL_FILES | CANCEL, b'material.mtl', MTL_HAPPY,
                  cancel_after=1)),
    ]
    for name, blob in seeds:
        (args.output / name).write_bytes(blob)
    print(f'materialized {len(seeds)} OBJ/MTL fuzz seeds in {args.output}')


if __name__ == '__main__':
    main()