"""Real-app X/Y/Z ground-axis rendering, framing, metadata and GPU-pick smoke."""
import argparse
import ctypes as C
import json
from pathlib import Path
import struct
import subprocess
import time

from run import ROOT, CopyData, find_window, send, user, W
from metadata import fixtures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--configuration',choices=['Debug','Release'],default='Debug')
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    if not user.SetProcessDpiAwarenessContext(W.HANDLE(-4)):
        raise C.WinError(C.get_last_error())

    directory = ROOT/'TestResults/ground-axis-fixtures'
    fixtures(directory)
    model = directory/'large-offset-tiny.glb'
    point_model = directory/'large-offset-le-points.ply'
    exe = ROOT/'x64'/args.configuration/'Preview3D.exe'
    app = subprocess.Popen([str(exe),'--app-smoke'],stderr=subprocess.PIPE)
    hwnd = 0
    original_native = None
    original_axis = None
    report = {'configuration':args.configuration,'meshAxes':{},'pointAxes':{},'failure':None}

    def query(field,lparam=0): return send(hwnd,0x8000+104,field,lparam)
    def number(field): return struct.unpack('<d',struct.pack('<Q',query(field)))[0]
    def wait(predicate,label,timeout=20):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            if app.poll() is not None: raise RuntimeError(f'exit {app.returncode}: {label}')
            value=predicate()
            if value: return value
            time.sleep(0.01)
        raise TimeoutError(label)
    def click(packed):
        point=W.POINT(packed&65535,packed>>16)
        user.ClientToScreen.argtypes=[W.HWND,C.POINTER(W.POINT)]
        user.ClientToScreen(hwnd,C.byref(point)); user.SetCursorPos(point.x,point.y)
        time.sleep(0.03); send(hwnd,0x201,1,packed); send(hwnd,0x202,0,packed)
    def open_model(path):
        text=C.create_unicode_buffer(str(path.resolve()))
        data=CopyData(104,C.sizeof(text),C.cast(text,C.c_void_p))
        generation=send(hwnd,0x4a,0,C.addressof(data))
        wait(lambda:query(0)==3 and query(4)==generation,'ready '+path.name)
    def pick_current(label,point_primitive=False):
        packed=query(29); x,y=packed&65535,packed>>16
        offsets=[(0,0)] if not point_primitive else [(dx,dy) for dy in (-1,0,1) for dx in (-1,0,1)]
        for dx,dy in offsets:
            completions=query(32); click((x+dx)|((y+dy)<<16))
            wait(lambda:query(32)>completions,label)
            if query(33)==1:return
        raise AssertionError(label+' missed')

    try:
        hwnd=wait(lambda:find_window(app.pid),'window')
        wait(lambda:query(2),'background')
        original_native=query(21); original_axis=query(71)
        open_model(model)

        expected={3:(2e-4,1e-4,0.0),2:(2e-4,0.0,1e-4),1:(0.0,1e-4,2e-4)}
        names={3:'Z',2:'Y',1:'X'}
        for axis in (3,2,1):
            frames=query(7); query(70,axis)
            wait(lambda:query(7)>frames,f'{names[axis]} Present')
            dimensions=(number(23),number(24),number(25))
            for actual,wanted in zip(dimensions,expected[axis]):
                assert abs(actual-wanted)<1e-10,(names[axis],dimensions)
            pick_current(f'{names[axis]} mesh pick')
            assert abs(number(26)-number(27))<1e-15,f'{names[axis]} did not become Home'
            report['meshAxes'][names[axis]]={'dimensions':dimensions,'gpuPick':True,'isHome':True}

        open_model(point_model)
        point_source=(0.001999974250793457,0.0009999275207519531,0.0)
        point_expected={3:point_source,2:(point_source[0],point_source[2],point_source[1]),
                        1:(point_source[2],point_source[1],point_source[0])}
        for axis in (3,2,1):
            frames=query(7); query(70,axis)
            wait(lambda:query(7)>frames,f'{names[axis]} point Present')
            dimensions=(number(23),number(24),number(25))
            for actual,wanted in zip(dimensions,point_expected[axis]):
                assert abs(actual-wanted)<1e-10,(names[axis],dimensions)
            pick_current(f'{names[axis]} point pick',True)
            assert abs(number(26)-number(27))<1e-15,f'{names[axis]} point view did not become Home'
            report['pointAxes'][names[axis]]={'dimensions':dimensions,'gpuPick':True,'isHome':True}
        assert query(35)==0,'D3D12 debug errors'
    except Exception as error:
        report['failure']=str(error)
    finally:
        if hwnd and app.poll() is None:
            if original_axis is not None: query(70,original_axis)
            if original_native is not None and query(21)!=original_native: query(30)
            send(hwnd,0x10)
        try: app.wait(timeout=10)
        except subprocess.TimeoutExpired: app.kill(); app.wait(); report['failure']=report['failure'] or 'close timeout'
        report['exitCode']=app.returncode
        report['graphicsDiagnostics']=app.stderr.read().decode('utf-8',errors='replace')[:4096]

    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    if report['failure'] or report['exitCode']: raise SystemExit(1)


if __name__=='__main__': main()
