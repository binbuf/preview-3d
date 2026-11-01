"""Generate the repository-owned TGA/DDS/ORM/HDR/EXR texture fixtures.

All bytes are produced by this script; no third-party asset is copied. Run from
the repository root:

    python tests/fixtures/textures/generate_textures.py

The tiny textures are deterministic so tests can assert exact colours.
"""
from __future__ import annotations

import binascii
import math
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def tga_header(width: int, height: int, depth: int, rle: bool, top: bool = False) -> bytes:
    descriptor = 0x20 if top else 0x00
    if depth == 32:
        descriptor |= 8
    return struct.pack(
        "<BBBBHHHHHHBB",
        0,          # id length
        0,          # no colour map
        10 if rle else 2,
        0,          # colour-map entry size
        0, 0,       # colour-map length/first index
        0, 0,       # origin
        width, height,
        depth,
        descriptor,
    )


def pixels(width: int, height: int, alpha: bool) -> list[tuple[int, int, int, int]]:
    out = []
    for y in range(height):
        for x in range(width):
            r = (x * 255) // max(1, width - 1)
            g = (y * 255) // max(1, height - 1)
            b = 96 if (x + y) % 2 else 32
            a = ((x + 1) * 255) // width if alpha else 255
            out.append((r, g, b, a))
    return out


def encode_tga(width: int, height: int, depth: int, rle: bool, top: bool = False) -> bytes:
    data = bytearray(tga_header(width, height, depth, rle, top))
    coloured = pixels(width, height, depth == 32)
    rows = [coloured[y * width:(y + 1) * width] for y in range(height)]
    if not top:
        rows = list(reversed(rows))  # TGA's default origin is bottom-left
    bytes_per_pixel = depth // 8

    def pixel_bytes(p: tuple[int, int, int, int]) -> bytes:
        r, g, b, a = p
        return bytes((b, g, r, a))[:bytes_per_pixel]

    for row in rows:
        if not rle:
            for p in row:
                data += pixel_bytes(p)
            continue
        # Literal packets capped at 128 pixels keep the RLE stream simple.
        for start in range(0, len(row), 128):
            packet = row[start:start + 128]
            data.append(len(packet) - 1)
            for p in packet:
                data += pixel_bytes(p)
    return bytes(data)


def dds_header(width: int, height: int, *, mip_count: int = 1, fourcc: bytes | None = None,
               dxgi: int | None = None, rgb_bits: int = 0, masks: tuple[int, int, int, int] = (0, 0, 0, 0),
               pitch: int = 0) -> bytes:
    flags = 0x1 | 0x2 | 0x4 | 0x1000  # caps|height|width|pitch
    if mip_count > 1:
        flags |= 0x20000
    if fourcc is not None:
        pf_flags = 0x4
        pf_fourcc = fourcc
    else:
        pf_flags = 0x1 | 0x40
        pf_fourcc = b"\0\0\0\0"
    if dxgi is not None:
        pf_flags = 0x4
        pf_fourcc = b"DX10"
    header = struct.pack(
        "<IIIIIII11I", 124, flags, height, width, pitch, 0, mip_count, *([0] * 11)
    )
    r, g, b, a = masks
    pixel_format = struct.pack("<II4sIIIII", 32, pf_flags, pf_fourcc, rgb_bits, r, g, b, a)
    caps = 0x1000 | (0x400000 if fourcc is not None or dxgi is not None else 0)
    if mip_count > 1:
        caps |= 0x8 | 0x400000
    header += pixel_format + struct.pack("<IIIII", caps, 0, 0, 0, 0)
    assert len(header) == 124, len(header)
    out = b"DDS " + header
    if dxgi is not None:
        out += struct.pack("<IIIII", dxgi, 3, 0, 1, 0)
    return out


