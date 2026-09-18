"""Real-app USD-008 activation, fallback, replacement, and relaunch smoke."""
import argparse
import base64
import ctypes
import hashlib
import json
import shutil
import subprocess
import time
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


def wait_for(predicate, seconds=20):
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


def ready(hwnd, path, source_format):
    return path.name in title(hwnd) and query(hwnd, 0) == 3 and query(hwnd, 17) == source_format


def prepare_fixtures():
    source = ROOT / 'tests' / 'fixtures' / 'usd-spike'
    target = ROOT / 'TestResults' / 'usd-008-activation' / 'models 雪'
    target.mkdir(parents=True, exist_ok=True)
    for fixture in source.glob('*.usda'):
        shutil.copyfile(fixture, target / fixture.name)
    shutil.copyfile(source / 'mesh.usda', target / 'fast-uppercase.USDA')
    shutil.copyfile(source / 'mesh.usda', target / 'sniffed-ascii.usd')
    (target / 'crate.USDC').write_bytes(base64.b64decode((source / 'cube.usdc.base64').read_bytes()))
    (target / 'archive.USDZ').write_bytes(base64.b64decode((source / 'cube.usdz.base64').read_bytes()))
    (target / 'malformed.usda').write_bytes(b'not a usd layer')
    (target / 'missing-composition.usda').write_text(
        '#usda 1.0\n(\n    subLayers = [@missing-required.usda@]\n)\n', encoding='utf-8')
    return target


def run(configuration):
    exe = ROOT / 'x64' / configuration / 'Preview3D.exe'
    fixtures = prepare_fixtures()
    paths = {path.name: path for path in fixtures.iterdir() if path.is_file()}
    checks = []
    primary = subprocess.Popen([str(exe), '--activation-smoke', '--open', str(paths['fast-uppercase.USDA'])])
    hwnd = None
    try:
        hwnd = wait_for(lambda: window_for_pid(primary.pid))
        wait_for(lambda: ready(hwnd, paths['fast-uppercase.USDA'], 9))
        checks.append('uppercase USDA direct command line')

        forward(exe, paths['sniffed-ascii.usd'])
        wait_for(lambda: ready(hwnd, paths['sniffed-ascii.usd'], 9))
        checks.append('generic .usd byte sniff through secondary activation')

        injected_open(hwnd, paths['crate.USDC'], 106)
        wait_for(lambda: ready(hwnd, paths['crate.USDC'], 10))
        checks.append('USDC picker route with detected crate metadata')

        injected_open(hwnd, paths['archive.USDZ'], 107)
        wait_for(lambda: ready(hwnd, paths['archive.USDZ'], 11))
        checks.append('USDZ one-file drop route with detected archive metadata')

        fallback = paths['compat-composition.usda']
        forward(exe, fallback)
        wait_for(lambda: ready(hwnd, fallback, 9))
        checks.append('composed stage through OpenUSD compatibility host')

        warning = paths['materials.usda']
        forward(exe, warning)
        wait_for(lambda: ready(hwnd, warning, 9) and query(hwnd, 40) > 0)
        checks.append('optional USD texture warning reaches viewer badge state')

        forward(exe, paths['malformed.usda'])
        wait_for(lambda: query(hwnd, 0) == 4 and query(hwnd, 45) and query(hwnd, 17) == 9)
        checks.append('fast USD failure retains prior usable content')
        forward(exe, paths['fast-uppercase.USDA'])
        wait_for(lambda: ready(hwnd, paths['fast-uppercase.USDA'], 9))

        forward(exe, paths['missing-composition.usda'])
        wait_for(lambda: query(hwnd, 0) == 4 and query(hwnd, 45) and query(hwnd, 17) == 9)
        checks.append('compatibility-host failure retains prior usable content')
        forward(exe, paths['fast-uppercase.USDA'])
        wait_for(lambda: ready(hwnd, paths['fast-uppercase.USDA'], 9))

        # Consecutive opens exercise both cancellation directions. The final
        # generation must be the only one published even if its predecessor
        # is still parsing or composing.
        forward(exe, paths['archive.USDZ'])
        forward(exe, fallback)
        wait_for(lambda: ready(hwnd, fallback, 9))
        checks.append('compatibility replacement supersedes fast USD')
        forward(exe, fallback)
        forward(exe, paths['fast-uppercase.USDA'])
        wait_for(lambda: ready(hwnd, paths['fast-uppercase.USDA'], 9))
        checks.append('fast USD replacement supersedes compatibility stage')

        USER32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        primary.wait(timeout=15)
        hwnd = None
        relaunched = subprocess.Popen([str(exe), '--activation-smoke', '--open', str(fallback)])
        try:
            relaunched_hwnd = wait_for(lambda: window_for_pid(relaunched.pid))
            wait_for(lambda: ready(relaunched_hwnd, fallback, 9))
            USER32.PostMessageW(relaunched_hwnd, WM_CLOSE, 0, 0)
            relaunched.wait(timeout=15)
        finally:
            if relaunched.poll() is None:
                relaunched.kill()
        checks.append('close and immediate compatibility relaunch')
        hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in paths.items()}
        return {'configuration': configuration, 'checks': checks, 'fixtureSha256': hashes, 'status': 'passed'}
    finally:
        if primary.poll() is None:
            if hwnd:
                USER32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
            try:
                primary.wait(timeout=8)
            except subprocess.TimeoutExpired:
                primary.kill()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--configuration', choices=('Debug', 'Release'), default='Debug')
    parser.add_argument('--result')
    args = parser.parse_args()
    result = run(args.configuration)
    rendered = json.dumps(result, indent=2)
    print(rendered)
    if args.result:
        Path(args.result).write_text(rendered + '\n', encoding='utf-8')
