"""Materialize bounded libFuzzer seeds for the glTF + compressed-codec target.

The envelope is compact and portable: an 8-byte header (magic, domain, flags,
reserved) followed by the domain payload. The adapter domain carries a GLB or
.json glTF source verbatim; the codec domains carry a small explicit
count/stride/dimension header plus the encoded stream, so a single engine
cycle reaches the SEC-02 decode preflights as well as the third-party
decoders.

The real compressed fixtures (Draco, meshopt, KTX2/Basis, WebP) are copied
byte-for-byte from the committed corpus under
``interactive-viewer/test-assets/corpus``; the hostile count/sparse/truncation
seeds are constructed here. Everything is deterministic and small. The
preparer refuses a non-empty output directory because libFuzzer mutates its
corpus in place.
"""

import argparse
import struct
from pathlib import Path


MAGIC = 0x5A544C47  # "GLTZ"

ADAPTER = 0
DRACO = 1
MESHOPT = 2
KTX2 = 3
WEBP = 4
WIC_RASTER = 5
SNIFF = 6

# Adapter / texture-domain flags (domain-specific below the shared bits).
SMALL_DIMENSION = 1 << 1
CANCEL = 1 << 7
DRACO_NORMAL = 1 << 0
DRACO_UV0 = 1 << 1
DRACO_TANGENT = 1 << 2
DRACO_COLOR = 1 << 3

CORPUS = (Path(__file__).resolve().parents[2]
          / 'interactive-viewer' / 'test-assets' / 'corpus')

# Minimized findings promoted into the smoke corpus now that their class is
# mitigated (SEC-16b). These are immutable reproducers, read byte-for-byte.
FINDINGS = Path(__file__).resolve().parent / 'corpus' / 'gltf'


def envelope(domain: int, payload: bytes, flags: int = 0) -> bytes:
    return struct.pack('<IBBH', MAGIC, domain, flags, 0) + payload


def read_corpus(name: str) -> bytes:
    path = CORPUS / name
    if not path.is_file():
        raise SystemExit(f'missing committed corpus fixture: {path}')
    return path.read_bytes()


def read_finding(name: str) -> bytes:
    path = FINDINGS / name
    if not path.is_file():
        raise SystemExit(f'missing committed findings fixture: {path}')
    return path.read_bytes()


def append_f32(blob: bytearray, value: float) -> None:
    blob += struct.pack('<f', value)


def append_u32(blob: bytearray, value: int) -> None:
    blob += struct.pack('<I', value)


def triangle_buffer() -> bytes:
    blob = bytearray()
    for value in (0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0):
        append_f32(blob, value)
    for value in (0, 1, 2):
        append_u32(blob, value)
    return bytes(blob)


def triangle_json(buffer_uri: str) -> str:
    return (
        '{"asset":{"version":"2.0"},"scene":0,'
        '"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],'
        '"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],'
        '"accessors":['
        '{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",'
        '"min":[0,0,0],"max":[1,1,0]},'
        '{"bufferView":1,"componentType":5125,"count":3,"type":"SCALAR"}],'
        '"bufferViews":['
        '{"buffer":0,"byteOffset":0,"byteLength":36},'
        '{"buffer":0,"byteOffset":36,"byteLength":12}],'
        '"buffers":[' + buffer_uri + ']}'
    )


def build_glb(text: str, binary: bytes) -> bytes:
    json_chunk = bytearray(text.encode())
    while len(json_chunk) % 4:
        json_chunk += b' '
    bin_chunk = bytearray(binary)
    while len(bin_chunk) % 4:
        bin_chunk += b'\x00'
    total = 12 + 8 + len(json_chunk) + 8 + len(bin_chunk)
    out = bytearray()
    out += struct.pack('<III', 0x46546C67, 2, total)
    out += struct.pack('<II', len(json_chunk), 0x4E4F534A)
    out += json_chunk
    out += struct.pack('<II', len(bin_chunk), 0x004E4942)
    out += bin_chunk
    return bytes(out)


def triangle_glb() -> bytes:
    return build_glb(triangle_json('{"byteLength":48}'), triangle_buffer())


def hostile_counts_glb() -> bytes:
    # Declares an accessor count far past Tier A with a tiny backing buffer:
    # the metadata preflight or range validation must fail closed.
    text = (
        '{"asset":{"version":"2.0"},"scene":0,'
        '"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],'
        '"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],'
        '"accessors":[{"bufferView":0,"componentType":5126,"count":4294967295,'
        '"type":"VEC3"}],'
        '"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":12}],'
        '"buffers":[{"byteLength":12}]}'
    )
    return build_glb(text, b'\x00' * 12)


