"""T21a Windows Photos-style background and ground-highlight verification.

The T20/T21 capture lane proves that images can be captured. This module proves
the actual appearance contract with real GPU pixels:

* the untouched backdrop is the selected display colour, not a tone-mapped
  linear value, and is stable across exposure and tone-map changes;
* the backdrop survives model load and every lighting mode;
* background/highlight are global persisted preferences and a manual preset
  does not reset them;
* highlight, contact shadow and grid are independent;
* the ground highlight never changes model pixels.

Evidence images are written beside the numeric results so an image-capable
reviewer can inspect the actual viewport, not a shader string.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import struct
import time

from common import ROOT, encoded_png, send, send_field, sha256_file, write_json
from viewer import Viewer, ViewerError

# Physics-independent display colours from Presentation.cpp. These are the
# values the GPU must put on screen after PNG decode, with a small tolerance
# only for UNORM/PNG quantisation.
EXPECTED = {
    0: ('Windows Photos dark', (0x1C, 0x1C, 0x1E)),
    1: ('Studio gray', (0x80, 0x80, 0x80)),
    4: ('Custom', (0x33, 0x66, 0x99)),
}
TOLERANCE = 3  # 8-bit channel units

FIELD_BACKGROUND = 94
FIELD_SHADOW = 102
FIELD_SHADOW_QUERY = 101
FIELD_GRID = 104
FIELD_GRID_QUERY = 103
FIELD_HIGHLIGHT_QUERY = 195
FIELD_HIGHLIGHT = 196
FIELD_CUSTOM_COLOR = 198
FIELD_SAVE = 199


def settings_path() -> Path:
    local = Path(os.environ.get('LOCALAPPDATA', Path.home() / 'AppData/Local'))
    return local / 'Binbuf' / 'Preview 3D' / 'settings.json'


def _f32(value):
    return struct.unpack('<I', struct.pack('<f', float(value)))[0]


def _mean(width, height, rgb, rect):
    x0, y0, x1, y1 = rect
    left = max(0, min(width - 1, int(x0 * width)))
    top = max(0, min(height - 1, int(y0 * height)))
    right = max(left + 1, min(width, int(x1 * width)))
    bottom = max(top + 1, min(height, int(y1 * height)))
    total = [0, 0, 0]
    count = 0
    for y in range(top, bottom):
        row = (y * width) * 3
        for x in range(left, right):
            index = row + x * 3
            total[0] += rgb[index]
            total[1] += rgb[index + 1]
            total[2] += rgb[index + 2]
            count += 1
    return tuple(channel / max(1, count) for channel in total)


def _mean_abs_difference(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / 3.0


def _corner(frame):
    return _mean(frame['width'], frame['height'], frame['rgb'], (0.01, 0.01, 0.10, 0.10))


def _save_image(output: Path, name: str, frame):
    path = output / 'images' / f'{name}.png'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(encoded_png(frame['width'], frame['height'], frame['rgb']))
    return {'name': name, 'path': str(path), 'sha256': sha256_file(path),
            'width': frame['width'], 'height': frame['height'],
            'corner': [round(v, 2) for v in _corner(frame)]}


def _settle(viewer):
    # Force synchronous render-thread passes until the swap chain has actually
    # presented. The very first PrintWindow capture can otherwise see the
    # window's white pre-first-frame surface.
    deadline = time.monotonic() + 8.0
    frame = None
    while time.monotonic() < deadline:
        send_field(viewer.hwnd, 106, 0)
        time.sleep(0.20)
        frame = viewer.capture()
        corner = _corner(frame)
        if max(corner) < 250.0:
            return frame
    return frame if frame is not None else viewer.capture()


def _query(viewer, field):
    return int(send_field(viewer.hwnd, field, 0))


def _set(viewer, field, value):
    send_field(viewer.hwnd, field, value)
    time.sleep(0.15)


def _write_settings(background=0, color=0x1C1C1E, highlight=True, raw=None):
    path = settings_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    if raw is not None:
        path.write_text(raw, encoding='utf-8')
        return
    path.write_text(json.dumps({
        'version': 8,
        'showNativeOrientation': False,
        'groundAxis': 'Automatic',
        'groundAxisInverted': False,
        'hideCursorWhileDragging': True,
        'hideModelUntilComplete': True,
        'toneMapMode': 0,
        'backgroundMode': background,
        'backgroundColor': color,
        'groundHighlightEnabled': highlight,
        'groundShadowEnabled': True,
        'groundShadowOverridden': False,
        'exposure': 1.0,
        'language': '',
    }, indent=2) + '\n', encoding='utf-8')


def _capture_variant(output, viewer, name, background, color, exposure,
                     tone, highlight, shadow, grid, images, failures, record):
    _set(viewer, FIELD_BACKGROUND, background)
    _set(viewer, FIELD_CUSTOM_COLOR, color)
    send_field(viewer.hwnd, 92, _f32(exposure))
    send_field(viewer.hwnd, 96, tone)
    _set(viewer, FIELD_HIGHLIGHT, 1 if highlight else 0)
    _set(viewer, FIELD_SHADOW, 1 if shadow else 0)
    _set(viewer, FIELD_GRID, 1 if grid else 0)
    frame = _settle(viewer)
    image = _save_image(output, name, frame)
    images.append(image)
    corner = image['corner']
    expected = EXPECTED[background][1]
    delta = max(abs(corner[0] - expected[0]), abs(corner[1] - expected[1]),
                abs(corner[2] - expected[2]))
    ok = delta <= TOLERANCE
    entry = {
        'name': name, 'background': background, 'backgroundColor': f'#{color:06X}',
        'exposure': exposure, 'toneMap': tone, 'highlight': highlight,
        'shadow': shadow, 'grid': grid,
        'cornerRgb': corner, 'expectedRgb': list(expected),
        'maxChannelDelta': round(delta, 2), 'pass': ok,
        'modelRegion': [round(v, 2) for v in _mean(
            frame['width'], frame['height'], frame['rgb'], (0.355, 0.30, 0.395, 0.36))],
        'groundRegion': [round(v, 2) for v in _mean(
            frame['width'], frame['height'], frame['rgb'], (0.36, 0.86, 0.64, 0.97))],
    }
    record.append(entry)
    if not ok:
        failures.append(
            f'{name}: backdrop {corner} expected {expected} (delta {delta:.1f})')
    return entry


def run(configuration: str, output: Path, fixture_path: Path, inventory=None):
    inventory = inventory or {}
    output = Path(output) / 'appearance'
    output.mkdir(parents=True, exist_ok=True)
    report = {
        'task': 'T21a', 'status': 'running', 'failures': [], 'checks': [],
        'images': [], 'persistence': {}, 'modes': {}, 'settingsUi': {},
    }
    failures = report['failures']
    images = report['images']
    settings = settings_path()
    original = settings.read_bytes() if settings.exists() else None
    try:
        # Deterministic start: the test owns the tiny settings file for the
        # duration, and restores the developer's file in finally.
        _write_settings()
        with Viewer(configuration, output, width=1280, height=860) as viewer:
            defaults = {
                'background': _query(viewer, 93),
                'customColor': _query(viewer, 197),
                'highlight': _query(viewer, 195),
            }
            report['defaults'] = defaults
            if defaults != {'background': 0, 'customColor': 0x1C1C1E, 'highlight': 1}:
                failures.append(f'default appearance is not Windows Photos dark: {defaults}')

            # No model yet: untouched backdrop must already be exact.
            empty = _save_image(output, 'no-model-dark', _settle(viewer))
            images.append(empty)
            corner = empty['corner']
            if max(abs(corner[i] - (0x1C, 0x1C, 0x1E)[i]) for i in range(3)) > TOLERANCE:
                failures.append(f'no-model backdrop is not #1C1C1E: {corner}')

            report['checks'].append({
                'id': 'no-model-dark', 'cornerRgb': corner,
                'pass': max(abs(corner[i] - (0x1C, 0x1C, 0x1E)[i]) for i in range(3)) <= TOLERANCE,
            })

            variants = []
            # Display-colour contract across exposure/tone-map.
            for background, color in ((0, 0x1C1C1E), (1, 0x1C1C1E), (4, 0x336699)):
                for exposure, tone in ((0.30, 0), (1.0, 1), (2.5, 2), (1.0, 0)):
                    variants.append(_capture_variant(
                        output, viewer,
                        f'empty-{EXPECTED[background][0].lower().replace(" ", "-")}'
                        f'-exp{exposure}-tm{tone}',
                        background, color, exposure, tone, True, True, True,
                        images, failures, report['checks']))
            report['emptyVariants'] = variants

            # Open the generated fixture; the backdrop must not change just
            # because a model is present.
            viewer.open(Path(fixture_path))
            for background, color in ((0, 0x1C1C1E), (1, 0x1C1C1E), (4, 0x336699)):
                _capture_variant(
                    output, viewer,
                    f'model-{EXPECTED[background][0].lower().replace(" ", "-")}',
                    background, color, 1.0, 0, True, True, True,
                    images, failures, report['checks'])

            # Independence matrix: 8 highlight/shadow/grid combinations.
            combos = []
            for highlight in (True, False):
                for shadow in (True, False):
                    for grid in (True, False):
                        combos.append(_capture_variant(
                            output, viewer,
                            f'combo-h{int(highlight)}-s{int(shadow)}-g{int(grid)}',
                            0, 0x1C1C1E, 1.0, 0, highlight, shadow, grid,
                            images, failures, report['checks']))
                        queried = (_query(viewer, FIELD_HIGHLIGHT_QUERY),
                                   _query(viewer, FIELD_SHADOW_QUERY),
                                   _query(viewer, FIELD_GRID_QUERY))
                        if queried != (int(highlight), int(shadow), int(grid)):
                            failures.append(
                                f'combo h{int(highlight)}s{int(shadow)}g{int(grid)} '
                                f'queried {queried}')
            report['combinations'] = combos
            # With the highlight ON, the shadow toggle must still change the
            # ground. This catches the old single-pass bug where the shadow was
            # always drawn whenever the highlight kept the pass alive.
            combo_by_name = {item['name']: item for item in combos}
            shadow_hl_on = _mean_abs_difference(
                combo_by_name['combo-h1-s1-g1']['groundRegion'],
                combo_by_name['combo-h1-s0-g1']['groundRegion'])
            report['shadowWithHighlight'] = {
                'groundDelta': round(shadow_hl_on, 4), 'pass': shadow_hl_on >= 1.0}
            if shadow_hl_on < 1.0:
                failures.append(
                    f'contact shadow toggle had no effect with highlight on ({shadow_hl_on:.2f})')

            # Model pixels must be identical with the highlight on/off; the
            # ground area must change. This is the core decoupling assertion.
            _set(viewer, FIELD_SHADOW, 0)
            _set(viewer, FIELD_GRID, 0)
            _set(viewer, FIELD_HIGHLIGHT, 1)
            on = _settle(viewer)
            _set(viewer, FIELD_HIGHLIGHT, 0)
            off = _settle(viewer)
            on_model = _mean(on['width'], on['height'], on['rgb'], (0.355, 0.30, 0.395, 0.36))
            off_model = _mean(off['width'], off['height'], off['rgb'], (0.355, 0.30, 0.395, 0.36))
            on_ground = _mean(on['width'], on['height'], on['rgb'], (0.36, 0.86, 0.64, 0.97))
            off_ground = _mean(off['width'], off['height'], off['rgb'], (0.36, 0.86, 0.64, 0.97))
            model_delta = _mean_abs_difference(on_model, off_model)
            ground_delta = _mean_abs_difference(on_ground, off_ground)
            report['decoupling'] = {
                'modelDelta': round(model_delta, 4),
                'groundDelta': round(ground_delta, 4),
                'modelRgbOn': [round(v, 2) for v in on_model],
                'modelRgbOff': [round(v, 2) for v in off_model],
                'groundRgbOn': [round(v, 2) for v in on_ground],
                'groundRgbOff': [round(v, 2) for v in off_ground],
                'pass': model_delta <= 1.0 and ground_delta >= 1.0,
            }
            if model_delta > 1.0:
                failures.append(f'ground highlight changed model pixels by {model_delta:.2f}')
            if ground_delta < 1.0:
                failures.append(f'ground highlight did not change the ground ({ground_delta:.2f})')
            images.append(_save_image(output, 'decoupling-highlight-on', on))
            images.append(_save_image(output, 'decoupling-highlight-off', off))

            # Shadow independence with highlight off.
            _set(viewer, FIELD_GRID, 0)
            _set(viewer, FIELD_HIGHLIGHT, 0)
            _set(viewer, FIELD_SHADOW, 1)
            shadow_on = _settle(viewer)
            _set(viewer, FIELD_SHADOW, 0)
            shadow_off = _settle(viewer)
            shadow_ground_on = _mean(shadow_on['width'], shadow_on['height'], shadow_on['rgb'],
                                     (0.36, 0.86, 0.64, 0.97))
            shadow_ground_off = _mean(shadow_off['width'], shadow_off['height'], shadow_off['rgb'],
                                      (0.36, 0.86, 0.64, 0.97))
            shadow_delta = _mean_abs_difference(shadow_ground_on, shadow_ground_off)
            report['shadowIndependence'] = {
                'highlightOffGroundDelta': round(shadow_delta, 4), 'pass': shadow_delta >= 1.0}
            if shadow_delta < 1.0:
                failures.append('contact shadow is not visible with the highlight off')

            # All three lighting modes preserve the appearance preferences.
            for mode, name in ((0, 'studio'), (1, 'clay'), (2, 'directional')):
                _set(viewer, FIELD_BACKGROUND, 4)
                _set(viewer, FIELD_CUSTOM_COLOR, 0x336699)
                _set(viewer, FIELD_HIGHLIGHT, 0)
                send_field(viewer.hwnd, 88, mode)
                time.sleep(0.35)
                state = (_query(viewer, 93), _query(viewer, 197), _query(viewer, 195))
                report['modes'][name] = {'state': list(state), 'pass': state == (4, 0x336699, 0)}
                if state != (4, 0x336699, 0):
                    failures.append(f'{name} lighting changed appearance preferences: {state}')

            # Manual preset must not reset the global appearance.
            send_field(viewer.hwnd, 90, 3)  # Transparent preset
            time.sleep(0.4)
            preset_state = (_query(viewer, 93), _query(viewer, 197), _query(viewer, 195))
            report['presetPrecedence'] = {
                'state': list(preset_state), 'pass': preset_state == (4, 0x336699, 0)}
            if preset_state != (4, 0x336699, 0):
                failures.append(f'manual preset reset appearance preferences: {preset_state}')

            # Settings panel images: real 100/150/200% DPI layout, high
            # contrast, a narrow window, and keyboard traversal. Field 67 is
            # the existing Debug DPI test control; 68 is high contrast.
            _set(viewer, FIELD_BACKGROUND, 0)
            _set(viewer, FIELD_CUSTOM_COLOR, 0x1C1C1E)
            _set(viewer, FIELD_HIGHLIGHT, 1)
            send_field(viewer.hwnd, 105, 0)  # show chrome for the Settings evidence
            send(viewer.hwnd, 0x111, 32792, 0)  # ID_VIEW_SETTINGS
            time.sleep(0.3)
            for dpi, label in ((96, '100'), (144, '150'), (192, '200')):
                send_field(viewer.hwnd, 67, dpi)
                time.sleep(0.35)
                ui = _save_image(output, f'settings-dpi-{label}', viewer.capture())
                images.append(ui)
                report['settingsUi'][f'dpi{label}'] = ui
            send_field(viewer.hwnd, 67, 96)
            send_field(viewer.hwnd, 68, 1)
            time.sleep(0.35)
            ui_contrast = _save_image(output, 'settings-high-contrast', viewer.capture())
            images.append(ui_contrast)
            report['settingsUi']['highContrast'] = ui_contrast
            send_field(viewer.hwnd, 68, 0)
            time.sleep(0.25)
            user32 = __import__('ctypes').windll.user32
            user32.SetWindowPos(viewer.hwnd, __import__('ctypes').c_void_p(-1), 40, 40,
                                900, 620, 0x0014)
            time.sleep(0.4)
            ui_narrow = _save_image(output, 'settings-narrow-window', viewer.capture())
            images.append(ui_narrow)
            report['settingsUi']['narrow'] = ui_narrow
            # Keyboard traversal reaches the two new controls through the same
            # accessibility order used by screen readers.
            for _ in range(24):
                send(viewer.hwnd, 0x100, 0x09, 0)  # WM_KEYDOWN VK_TAB
                time.sleep(0.05)
            report['settingsUi']['keyboardTabWalk'] = 'exercised without exception'

            # Car/Camera glass review on all three opaque backdrops. These are
            # local restricted assets; absence is reported, never a pass claim.
            external = {}
            seeds = inventory.get('seedAssets', {}) if inventory.get('available') else {}
            views = {
                'car': [('front', 0.0, 90.0, 1.0), ('window', 18.0, 70.0, 0.48)],
                'camera': [('front', 0.0, 90.0, 1.0), ('lens', 0.0, 90.0, 0.42)],
            }
            if any((seeds.get(alias) or {}).get('available') for alias in views):
                send_field(viewer.hwnd, 105, 1)  # back to capture-clean viewport
            for alias, asset_views in views.items():
                seed = seeds.get(alias)
                if not seed or not seed.get('available'):
                    external[alias] = {'status': 'unavailable'}
                    continue
                try:
                    viewer.open(Path(seed['entry']))
                except ViewerError as error:
                    external[alias] = {'status': 'failed', 'error': str(error)}
                    continue
                captures = []
                for bg_name, background in (('dark', 0), ('gray', 1), ('custom', 4)):
                    _set(viewer, FIELD_BACKGROUND, background)
                    _set(viewer, FIELD_CUSTOM_COLOR, 0x336699)
                    _set(viewer, FIELD_HIGHLIGHT, 1)
                    _set(viewer, FIELD_SHADOW, 1)
                    _set(viewer, FIELD_GRID, 1)
                    send_field(viewer.hwnd, 92, _f32(1.0))
                    send_field(viewer.hwnd, 96, 0)
                    for view_name, yaw, pitch, scale in asset_views:
                        viewer.set_camera(yaw, pitch, scale)
                        frame = _settle(viewer)
                        image = _save_image(
                            output, f'external-{alias}-{view_name}-{bg_name}', frame)
                        captures.append(image)
                        images.append(image)
                external[alias] = {'status': 'captured', 'images': captures}
            report['external'] = external

        # Restart persistence: the second process reads the saved file.
        _write_settings(background=4, color=0xA1B2C3, highlight=False)
        with Viewer(configuration, output, width=1024, height=700) as viewer:
            state = (_query(viewer, 93), _query(viewer, 197), _query(viewer, 195))
            report['persistence']['restart'] = {'state': list(state),
                                                'pass': state == (4, 0xA1B2C3, 0)}
            if state != (4, 0xA1B2C3, 0):
                failures.append(f'background/highlight did not persist across restart: {state}')
            viewer.open(Path(fixture_path))
            after_open = (_query(viewer, 93), _query(viewer, 197), _query(viewer, 195))
            report['persistence']['afterModelOpen'] = {
                'state': list(after_open), 'pass': after_open == (4, 0xA1B2C3, 0)}
            if after_open != (4, 0xA1B2C3, 0):
                failures.append(f'model open changed saved appearance: {after_open}')
            images.append(_save_image(output, 'restart-custom-highlight-off', _settle(viewer)))

        # Malformed / out-of-range values fall back without breaking unrelated
        # settings (the model stays hidden-until-complete = true).
        _write_settings(raw='{\n  "version": 999,\n  "backgroundMode": 99,\n'
                            '  "backgroundColor": 999999999,\n'
                            '  "groundHighlightEnabled": "yes",\n'
                            '  "hideModelUntilComplete": false\n}\n')
        with Viewer(configuration, output, width=1024, height=700) as viewer:
            fallback = (_query(viewer, 93), _query(viewer, 197), _query(viewer, 195))
            report['persistence']['malformedFallback'] = {
                'state': list(fallback), 'pass': fallback == (0, 0x1C1C1E, 1)}
            if fallback != (0, 0x1C1C1E, 1):
                failures.append(f'malformed settings did not fall back safely: {fallback}')
    finally:
        if original is None:
            if settings.exists():
                settings.unlink()
        else:
            settings.parent.mkdir(parents=True, exist_ok=True)
            settings.write_bytes(original)

    report['status'] = 'pass' if not failures else 'fail'
    write_json(output / 'appearance.json', report)
    return report
