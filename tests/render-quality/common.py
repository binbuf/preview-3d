"""Shared primitives for the T20 render-quality evidence lane.

The module deliberately has no third-party dependencies: PNG encode/decode,
viewport cropping, deterministic sampling and the small comparison metrics are
implemented here so verification can run on a clean Windows agent. The viewer
window capture helpers are reused from ``tests/app-smoke`` (PrintWindow +
PW_RENDERFULLCONTENT), which is the same path the T14-T18 evidence used.
"""
from __future__ import annotations

import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[2]
APP_SMOKE = ROOT / 'tests' / 'app-smoke'
if str(APP_SMOKE) not in sys.path:
    sys.path.insert(0, str(APP_SMOKE))

from run import CopyData, find_window, send, user  # noqa: E402
from studio import capture as capture_window  # noqa: E402
from studio import png, wait_for_window  # noqa: E402

RESULT_SCHEMA = 1


def sha256_file(path: Path) -> str:
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_json(path: Path, value) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def read_json(path: Path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def decode_png(data: bytes):
    """Decode an 8-bit RGB or RGBA non-interlaced PNG to (w, h, rgb bytes)."""
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    at, width = 8, 0
    height = depth = color = 0
    compressed = bytearray()
    while at < len(data):
        length = struct.unpack_from('>I', data, at)[0]
        kind = data[at + 4:at + 8]
        chunk = data[at + 8:at + 8 + length]
        at += length + 12
        if kind == b'IHDR':
            width, height, depth, color, compression, filtering, interlace = \
                struct.unpack('>IIBBBBB', chunk)
            if (depth, compression, filtering, interlace) != (8, 0, 0, 0):
                raise ValueError('unsupported PNG encoding')
            if color not in (2, 6):
                raise ValueError(f'unsupported PNG colour type {color}')
        elif kind == b'IDAT':
            compressed += chunk
        elif kind == b'IEND':
            break
    raw = zlib.decompress(bytes(compressed))
    channels = 4 if color == 6 else 3
    stride = width * channels
    previous = bytearray(stride)
    rgb = bytearray(width * height * 3)
    cursor = 0
    for y in range(height):
        filter_type = raw[cursor]
        cursor += 1
        line = bytearray(raw[cursor:cursor + stride])
        cursor += stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            upper_left = previous[i - channels] if i >= channels else 0
            if filter_type == 1:
                line[i] = (line[i] + left) & 0xFF
            elif filter_type == 2:
                line[i] = (line[i] + up) & 0xFF
            elif filter_type == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif filter_type == 4:
                p = left + up - upper_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upper_left)
                nearest = left if pa <= pb and pa <= pc else (up if pb <= pc else upper_left)
                line[i] = (line[i] + nearest) & 0xFF
            elif filter_type != 0:
                raise ValueError(f'unsupported PNG filter {filter_type}')
        previous = line
        out = y * width * 3
        if channels == 3:
            rgb[out:out + width * 3] = line
        else:
            for x in range(width):
                src = x * 4
                dst = out + x * 3
                rgb[dst:dst + 3] = line[src:src + 3]
    return width, height, bytes(rgb)


def crop(width: int, height: int, rgb: bytes, rect):
    """Crop normalised (x0, y0, x1, y1) to a new RGB image."""
    x0, y0, x1, y1 = rect
    left = max(0, min(width - 1, int(round(x0 * width))))
    top = max(0, min(height - 1, int(round(y0 * height))))
    right = max(left + 1, min(width, int(round(x1 * width))))
    bottom = max(top + 1, min(height, int(round(y1 * height))))
    rows = [rgb[(y * width + left) * 3:(y * width + right) * 3] for y in range(top, bottom)]
    return right - left, bottom - top, b''.join(rows)


