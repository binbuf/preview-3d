"""TSK-201: visible progressive display, delayed copies, bounded queues and cancellation."""
import argparse
import copy
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time

from run import ROOT, CopyData, find_window, processes, send, user, W


def fixture(path, count):
    # Reuse the pinned triangle's binary accessors; independent nodes/meshes
    # force additive chunk publication without a large committed binary.
    source = (ROOT/'interactive-viewer/test-assets/tri_tight.glb').read_bytes()
    length, kind = struct.unpack_from('<II', source, 12)
    assert kind == 0x4e4f534a
    document = json.loads(source[20:20+length])
    binary_at = 20+length
    binary_length, kind = struct.unpack_from('<II',source,binary_at)
    assert kind == 0x004e4942
    binary = source[binary_at+8:binary_at+8+binary_length]
    mesh = copy.deepcopy(document['meshes'][0])
    document['meshes'] = [copy.deepcopy(mesh) for _ in range(count)]
    document['nodes'] = [{'mesh':i,'translation':[float(i%16),float(i//16),0.0]} for i in range(count)]
    document['scenes'] = [{'nodes':list(range(count))}]
    document['scene'] = 0
    text = json.dumps(document,sort_keys=True,separators=(',',':')).encode()
    text += b' '*((-len(text))%4)
    result = struct.pack('<III',0x46546c67,2,28+len(text)+len(binary))
    result += struct.pack('<II',len(text),0x4e4f534a)+text
    result += struct.pack('<II',len(binary),0x004e4942)+binary
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_bytes(result)
    return hashlib.sha256(result).hexdigest()


def run(exe, flag, asset, count, cap):
    app = subprocess.Popen([str(exe),flag])
    report = {'mode':flag,'failure':None,'maxUiMs':0}
    hwnd = 0
    workers = set()
    def query(field):
        at = time.perf_counter()
        value = send(hwnd,0x8000+104,field)
        report['maxUiMs'] = max(report['maxUiMs'],(time.perf_counter()-at)*1000)
        return value
    def wait(predicate, label, timeout=25, require_alive=True):
        until = time.monotonic()+timeout
        while time.monotonic()<until:
            if require_alive and app.poll() is not None: raise RuntimeError(f'exit {app.returncode}: {label}')
            workers.update(pid for pid,(parent,name) in processes().items()
                if parent==app.pid and name=='Preview3DImportWorker.exe')
            value = predicate()
            if value: return value
            time.sleep(0.01)
        raise TimeoutError(label)
    def open_file(path):
        text = C.create_unicode_buffer(str(path.resolve()))
        data = CopyData(104,C.sizeof(text),C.cast(text,C.c_void_p))
        return send(hwnd,0x4a,0,C.addressof(data))
    try:
        hwnd = wait(lambda:find_window(app.pid),'window')
        wait(lambda:query(2),'background')
        report['maxUiMs'] = 0
        old = open_file(ROOT/'interactive-viewer/test-assets/tri_tight.glb')
        wait(lambda:query(4)==old and query(0)==3,'baseline Ready')
        frames = query(7)
        distance = query(12)
        generation = open_file(asset)
        assert query(0)==2 and query(4)==old, 'prior model lost during copy delay'
        send(hwnd,0x20a,120<<16) # WM_MOUSEWHEEL: real camera dolly input while Loading
        wait(lambda:query(12)!=distance,'camera input during delayed copy')
        report['cameraInputDuringLoading'] = True
        wait(lambda:query(7)>frames,'prior model/chrome Present during coordinator delay')
        assert query(4)==old, 'delay did not preserve prior geometry'
        if flag=='--texture-batch-smoke':
            wait(lambda:query(4)==generation and query(0)==3,'cross-batch texture model')
            assert query(8)==1 and query(11)==1, 'material lost its earlier-batch texture binding'
            report['texturedChunkCount'] = query(11)
        elif flag=='--progressive-smoke':
            # Suppress UI upload notifications while the render thread accepts
            # first geometry. Cancellation must pull the actual display snapshot.
            send(hwnd,0x8000+104,46,1)
            wait(lambda:query(4)==generation,'first progressive geometry')
            assert query(0)==2, 'early geometry was labeled Ready'
            first = query(8)
            assert 0<first<count, 'first publication was not partial'
            report['firstChunkCount'] = first
            send(hwnd,0x100,0x1b)
            assert query(0)==5 and not query(16) and not query(45), 'cancelled partial geometry was labeled Ready'
            assert query(14)==first, 'cancel retained metadata for the superseded scene'
            assert query(47)==generation, 'cancel retained stale selection/metadata identity'
            report['cancelledPartialState'] = query(0)
            failed_replacement = open_file(asset)
            wait(lambda:query(4)==failed_replacement,'replacement geometry before delayed UI metadata')
            open_file(asset.parent/'unsupported.FBX')
            assert query(0)==4 and query(47)==failed_replacement, 'failure retained stale display metadata'
            report['failureWithDelayedUiMetadata'] = True
            send(hwnd,0x8000+104,46,0)
            generation = open_file(asset)
            wait(lambda:query(0)==3,'terminal catalog and copies')
            assert query(8)==count, 'a later batch overwrote or dropped earlier chunks'
            report['terminalChunkCount'] = query(8)
        else:
            if cap==128*1024*1024:
                wait(lambda:query(9)==4,'count backpressure')
            else:
                wait(lambda:query(6)>cap//2,'byte backpressure')
            report['peakQueueBytes'] = query(6)
            report['peakQueueCount'] = query(9)
            assert report['peakQueueBytes']<=cap and report['peakQueueCount']<=4
            # Cancel while downstream capacity is exhausted; then rapidly
            # replace/cancel again. Older publications must never reappear.
            send(hwnd,0x100,0x1b)
            assert query(0) in (3,5), 'cancel did not recover prior/usable content'
            assert query(0)!=3 or query(16), 'cancelled incomplete geometry was labeled Ready'
            for _ in range(3):
                open_file(asset)
                send(hwnd,0x100,0x1b)
            reopened = open_file(ROOT/'interactive-viewer/test-assets/tri_tight.glb')
            wait(lambda:query(4)==reopened and query(0)==3,'valid replacement after backpressure cancellation')
            assert query(8)==1, 'stale chunks leaked into replacement'
        report.setdefault('peakQueueBytes',query(6))
        report.setdefault('peakQueueCount',query(9))
        assert report['peakQueueBytes']<=cap
        # Close with queued/coordinator work and an active sandbox process.
        open_file(asset)
        wait(lambda:query(10)>0,'pending upload')
        at = time.perf_counter()
        send(hwnd,0x10)
        app.wait(timeout=10)
        report['closeMs'] = (time.perf_counter()-at)*1000
        assert app.returncode==0, f'close exited {app.returncode}'
        wait(lambda:not [pid for pid,(parent,name) in processes().items()
            if name=='Preview3DImportWorker.exe' and (parent==app.pid or pid in workers)],'worker termination',require_alive=False)
    except Exception as error:
        report['failure'] = str(error)
    finally:
        if app.poll() is None: app.kill(); app.wait(timeout=10)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configuration',choices=['Debug','Release'],default='Debug')
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    if not user.SetProcessDpiAwarenessContext(W.HANDLE(-4)): raise C.WinError(C.get_last_error())
    output = args.output.resolve()
    small, many = output.parent/'progressive-64.glb', output.parent/'progressive-256.glb'
    hashes = {small.name:fixture(small,64),many.name:fixture(many,256)}
    assert fixture(small,64)==hashes[small.name] and fixture(many,256)==hashes[many.name]
    exe = ROOT/f'x64/{args.configuration}/Preview3D.exe'
    runs = [run(exe,'--progressive-smoke',small,64,128*1024*1024),
            run(exe,'--queue-smoke',many,256,128*1024*1024),
            run(exe,'--queue-byte-smoke',many,256,16*1024),
            run(exe,'--texture-batch-smoke',ROOT/'interactive-viewer/test-assets/basisu_textured_triangle.glb',1,128*1024*1024)]
    report = {'configuration':args.configuration,'fixtureSha256':hashes,
              'exeSha256':hashlib.sha256(exe.read_bytes()).hexdigest(), 'runs':runs}
    output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    if any(item['failure'] for item in runs): raise SystemExit(1)


if __name__=='__main__': main()
