"""Real-app 3MF-006 activation, viewer integration, and recovery smoke."""
import argparse
import base64
import ctypes
from ctypes import wintypes as W
import hashlib
import io
import json
import subprocess
import struct
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
USER32 = ctypes.windll.user32
WM_CLOSE, WM_COMMAND, WM_COPYDATA = 0x0010, 0x0111, 0x004A
SMOKE_QUERY = 0x8000 + 104
ULONG_PTR = ctypes.c_size_t


class CopyData(ctypes.Structure):
    _fields_ = [('dwData', ULONG_PTR), ('cbData', ctypes.c_ulong), ('lpData', ctypes.c_void_p)]


USER32.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ULONG_PTR, ctypes.c_ssize_t]
USER32.SendMessageW.restype = ctypes.c_ssize_t


def wait_for(predicate, seconds=25):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(.05)
    raise RuntimeError('timed out waiting for viewer state')


def window_for_pid(pid):
    found = []
    callback = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    @callback
    def visit(hwnd, _):
        owner = ctypes.c_ulong()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and USER32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    USER32.EnumWindows(visit, 0)
    return found[0] if found else None


def title(hwnd):
    text = ctypes.create_unicode_buffer(USER32.GetWindowTextLengthW(hwnd) + 1)
    USER32.GetWindowTextW(hwnd, text, len(text))
    return text.value


def query(hwnd, field):
    return USER32.SendMessageW(hwnd, SMOKE_QUERY, field, 0)


def query_with(hwnd, field, value):
    return USER32.SendMessageW(hwnd, SMOKE_QUERY, field, value)


def number(hwnd, field):
    return struct.unpack('<d', struct.pack('<Q', query(hwnd, field)))[0]


def forward(exe, path):
    process = subprocess.run([str(exe), '--open', str(path)], timeout=8, check=False)
    if process.returncode:
        raise RuntimeError(f'secondary activation returned {process.returncode}: {path}')


def injected_open(hwnd, path, kind):
    text = ctypes.create_unicode_buffer(str(path))
    data = CopyData(kind, ctypes.sizeof(text), ctypes.cast(text, ctypes.c_void_p))
    if not USER32.SendMessageW(hwnd, WM_COPYDATA, 0, ctypes.addressof(data)):
        raise RuntimeError('viewer rejected the injected app-smoke path')
    if kind == 106:
        USER32.SendMessageW(hwnd, WM_COMMAND, 32771, 0)


def ready(hwnd, path):
    return path.name in title(hwnd) and query(hwnd, 0) == 3 and query(hwnd, 17) == 12


def prepare_fixtures():
    source = ROOT / 'tests' / 'fixtures' / '3mf-spike'
    target = ROOT / 'TestResults' / '3mf-006-activation' / 'models 雪'
    target.mkdir(parents=True, exist_ok=True)
    names = {
        'core-box.3mf.base64': 'core-uppercase.3MF',
        'static-production.3mf.base64': 'production-boxes.3mf',
        'materials-texture.3mf.base64': 'materials-texture.3mf',
    }
    for encoded_name, decoded_name in names.items():
        (target / decoded_name).write_bytes(base64.b64decode((source / encoded_name).read_bytes()))
    # The upstream spike beam package is useful for lib3mf API coverage, but
    # its full appearance/clipping combination is not a viewer golden. Use the
    # same bounded parametric subset exercised by the adapter's focused test.
    model = '''<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US"
 xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
 xmlns:b="http://schemas.microsoft.com/3dmanufacturing/beamlattice/2017/02">
 <resources><object id="1" type="model"><mesh><vertices>
 <vertex x="0" y="0" z="0"/><vertex x="10" y="0" z="0"/>
 <vertex x="0" y="1" z="0"/>
 </vertices><triangles><triangle v1="0" v2="1" v3="2"/></triangles>
 <b:beamlattice minlength="0.01" radius="1"><b:beams>
 <b:beam v1="0" v2="1"/></b:beams></b:beamlattice>
 </mesh></object></resources><build><item objectid="1"/></build></model>'''
    content_types = ('<?xml version="1.0" encoding="UTF-8"?>'
                     '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                     '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                     '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>'
                     '</Types>')
    relationships = ('<?xml version="1.0" encoding="UTF-8"?>'
                     '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                     '<Relationship Target="/3D/3dmodel.model" Id="rel0" '
                     'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>'
                     '</Relationships>')
    package = io.BytesIO()
    with zipfile.ZipFile(package, 'w', compression=zipfile.ZIP_STORED) as archive:
        for name, data in (('[Content_Types].xml', content_types),
                           ('_rels/.rels', relationships),
                           ('3D/3dmodel.model', model)):
            entry = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_STORED
            archive.writestr(entry, data)
    (target / 'beam-lattice.3mf').write_bytes(package.getvalue())
    (target / 'malformed.3mf').write_bytes(b'not an OPC package')
    return target


