#!/usr/bin/env python3
"""Prepare the T10/T08 Assimp dispatcher seed corpus.

Each seed is a one-byte closed format discriminator (0 DAE, 1 3DS, 2 DXF,
3 DirectX, 4 OFF, 5 AMF, 6 X3D, 7 glTF, 8 OBJ, 9 FBX, 10 3MF) followed by the
generated fixture bytes. The fixtures are repository-owned (see
tests/fixtures/assimp-spike/README.md).
"""

from __future__ import annotations

import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests" / "fixtures" / "assimp-spike"

SEEDS = {
    "dae-box": (0, "box.dae"),
    "dae-missing-texture": (0, "missing-texture.dae"),
    "dae-truncated": (0, "truncated.dae"),
    "3ds-box": (1, "box.3ds"),
    "3ds-truncated": (1, "truncated.3ds"),
    "dxf-triangles": (2, "triangle.dxf"),
    "x-box": (3, "box.x"),
    "off-box": (4, "box.off"),
    "off-malformed": (4, "malformed.off"),
    "off-over-budget": (4, "over-budget.off"),
    "amf-box": (5, "box.amf"),
    "amf-malformed": (5, "malformed.amf"),
    "x3d-box": (6, "box.x3d"),
    # T08 same-family containers.
    "gltf-box": (7, "box.gltf"),
    "gltf-required-extension": (7, "required-extension.gltf"),
    "obj-box": (8, "box.obj"),
    "obj-line-only": (8, "line-only.obj"),
    "obj-missing-vt": (8, "missing-vt.obj"),
    "obj-malformed": (8, "malformed.obj"),
    "fbx-box": (9, "box.fbx"),
    "fbx-line-only": (9, "line-only.fbx"),
    "fbx-malformed": (9, "malformed.fbx"),
    "3mf-box": (10, "box.3mf"),
    "3mf-required-extension": (10, "required-extension.3mf"),
    "3mf-malformed": (10, "malformed.3mf"),
}


def main() -> int:
    destination = pathlib.Path(sys.argv[1])
    destination.mkdir(parents=True, exist_ok=True)
    for name, (format_id, fixture) in SEEDS.items():
        source = FIXTURES / fixture
        if not source.is_file():
            print(f"missing fixture {source}", file=sys.stderr)
            return 1
        (destination / name).write_bytes(bytes((format_id,)) + source.read_bytes())
    # Keep the seed directory in sync when the caller reuses it locally.
    for stale in destination.iterdir():
        if stale.name not in SEEDS:
            if stale.is_dir():
                shutil.rmtree(stale)
            else:
                stale.unlink()
    print(f"prepared {len(SEEDS)} Assimp seeds in {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