def downsampled(width: int, height: int, rgb: bytes, size: int = 64):
    samples = []
    for gy in range(size):
        y = min(height - 1, (gy * height) // size)
        for gx in range(size):
            x = min(width - 1, (gx * width) // size)
            i = (y * width + x) * 3
            samples.append((rgb[i], rgb[i + 1], rgb[i + 2]))
    return samples


def mae(a, b) -> float:
    return sum(abs(x - y) for pa, pb in zip(a, b) for x, y in zip(pa, pb)) / (len(a) * 3) / 255.0


def ssim(a, b) -> float:
    def gray(samples):
        return [0.2126 * r + 0.7152 * g + 0.0722 * b for r, g, b in samples]
    x, y = gray(a), gray(b)
    n = len(x)
    mean_x, mean_y = sum(x) / n, sum(y) / n
    var_x = sum((v - mean_x) ** 2 for v in x) / n
    var_y = sum((v - mean_y) ** 2 for v in y) / n
    cov = sum((u - mean_x) * (v - mean_y) for u, v in zip(x, y)) / n
    c1 = (0.01 * 255) ** 2
    c2 = (0.03 * 255) ** 2
    return ((2 * mean_x * mean_y + c1) * (2 * cov + c2)) / (
        (mean_x ** 2 + mean_y ** 2 + c1) * (var_x + var_y + c2))


def image_stats(width: int, height: int, rgb: bytes):
    """Cheap evidence that a capture is not blank or a single flat colour."""
    step = max(1, min(width, height) // 96)
    values = []
    unique = set()
    for y in range(0, height, step):
        for x in range(0, width, step):
            i = (y * width + x) * 3
            pixel = (rgb[i], rgb[i + 1], rgb[i + 2])
            values.append(sum(pixel) / 3.0)
            unique.add(pixel)
    mean = sum(values) / len(values) if values else 0.0
    variance = sum((v - mean) ** 2 for v in values) / len(values) if values else 0.0
    return {
        'meanLuma': round(mean, 3),
        'lumaStdDev': round(math.sqrt(variance), 3),
        'uniqueSampleColours': len(unique),
        'pixels': width * height,
    }


def capture_region(hwnd, dpi: int, toolbar_px: int = 52, bottom_px: int = 44):
    """Capture the client area and crop out the viewer chrome.

    The returned rectangle is the physical viewport in device pixels. The
    PrintWindow path does not include the mouse cursor, and callers must not
    hover a control while capturing, so tooltips cannot be present.
    """
    width, height, rgb = capture_window(hwnd)
    scale = dpi / 96.0
    top = min(height - 1, int(round(toolbar_px * scale)))
    bottom = max(top + 1, height - int(round(bottom_px * scale)))
    rows = [rgb[(y * width) * 3:(y * width + width) * 3] for y in range(top, bottom)]
    viewport = b''.join(rows)
    return width, bottom - top, viewport, {'left': 0, 'top': top, 'width': width, 'height': bottom - top}


def encoded_png(width: int, height: int, rgb: bytes) -> bytes:
    return png(width, height, rgb)


class _BITMAPINFOHEADER(C.Structure):
    _fields_ = [('biSize', W.DWORD), ('biWidth', W.LONG), ('biHeight', W.LONG),
                ('biPlanes', W.WORD), ('biBitCount', W.WORD),
                ('biCompression', W.DWORD), ('biSizeImage', W.DWORD),
                ('biXPelsPerMeter', W.LONG), ('biYPelsPerMeter', W.LONG),
                ('biClrUsed', W.DWORD), ('biClrImportant', W.DWORD)]


def send_field(hwnd, field, value=0):
    return send(hwnd, 0x8000 + 104, field, value)


def query(hwnd, field):
    return send_field(hwnd, field, 0) & 0xFFFFFFFF


def query_test(hwnd, field):
    """Read a T20 camera field (lParam == -1), never mutating it."""
    value = send_field(hwnd, field, -1) & 0xFFFFFFFFFFFFFFFF
    if value >= 1 << 63:
        value -= 1 << 64
    return value


def query_u64(hwnd, field):
    return send_field(hwnd, field, 0) & 0xFFFFFFFFFFFFFFFF


def query_f64(hwnd, field):
    return struct.unpack('<d', struct.pack('<Q', query_u64(hwnd, field)))[0]
