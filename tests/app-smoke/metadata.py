"""TSK-202: generated precision fixtures, immutable metadata, GPU picks and epoch framing."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time

from run import ROOT, CopyData, find_window, processes, send, user, W
from progressive import fixture as progressive_fixture


def fixtures(directory):
    directory.mkdir(parents=True,exist_ok=True)
    result = {}
    positions = [(0,0,0),(2,0,0),(2,1,0),(0,1,0)]
    binary = b''.join(struct.pack('<3f',*p) for p in positions)+struct.pack('<6I',0,1,2,0,2,3)
    document = {'asset':{'version':'2.0'},'scene':0,'scenes':[{'nodes':[0]}],
        'nodes':[{'mesh':0,'translation':[1e12,-2e12,3e12],'scale':[1e-4,1e-4,1e-4]}],
        # Double-sided so every X/Y/Z grounding remains visible to the fixed
        # Home camera; this fixture tests transforms/picking, not culling.
        'materials':[{'doubleSided':True}],
        'meshes':[{'primitives':[{'attributes':{'POSITION':0},'indices':1,'material':0}]}],
        'accessors':[{'bufferView':0,'componentType':5126,'count':4,'type':'VEC3',
                      'min':[-999,-999,-999],'max':[999,999,999]},
                     {'bufferView':1,'componentType':5125,'count':6,'type':'SCALAR'}],
        'bufferViews':[{'buffer':0,'byteLength':48},{'buffer':0,'byteOffset':48,'byteLength':24}],
        'buffers':[{'byteLength':72}]}
    text = json.dumps(document,sort_keys=True,separators=(',',':')).encode()
    text += b' '*((-len(text))%4)
    data = struct.pack('<III',0x46546c67,2,28+len(text)+len(binary))
    data += struct.pack('<II',len(text),0x4e4f534a)+text+struct.pack('<II',len(binary),0x004e4942)+binary
    result['large-offset-tiny.glb'] = data
    for endian,label in [('<','le'),('>','be')]:
        for mesh in [False,True]:
            header = f'ply\nformat binary_{"little" if endian=="<" else "big"}_endian 1.0\nelement vertex 4\nproperty double x\nproperty double y\nproperty double z\n'
            if mesh: header += 'element face 2\nproperty list uchar uint vertex_indices\n'
            header += 'end_header\n'
            body = b''.join(struct.pack(endian+'3d',1e9+x*0.001,-2e9+y*0.001,3e9) for x,y,z in positions)
            if mesh: body += struct.pack(endian+'B3IB3I',3,0,1,2,3,0,2,3)
            result[f'large-offset-{label}-{"mesh" if mesh else "points"}.ply'] = header.encode()+body
    body = bytearray(80)+struct.pack('<I',2)
    for indices in [(0,1,2),(0,2,3)]:
        body += struct.pack('<3f',0,0,1)
        for i in indices: body += struct.pack('<3f',*(v*1e-6 for v in positions[i]))
        body += b'\0\0'
    result['tiny.stl'] = bytes(body)
    for name,data in result.items(): (directory/name).write_bytes(data)
    return {name:hashlib.sha256(data).hexdigest() for name,data in result.items()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--configuration',choices=['Debug','Release'],default='Debug')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--fbx-only',action='store_true',help='Run only the FBX product metadata and static-pose checks')
    args = parser.parse_args()
    if not user.SetProcessDpiAwarenessContext(W.HANDLE(-4)):
        raise C.WinError(C.get_last_error())
    directory = ROOT/'TestResults/tsk-202-fixtures'
    hashes = fixtures(directory)
    assert hashes == fixtures(directory), 'precision generation is nondeterministic'
    progressive = directory/'progressive.glb'
    hashes[progressive.name] = progressive_fixture(progressive,64)
    assert hashes[progressive.name] == progressive_fixture(progressive,64)
    exe = ROOT/'x64'/args.configuration/'Preview3D.exe'
    report = {'configuration':args.configuration,'fixtureSha256':hashes,
        'exeSha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'failure':None,'scenes':[]}
    app = subprocess.Popen([str(exe),'--progressive-smoke'],stderr=subprocess.PIPE)
    hwnd = 0
    workers = set()
    original_native = None
    original_ground_axis = None
    def query(field): return send(hwnd,0x8000+104,field)
    def number(field): return struct.unpack('<d',struct.pack('<Q',query(field)))[0]
    def wait(predicate,label,timeout=25):
        end = time.monotonic()+timeout
        while time.monotonic()<end:
            if app.poll() is not None: raise RuntimeError(f'exit {app.returncode}: {label}')
            workers.update(pid for pid,(parent,name) in processes().items() if parent==app.pid and name=='Preview3DImportWorker.exe')
            value = predicate()
            if value: return value
            time.sleep(0.01)
        raise TimeoutError(label)
    def open_file(path):
        text = C.create_unicode_buffer(str(path.resolve()))
        data = CopyData(104,C.sizeof(text),C.cast(text,C.c_void_p))
        return send(hwnd,0x4a,0,C.addressof(data))
    def click(packed):
        point = W.POINT(packed&65535,packed>>16)
        user.ClientToScreen.argtypes = [W.HWND,C.POINTER(W.POINT)]
        user.ClientToScreen(hwnd,C.byref(point))
        user.SetCursorPos(point.x,point.y)
        time.sleep(0.03)
        send(hwnd,0x201,1,packed); send(hwnd,0x202,0,packed)
    try:
        hwnd = wait(lambda:find_window(app.pid),'window')
        wait(lambda:query(2),'background')
        original_native = query(21)
        original_ground_axis = query(71)
        if original_native: query(30)
        send(hwnd,0x8000+104,70,0) # Automatic; does not alter persisted settings.
        for name in (() if args.fbx_only else hashes):
            if name == progressive.name: continue
            generation = open_file(directory/name)
            wait(lambda:query(0)==3 and query(4)==generation,'verified scene '+name)
            assert query(16)==1 and query(28)==0, 'missing verified bounds or retained CPU mesh'
            points = 'points' in name
            expected_vertices = 6 if name.endswith('.stl') else 4
            assert query(13)==expected_vertices and query(14)==(0 if points else 2) and query(15)==(4 if points else 0)
            assert query(17)==(4 if name.endswith('.glb') else 2 if name.endswith('.stl') else 3)
            width,height = number(23),number(25 if name.endswith('.glb') else 24)
            scale = 1e-4 if name.endswith('.glb') else 1e-6 if name.endswith('.stl') else 1e-3
            assert abs(width/scale-2)<0.001 and abs(height/scale-1)<0.001, (name,width,height)
            frames = query(7)
            send(hwnd,0x111,32789) # Existing Info surface, reserving the viewport.
            # Wait for the resized viewport/chrome Present before requesting a
            # depth-tested pick; points occupy one diagnostic pixel in TSK-202.
            wait(lambda:query(7)>frames,'Info Present')
            center = query(29)
            if points:
                # Rasterization rounds a point to one pixel. Check a bounded
                # 3x3 neighborhood around its exact projected source position.
                x,y = center&65535,center>>16
                for dy in [-1,0,1]:
                    for dx in [-1,0,1]:
                        click((x+dx)|((y+dy)<<16))
                        try: wait(lambda:query(22),'point pick',0.15)
                        except TimeoutError: pass
                        if query(22): break
                    if query(22): break
            else: click(center)
            wait(lambda:query(22)==1,'GPU geometry selection '+name)
            send(hwnd,0x111,32772) # Frame selected.
            click((60)|(120<<16)); wait(lambda:query(22)==0,'empty pixel deselect')
            report['scenes'].append({'fixture':name,'vertices':query(13),'triangles':query(14),
                'points':query(15),'width':width,'height':height,'gpuPick':True,'cpuPickPayloadCount':query(28)})
            if name.endswith('.glb'):
                query(30); assert abs(number(24)-1e-4)<1e-10 and abs(number(25))<1e-10
                query(30); assert abs(number(25)-1e-4)<1e-10 and abs(number(24))<1e-10
                report['nativeOrientationDimensions'] = True
                # Exercise every explicit source/model up axis. X grounding
                # moves the original X extent to world height and must remain
                # GPU-pickable through the same transform used for drawing.
                send(hwnd,0x8000+104,70,3) # Z
                assert abs(number(24)-1e-4)<1e-10 and abs(number(25))<1e-10
                frames = query(7)
                send(hwnd,0x8000+104,70,1) # X
                assert abs(number(23))<1e-10 and abs(number(25)-2e-4)<1e-10
                wait(lambda:query(7)>frames,'X-grounded Present')
                center = query(29); click(center)
                wait(lambda:query(22)==1,'X-grounded GPU geometry selection')
                send(hwnd,0x8000+104,70,2) # Y
                assert abs(number(23)-2e-4)<1e-10 and abs(number(25)-1e-4)<1e-10
                report['groundAxisCycle'] = {'Z':True,'Y':True,'X':True,'gpuPick':True}
                send(hwnd,0x8000+104,70,0) # Restore automatic for remaining fixtures.
            send(hwnd,0x111,32789)
        fbx = ROOT/'tests/fixtures/fbx-spike/combined-skin-blend-ascii.fbx'
        generation = open_file(fbx)
        wait(lambda:query(0)==3 and query(4)==generation,'deformed FBX metadata and pose')
        assert query(17)==8 and query(16)==1, 'FBX format or transformed bounds were lost'
        assert query(13)==528 and query(14)==176 and query(15)==0
        assert query(19)>0 and query(20)==15 and query(80)>0
        assert query(81)==1 and query(82)==1 and query(83)==4
        assert query(8)>0, 'deformed FBX produced no displayed GPU chunks'
        report['scenes'].append({'fixture':fbx.name,'vertices':query(13),'triangles':query(14),
            'materials':query(19),'nodes':query(20),'meshes':query(80),'animations':query(81),
            'skins':query(82),'bones':query(83),'verifiedBounds':bool(query(16)),'gpuChunks':query(8)})
        if not args.fbx_only:
            generation = open_file(progressive)
            wait(lambda:query(4)==generation and query(8)<64,'partial metadata')
            assert query(0)==2 and query(16)==0 and query(13)==query(8)*3
            first = query(8)
            send(hwnd,0x20a,120<<16)
            target_distance = number(31)
            home = number(27)
            wait(lambda:query(0)==3,'terminal verified metadata')
            assert query(16)==1 and query(13)==192 and query(14)==64 and query(20)==64
            assert number(31)==target_distance, 'late bounds correction overwrote user movement'
            assert number(27)>home, 'verified bounds did not update home framing'
            send(hwnd,0x111,32773)
            assert number(31)>target_distance, 'Reset did not use verified home bounds'
            report['interactionEpochFraming'] = {'firstChunks':first,'terminalVertices':query(13),'userTargetPreserved':True,'resetUsesVerifiedBounds':True}
            # A second import without user movement must correct live framing too.
            generation = open_file(progressive)
            wait(lambda:query(4)==generation and query(8)<64,'untouched partial framing')
            target_distance = number(31)
            wait(lambda:query(0)==3,'untouched terminal framing')
            assert number(31)>target_distance and number(31)==number(27)
            report['untouchedCameraCorrection'] = True
        report['debugLayerAvailable'] = bool(query(36))
        report['debugLayerErrors'] = query(35)
        assert query(35)==0, 'D3D12 debug errors'
    except Exception as error:
        report['failure'] = str(error)
        if hwnd and app.poll() is None:
            report['diagnostics'] = {'debugErrors':query(35),'projectedPixel':query(29),
                'pickRequests':query(34),'pickCompletions':query(32),'pickHit':query(33),
                'cameraDistance':number(26),'cameraHomeDistance':number(27),'selected':query(22)}
    finally:
        if hwnd and app.poll() is None:
            if original_ground_axis is not None: send(hwnd,0x8000+104,70,original_ground_axis)
            if original_native is not None and query(21)!=original_native: query(30)
            send(hwnd,0x10)
        try: app.wait(timeout=10)
        except subprocess.TimeoutExpired: app.kill(); app.wait(); report['failure'] = report['failure'] or 'close timeout'
        time.sleep(0.1)
        live = processes()
        report['survivingWorkers'] = sorted(workers & live.keys())
        if report['survivingWorkers']: report['failure'] = report['failure'] or 'worker leaked'
        report['exitCode'] = app.returncode
        report['graphicsDiagnostics'] = app.stderr.read().decode('utf-8',errors='replace')[:4096]
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    if report['failure'] or report['exitCode']: raise SystemExit(1)


if __name__=='__main__': main()