def sparse_out_of_range_gltf() -> bytes:
    # A sparse accessor whose indices bufferView lies beyond the buffer and
    # whose sparse count exceeds the base count.
    text = (
        '{"asset":{"version":"2.0"},"scene":0,'
        '"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],'
        '"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],'
        '"accessors":[{"componentType":5126,"count":4,"type":"VEC3",'
        '"sparse":{"count":9999,"indices":{"bufferView":7,"componentType":5125},'
        '"values":{"bufferView":8}}}],'
        '"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":12}],'
        '"buffers":[{"byteLength":12}]}'
    )
    return text.encode()


def hostile_draco_stream(encoded_vertices: int, faces: int) -> bytes:
    # A well-formed Draco fixed header with a hostile declared connectivity
    # preamble, mirroring the SEC-02 unit fixture.
    stream = bytearray(b'DRACO')
    stream += bytes((2, 2, 1, 1, 0, 0))
    stream += b'\x00'  # edgebreaker traversal decoder selector

    def varint(value: int) -> None:
        while True:
            byte = value & 0x7F
            value >>= 7
            stream.append(byte | (0x80 if value else 0x00))
            if not value:
                break

    varint(encoded_vertices)
    varint(faces)
    stream += b'\x00'  # num_attribute_data
    varint(1 if faces else 0)  # num_encoded_symbols
    varint(0)  # num_encoded_split_symbols
    stream += b'\x00' * 200
    return bytes(stream)


def hostile_ktx2_stream(width: int, height: int, levels: int) -> bytes:
    stream = bytearray(80 + levels * 24)
    stream[0:12] = bytes((0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
                          0x0D, 0x0A, 0x1A, 0x0A))
    struct.pack_into('<I', stream, 12, 0)          # vkFormat: Basis
    struct.pack_into('<I', stream, 16, 1)          # typeSize
    struct.pack_into('<I', stream, 20, width)
    struct.pack_into('<I', stream, 24, height)
    struct.pack_into('<I', stream, 36, 1)          # faceCount
    struct.pack_into('<I', stream, 40, levels)
    for level in range(levels):
        base = 80 + level * 24
        struct.pack_into('<Q', stream, base, 80 + levels * 24)     # offset
        struct.pack_into('<Q', stream, base + 8, 1)                # length
        struct.pack_into('<Q', stream, base + 16, 4)               # uncompressed
    return bytes(stream)


