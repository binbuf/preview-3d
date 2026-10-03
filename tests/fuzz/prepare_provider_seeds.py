"""Materialize bounded libFuzzer seeds for the SEC-17 provider pipeline target.

The envelope is 24 bytes: magic, domain, family, flags, reserved, cx, a
reported/stream size, and the payload length, followed by the payload. The
pipeline domain carries one real provider fixture per family (the same files
``Tests.ProviderHost.exe`` renders); the stream/sampler/raster domains carry
small crafted hostile inputs.

The provider never opens a path, so reading the committed fixtures here just
fills the in-memory source. The preparer refuses a non-empty output directory
because libFuzzer mutates its corpus in place.
"""

import argparse
import base64
import struct
from pathlib import Path

MAGIC = 0x5A565250  # "PRVZ"

PIPELINE = 0
STREAM = 1
SAMPLER = 2
RASTER = 3

# Family numeric values (preview3d::provider::Family).
GLTF, STL, PLY, OBJ, FBX, THREEMF, USD, STEP = 1, 2, 3, 4, 5, 6, 7, 8

# Stream flags.
NON_SEEKABLE = 1 << 0
STAT_FAILS = 1 << 1
STAT_SIZE = 1 << 2
SHORT_READ = 1 << 3
STAT_WRONG_TYPE = 1 << 4

ROOT = Path(__file__).resolve().parents[2]
CORPUS = ROOT / 'interactive-viewer' / 'test-assets' / 'corpus'
FBX_DIR = ROOT / 'tests' / 'fixtures' / 'fbx-spike'
THREEMF_DIR = ROOT / 'tests' / 'fixtures' / '3mf-spike'
USD_DIR = ROOT / 'tests' / 'fixtures' / 'usd-spike'
STEP_DIR = ROOT / 'tests' / 'fixtures' / 'stp-spike'


def envelope(domain, payload, family=0, flags=0, cx=256, reported=0):
    return struct.pack('<IBBBBIQI', MAGIC, domain, family, flags, 0, cx,
                       reported, len(payload)) + payload


def read(path):
    path = Path(path)
    if not path.is_file():
        raise SystemExit(f'missing fixture: {path}')
    return path.read_bytes()


def read_base64(path):
    return base64.b64decode(read(path), validate=False)


CUBE_OBJ = (
    "mtllib cube-sidecar.mtl\no cube\nusemtl cube\n"
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
    "v 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
    "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\n"
    "f 4 8 7 3\nf 1 5 8 4\nf 2 3 7 6\n"
).encode()


def cube_stl():
    cube = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0),
            (0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
             (3, 7, 6, 2), (0, 4, 7, 3), (1, 2, 6, 5)]
    out = bytearray(80 + 4)
    struct.pack_into('<I', out, 80, 12)
    for a, b, c, d in quads:
        for tri in ((a, b, c), (a, c, d)):
            out += struct.pack('<12fH', 0, 0, 0,
                               *cube[tri[0]], *cube[tri[1]], *cube[tri[2]], 0)
    return bytes(out)


def f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def bad_f32_words():
    # NaN, +Inf, -Inf, and a huge finite value, as raw bit patterns.
    return struct.pack('<4I', 0x7FC00000, 0x7F800000, 0xFF800000, 0x7F7FFFFF)


