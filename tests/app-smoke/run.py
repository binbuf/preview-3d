"""Visible real-app lifecycle smoke. Requires Windows and a built solution."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import math
from pathlib import Path
import platform
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
user = C.WinDLL('user32', use_last_error=True)
kernel = C.WinDLL('kernel32', use_last_error=True)
psapi = C.WinDLL('psapi', use_last_error=True)
ULONG_PTR = C.c_size_t
user.SendMessageTimeoutW.argtypes = [W.HWND,W.UINT,W.WPARAM,W.LPARAM,W.UINT,W.UINT,C.POINTER(ULONG_PTR)]
user.SendMessageTimeoutW.restype = W.LPARAM
user.GetWindowThreadProcessId.argtypes = [W.HWND,C.POINTER(W.DWORD)]
user.GetClassNameW.argtypes = [W.HWND,W.LPWSTR,C.c_int]
user.GetClientRect.argtypes = [W.HWND,C.POINTER(W.RECT)]
user.SetWindowPos.argtypes = [W.HWND,W.HWND,C.c_int,C.c_int,C.c_int,C.c_int,W.UINT]
user.SetProcessDpiAwarenessContext.argtypes = [W.HANDLE]
user.GetDpiForWindow.argtypes = [W.HWND]
kernel.OpenProcess.argtypes = [W.DWORD,W.BOOL,W.DWORD]
kernel.OpenProcess.restype = W.HANDLE
kernel.CloseHandle.argtypes = [W.HANDLE]


class CopyData(C.Structure):
    _fields_ = [('dwData',ULONG_PTR),('cbData',W.DWORD),('lpData',C.c_void_p)]


class Memory(C.Structure):
    _fields_ = [('cb',W.DWORD),('faults',W.DWORD)] + [(name,C.c_size_t) for name in
        ['peakWorkingSet','workingSet','peakPaged','paged','peakNonPaged','nonPaged','pagefile','peakPagefile','private']]


class ProcessEntry(C.Structure):
    _fields_ = [('dwSize',W.DWORD),('usage',W.DWORD),('pid',W.DWORD),('heap',ULONG_PTR),
               ('module',W.DWORD),('threads',W.DWORD),('parent',W.DWORD),('priority',W.LONG),
               ('flags',W.DWORD),('exe',W.WCHAR*260)]


kernel.CreateToolhelp32Snapshot.argtypes = [W.DWORD,W.DWORD]
kernel.CreateToolhelp32Snapshot.restype = W.HANDLE
kernel.Process32FirstW.argtypes = [W.HANDLE,C.POINTER(ProcessEntry)]
kernel.Process32NextW.argtypes = [W.HANDLE,C.POINTER(ProcessEntry)]
psapi.GetProcessMemoryInfo.argtypes = [W.HANDLE,C.POINTER(Memory),W.DWORD]


def processes():
    snapshot = kernel.CreateToolhelp32Snapshot(2,0)
    if snapshot == W.HANDLE(-1).value:
        raise C.WinError(C.get_last_error())
    result = {}
    try:
        entry = ProcessEntry()
        entry.dwSize = C.sizeof(entry)
        more = kernel.Process32FirstW(snapshot,C.byref(entry))
        while more:
            result[entry.pid] = (entry.parent,entry.exe)
            more = kernel.Process32NextW(snapshot,C.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    return result


def private_bytes(pid):
    handle = kernel.OpenProcess(0x1000|0x10,False,pid)
    if not handle:
        return 0
    try:
        memory = Memory()
        memory.cb = C.sizeof(memory)
        return memory.private if psapi.GetProcessMemoryInfo(handle,C.byref(memory),memory.cb) else 0
    finally:
        kernel.CloseHandle(handle)


def send(hwnd, message, wparam=0, lparam=0):
    result = ULONG_PTR()
    if not user.SendMessageTimeoutW(hwnd,message,wparam,lparam,2,1000,C.byref(result)):
        raise RuntimeError(f'UI message timeout/error: {C.get_last_error()}')
    return result.value


def find_window(pid):
    found = []
    callback_type = C.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
    @callback_type
    def visit(hwnd, _):
        owner = W.DWORD()
        user.GetWindowThreadProcessId(hwnd,C.byref(owner))
        name = C.create_unicode_buffer(128)
        user.GetClassNameW(hwnd,name,128)
        if owner.value == pid and name.value == 'Preview3DWindow':
            found.append(hwnd)
        return True
    user.EnumWindows(visit,0)
    return found[0] if found else 0


def one_run(exe, corpus, timeout, require_points):
    result = {'events':[], 'viewerPeakPrivateBytes':0, 'workerPeakPrivateBytes':0,
              'maxObservedUiRoundTripMs':0, 'failure':None}
    started_us = time.perf_counter_ns()//1000
    app = subprocess.Popen([str(exe),'--app-smoke'])
    worker_ids = set()
    hwnd = 0
    def sample():
        if hwnd:
            ping_at = time.perf_counter()
            send(hwnd,0x8000+104,0)
            result['maxObservedUiRoundTripMs'] = max(result['maxObservedUiRoundTripMs'],(time.perf_counter()-ping_at)*1000)
        result['viewerPeakPrivateBytes'] = max(result['viewerPeakPrivateBytes'],private_bytes(app.pid))
        active = processes()
        total = 0
        for pid,(parent,name) in active.items():
            if parent == app.pid and name == 'Preview3DImportWorker.exe':
                worker_ids.add(pid)
                total += private_bytes(pid)
        result['workerPeakPrivateBytes'] = max(result['workerPeakPrivateBytes'],total)

    def wait_for(predicate, label):
        deadline = time.monotonic()+timeout
        while time.monotonic() < deadline:
            if app.poll() is not None:
                raise RuntimeError(f'app exited {app.returncode} while waiting for {label}')
            value = predicate()
            sample()
            if value:
                return value
            time.sleep(0.02) # polling only; every transition requires an observed acknowledgement
        raise TimeoutError(label)

    def query(field):
        return send(hwnd,0x8000+104,field)

    def open_file(name, cancel=False):
        text = C.create_unicode_buffer(str((corpus/name).resolve()))
        data = CopyData(105 if cancel else 104,C.sizeof(text),C.cast(text,C.c_void_p))
        at_us = time.perf_counter_ns()//1000
        generation = send(hwnd,0x4a,0,C.addressof(data))
        if not generation:
            raise RuntimeError('app did not accept smoke command')
        return generation,at_us

    try:
        hwnd = wait_for(lambda:find_window(app.pid),'main window')
        result['windowDpi'] = user.GetDpiForWindow(hwnd)
        background = wait_for(lambda:query(2),'first visible background Present')
        result['firstBackgroundMs'] = (background-started_us)/1000
        result['baselineViewerPrivateBytes'] = private_bytes(app.pid)
        if query(0) != 1:
            raise RuntimeError('expected initial Empty state')
        fixtures = ['A-small-glb.glb','A-small-stl.stl','A-small-ply-mesh-be.ply',
                    'sidecar-approved.gltf','sparse-valid.gltf','meshopt.glb','webp.gltf']
        if require_points:
            fixtures.insert(3,'A-small-ply-points-le.ply')
        for name in fixtures:
            generation,opened = open_file(name)
            def geometry_ready():
                state = query(0)
                if state == 4:
                    raise RuntimeError(f'{name}: app reported Failed before geometry presentation')
                return query(4)==generation and state==3
            wait_for(geometry_ready,f'{name} geometry Present and Ready')
            result['events'].append({'action':'open/replace','fixture':name,'generation':generation,
                                     'firstGeometryMs':(query(3)-opened)/1000})
        old_generation = query(4)
        generation,_ = open_file('Pressure.glb',True)
        if query(0)!=3 or query(4)!=old_generation:
            raise RuntimeError('cancel did not preserve the prior Ready model')
        result['events'].append({'action':'cancel-pending','generation':generation,'preservedGeneration':old_generation})
        # A real asynchronous open followed by Cancel (in addition to the
        # atomic pending-cancel command) observes Loading before cancellation.
        generation,_ = open_file('Pressure.glb')
        if query(0)!=2:
            raise RuntimeError('expected Loading before asynchronous cancel')
        send(hwnd,0x100,0x1b) # WM_KEYDOWN Escape routes through the existing CancelOpen handler
        if query(0)!=3 or query(4)!=old_generation:
            raise RuntimeError('asynchronous cancellation failed to preserve prior content')
        result['events'].append({'action':'cancel-loading','generation':generation})
        for width,height in [(900,700),(1280,850),(760,620)]:
            if not user.SetWindowPos(hwnd,None,20,20,width,height,0x14):
                raise C.WinError(C.get_last_error())
            rect = W.RECT()
            user.GetClientRect(hwnd,C.byref(rect))
            extent = (rect.right << 32) | rect.bottom
            wait_for(lambda:query(5)==extent,'render-thread resize acknowledgement')
            result['events'].append({'action':'resize','client':[rect.right,rect.bottom]})
        generation,_ = open_file('truncated.glb')
        wait_for(lambda:query(0)==4,'malformed import failure')
        generation,opened = open_file('A-small-glb.glb')
        wait_for(lambda:query(4)==generation and query(0)==3,'valid reopen after failure/cancel')
        result['events'].append({'action':'recover','firstGeometryMs':(query(3)-opened)/1000})
        # Close while another generation is active.
        open_file('Pressure.glb')
        close_at = time.perf_counter()
        send(hwnd,0x10)
        app.wait(timeout=timeout)
        result['closeMs'] = (time.perf_counter()-close_at)*1000
        result['exitCode'] = app.returncode
        if app.returncode:
            raise RuntimeError(f'app exit {app.returncode}')
        deadline = time.monotonic()+timeout
        while True:
            active = processes()
            survivors = [pid for pid,(parent,name) in active.items()
                         if name=='Preview3DImportWorker.exe' and (parent==app.pid or pid in worker_ids)]
            if not survivors:
                break
            if time.monotonic()>=deadline:
                raise RuntimeError(f'import workers survived close: {survivors}')
            time.sleep(0.02)
    except Exception as error:
        result['failure'] = str(error)
    finally:
        if app.poll() is None:
            app.kill()
            app.wait(timeout=timeout)
        result['durationMs'] = (time.perf_counter_ns()//1000-started_us)/1000
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configuration',choices=['Debug','Release'],default='Debug')
    parser.add_argument('--corpus',type=Path,default=ROOT/'interactive-viewer/test-assets/corpus')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--runs',type=int,default=3)
    parser.add_argument('--timeout',type=float,default=15)
    parser.add_argument('--require-points',action='store_true',help='Require navigable point presentation (round splats and colors are TSK-208)')
    args = parser.parse_args()
    if not user.SetProcessDpiAwarenessContext(W.HANDLE(-4)):
        raise C.WinError(C.get_last_error())
    if args.runs<1 or args.timeout<=0:
        parser.error('runs and timeout must be positive')
    exe = ROOT/f'x64/{args.configuration}/Preview3D.exe'
    manifest = json.loads((args.corpus/'manifest.json').read_text())
    for entry in manifest['entries']:
        path = args.corpus/entry['path']
        with path.open('rb') as stream:
            if hashlib.file_digest(stream,'sha256').hexdigest()!=entry['sha256']:
                raise SystemExit(f'fixture checksum mismatch: {entry["path"]}')
    command = "$os=Get-CimInstance Win32_OperatingSystem; $cpu=Get-CimInstance Win32_Processor; $gpu=Get-CimInstance Win32_VideoController; $disk=Get-PhysicalDisk; @{os=$os.Caption;build=$os.BuildNumber;cpu=@($cpu.Name);ramBytes=$os.TotalVisibleMemorySize*1024;gpu=@($gpu|Select-Object Name,DriverVersion,CurrentRefreshRate,CurrentHorizontalResolution,CurrentVerticalResolution);disks=@($disk|Select-Object FriendlyName,MediaType,BusType);power=(powercfg /getactivescheme)}|ConvertTo-Json -Depth 5"
    hardware = json.loads(subprocess.check_output(['powershell','-NoProfile','-Command',command],text=True,encoding='utf-8-sig'))
    runs = [one_run(exe,args.corpus,args.timeout,args.require_points) for _ in range(args.runs)]
    def summary(values):
        return {'median':statistics.median(values),'p95':sorted(values)[math.ceil(len(values)*0.95)-1],'maximum':max(values)} if values else None
    report = {'schema':1,'configuration':args.configuration,'buildId':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
              'workingDiffSha256':hashlib.sha256(subprocess.check_output(['git','-c','core.warnCRLF=false','diff','HEAD'],cwd=ROOT)).hexdigest(),
              'harnessSha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'generatorSha256':hashlib.sha256((ROOT/'tests/fixtures/generate.py').read_bytes()).hexdigest(),
              'exeSha256':hashlib.sha256(exe.read_bytes()).hexdigest(), 'hardware':hardware,
              'python':platform.python_version(),'clock':str(time.get_clock_info('perf_counter')),
              'cacheState':'OS/driver caches uncontrolled; fresh process each run; no derived cache',
              'measurement':'successful non-occluded Present return/QPC; sampled private commit (20 ms); no ETW scanout or input-to-present claim',
              'fixtureManifestSha256':hashlib.sha256((args.corpus/'manifest.json').read_bytes()).hexdigest(),
              'pointPresentationRequired':args.require_points,
              'runCount':args.runs,'failureCount':sum(r['failure'] is not None for r in runs),'runs':runs,
              'summary': {key:summary([r[key] for r in runs if key in r]) for key in
                          ['firstBackgroundMs','closeMs','viewerPeakPrivateBytes','workerPeakPrivateBytes','maxObservedUiRoundTripMs']}}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'failures':report['failureCount'],'summary':report['summary']},indent=2))
    if report['failureCount']:
        raise SystemExit(1)


if __name__=='__main__':
    main()