def png_header(width: int, height: int) -> bytes:
    return (b'\x89PNG\r\n\x1a\n' + struct.pack('>I', 13) + b'IHDR'
            + struct.pack('>II', width, height) + bytes((8, 6, 0, 0, 0)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists() and any(args.output.iterdir()):
        raise SystemExit(f'refusing non-empty seed directory: {args.output}; use a new path')
    args.output.mkdir(parents=True, exist_ok=True)

    valid_glb = triangle_glb()
    seeds: list[tuple[str, bytes]] = [
        # --- Adapter domain: GLB/JSON, accessor/sparse/node validation ---
        ('triangle.adapter.seed', envelope(ADAPTER, valid_glb)),
        ('triangle-data-uri.adapter.seed',
         envelope(ADAPTER, triangle_json(
             '{"byteLength":48,"uri":"data:application/octet-stream;base64,'
             'AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAQAAAAIAAAA="}'
         ).encode())),
        ('truncated-corpus.adapter.seed',
         envelope(ADAPTER, read_corpus('truncated.glb'))),
        ('truncated-mid-header.adapter.seed', envelope(ADAPTER, valid_glb[:14])),
        ('hostile-counts.adapter.seed', envelope(ADAPTER, hostile_counts_glb())),
        ('sparse-out-of-range.adapter.seed',
         envelope(ADAPTER, sparse_out_of_range_gltf())),
        ('sparse-invalid-corpus.adapter.seed',
         envelope(ADAPTER, read_corpus('sparse-invalid.gltf'))),
        # Embedded compressed corpus through the real adapter.
        ('draco-corpus.adapter.seed',
         envelope(ADAPTER, read_corpus('draco_triangle.glb'))),
        ('draco-position-only.adapter.seed',
         envelope(ADAPTER, read_corpus('draco_position_only.glb'))),
        ('meshopt-corpus.adapter.seed',
         envelope(ADAPTER, read_corpus('meshopt.glb'))),
        # Valid BasisLZ/ETC1S transcode: safe again after the SEC-16b table
        # preflight, so the frozen textured triangle drives the real path.
        ('basisu-corpus.adapter.seed',
         envelope(ADAPTER, read_corpus('basisu_textured_triangle.glb'))),
        ('basisu-corrupt.adapter.seed',
         envelope(ADAPTER, read_corpus('basisu_corrupt_ktx2.glb'))),
        ('webp-corpus.adapter.seed', envelope(ADAPTER, read_corpus('webp.gltf'))),
        ('cancel.adapter.seed', envelope(ADAPTER, valid_glb, CANCEL)),
        # Promoted minimized findings (SEC-16/SEC-16b). Both now reject cleanly:
        # fastgltf's non-multiple-of-four data-URI base64, and the ETC1S
        # malformed-Huffman-table container. See corpus/gltf/README.md. The
        # .env files are already Adapter-domain envelopes.
        ('fastgltf-base64-overflow.adapter.seed',
         read_finding('fastgltf-base64-overflow.env')),
        ('basislz-etc1s-crash.adapter.seed',
         read_finding('basislz-etc1s-crash.env')),
        # --- Draco domain: SEC-02 declared vs expected connectivity ---
        ('draco-mismatch.seed',
         envelope(DRACO,
                  struct.pack('<II', 3, 3)
                  + hostile_draco_stream(3, 1), DRACO_NORMAL | DRACO_UV0)),
        ('draco-over-budget.seed',
         envelope(DRACO,
                  struct.pack('<II', 0xFFFFFFFF, 0xFFFFFFFF)
                  + hostile_draco_stream(0xFFFFFFFF, 0xFFFFFFFF))),
        ('draco-zero-counts.seed',
         envelope(DRACO, struct.pack('<II', 0, 0) + hostile_draco_stream(0, 0))),
        ('draco-not-magic.seed',
         envelope(DRACO, struct.pack('<II', 3, 3) + b'not-a-draco-stream')),
        # --- Meshopt domain: shape/stride/decoded-byte checks ---
        ('meshopt-attributes.seed', envelope(MESHOPT,
                                             struct.pack('<IIQ', 3, 12, 36)
                                             + b'\x00' * 48)),
        ('meshopt-mismatch.seed', envelope(MESHOPT,
                                           struct.pack('<IIQ', 3, 4, 4096)
                                           + b'\x00' * 32, flags=0)),
        ('meshopt-triangles.seed', envelope(MESHOPT,
                                            struct.pack('<IIQ', 3, 4, 12)
                                            + b'\x00' * 32, flags=1)),
        ('meshopt-indices.seed', envelope(MESHOPT,
                                          struct.pack('<IIQ', 3, 2, 6)
                                          + b'\x00' * 32, flags=2)),
        # --- KTX2/Basis domain: header/level preflight ---
        ('ktx2-over-declared.seed',
         envelope(KTX2, hostile_ktx2_stream(0xFFFF, 0xFFFF, 2))),
        ('ktx2-many-levels.seed',
         envelope(KTX2, hostile_ktx2_stream(1024, 1024, 64))),
        ('ktx2-zero-dims.seed',
         envelope(KTX2, hostile_ktx2_stream(0, 0, 1))),
        # Real frozen ETC1S container, plus the minimized malformed-tables one.
        ('ktx2-basislz-sample.seed',
         envelope(KTX2, (CORPUS.parent / 'basisu_sample.ktx2').read_bytes())),
        ('ktx2-basislz-malformed-tables.seed',
         envelope(KTX2, read_finding('basislz-etc1s-crash.ktx2'))),
        # --- WebP domain: metadata/dimension preflight ---
        ('webp-sample.seed', envelope(WEBP, read_corpus('sample.webp'))),
        ('webp-riff-only.seed',
         envelope(WEBP, b'RIFF' + struct.pack('<I', 0xFFFFFFFF) + b'WEBPVP8 '
                  + b'\x00' * 16)),
        ('webp-animated.seed',
         envelope(WEBP, b'RIFF' + struct.pack('<I', 64) + b'WEBPVP8X'
                  + b'\x00' * 48)),
        # --- WIC raster domain: PNG/JPEG metadata ---
        ('wic-png-over-dim.seed',
         envelope(WIC_RASTER, png_header(0xFFFF, 0xFFFF))),
        ('wic-png-zero-dim.seed', envelope(WIC_RASTER, png_header(0, 0))),
        ('wic-jpeg-soi.seed', envelope(WIC_RASTER, b'\xFF\xD8\xFF\xE0' + b'\x00' * 60)),
        # --- Sniff domain: byte sniffing + reference-extension policy ---
        ('sniff-webp.seed', envelope(SNIFF, b'RIFF\x00\x00\x00\x00WEBP')),
        ('sniff-ktx2.seed',
         envelope(SNIFF, bytes((0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30,
                                0xBB, 0x0D, 0x0A, 0x1A, 0x0A)))),
        ('sniff-extensions.seed',
         envelope(SNIFF, b'../../escape.png\0.jpg') + b' http://x/y.ktx2?q=1'),
    ]
    for name, blob in seeds:
        (args.output / name).write_bytes(blob)
    print(f'materialized {len(seeds)} glTF/codec fuzz seeds in {args.output}')


if __name__ == '__main__':
    main()