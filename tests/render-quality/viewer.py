"""Deterministic Preview3D control and capture for the render-quality lane."""
from __future__ import annotations

import ctypes as C
from ctypes import wintypes as W
import subprocess
import time
from pathlib import Path

from common import (ROOT, CopyData, capture_region, encoded_png, find_window,
                    image_stats, query, query_test, send, user, wait_for_window)

user.GetClientRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
user.GetDpiForWindow.argtypes = [W.HWND]


class ViewerError(RuntimeError):
    pass


def readiness_failure(state):
    """Why a viewer readiness snapshot is not a terminal resident complete load.

    Pure and side-effect free so the capture-contract negative self-tests can
    prove the rule without launching a viewer. ``None`` means the snapshot is
    acceptable: expected generation/Ready, complete (or explicit degraded),
    resident geometry, no pending detail and no terminal error. A terminal
    error wins even when a previous frame was complete, which is exactly the
    microscope's former late-ResourceLimit pattern.
    """
    if state.get('state') == 3 or state.get('errorCode', 0) not in (0,):
        return 'terminal error'
    if state.get('state') != 2:
        return 'not Ready'
    if not state.get('complete'):
        return 'complete handoff not reached'
    if state.get('requestedDetails', 0) != 0:
        return 'detail request still pending'
    if state.get('vertices', 0) <= 0:
        return 'no resident geometry'
    return None


class LoadFailed(ViewerError):
    def __init__(self, message, state, code, stage):
        super().__init__(message)
        self.state = state
        self.code = code
        self.stage = stage


class Viewport:
    def __init__(self, hwnd):
        rect = W.RECT()
        if not user.GetClientRect(hwnd, C.byref(rect)):
            raise C.WinError(C.get_last_error())
        self.width = rect.right
        self.height = rect.bottom
        self.dpi = int(user.GetDpiForWindow(hwnd))
        self.scale = self.dpi / 96.0


def set_dpi_awareness():
    if not user.SetProcessDpiAwarenessContext(W.HANDLE(-4)):
        raise C.WinError(C.get_last_error())


