"""Deterministic generated fixtures for the render-quality lane.

These files are generated from code in this repository, so they can be
redistributed with the test tree and do not depend on the restricted external
model corpus. Each fixture is written with a small sidecar describing its
provenance and expected material roles.

Roles, not asset names, drive the capture regions:
  * dielectric / metal / glass spheres expose Studio, Clay and Directional;
  * the glass panel carries Khronos transmission/volume extensions plus an
    alpha-blended tint surface for a material-reference route;
  * the STL box is a CAD-like surface with hard edges and a ground contact.
"""
from __future__ import annotations

import json
import math
from pathlib import Path
import struct

from common import write_json
from studio import uv_sphere


def _glb(document: dict, binary: bytes, path: Path) -> Path:
    text = json.dumps(document, sort_keys=True, separators=(',', ':')).encode()
    text += b' ' * ((-len(text)) % 4)
    binary += b'\0' * ((-len(binary)) % 4)
    data = struct.pack('<III', 0x46546C67, 2, 28 + len(text) + len(binary))
    data += struct.pack('<II', len(text), 0x4E4F534A) + text
    data += struct.pack('<II', len(binary), 0x004E4942) + binary
    path.write_bytes(data)
    return path


def _mesh_builder():
    chunks, views, accessors = [], [], []
    offset = 0

    def add(data: bytes, target: int) -> int:
        nonlocal offset
        while offset % 4:
            chunks.append(b'\0')
            offset += 1
        start = offset
        chunks.append(data)
        offset += len(data)
        views.append({'buffer': 0, 'byteOffset': start, 'byteLength': len(data),
                      'target': target})
        return len(views) - 1

    def add_mesh(positions, normals, uvs, indices):
        pos = add(b''.join(struct.pack('<3f', *p) for p in positions), 34962)
        nrm = add(b''.join(struct.pack('<3f', *n) for n in normals), 34962)
        uv = add(b''.join(struct.pack('<2f', *t) for t in uvs), 34962)
        idx = add(b''.join(struct.pack('<I', i) for i in indices), 34963)
        base = len(accessors)
        accessors.extend([
            {'bufferView': pos, 'componentType': 5126, 'count': len(positions),
             'type': 'VEC3', 'min': [-1, -1, -1], 'max': [1, 1, 1]},
            {'bufferView': nrm, 'componentType': 5126, 'count': len(normals), 'type': 'VEC3'},
            {'bufferView': uv, 'componentType': 5126, 'count': len(uvs), 'type': 'VEC2'},
            {'bufferView': idx, 'componentType': 5125, 'count': len(indices), 'type': 'SCALAR'},
        ])
        return base

    def finish(document):
        binary = b''.join(chunks)
        document['accessors'] = accessors
        document['bufferViews'] = views
        document['buffers'] = [{'byteLength': len(binary)}]
        return document, binary

    return add_mesh, finish


