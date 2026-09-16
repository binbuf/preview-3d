"""TSK-204: real card actions, typed failures, clipboard privacy and valid reopen."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time
from run import ROOT, CopyData, find_window, processes, send, user, kernel, W

user.OpenClipboard.argtypes = [W.HWND]
user.GetClipboardData.argtypes = [W.UINT]
user.GetClipboardData.restype = W.HANDLE
kernel.GlobalLock.argtypes = [W.HANDLE]
kernel.GlobalLock.restype = C.c_void_p
kernel.GlobalUnlock.argtypes = [W.HANDLE]


def clipboard():
    deadline = time.monotonic() + 2
    while not user.OpenClipboard(None):
        if time.monotonic() >= deadline:
            raise RuntimeError('clipboard unavailable')
        time.sleep(.02)
    try:
        memory = user.GetClipboardData(13)
        pointer = kernel.GlobalLock(memory)
        assert pointer
        try:
            return C.wstring_at(pointer)
        finally:
            kernel.GlobalUnlock(memory)
    finally:
        user.CloseClipboard()


def fixtures(directory):
    directory.mkdir(parents=True, exist_ok=True)
    inputs = {
        'zero.glb': b'', 'zero.gltf': b'', 'zero.stl': b'', 'zero.ply': b'',
        'unsupported.FBX': b'FBX',
        'ascii.STL': b'solid triangle\nendsolid triangle\n',
        'ascii.PLY': b'ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n0 0 0\n',
        'empty.stl': bytes(84),
        'empty.ply': b'ply\nformat binary_little_endian 1.0\nelement vertex 0\nproperty float x\nproperty float y\nproperty float z\nend_header\n',
        'empty.gltf': b'{"asset":{"version":"2.0"},"scenes":[{"nodes":[]}]}',
        'required.gltf': b'{"asset":{"version":"2.0"},"extensionsRequired":["UNSUPPORTED_private"]}',
        'malformed.glb': b'not a model',
        'malformed.stl': bytes(10),
        'malformed.ply': b'not a ply',
    }
    triangle = (ROOT/'interactive-viewer/test-assets/tri_tight.glb').read_bytes()
    json_length = struct.unpack_from('<I', triangle, 12)[0]
    document = json.loads(triangle[20:20+json_length])
    document['extensionsUsed'] = [f'UNSUPPORTED_optional_{i}' for i in range(100)]
    text = json.dumps(document, sort_keys=True, separators=(',', ':')).encode()
    text += b' '*(-len(text)%4)
    binary = triangle[20+json_length:]
    inputs['optional.glb'] = struct.pack('<III',0x46546c67,2,20+len(text)+len(binary))+struct.pack('<II',len(text),0x4e4f534a)+text+binary
    for name, data in inputs.items():
        (directory/name).write_bytes(data)
    return {name: hashlib.sha256(data).hexdigest() for name, data in inputs.items()}


def run(exe, directory):
    app = subprocess.Popen([str(exe), '--app-smoke'])
    report = {'failure': None, 'events': []}
    hwnd = 0
    def query(field):
        return send(hwnd, 0x8000+104, field)
    def wait(predicate, label):
        until = time.monotonic()+25
        while time.monotonic()<until:
            assert app.poll() is None, f'app exited while waiting for {label}'
            if predicate():
                return
            time.sleep(.02)
        raise TimeoutError(label)
    def open_file(path, command=104):
        text = C.create_unicode_buffer(str(path))
        data = CopyData(command,C.sizeof(text),C.cast(text,C.c_void_p))
        return send(hwnd,0x4a,0,C.addressof(data))
    triangle = ROOT/'interactive-viewer/test-assets/tri_tight.glb'
    def valid_reopen():
        generation = open_file(triangle)
        wait(lambda: query(0)==3 and query(4)==generation, 'valid reopen Ready')
        assert query(16)==1 and query(45)==1, 'Ready without verified bounds and a complete render catalog'
        return generation
    def failure(path, expected, phase=None):
        old = query(4)
        before = query(12)
        generation = open_file(path)
        wait(lambda: query(0)==4, f'failure {path}')
        assert query(41)==expected, f'{path}: code {query(41)}, expected {expected}'
        assert query(4)==old, 'prior geometry lost on failure'
        send(hwnd,0x20a,120<<16)
        wait(lambda: query(12)!=before, 'prior camera input on failure')
        assert query(43)==1
        text = clipboard()
        assert str(directory) not in text and str(path) not in text and 'Preview3D-private-source' not in text, text
        extension = Path(str(path)).suffix[1:].upper()
        label = 'glTF' if extension=='GLTF' else extension
        assert f'Format: {label}' in text, text
        if phase:
            assert f'Phase: {phase}' in text, text
        report['events'].append({'input': Path(str(path)).name, 'generation': generation, 'code':expected, 'details': text})
    try:
        wait(lambda: find_window(app.pid), 'window')
        hwnd = find_window(app.pid)
        wait(lambda: query(2), 'visible background')
        valid_reopen()
        for name in ['zero.glb','zero.gltf','zero.stl','zero.ply']:
            failure(directory/name,11,'opening source')
            valid_reopen()
        for name, code in [('unsupported.FBX',8),('ascii.STL',9),('ascii.PLY',9),('empty.stl',11),('empty.ply',11),('empty.gltf',11),('required.gltf',10),('malformed.glb',1),('malformed.stl',1),('malformed.ply',1)]:
            failure(directory/name,code, 'opening source' if code==8 else 'parsing geometry')
            valid_reopen()
        corpus = ROOT/'interactive-viewer/test-assets/corpus'
        for name, code in [('sidecar-missing.gltf',6),('sidecar-traversal.gltf',5)]:
            failure(corpus/name,code,'resolving sidecars')
            valid_reopen()
        failure(r'\\server\private\remote.PLY',5,'opening source')
        valid_reopen()
        for fault, code, phase in [(1,14,None),(2,15,'waiting for importer'),(3,2,'parsing geometry'),(4,16,'uploading geometry / textures'),(5,3,'validating import')]:
            send(hwnd,0x8000+104,44,fault)
            failure(triangle,code,phase)
            send(hwnd,0x8000+104,44,0)
            send(hwnd,0x111,32775)  # actual Retry card command
            wait(lambda: query(0)==3, 'Retry Ready')
            report['events'][-1]['retrySucceeded'] = True
        retry_path = directory/'retry-private.glb'
        retry_path.write_bytes(b'not a model')
        failure(retry_path,1,'parsing geometry')
        retry_path.write_bytes(triangle.read_bytes())
        send(hwnd,0x111,32775)
        wait(lambda: query(0)==3,'same-path corrected Retry')
        failure(directory/'unsupported.FBX',8,'opening source')
        open_file(triangle,106)  # bounded stand-in for picker selection
        send(hwnd,0x111,32776)  # actual Open another card command
        wait(lambda: query(0)==3,'Open another Ready')
        generation = open_file(directory/'optional.glb')
        wait(lambda: query(0)==3 and query(4)==generation,'optional feature fallback')
        assert 0<query(40)<=256, 'warnings unbounded or absent'
        report['warningCharacters'] = query(40)
        # Cancel before old replies can enter the UI, followed by invalid activation.
        open_file(triangle,105)
        failure(directory/'unsupported.FBX',8)
        time.sleep(.3)
        assert query(0)==4, 'stale result replaced failure card'
        valid_reopen()
        send(hwnd,0x10)
        app.wait(timeout=10)
        assert app.returncode==0
        until = time.monotonic()+5
        while time.monotonic()<until and any(parent==app.pid for parent,name in processes().values() if name=='Preview3DImportWorker.exe'):
            time.sleep(.05)
        assert not any(parent==app.pid for parent,name in processes().values() if name=='Preview3DImportWorker.exe')
    except Exception as error:
        report['failure'] = str(error)
    finally:
        if app.poll() is None:
            app.kill(); app.wait(timeout=10)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configuration', choices=['Debug','Release'],default='Debug')
    parser.add_argument('--output', type=Path,required=True)
    args = parser.parse_args()
    output = args.output.resolve(); output.parent.mkdir(parents=True,exist_ok=True)
    directory = output.parent/'recovery-fixtures'
    hashes = fixtures(directory); assert fixtures(directory)==hashes
    exe = ROOT/f'x64/{args.configuration}/Preview3D.exe'
    report = {'configuration':args.configuration,'fixtureSha256':hashes,'exeSha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'run':run(exe,directory)}
    output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    if report['run']['failure']:
        raise SystemExit(1)


if __name__=='__main__':
    main()