def block_bytes(width: int, height: int, bytes_per_block: int) -> bytes:
    blocks = ((width + 3) // 4) * ((height + 3) // 4)
    return bytes(bytes_per_block * blocks)


# --- Radiance HDR (RGBE) ---------------------------------------------------

def encode_rgbe(r: float, g: float, b: float) -> bytes:
    maximum = max(r, g, b)
    if maximum < 1e-32:
        return bytes((0, 0, 0, 0))
    exponent = math.frexp(maximum)[1]          # maximum < 2**exponent
    scale = math.ldexp(1.0, exponent - 8)      # decoded value = byte * scale
    channel = lambda value: max(0, min(255, int(round(value / scale))))
    return bytes((channel(r), channel(g), channel(b), exponent + 128))


def hdr_values(x: int, y: int) -> tuple[float, float, float]:
    # (0, 0) is an exact HDR value above 1.0; the rest is a small gradient.
    red = 4.0 if x == 0 and y == 0 else 1.0 + 0.25 * x
    green = 0.5 + 0.125 * y
    blue = 0.25
    return red, green, blue


def hdr_new_rle_row(width: int, texels: list[tuple[float, float, float]]) -> bytes:
    out = bytearray((2, 2, (width >> 8) & 0xFF, width & 0xFF))
    encoded = [encode_rgbe(*texel) for texel in texels]
    for channel in range(4):
        values = [pixel[channel] for pixel in encoded]
        for start in range(0, width, 128):
            packet = values[start:start + 128]
            out.append(len(packet))
            out += bytes(packet)
    return bytes(out)


def encode_hdr(width: int, height: int, rle: bool) -> bytes:
    header = bytearray(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n")
    header += f"-Y {height} +X {width}\n".encode("ascii")
    for y in range(height):
        row = [hdr_values(x, y) for x in range(width)]
        if rle and 8 <= width <= 32767:
            header += hdr_new_rle_row(width, row)
        else:
            header += b"".join(encode_rgbe(*texel) for texel in row)
    return bytes(header)


# --- OpenEXR ---------------------------------------------------------------

EXR_NONE, EXR_RLE, EXR_ZIPS, EXR_ZIP, EXR_PIZ = 0, 1, 2, 3, 4
EXR_HALF, EXR_FLOAT = 1, 2


def exr_attribute(name: str, kind: str, payload: bytes) -> bytes:
    return (name.encode("ascii") + b"\0" + kind.encode("ascii") + b"\0"
            + struct.pack("<i", len(payload)) + payload)


def exr_channel(name: str, pixel_type: int) -> bytes:
    return (name.encode("ascii") + b"\0" + struct.pack("<i", pixel_type)
            + bytes((0, 0, 0, 0)) + struct.pack("<ii", 1, 1))


def exr_zip_payload(raw: bytes) -> bytes:
    size = len(raw)
    reordered = bytearray(size)
    first, second = 0, (size + 1) // 2
    index = 0
    while True:
        if index < size:
            reordered[first] = raw[index]
            first += 1
            index += 1
        else:
            break
        if index < size:
            reordered[second] = raw[index]
            second += 1
            index += 1
        else:
            break
    previous = reordered[0]
    for i in range(1, size):
        current = reordered[i]
        reordered[i] = (current - previous + 128 + 256) & 0xFF
        previous = current
    return zlib.compress(bytes(reordered), 9)


def encode_exr(width: int, height: int, channels: list[tuple[str, int]],
               values: list[list[float]], compression: int) -> bytes:
    channel_list = b"".join(exr_channel(name, pixel_type)
                            for name, pixel_type in channels) + b"\0"
    header = bytearray(struct.pack("<II", 20000630, 2))
    header += exr_attribute("channels", "chlist", channel_list)
    header += exr_attribute("compression", "compression", bytes((compression,)))
    box = struct.pack("<iiii", 0, 0, width - 1, height - 1)
    header += exr_attribute("dataWindow", "box2i", box)
    header += exr_attribute("displayWindow", "box2i", box)
    header += exr_attribute("lineOrder", "lineOrder", b"\0")
    header += exr_attribute("pixelAspectRatio", "float", struct.pack("<f", 1.0))
    header += exr_attribute("screenWindowCenter", "v2f",
                            struct.pack("<ff", 0.0, 0.0))
    header += exr_attribute("screenWindowWidth", "float", struct.pack("<f", 1.0))
    header += b"\0"

    scanlines = 16 if compression == EXR_ZIP else 1
    chunk_count = (height + scanlines - 1) // scanlines
    offset_table_size = 8 * chunk_count
    position = len(header) + offset_table_size
    offset_table = bytearray()
    chunks = bytearray()
    for start in range(0, height, scanlines):
        raw = bytearray()
        for y in range(start, min(height, start + scanlines)):
            for (_, pixel_type), data in zip(channels, values):
                for x in range(width):
                    value = data[y * width + x]
                    raw += struct.pack("<e", value) if pixel_type == EXR_HALF \
                        else struct.pack("<f", value)
        payload = bytes(raw) if compression == EXR_NONE else exr_zip_payload(bytes(raw))
        offset_table += struct.pack("<Q", position)
        chunk = struct.pack("<ii", start, len(payload)) + payload
        chunks += chunk
        position += len(chunk)
    return bytes(header) + bytes(offset_table) + bytes(chunks)


def exr_color_channels(width: int, height: int):
    red = [hdr_values(x, y)[0] for y in range(height) for x in range(width)]
    green = [hdr_values(x, y)[1] for y in range(height) for x in range(width)]
    blue = [hdr_values(x, y)[2] for y in range(height) for x in range(width)]
    return [("R", EXR_HALF), ("G", EXR_HALF), ("B", EXR_HALF)], [red, green, blue]


def main() -> None:
    (ROOT / "tga24_uncompressed.tga").write_bytes(encode_tga(4, 4, 24, False))
    (ROOT / "tga32_uncompressed.tga").write_bytes(encode_tga(4, 4, 32, False, top=True))
    (ROOT / "tga24_rle.tga").write_bytes(encode_tga(8, 8, 24, True))
    (ROOT / "tga32_rle.tga").write_bytes(encode_tga(8, 8, 32, True))

    (ROOT / "dds_bc1.dds").write_bytes(dds_header(8, 8, fourcc=b"DXT1") + block_bytes(8, 8, 8))
    bc1_mips = (block_bytes(8, 8, 8) + block_bytes(4, 4, 8)
                + block_bytes(2, 2, 8) + block_bytes(1, 1, 8))
    (ROOT / "dds_bc1_mips.dds").write_bytes(dds_header(8, 8, mip_count=4, fourcc=b"DXT1") + bc1_mips)
    (ROOT / "dds_bc3.dds").write_bytes(dds_header(8, 8, fourcc=b"DXT5") + block_bytes(8, 8, 16))
    (ROOT / "dds_bc5.dds").write_bytes(dds_header(8, 8, fourcc=b"ATI2") + block_bytes(8, 8, 16))
    (ROOT / "dds_bc7.dds").write_bytes(dds_header(8, 8, dxgi=98) + block_bytes(8, 8, 16))
    rgba = bytes(byte for y in range(4) for x in range(4)
                 for byte in (x * 32 % 256, y * 32 % 256, (x + y) * 16 % 256, 255))
    (ROOT / "dds_rgba8.dds").write_bytes(
        dds_header(4, 4, rgb_bits=32, masks=(0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000),
                   pitch=16) + rgba)
    # A header that passes the fixed checks but whose declared blocks are absent.
    (ROOT / "dds_truncated.dds").write_bytes(
        dds_header(8, 8, fourcc=b"DXT5") + block_bytes(8, 8, 16)[:20])

    # --- HDR/EXR fixtures -------------------------------------------------
    hdr_color = encode_hdr(8, 8, rle=True)
    (ROOT / "hdr_color.hdr").write_bytes(hdr_color)
    (ROOT / "hdr_flat.hdr").write_bytes(encode_hdr(4, 4, rle=False))

    color_channels, color_values = exr_color_channels(4, 4)
    exr_color = encode_exr(4, 4, color_channels, color_values, EXR_ZIP)
    (ROOT / "exr_color.exr").write_bytes(exr_color)

    rgba_values = [
        [2.0] + [1.0] * 15,
        [0.5] + [1.0] * 15,
        [0.25] + [0.25] * 15,
        [0.5] + [1.0] * 15,
    ]
    (ROOT / "exr_float_rgba.exr").write_bytes(
        encode_exr(4, 4, [("R", EXR_FLOAT), ("G", EXR_FLOAT), ("B", EXR_FLOAT),
                          ("A", EXR_FLOAT)], rgba_values, EXR_NONE))

    half_rgb = [[0.5] * 16, [0.5] * 16, [1.0] * 16]
    normal_exr = encode_exr(4, 4, [("R", EXR_HALF), ("G", EXR_HALF), ("B", EXR_HALF)],
                            half_rgb, EXR_ZIP)
    (ROOT / "exr_normal.exr").write_bytes(normal_exr)
    (ROOT / "exr_roughness.exr").write_bytes(
        encode_exr(4, 4, [("Y", EXR_HALF)], [[0.25] * 16], EXR_ZIPS))

    (ROOT / "exr_truncated.exr").write_bytes(exr_color[:-32])
    (ROOT / "exr_tiled.exr").write_bytes(struct.pack("<II", 20000630, 2 | 0x200))
    (ROOT / "exr_deep.exr").write_bytes(struct.pack("<II", 20000630, 2 | 0x800))
    (ROOT / "exr_multipart.exr").write_bytes(struct.pack("<II", 20000630, 2 | 0x1000))
    (ROOT / "exr_piz.exr").write_bytes(
        encode_exr(4, 4, color_channels, color_values, EXR_PIZ))
    (ROOT / "exr_uint_channel.exr").write_bytes(
        encode_exr(4, 4, [("R", 0), ("G", 0), ("B", 0)],
                   [color_values[0], color_values[1], color_values[2]], EXR_NONE))

    (ROOT / "hdr_bad_magic.hdr").write_bytes(b"#?NOTRADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 4\n")
    (ROOT / "hdr_bad_format.hdr").write_bytes(
        b"#?RADIANCE\nFORMAT=32-bit_rle_xyze\n\n-Y 4 +X 4\n" + b"\0" * 64)
    (ROOT / "hdr_bad_resolution.hdr").write_bytes(
        b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\nY 4 X 4\n" + b"\0" * 64)
    (ROOT / "hdr_truncated.hdr").write_bytes(hdr_color[:-16])

    bad_exr = ROOT / "bad-exr"
    bad_exr.mkdir(exist_ok=True)
    (bad_exr / "bad.exr").write_bytes(exr_color[:-32])
    (bad_exr / "scene.mtl").write_text(
        "newmtl Material\nKd 1 1 1\nmap_Kd bad.exr\n", encoding="ascii")
    (bad_exr / "scene.obj").write_text(
        "mtllib scene.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl Material\nf 1 2 3\n",
        encoding="ascii")

    package = ROOT / "package"
    package.mkdir(exist_ok=True)
    (package / "albedo.tga").write_bytes((ROOT / "tga24_uncompressed.tga").read_bytes())
    (package / "normal.dds").write_bytes((ROOT / "dds_bc5.dds").read_bytes())
    (package / "scene.mtl").write_text(
        "# Repository-owned fixture: TGA base colour and DDS BC5 normal map.\n"
        "newmtl Material\n"
        "Kd 1 1 1\n"
        "map_Kd albedo.tga\n"
        "norm normal.dds\n",
        encoding="ascii",
    )
    (package / "scene.obj").write_text(
        "# Repository-owned fixture for the TGA/DDS decode path.\n"
        "mtllib scene.mtl\n"
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 0 1\n"
        "vn 0 0 1\n"
        "usemtl Material\n"
        "f 1/1/1 2/2/1 3/3/1\n",
        encoding="ascii",
    )

    # An OBJ package with EXR base colour and EXR normal maps. This is the
    # "EXR normal map used instead of a missing-material warning" fixture.
    exr_package = ROOT / "exr"
    exr_package.mkdir(exist_ok=True)
    (exr_package / "albedo.exr").write_bytes(exr_color)
    (exr_package / "normal.exr").write_bytes(normal_exr)
    (exr_package / "scene.mtl").write_text(
        "# Repository-owned fixture: EXR base colour and normal maps.\n"
        "newmtl Material\n"
        "Kd 1 1 1\n"
        "map_Kd albedo.exr\n"
        "norm normal.exr\n",
        encoding="ascii",
    )
    (exr_package / "scene.obj").write_text(
        "# Repository-owned fixture for the EXR decode path.\n"
        "mtllib scene.mtl\n"
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 0 1\n"
        "vn 0 0 1\n"
        "usemtl Material\n"
        "f 1/1/1 2/2/1 3/3/1\n",
        encoding="ascii",
    )

    # glTF + sidecar EXR albedo/normal maps, exercising the external-image path.
    exr_gltf = ROOT / "exr-gltf"
    exr_gltf.mkdir(exist_ok=True)
    (exr_gltf / "albedo.exr").write_bytes(exr_color)
    (exr_gltf / "normal.exr").write_bytes(normal_exr)
    (exr_gltf / "mesh.bin").write_bytes(
        struct.pack("<9f", -1, -1, 0, 1, -1, 0, 0, 1, 0))
    (exr_gltf / "scene.gltf").write_text(
        '{"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],'
        '"nodes":[{"mesh":0}],'
        '"meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],'
        '"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}},'
        '"normalTexture":{"index":1}}],'
        '"textures":[{"source":0},{"source":1}],'
        '"images":[{"uri":"albedo.exr"},{"uri":"normal.exr"}],'
        '"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],'
        '"bufferViews":[{"buffer":0,"byteLength":36}],'
        '"buffers":[{"uri":"mesh.bin","byteLength":36}]}',
        encoding="ascii",
    )

    # A TGA-named file whose header is invalid: the optional map must fall back
    # with a warning rather than reach the decoder as a texture.
    bad = ROOT / "bad"
    bad.mkdir(exist_ok=True)
    (bad / "bad.tga").write_bytes(bytes(range(18)))
    (bad / "bad.dds").write_bytes((ROOT / "dds_truncated.dds").read_bytes())
    (bad / "scene.mtl").write_text(
        "newmtl Material\nKd 1 1 1\nmap_Kd bad.tga\n", encoding="ascii")
    (bad / "scene.obj").write_text(
        "mtllib scene.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl Material\nf 1 2 3\n",
        encoding="ascii")
    (bad / "scene-dds.mtl").write_text(
        "newmtl Material\nKd 1 1 1\nnorm bad.dds\n", encoding="ascii")
    (bad / "scene-dds.obj").write_text(
        "mtllib scene-dds.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\n"
        "usemtl Material\nf 1/1 2/2 3/3\n",
        encoding="ascii")

    # ORM: R carries a non-one occlusion value (32), G roughness, B metallic.
    orm = ROOT / "orm"
    orm.mkdir(exist_ok=True)
    def png_rgba(width: int, height: int, colour: tuple[int, int, int, int]) -> bytes:
        raw = b"".join(b"\x00" + bytes(colour) * width for _ in range(height))
        def chunk(kind: bytes, payload: bytes) -> bytes:
            return (struct.pack(">I", len(payload)) + kind + payload
                    + struct.pack(">I", binascii.crc32(kind + payload) & 0xFFFFFFFF))
        ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
        return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    (orm / "orm.png").write_bytes(png_rgba(4, 4, (32, 200, 20, 255)))
    positions = struct.pack("<9f", -1, -1, 0, 1, -1, 0, 0, 1, 0)
    (orm / "mesh.bin").write_bytes(positions)
    (orm / "scene.gltf").write_text(
        '{"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],'
        '"nodes":[{"mesh":0}],'
        '"meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],'
        '"materials":[{"pbrMetallicRoughness":{"metallicFactor":1.0,"roughnessFactor":1.0,'
        '"metallicRoughnessTexture":{"index":0}}}],'
        '"textures":[{"source":0}],"images":[{"uri":"orm.png"}],'
        '"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],'
        '"bufferViews":[{"buffer":0,"byteLength":36}],'
        '"buffers":[{"uri":"mesh.bin","byteLength":36}]}',
        encoding="ascii",
    )


if __name__ == "__main__":
    main()
