"""Real-app STEP-007 activation, viewer integration, and recovery smoke."""
import argparse
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
STEP_FORMAT = 13
MALFORMED_DATA = 1
UNSUPPORTED_REQUIRED_FEATURE = 10
STEP_PHASE_EMIT = 6


class CopyData(ctypes.Structure):
    _fields_ = [('dwData', ULONG_PTR), ('cbData', ctypes.c_ulong), ('lpData', ctypes.c_void_p)]


USER32.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ULONG_PTR, ctypes.c_ssize_t]
USER32.SendMessageW.restype = ctypes.c_ssize_t


def wait_for(predicate, seconds=30):
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


def forward(exe, path):
    process = subprocess.run([str(exe), '--open', str(path)], timeout=10, check=False)
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
    return (path.name in title(hwnd) and query(hwnd, 0) == 3
            and query(hwnd, 17) == STEP_FORMAT and query(hwnd, 14) > 0)


def prepare_fixtures():
    source = ROOT / 'tests' / 'fixtures' / 'stp-spike'
    target = ROOT / 'TestResults' / 'step-007-activation' / 'models 雪'
    target.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / 'part_ap203.stp', target / 'part-uppercase.STEP')
    shutil.copyfile(source / 'part_ap214.stp', target / 'part-ap214.stp')
    shutil.copyfile(source / 'assembly_ap214.stp', target / 'assembly-ap214.stp')
    shutil.copyfile(source / 'tessellated_ap242.stp', target / 'tessellated-ap242.stp')
    shutil.copyfile(source / 'external_document_ap214.stp', target / 'external-document.stp')
    (target / 'malformed.stp').write_bytes(b'not an ISO 10303-21 file')
    return target


def run(configuration, exe_override=None):
    exe = exe_override or ROOT / 'x64' / configuration / 'Preview3D.exe'
    fixtures = prepare_fixtures()
    paths = {path.name: path for path in fixtures.iterdir() if path.is_file()}
    checks = []
    primary = subprocess.Popen([
        str(exe), '--activation-smoke', '--open', str(paths['part-uppercase.STEP'])])
    hwnd = None
    try:
        hwnd = wait_for(lambda: window_for_pid(primary.pid))
        wait_for(lambda: ready(hwnd, paths['part-uppercase.STEP']))
        if query(hwnd, 16) != 1:
            raise RuntimeError('STEP bounds were not verified')
        if query(hwnd, 13) <= 0:
            raise RuntimeError('STEP part published no vertices')
        for field in (23, 24, 25):
            if query(hwnd, field) == 0:
                raise RuntimeError('STEP dimensions were not published')
        if query(hwnd, 84) != STEP_PHASE_EMIT or query(hwnd, 86) <= 0:
            raise RuntimeError(
                f'STEP progress did not reach Emit: phase={query(hwnd, 84)} '
                f'total={query(hwnd, 86)}')
        checks.append('uppercase STEP direct command line with verified bounds and phase progress')

        forward(exe, paths['part-ap214.stp'])
        wait_for(lambda: ready(hwnd, paths['part-ap214.stp']))
        checks.append('AP214 part through secondary activation')

        injected_open(hwnd, paths['tessellated-ap242.stp'], 106)
        wait_for(lambda: ready(hwnd, paths['tessellated-ap242.stp']))
        checks.append('AP242 authored tessellation through picker route')

        injected_open(hwnd, paths['assembly-ap214.stp'], 107)
        wait_for(lambda: ready(hwnd, paths['assembly-ap214.stp']) and query(hwnd, 20) >= 1)
        checks.append('assembly through one-file drop route with node instances')

        prior_triangles = query(hwnd, 14)
        forward(exe, paths['malformed.stp'])
        wait_for(lambda: query(hwnd, 0) == 4 and query(hwnd, 45) and query(hwnd, 17) == STEP_FORMAT)
        if query(hwnd, 41) != MALFORMED_DATA:
            raise RuntimeError(f'malformed STEP reported code {query(hwnd, 41)}')
        if query(hwnd, 14) != prior_triangles:
            raise RuntimeError('malformed STEP did not retain the prior usable model')
        checks.append('malformed STEP retains prior usable content')
        forward(exe, paths['part-uppercase.STEP'])
        wait_for(lambda: ready(hwnd, paths['part-uppercase.STEP']))

        forward(exe, paths['external-document.stp'])
        wait_for(lambda: query(hwnd, 0) == 4 and query(hwnd, 45) and query(hwnd, 17) == STEP_FORMAT)
        if query(hwnd, 41) != UNSUPPORTED_REQUIRED_FEATURE:
            raise RuntimeError(f'external STEP reported code {query(hwnd, 41)}')
        checks.append('required external STEP document fails typed without bypass')
        forward(exe, paths['part-uppercase.STEP'])
        wait_for(lambda: ready(hwnd, paths['part-uppercase.STEP']))

        # The same shipping route must recover from a STEP host crash and hang.
        for fault, label in ((1, 'crash'), (2, 'timeout')):
            query_with(hwnd, 44, fault)
            forward(exe, paths['part-ap214.stp'])
            wait_for(lambda: query(hwnd, 0) == 4 and query(hwnd, 45))
            query_with(hwnd, 44, 0)
            forward(exe, paths['part-uppercase.STEP'])
            wait_for(lambda: ready(hwnd, paths['part-uppercase.STEP']))
            checks.append(f'{label} recovery preserves the later valid STEP open')

        injected_open(hwnd, paths['part-ap214.stp'], 105)
        if query(hwnd, 0) != 3 or query(hwnd, 17) != STEP_FORMAT:
            raise RuntimeError('cancel did not retain the prior Ready STEP document')
        checks.append('pending STEP cancellation retains prior document')

        forward(exe, paths['assembly-ap214.stp'])
        forward(exe, paths['part-uppercase.STEP'])
        wait_for(lambda: ready(hwnd, paths['part-uppercase.STEP']))
        checks.append('replacement publishes only the latest STEP generation')

        USER32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        primary.wait(timeout=20)
        hwnd = None
        relaunched = subprocess.Popen([
            str(exe), '--activation-smoke', '--open', str(paths['assembly-ap214.stp'])])
        try:
            relaunched_hwnd = wait_for(lambda: window_for_pid(relaunched.pid))
            wait_for(lambda: ready(relaunched_hwnd, paths['assembly-ap214.stp']))
            USER32.PostMessageW(relaunched_hwnd, WM_CLOSE, 0, 0)
            relaunched.wait(timeout=20)
        finally:
            if relaunched.poll() is None:
                relaunched.kill()
        checks.append('close and immediate STEP relaunch')
        hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest()
                  for name, path in paths.items()}
        return {'configuration': configuration, 'checks': checks,
                'fixtureSha256': hashes, 'status': 'passed'}
    finally:
        if primary.poll() is None:
            if hwnd:
                query_with(hwnd, 44, 0)
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