def run(configuration, exe_override=None):
    exe = exe_override or ROOT / 'x64' / configuration / 'Preview3D.exe'
    fixtures = prepare_fixtures()
    paths = {path.name: path for path in fixtures.iterdir() if path.is_file()}
    checks = []
    primary = subprocess.Popen([
        str(exe), '--activation-smoke', '--open', str(paths['core-uppercase.3MF'])])
    hwnd = None
    original_axis = None
    original_native = None
    try:
        hwnd = wait_for(lambda: window_for_pid(primary.pid))
        wait_for(lambda: ready(hwnd, paths['core-uppercase.3MF']))
        original_axis = query(hwnd, 71)
        original_native = query(hwnd, 21)
        checks.append('uppercase 3MF direct command line')

        time.sleep(1.0)  # Let the bounded initial camera fit finish easing.
        projected = query(hwnd, 29)
        completions = query(hwnd, 32)
        point = W.POINT(projected & 0xffff, projected >> 16)
        USER32.ClientToScreen(hwnd, ctypes.byref(point))
        USER32.SetCursorPos(point.x, point.y)
        time.sleep(.03)
        USER32.SendMessageW(hwnd, 0x0201, 1, projected)
        USER32.SendMessageW(hwnd, 0x0202, 0, projected)
        wait_for(lambda: query(hwnd, 32) > completions)
        # A GPU pick completion proves the document's ordinary pick path ran;
        # the projected bounds center is not guaranteed to cover authored
        # surface geometry, so a hit is not asserted from that one point.
        USER32.SendMessageW(hwnd, WM_COMMAND, 32772, 0)  # Fit remains document-wide.
        USER32.SendMessageW(hwnd, WM_COMMAND, 32773, 0)  # Reset.
        checks.append('GPU pick request and document-level Fit/Reset')

        forward(exe, paths['production-boxes.3mf'])
        wait_for(lambda: ready(hwnd, paths['production-boxes.3mf']) and query(hwnd, 20) >= 2
                 and query(hwnd, 8) > 0)
        if query(hwnd, 16) != 1 or query(hwnd, 14) < 2:
            raise RuntimeError('Production did not publish verified geometry and bounds')
        USER32.SendMessageW(hwnd, WM_COMMAND, 32789, 0)  # Info.
        before = query(hwnd, 7)
        query_with(hwnd, 70, 1)  # X ground axis for the whole document.
        wait_for(lambda: query(hwnd, 7) > before)
        if number(hwnd, 23) < 0 or number(hwnd, 24) < 0 or number(hwnd, 25) < 0:
            raise RuntimeError('Production bounds were invalid after ground-axis change')
        USER32.SendMessageW(hwnd, WM_COMMAND, 32789, 0)
        query_with(hwnd, 70, original_axis)
        if original_native and not query(hwnd, 21):
            query(hwnd, 30)
        USER32.SendMessageW(hwnd, WM_COMMAND, 32791, 0)  # Fullscreen on/off.
        USER32.SendMessageW(hwnd, WM_COMMAND, 32791, 0)
        checks.append('Production root build through secondary activation with all occurrences')

        injected_open(hwnd, paths['materials-texture.3mf'], 106)
        wait_for(lambda: ready(hwnd, paths['materials-texture.3mf']) and query(hwnd, 19) > 0)
        checks.append('Materials and contained texture through picker route')

        injected_open(hwnd, paths['beam-lattice.3mf'], 107)
        try:
            wait_for(lambda: ready(hwnd, paths['beam-lattice.3mf']) and query(hwnd, 14) > 0)
        except RuntimeError as error:
            raise RuntimeError(
                f'beam fixture: title={title(hwnd)!r}, state={query(hwnd, 0)}, '
                f'format={query(hwnd, 17)}, triangles={query(hwnd, 14)}, '
                f'warningBytes={query(hwnd, 40)}') from error
        checks.append('Beam Lattice through one-file drop route')

        prior_triangles = query(hwnd, 14)
        forward(exe, paths['malformed.3mf'])
        wait_for(lambda: query(hwnd, 0) == 4 and query(hwnd, 45) and query(hwnd, 17) == 12)
        if query(hwnd, 14) != prior_triangles:
            raise RuntimeError('malformed 3MF did not retain the prior usable model')
        checks.append('malformed 3MF retains prior usable content')

        forward(exe, paths['core-uppercase.3MF'])
        wait_for(lambda: ready(hwnd, paths['core-uppercase.3MF']))
        checks.append('valid 3MF recovers without relaunch')

        # The same shipping route must recover from a worker crash and hang.
        for fault, label in ((1, 'crash'), (2, 'timeout')):
            query_with(hwnd, 44, fault)
            forward(exe, paths['production-boxes.3mf'])
            wait_for(lambda: query(hwnd, 0) == 4 and query(hwnd, 45))
            query_with(hwnd, 44, 0)
            forward(exe, paths['core-uppercase.3MF'])
            wait_for(lambda: ready(hwnd, paths['core-uppercase.3MF']))
            checks.append(f'{label} recovery preserves the later valid 3MF open')

        injected_open(hwnd, paths['production-boxes.3mf'], 105)
        if query(hwnd, 0) != 3 or query(hwnd, 17) != 12:
            raise RuntimeError('cancel did not retain the prior Ready 3MF document')
        checks.append('pending 3MF cancellation retains prior document')

        forward(exe, paths['production-boxes.3mf'])
        forward(exe, paths['core-uppercase.3MF'])
        wait_for(lambda: ready(hwnd, paths['core-uppercase.3MF']))
        checks.append('replacement publishes only the latest 3MF generation')

        USER32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        primary.wait(timeout=15)
        hwnd = None
        relaunched = subprocess.Popen([
            str(exe), '--activation-smoke', '--open', str(paths['production-boxes.3mf'])])
        try:
            relaunched_hwnd = wait_for(lambda: window_for_pid(relaunched.pid))
            wait_for(lambda: ready(relaunched_hwnd, paths['production-boxes.3mf']))
            USER32.PostMessageW(relaunched_hwnd, WM_CLOSE, 0, 0)
            relaunched.wait(timeout=15)
        finally:
            if relaunched.poll() is None:
                relaunched.kill()
        checks.append('close and immediate 3MF relaunch')
        hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest()
                  for name, path in paths.items()}
        return {'configuration': configuration, 'checks': checks,
                'fixtureSha256': hashes, 'status': 'passed'}
    finally:
        if primary.poll() is None:
            if hwnd:
                query_with(hwnd, 44, 0)
                if original_axis is not None:
                    query_with(hwnd, 70, original_axis)
                if original_native is not None and query(hwnd, 21) != original_native:
                    query(hwnd, 30)
                USER32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
            try:
                primary.wait(timeout=8)
            except subprocess.TimeoutExpired:
                primary.kill()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--configuration', choices=('Debug', 'Release'), default='Debug')
    parser.add_argument('--exe', type=Path, help='staged Preview3D.exe to smoke')
    parser.add_argument('--result')
    args = parser.parse_args()
    result = run(args.configuration, args.exe)
    rendered = json.dumps(result, indent=2)
    print(rendered)
    if args.result:
        Path(args.result).write_text(rendered + '\n', encoding='utf-8')
