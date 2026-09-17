"""Real-app TSK-304 singleton activation/replacement/recovery smoke."""
import argparse
import base64
import ctypes
import json
import subprocess
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
USER32=ctypes.windll.user32
WM_CLOSE=0x0010
WM_COMMAND=0x0111
WM_COPYDATA=0x004a
SMOKE_QUERY=0x8000+104
ULONG_PTR=ctypes.c_size_t

class CopyData(ctypes.Structure):
    _fields_=[('dwData',ULONG_PTR),('cbData',ctypes.c_ulong),('lpData',ctypes.c_void_p)]

USER32.SendMessageW.argtypes=[ctypes.c_void_p,ctypes.c_uint,ULONG_PTR,ctypes.c_ssize_t]
USER32.SendMessageW.restype=ctypes.c_ssize_t

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

def query(hwnd,field):
    return USER32.SendMessageW(hwnd,SMOKE_QUERY,field,0)

def picker(hwnd,path):
    text=ctypes.create_unicode_buffer(str(path))
    data=CopyData(106,ctypes.sizeof(text),ctypes.cast(text,ctypes.c_void_p))
    assert USER32.SendMessageW(hwnd,WM_COPYDATA,0,ctypes.addressof(data))
    USER32.SendMessageW(hwnd,WM_COMMAND,32771,0)

def drop(hwnd,path):
    text=ctypes.create_unicode_buffer(str(path))
    data=CopyData(107,ctypes.sizeof(text),ctypes.cast(text,ctypes.c_void_p))
    assert USER32.SendMessageW(hwnd,WM_COPYDATA,0,ctypes.addressof(data))

def run(configuration):
    exe=ROOT/'x64'/configuration/'Preview3D.exe'
    corpus=ROOT/'interactive-viewer'/'test-assets'/'corpus'
    fixtures=ROOT/'tests'/'fixtures'/'fbx-spike'
    scratch=ROOT/'TestResults'/'fbx-006-activation'
    scratch.mkdir(parents=True,exist_ok=True)
    binary=scratch/'cube-binary.FBX'
    binary.write_bytes(base64.b64decode((fixtures/'cube-binary.fbx.base64').read_bytes()))
    ascii_fbx=fixtures/'combined-skin-blend-ascii.fbx'
    picker_fbx=fixtures/'hierarchy-instances-pivots-ascii.fbx'
    primary=subprocess.Popen([str(exe),'--activation-smoke','--open',str(ascii_fbx)])
    hwnd=None
    checks=[]
    try:
        hwnd=wait_for(lambda:window_for_pid(primary.pid))
        wait_for(lambda:query(hwnd,0)==3 and query(hwnd,17)==8)
        checks.append('direct ASCII FBX command line')

        first_secondary=forward(exe,'--open',binary)
        wait_for(lambda:'cube-binary.FBX' in title(hwnd) and query(hwnd,0)==3 and query(hwnd,17)==8)
        checks.append('uppercase binary FBX secondary activation')

        picker(hwnd,picker_fbx)
        wait_for(lambda:picker_fbx.name in title(hwnd) and query(hwnd,0)==3 and query(hwnd,17)==8)
        checks.append('ASCII FBX picker route')

        drop(hwnd,binary)
        try:
            wait_for(lambda:binary.name in title(hwnd) and query(hwnd,0)==3 and query(hwnd,17)==8)
        except RuntimeError as error:
            raise RuntimeError(
                f'FBX drop did not load: title={title(hwnd)!r}, phase={query(hwnd,0)}, format={query(hwnd,17)}'
            ) from error
        checks.append('binary FBX one-file drop route')

        replacement_secondary=forward(exe,corpus/'Pressure.glb')
        forward(exe,'--open',ascii_fbx)
        wait_for(lambda:ascii_fbx.name in title(hwnd) and query(hwnd,0)==3)
        checks.append('second-process FBX replacement during load')

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
