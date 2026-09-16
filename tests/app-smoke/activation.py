"""Real-app TSK-304 singleton activation/replacement/recovery smoke."""
import argparse
import ctypes
import json
import subprocess
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
USER32=ctypes.windll.user32
WM_CLOSE=0x0010

def window_for_pid(pid):
    found=[]
    callback=ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p)
    @callback
    def visit(hwnd,_):
        owner=ctypes.c_ulong()
        USER32.GetWindowThreadProcessId(hwnd,ctypes.byref(owner))
        if owner.value==pid and USER32.IsWindowVisible(hwnd): found.append(hwnd)
        return True
    USER32.EnumWindows(visit,0)
    return found[0] if found else None

def wait_for(predicate,seconds=12):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        value=predicate()
        if value: return value
        time.sleep(.05)
    raise RuntimeError('timed out waiting for viewer state')

def title(hwnd):
    size=USER32.GetWindowTextLengthW(hwnd)+1
    buffer=ctypes.create_unicode_buffer(size)
    USER32.GetWindowTextW(hwnd,buffer,size)
    return buffer.value

def forward(exe,*arguments):
    process=subprocess.Popen([str(exe),*map(str,arguments)])
    code=process.wait(timeout=5)
    if code!=0: raise RuntimeError(f'secondary activation returned {code}: {arguments}')
    return process.pid

def run(configuration):
    exe=ROOT/'x64'/configuration/'Preview3D.exe'
    corpus=ROOT/'interactive-viewer'/'test-assets'/'corpus'
    primary=subprocess.Popen([str(exe)])
    hwnd=None
    checks=[]
    try:
        hwnd=wait_for(lambda:window_for_pid(primary.pid))
        first_secondary=forward(exe,corpus/'Pressure.glb')
        replacement_secondary=forward(exe,'--open',corpus/'A-small-stl.stl')
        wait_for(lambda:'A-small-stl.stl' in title(hwnd))
        checks.append('second-process replacement during load')

        forward(exe,corpus/'empty.ply')
        wait_for(lambda:'empty.ply' in title(hwnd))
        forward(exe,corpus/'A-small-glb.glb')
        wait_for(lambda:'A-small-glb.glb' in title(hwnd))
        checks.append('valid activation after document failure')

        activate_pid=forward(exe)
        if primary.poll() is not None: raise RuntimeError('primary exited after Activate request')
        if len({primary.pid,first_secondary,replacement_secondary,activate_pid})!=4:
            raise RuntimeError('process identifiers unexpectedly reused during smoke')
        checks.append('pathless activation preserves one primary')

        USER32.PostMessageW(hwnd,WM_CLOSE,0,0)
        primary.wait(timeout=12)
        hwnd=None
        relaunched=subprocess.Popen([str(exe)])
        try:
            relaunched_hwnd=wait_for(lambda:window_for_pid(relaunched.pid))
            USER32.PostMessageW(relaunched_hwnd,WM_CLOSE,0,0)
            relaunched.wait(timeout=12)
        finally:
            if relaunched.poll() is None: relaunched.kill()
        checks.append('close and immediate relaunch')
        return {'configuration':configuration,'checks':checks,'status':'passed'}
    finally:
        if primary.poll() is None:
            if hwnd: USER32.PostMessageW(hwnd,WM_CLOSE,0,0)
            try: primary.wait(timeout=8)
            except subprocess.TimeoutExpired: primary.kill()

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--configuration',choices=('Debug','Release'),default='Debug')
    parser.add_argument('--result')
    args=parser.parse_args()
    result=run(args.configuration)
    rendered=json.dumps(result,indent=2)
    print(rendered)
    if args.result: Path(args.result).write_text(rendered+'\n',encoding='utf-8')