class Viewer:
    def __init__(self, configuration='Debug', output: Path | None = None,
                 width=1280, height=860, settle_timeout=120.0, environment=None):
        self.environment = environment
        self.exe = ROOT / f'x64/{configuration}/Preview3D.exe'
        if not self.exe.exists():
            raise ViewerError(f'built viewer not found: {self.exe}')
        self.output = Path(output) if output else None
        self.width = width
        self.height = height
        self.settle_timeout = settle_timeout
        self.process = None
        self.hwnd = 0

    def __enter__(self):
        self.process = subprocess.Popen([str(self.exe), '--app-smoke'],
                                        stderr=subprocess.PIPE,
                                        env=self.environment)
        self.hwnd = wait_for_window(self.process)
        # Keep the evidence window above the developer console while capturing;
        # an occluded flip-model swap chain can otherwise return a stale or
        # partially cleared PrintWindow surface.
        user.SetWindowPos(self.hwnd, C.c_void_p(-1), 30, 30, self.width, self.height, 0x14)
        user.SetForegroundWindow(self.hwnd)
        send(self.hwnd, 0x8000 + 104, 105, 1)  # capture-clean viewport
        time.sleep(0.4)
        return self

    def __exit__(self, exc_type, exc, tb):
        try:
            if self.hwnd:
                send(self.hwnd, 0x10)
        except Exception:
            pass
        if self.process and self.process.poll() is None:
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=8)
        if self.process and self.process.stderr:
            try:
                self.stderr = self.process.stderr.read().decode('utf-8', errors='replace')[:8192]
            except Exception:
                self.stderr = ''
            self.process.stderr.close()

    def open(self, path: Path):
        text = C.create_unicode_buffer(str(Path(path).resolve()))
        data = CopyData(104, C.sizeof(text), C.cast(text, C.c_void_p))
        generation = send(self.hwnd, 0x4A, 0, C.addressof(data))
        if not generation:
            raise LoadFailed('viewer rejected the input path', None, None, None)
        started = time.monotonic()
        deadline = started + self.settle_timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise ViewerError(f'viewer exited with {self.process.returncode}')
            state = query(self.hwnd, 0) - 1
            loaded_generation = send(self.hwnd, 0x8000 + 104, 47, 0)
            if state == 3:
                raise LoadFailed('terminal load failure (state Failed)', state,
                                 query(self.hwnd, 41), query(self.hwnd, 42))
            if state == 2 and loaded_generation == generation and query(self.hwnd, 45):
                # Bounds are verified, but textures may still be arriving.
                # T23 tracks the complete-load contract; T20 must at least
                # refuse to call an early, texture-less frame settled.
                stable = self.wait_textures_settled()
                return generation, time.monotonic() - started, stable
            time.sleep(0.05)
        raise LoadFailed(f'load did not settle within {self.settle_timeout:.0f}s',
                         query(self.hwnd, 0) - 1, query(self.hwnd, 41),
                         query(self.hwnd, 42))

    def texture_state(self):
        return {
            'extent': query(self.hwnd, 37),
            'count': query(self.hwnd, 38),
            'mips': query(self.hwnd, 39),
        }

    def import_state(self):
        """One bounded readiness snapshot from the Debug test-control surface.

        ``complete`` is the viewer's terminal handoff (the initial coarse proxy
        plus every required upload), ``requestedDetails`` proves no refinement
        is still in flight, and ``errorCode``/``errorStage`` expose a terminal
        error that arrived after geometry first became visible.
        """
        return {
            'state': query(self.hwnd, 0) - 1,
            'generation': query(self.hwnd, 1),
            'complete': bool(query(self.hwnd, 45)),
            'errorCode': query(self.hwnd, 41),
            'errorStage': query(self.hwnd, 42),
            'vertices': query(self.hwnd, 13),
            'triangles': query(self.hwnd, 14),
            'materials': query(self.hwnd, 19),
            'detailRequests': query(self.hwnd, 59),
            'requestedDetails': query(self.hwnd, 61),
            'textureExtent': query(self.hwnd, 37),
            'textureCount': query(self.hwnd, 38),
            'textureMips': query(self.hwnd, 39),
            'gpuBytes': query(self.hwnd, 55),
            'targetBytes': query(self.hwnd, 56),
            'presentedFrames': query(self.hwnd, 7),
        }

    @staticmethod
    def terminal_failure(state):
        """The typed terminal condition that must never count as a load."""
        return state['state'] == 3 or state['errorCode'] not in (0,)

    def wait_complete_hold(self, generation=None, hold_seconds=6.0, interval=0.25):
        """Hold a loaded model and fail on a late terminal error.

        The microscope used to become visible and then receive a
        ``GenerationDeadline``/``ResourceLimit`` failure minutes later, after
        every texture had already uploaded. A capture taken in that window is
        not a qualified image. Keep polling the same terminal fields for the
        whole hold so a late error is reported, not hidden by an earlier
        successful snapshot.
        """
        deadline = time.monotonic() + hold_seconds
        last = None
        while time.monotonic() < deadline:
            state = self.import_state()
            if self.terminal_failure(state):
                raise LoadFailed('late terminal error after complete geometry',
                                 state['state'], state['errorCode'], state['errorStage'])
            if generation is not None and state['generation'] != generation:
                raise ViewerError(
                    f'loaded generation {state["generation"]} does not match {generation}')
            if not state['complete'] or state['requestedDetails']:
                raise LoadFailed('complete-load readiness regressed during the hold',
                                 state['state'], state['errorCode'], state['errorStage'])
            last = state
            time.sleep(interval)
        return last if last is not None else self.import_state()

    def capabilities(self):
        return query(self.hwnd, 107)

    def model_transform(self):
        import struct

        values = []
        for field in range(108, 124):
            bits = send(self.hwnd, 0x8000 + 104, field, 0) & 0xFFFFFFFF
            values.append(round(struct.unpack('<f', struct.pack('<I', bits))[0], 9))
        return values

    def wait_textures_settled(self, timeout=45.0, quiet_seconds=2.0):
        """Wait for a terminal, resident, settled complete load.

        T23 replaces the old brightness-only completion test. A frame is only
        settled when the viewer reports the expected generation as terminal and
        complete with geometry and texture residency, no detailed upload is
        still in flight, and both the texture counters and the pixels have been
        quiet for ``quiet_seconds``. A terminal error discovered at any point --
        including one that arrives after visible geometry -- raises
        ``LoadFailed`` instead of returning a plausible-looking frame.
        """
        deadline = time.monotonic() + timeout
        quiet_started = None
        previous_textures = None
        previous_digest = None
        streak = 0
        last = None
        while time.monotonic() < deadline:
            state = self.import_state()
            if self.terminal_failure(state):
                raise LoadFailed('terminal load failure while waiting for complete frames',
                                 state['state'], state['errorCode'], state['errorStage'])
            frame = self.capture()
            textures = self.texture_state()
            stats = frame['stats']
            visible = (stats.get('uniqueSampleColours', 0) >= 20
                       and stats.get('lumaStdDev', 0) >= 3.0)
            # Required residency, not just brightness: the terminal handoff has
            # happened, real geometry uploaded, no refinement pending.
            resident = readiness_failure(state) is None
            digest = __import__('common').sha256_bytes(frame['rgb'])
            if resident and visible and textures == previous_textures and digest == previous_digest:
                quiet_started = quiet_started or time.monotonic()
                streak += 1
                if (time.monotonic() - quiet_started >= quiet_seconds and streak >= 3):
                    final = self.import_state()
                    if self.terminal_failure(final):
                        raise LoadFailed(
                            'terminal load failure at the end of the settle window',
                            final['state'], final['errorCode'], final['errorStage'])
                    if readiness_failure(final) is not None:
                        quiet_started = None
                        streak = 0
                        previous_textures = textures
                        previous_digest = digest
                        last = frame
                        time.sleep(0.2)
                        continue
                    frame['sha256'] = digest
                    frame['textures'] = textures
                    frame['readiness'] = final
                    return frame
            else:
                quiet_started = time.monotonic() if (resident and visible) else None
                streak = 0
            previous_textures = textures
            previous_digest = digest
            last = frame
            time.sleep(0.2)
        if last is not None:
            last['textures'] = previous_textures
            last['sha256'] = previous_digest
            last['settleTimedOut'] = True
            last['readiness'] = self.import_state()
            return last
        raise ViewerError('viewport never produced stable frames')

    def capture(self):
        # Nudge the window to the front before reading the swap chain.
        user.SetWindowPos(self.hwnd, C.c_void_p(-1), 30, 30, 0, 0, 0x0013)
        viewport = Viewport(self.hwnd)
        width, height, rgb, rect = capture_region(self.hwnd, viewport.dpi)
        return {
            'width': width,
            'height': height,
            'rgb': rgb,
            'rect': rect,
            'client': [viewport.width, viewport.height],
            'dpi': viewport.dpi,
            'dpiScale': round(viewport.scale, 4),
            'stats': image_stats(width, height, rgb),
        }

    def wait_stable(self, timeout=20.0, interval=0.25, repeats=3):
        deadline = time.monotonic() + timeout
        previous = None
        streak = 0
        while time.monotonic() < deadline:
            frame = self.capture()
            stats = frame['stats']
            visible = (stats.get('uniqueSampleColours', 0) >= 8
                       and stats.get('lumaStdDev', 0) >= 1.0)
            digest = __import__('common').sha256_bytes(frame['rgb'])
            if visible and digest == previous:
                streak += 1
                if streak >= repeats - 1:
                    frame['sha256'] = digest
                    return frame
            else:
                streak = 0
                previous = digest if visible else None
            time.sleep(interval)
        raise ViewerError('viewport never produced stable, non-blank frames')

    def set_camera(self, yaw_degrees=0.0, pitch_degrees=0.0, distance_scale=1.0,
                   projection='perspective'):
        send(self.hwnd, 0x8000 + 104, 36, 0)  # first-open framing, immediately
        send(self.hwnd, 0x8000 + 104, 31, int(round(yaw_degrees * 100)))
        send(self.hwnd, 0x8000 + 104, 32, int(round(pitch_degrees * 100)))
        send(self.hwnd, 0x8000 + 104, 33, int(round(distance_scale * 1000)))
        send(self.hwnd, 0x8000 + 104, 35, 0 if projection == 'perspective' else 1)
        time.sleep(0.1)
        before = query(self.hwnd, 7)
        send(self.hwnd, 0x8000 + 104, 106, 0)
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline and query(self.hwnd, 7) <= before:
            time.sleep(0.02)
        time.sleep(0.25)
        return self.wait_stable()

    def request_open(self, path: Path):
        """Send an open without waiting; returns the viewer generation."""
        text = C.create_unicode_buffer(str(Path(path).resolve()))
        data = CopyData(104, C.sizeof(text), C.cast(text, C.c_void_p))
        generation = send(self.hwnd, 0x4A, 0, C.addressof(data))
        if not generation:
            raise LoadFailed('viewer rejected the input path', None, None, None)
        return generation

    def wait_open(self, generation: int, timeout: float | None = None):
        """Wait for a generation requested with request_open()."""
        timeout = self.settle_timeout if timeout is None else timeout
        started = time.monotonic()
        deadline = started + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise ViewerError(f'viewer exited with {self.process.returncode}')
            state = query(self.hwnd, 0) - 1
            loaded_generation = send(self.hwnd, 0x8000 + 104, 47, 0)
            if state == 3:
                raise LoadFailed('terminal load failure (state Failed)', state,
                                 query(self.hwnd, 41), query(self.hwnd, 42))
            if state == 2 and loaded_generation == generation and query(self.hwnd, 45):
                stable = self.wait_textures_settled()
                return generation, time.monotonic() - started, stable
            time.sleep(0.05)
        raise LoadFailed(f'load did not settle within {timeout:.0f}s',
                         query(self.hwnd, 0) - 1, query(self.hwnd, 41),
                         query(self.hwnd, 42))

    def pick_at(self, x: int, y: int, timeout: float = 5.0):
        """Click a client pixel and wait for the deterministic picked identity."""
        before = query(self.hwnd, 254)
        point = W.POINT(int(x), int(y))
        user.ClientToScreen.argtypes = [W.HWND, C.POINTER(W.POINT)]
        user.ClientToScreen(self.hwnd, C.byref(point))
        user.SetCursorPos(point.x, point.y)
        time.sleep(0.03)
        packed = (int(x) & 0xFFFF) | ((int(y) & 0xFFFF) << 16)
        send(self.hwnd, 0x201, 1, packed)
        send(self.hwnd, 0x202, 0, packed)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and query(self.hwnd, 254) <= before:
            time.sleep(0.02)
        return {
            'hit': bool(query(self.hwnd, 22)),
            'id': query(self.hwnd, 251),
            'requests': query(self.hwnd, 34),
            'completions': query(self.hwnd, 254),
        }

    def pick_image(self, frame, x: float, y: float, timeout: float = 5.0):
        """Pick from viewport-crop image coordinates in physical client pixels."""
        before = query(self.hwnd, 254)
        scale = float(frame.get('dpiScale') or 1.0)
        px = max(0, min(65535, int(round(x * scale))))
        py = max(0, min(65535, int(round((frame['rect']['top'] + y) * scale))))
        send(self.hwnd, 0x8000 + 104, 258, px | (py << 16))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and query(self.hwnd, 254) <= before:
            time.sleep(0.02)
        return {
            'hit': bool(query(self.hwnd, 22)),
            'id': query(self.hwnd, 251),
            'completions': query(self.hwnd, 254),
        }

    def resize(self, width: int, height: int, settle: float = 1.5):
        """Resize the evidence window and let the render target rebuild."""
        user.SetWindowPos(self.hwnd, C.c_void_p(-1), 30, 30, int(width), int(height), 0x14)
        time.sleep(settle)
        return self.wait_stable()

    def apply_controlled_presentation(self):
        """One recorded neutral condition for every controlled comparison."""
        import struct

        def f32(value):
            return struct.unpack('<I', struct.pack('<f', float(value)))[0]

        send(self.hwnd, 0x8000 + 104, 70, 0)      # Automatic ground axis
        if query(self.hwnd, 21):
            send(self.hwnd, 0x8000 + 104, 30, 0)  # native orientation off
        send(self.hwnd, 0x8000 + 104, 90, 0)      # Studio PBR preset
        send(self.hwnd, 0x8000 + 104, 92, f32(1.0))  # exposure
        send(self.hwnd, 0x8000 + 104, 94, 0)      # dark background
        send(self.hwnd, 0x8000 + 104, 96, 0)      # PBR Neutral tone map
        send(self.hwnd, 0x8000 + 104, 98, f32(1.0))  # studio intensity
        send(self.hwnd, 0x8000 + 104, 101, 1)     # ground shadow query (no set)
        send(self.hwnd, 0x8000 + 104, 102, 1)     # ground shadow on
        send(self.hwnd, 0x8000 + 104, 104, 1)     # grid on
        send(self.hwnd, 0x8000 + 104, 196, 1)     # ground highlight on
        time.sleep(0.35)

    def presentation_state(self):
        import struct

        def f32(field):
            value = send(self.hwnd, 0x8000 + 104, field, 0) & 0xFFFFFFFF
            return struct.unpack('<f', struct.pack('<I', value))[0]

        return {
            'preset': query(self.hwnd, 89),
            'exposure': round(f32(91), 4),
            'background': query(self.hwnd, 93),
            'backgroundColor': query(self.hwnd, 197),
            'groundHighlight': bool(query(self.hwnd, 195)),
            'toneMap': query(self.hwnd, 95),
            'studioIntensity': round(f32(97), 4),
            'shadow': bool(query(self.hwnd, 101)),
            'grid': bool(query(self.hwnd, 103)),
            'lighting': query(self.hwnd, 87),
        }

    def camera_state(self):
        return {
            'yawDegrees': query_test(self.hwnd, 31) / 100.0,
            'pitchDegrees': query_test(self.hwnd, 32) / 100.0,
            'distanceScale': query_test(self.hwnd, 33) / 1000.0,
            'projection': 'orthographic' if query_test(self.hwnd, 35) else 'perspective',
            'fovYDegrees': 45.0,
            'distance': __import__('struct').unpack(
                '<d', __import__('struct').pack('<Q', send(self.hwnd, 0x8000 + 104, 26, 0)
                                                & 0xFFFFFFFFFFFFFFFF))[0],
            'homeDistance': __import__('struct').unpack(
                '<d', __import__('struct').pack('<Q', send(self.hwnd, 0x8000 + 104, 27, 0)
                                                & 0xFFFFFFFFFFFFFFFF))[0],
        }

    def model_state(self):
        return {
            'state': query(self.hwnd, 0) - 1,
            'generation': query(self.hwnd, 1),
            'complete': bool(query(self.hwnd, 45)),
            'boundsVerified': bool(query(self.hwnd, 16)),
            'vertices': query(self.hwnd, 13),
            'triangles': query(self.hwnd, 14),
            'materials': query(self.hwnd, 19),
            'groundAxis': query(self.hwnd, 71),
            'nativeOrientation': bool(query(self.hwnd, 21)),
            'dimensions': [
                __import__('struct').unpack(
                    '<d', __import__('struct').pack('<Q', send(self.hwnd, 0x8000 + 104, f, 0)
                                                    & 0xFFFFFFFFFFFFFFFF))[0]
                for f in (23, 24, 25)
            ],
            'sourceToViewerTransformRowMajor': self.model_transform(),
        }

    def performance(self):
        import struct

        def f64(field):
            return struct.unpack('<d', struct.pack('<Q', send(self.hwnd, 0x8000 + 104, field, 0)
                                                   & 0xFFFFFFFFFFFFFFFF))[0]

        return {
            'meanFrameMs': round(f64(99), 4),
            'p95FrameMs': round(f64(100), 4),
            'accountedGpuBytes': query(self.hwnd, 55),
            'privateBytes': query(self.hwnd, 77),
            'presentedFrames': query(self.hwnd, 7),
        }

    def save_capture(self, name: str, directory: Path):
        frame = self.capture()
        directory = Path(directory)
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / name
        path.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
        frame['path'] = str(path)
        return frame