def sphere_grid(path: Path) -> Path:
    """Six material spheres: white/black/red dielectric, gold/steel, glass."""
    materials = [
        {'name': 'white dielectric', 'pbrMetallicRoughness': {
            'baseColorFactor': [0.92, 0.92, 0.92, 1], 'metallicFactor': 0, 'roughnessFactor': 0.45}},
        {'name': 'black dielectric', 'pbrMetallicRoughness': {
            'baseColorFactor': [0.02, 0.02, 0.02, 1], 'metallicFactor': 0, 'roughnessFactor': 0.32}},
        {'name': 'red dielectric', 'pbrMetallicRoughness': {
            'baseColorFactor': [0.72, 0.04, 0.03, 1], 'metallicFactor': 0, 'roughnessFactor': 0.40}},
        {'name': 'rough gold metal', 'pbrMetallicRoughness': {
            'baseColorFactor': [1.0, 0.72, 0.28, 1], 'metallicFactor': 1, 'roughnessFactor': 0.38}},
        {'name': 'polished steel', 'pbrMetallicRoughness': {
            'baseColorFactor': [0.82, 0.85, 0.90, 1], 'metallicFactor': 1, 'roughnessFactor': 0.06}},
        {'name': 'transparent dielectric', 'alphaMode': 'BLEND',
         'pbrMetallicRoughness': {
             'baseColorFactor': [0.55, 0.72, 0.82, 0.35], 'metallicFactor': 0,
             'roughnessFactor': 0.05}},
    ]
    positions, normals, uvs, indices = uv_sphere()
    locations = [(-2.2, 1.1), (0.0, 1.1), (2.2, 1.1),
                 (-2.2, -1.1), (0.0, -1.1), (2.2, -1.1)]
    add_mesh, finish = _mesh_builder()
    primitives = []
    for index, (x, y) in enumerate(locations):
        scale, z = 0.82, 0.82
        base = add_mesh(
            [(px * scale + x, py * scale + y, pz * scale + z) for px, py, pz in positions],
            normals, uvs, indices)
        primitives.append({'attributes': {'POSITION': base, 'NORMAL': base + 1,
                                         'TEXCOORD_0': base + 2},
                           'indices': base + 3, 'material': index})
    document = {
        'asset': {'version': '2.0', 'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
        'meshes': [{'primitives': primitives}], 'materials': materials,
    }
    document, binary = finish(document)
    return _glb(document, binary, path)


def glass_reference(path: Path) -> Path:
    """Khronos-style glass roles: thin tint, rough transmission and a clear lens."""
    materials = [
        {
            'name': 'khronos thin tinted transmission',
            'doubleSided': True,
            'extensions': {
                'KHR_materials_transmission': {'transmissionFactor': 0.92},
                'KHR_materials_volume': {'thicknessFactor': 0.02,
                                         'attenuationDistance': 0.35,
                                         'attenuationColor': [0.72, 0.16, 0.08]},
            },
            'pbrMetallicRoughness': {
                'baseColorFactor': [0.84, 0.34, 0.22, 1.0],
                'metallicFactor': 0.0, 'roughnessFactor': 0.04},
        },
        {
            'name': 'khronos rough transmission',
            'extensions': {'KHR_materials_transmission': {'transmissionFactor': 0.75}},
            'pbrMetallicRoughness': {
                'baseColorFactor': [0.78, 0.86, 0.92, 1.0],
                'metallicFactor': 0.0, 'roughnessFactor': 0.42},
        },
        {
            'name': 'khronos clear lens dielectric',
            'alphaMode': 'BLEND',
            'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                           'KHR_materials_ior': {'ior': 1.5}},
            'pbrMetallicRoughness': {
                'baseColorFactor': [0.9, 0.95, 1.0, 0.28],
                'metallicFactor': 0.0, 'roughnessFactor': 0.02},
        },
    ]
    positions, normals, uvs, indices = uv_sphere(segments=32, rings=20)
    add_mesh, finish = _mesh_builder()
    primitives = []
    for index, (x, scale) in enumerate([(-1.7, 1.15), (0.0, 1.15), (1.7, 1.15)]):
        base = add_mesh([(px * scale + x, py * scale, pz * scale) for px, py, pz in positions],
                        normals, uvs, indices)
        primitives.append({'attributes': {'POSITION': base, 'NORMAL': base + 1,
                                         'TEXCOORD_0': base + 2},
                           'indices': base + 3, 'material': index})
    document = {
        'asset': {'version': '2.0', 'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'extensionsUsed': ['KHR_materials_transmission', 'KHR_materials_volume',
                           'KHR_materials_ior'],
        'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
        'meshes': [{'primitives': primitives}], 'materials': materials,
    }
    document, binary = finish(document)
    return _glb(document, binary, path)


def stl_box(path: Path) -> Path:
    """Binary STL box: hard edges, planar faces and a ground contact face."""
    corners = [
        (-1.0, -1.0, 0.0), (1.0, -1.0, 0.0), (1.0, 1.0, 0.0), (-1.0, 1.0, 0.0),
        (-1.0, -1.0, 2.0), (1.0, -1.0, 2.0), (1.0, 1.0, 2.0), (-1.0, 1.0, 2.0),
    ]
    faces = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7),
             (0, 1, 5), (0, 5, 4), (1, 2, 6), (1, 6, 5),
             (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
    data = bytearray(b'preview-3d T20 generated STL box\0'.ljust(80, b'\0'))
    data += struct.pack('<I', len(faces))
    for a, b, c in faces:
        data += struct.pack('<3f', 0.0, 0.0, 0.0)
        for index in (a, b, c):
            data += struct.pack('<3f', *corners[index])
        data += struct.pack('<H', 0)
    path.write_bytes(data)
    return path


FIXTURES = {
    'sphere-grid': (sphere_grid, 'sphere-grid.glb'),
    'glass-reference': (glass_reference, 'glass-reference.glb'),
    'stl-box': (stl_box, 'stl-box.stl'),
}


def generate(output: Path):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    generated = {}
    for name, (builder, filename) in FIXTURES.items():
        path = builder(output / filename)
        generated[name] = {
            'path': str(path),
            'sha256': __import__('common').sha256_file(path),
            'bytes': path.stat().st_size,
            'provenance': {
                'generator': 'tests/render-quality/fixtures.py',
                'redistribution': 'Generated in this repository; redistributable with the test tree.',
                'roles': {
                    'sphere-grid': ['white dielectric', 'black dielectric', 'red dielectric',
                                    'rough gold metal', 'polished steel', 'transparent dielectric'],
                    'glass-reference': ['thin tinted transmission', 'rough transmission',
                                        'clear lens dielectric'],
                    'stl-box': ['hard-edged STEP/STL-like surface', 'ground contact'],
                }[name],
            },
        }
    write_json(output / 'fixtures.json', {'schema': 1, 'fixtures': generated})
    return generated