def triangle_record(positions, material=0):
    # 24 little-endian words: positions (0..8), normals (9..17), color (18..21),
    # origin/padding (22..23). The sampler also reads word 21 as the 1-based
    # material index and word 20 in the raster domain; keep them bounded.
    words = [0] * 24
    for v, position in enumerate(positions):
        for c in range(3):
            words[3 * v + c] = f32_bits(position[c])
            words[9 + 3 * v + c] = f32_bits(0.0 if c != 2 else 1.0)
    words[18] = words[19] = words[21] = f32_bits(1.0)
    words[20] = material
    return struct.pack('<24I', *words)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists() and any(args.output.iterdir()):
        raise SystemExit(f'refusing non-empty seed directory: {args.output}; use a new path')
    args.output.mkdir(parents=True, exist_ok=True)

    seeds = []

    def add(name, blob):
        seeds.append((name, blob))

    # --- Pipeline domain: one bounded smoke per family ---------------------
    stl = cube_stl()
    ply_mesh = CORPUS / 'A-small-ply-mesh-le.ply'
    ply_points = CORPUS / 'A-small-ply-points-le.ply'
    glb = CORPUS / 'A-small-glb.glb'
    draco = CORPUS / 'draco_triangle.glb'
    basisu = CORPUS / 'basisu_textured_triangle.glb'
    obj = CUBE_OBJ
    fbx = FBX_DIR / 'hierarchy-instances-pivots-ascii.fbx'
    three_mf = THREEMF_DIR / 'core-box.3mf.base64'
    usd_ascii = USD_DIR / 'mesh.usda'
    usd_crate = USD_DIR / 'cube.usdc.base64'
    usd_zip = USD_DIR / 'cube.usdz.base64'
    step = STEP_DIR / 'part_ap214.stp'

    add('pipeline-stl.pipeline.seed', envelope(PIPELINE, stl, family=STL))
    add('pipeline-ply-mesh.pipeline.seed',
        envelope(PIPELINE, read(ply_mesh), family=PLY))
    add('pipeline-ply-points.pipeline.seed',
        envelope(PIPELINE, read(ply_points), family=PLY))
    add('pipeline-obj.pipeline.seed', envelope(PIPELINE, obj, family=OBJ))
    add('pipeline-gltf.pipeline.seed', envelope(PIPELINE, read(glb), family=GLTF))
    add('pipeline-gltf-draco.pipeline.seed',
        envelope(PIPELINE, read(draco), family=GLTF))
    add('pipeline-gltf-basisu.pipeline.seed',
        envelope(PIPELINE, read(basisu), family=GLTF))
    add('pipeline-fbx.pipeline.seed', envelope(PIPELINE, read(fbx), family=FBX))
    add('pipeline-3mf.pipeline.seed',
        envelope(PIPELINE, read_base64(three_mf), family=THREEMF))
    add('pipeline-usd-ascii.pipeline.seed',
        envelope(PIPELINE, read(usd_ascii), family=USD))
    add('pipeline-usd-crate.pipeline.seed',
        envelope(PIPELINE, read_base64(usd_crate), family=USD))
    add('pipeline-usd-zip.pipeline.seed',
        envelope(PIPELINE, read_base64(usd_zip), family=USD))
    add('pipeline-step.pipeline.seed', envelope(PIPELINE, read(step), family=STEP))

    # Malformed/truncated per family and hostile stream behaviour.
    add('pipeline-gltf-truncated.pipeline.seed',
        envelope(PIPELINE, read(glb)[:32], family=GLTF))
    add('pipeline-step-truncated.pipeline.seed',
        envelope(PIPELINE, read(step)[:64], family=STEP))
    add('pipeline-stl-nonseekable.pipeline.seed',
        envelope(PIPELINE, stl, family=STL, flags=NON_SEEKABLE))
    add('pipeline-gltf-statfail.pipeline.seed',
        envelope(PIPELINE, read(glb), family=GLTF, flags=STAT_FAILS))
    add('pipeline-gltf-shortread.pipeline.seed',
        envelope(PIPELINE, read(glb), family=GLTF, flags=SHORT_READ))
    add('pipeline-ply-huge-stat.pipeline.seed',
        envelope(PIPELINE, read(ply_mesh), family=PLY, flags=STAT_SIZE,
                 reported=0x8000000000000000))
    add('pipeline-ply-overcap-stat.pipeline.seed',
        envelope(PIPELINE, read(ply_mesh), family=PLY, flags=STAT_SIZE,
                 reported=0xFFFFFFFFFFFFFFFF))
    add('pipeline-usd-wrong-type.pipeline.seed',
        envelope(PIPELINE, read(usd_ascii), family=USD, flags=STAT_WRONG_TYPE))

    # --- Stream domain: bounded source edge cases --------------------------
    add('stream-normal.stream.seed', envelope(STREAM, b'hello bounded stream'))
    add('stream-nonseekable.stream.seed',
        envelope(STREAM, b'non-seekable-payload', flags=NON_SEEKABLE))
    add('stream-statfail.stream.seed',
        envelope(STREAM, b'stat-fails-payload', flags=STAT_FAILS))
    add('stream-shortread.stream.seed',
        envelope(STREAM, b'short-read-payload', flags=SHORT_READ))
    add('stream-huge-size.stream.seed',
        envelope(STREAM, b'huge-size-payload', flags=STAT_SIZE,
                 reported=0x8000000000000000))
    add('stream-overcap.stream.seed',
        envelope(STREAM, b'over-cap-payload', flags=STAT_SIZE,
                 reported=0xFFFFFFFFFFFFFFFF))
    add('stream-empty.stream.seed', envelope(STREAM, b''))

    # --- Sampler domain ----------------------------------------------------
    add('sampler-degenerate.sampler.seed',
        envelope(SAMPLER, triangle_record([(0, 0, 0), (0, 0, 0), (0, 0, 0)])))
    add('sampler-nonfinite.sampler.seed',
        envelope(SAMPLER, bad_f32_words() * 6))
    add('sampler-normal.sampler.seed',
        envelope(SAMPLER, triangle_record([(0, 0, 0), (2, 0, 0), (0, 2, 0)])))

    # --- Raster domain: clipping, degenerate transforms, extreme aspect ----
    add('raster-triangle.raster.seed',
        envelope(RASTER, triangle_record([(0, 0, 0), (2, 0, 0), (0, 2, 0)]),
                 cx=256))
    add('raster-degenerate.raster.seed',
        envelope(RASTER, triangle_record([(0, 0, 0), (0, 0, 0), (0, 0, 0)]),
                 cx=256))
    add('raster-nonfinite.raster.seed',
        envelope(RASTER, bad_f32_words() * 6, cx=256))
    add('raster-extreme-aspect.raster.seed',
        envelope(RASTER,
                 triangle_record([(0, 0, 0), (1e30, 0, 0), (0, 1e-30, 0)]),
                 cx=0xFFFFFFFF))
    add('raster-cx-zero.raster.seed',
        envelope(RASTER, triangle_record([(0, 0, 0), (1, 0, 0), (0, 1, 0)]),
                 cx=0))

    for name, blob in seeds:
        (args.output / name).write_bytes(blob)
    print(f'materialized {len(seeds)} provider fuzz seeds in {args.output}')


if __name__ == '__main__':
    main()