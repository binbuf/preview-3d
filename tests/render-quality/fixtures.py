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

import base64
import json
import math
from pathlib import Path
import struct

from common import encoded_png, write_json
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

    def add_mesh(positions, normals, uvs, indices, tangents=None, colors=None):
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
        if tangents is not None:
            tangent = add(b''.join(struct.pack('<4f', *t) for t in tangents), 34962)
            accessors.append({'bufferView': tangent, 'componentType': 5126,
                              'count': len(tangents), 'type': 'VEC4'})
        if colors is not None:
            color = add(b''.join(struct.pack('<4f', *c) for c in colors), 34962)
            accessors.append({'bufferView': color, 'componentType': 5126,
                              'count': len(colors), 'type': 'VEC4'})
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


def material_probe(path: Path) -> Path:
    """T21 material/light probe: five constant-material spheres in a row.

    Every value is authored here and repeated in ``EXPECTED['material-probe']``
    so the test tooling can compare the source document with the viewer's
    validated wire values without the viewer ever parsing glTF.
    """
    materials = [
        {'name': 'probe dielectric smooth',
         'pbrMetallicRoughness': {'baseColorFactor': [0.8, 0.2, 0.1, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.1}},
        {'name': 'probe metal rough',
         'pbrMetallicRoughness': {'baseColorFactor': [0.2, 0.7, 0.3, 1.0],
                                  'metallicFactor': 1.0, 'roughnessFactor': 0.8}},
        {'name': 'probe clearcoat',
         'extensions': {'KHR_materials_clearcoat': {
             'clearcoatFactor': 0.75, 'clearcoatRoughnessFactor': 0.1}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.1, 0.3, 0.9, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.4}},
        {'name': 'probe blend sheen emission',
         'alphaMode': 'BLEND',
         'emissiveFactor': [0.2, 0.1, 0.05],
         'extensions': {
             'KHR_materials_emissive_strength': {'emissiveStrength': 2.0},
             'KHR_materials_sheen': {'sheenColorFactor': [0.4, 0.5, 0.6],
                                     'sheenRoughnessFactor': 0.5},
             'KHR_materials_specular': {'specularFactor': 0.8,
                                        'specularColorFactor': [1.0, 0.9, 0.8]},
             'KHR_materials_ior': {'ior': 1.7},
         },
         'pbrMetallicRoughness': {'baseColorFactor': [0.9, 0.8, 0.2, 0.5],
                                  'metallicFactor': 0.5, 'roughnessFactor': 0.3}},
        {'name': 'probe transmissive',
         'alphaMode': 'BLEND',
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 0.9},
                        'KHR_materials_ior': {'ior': 1.33}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.7, 0.9, 0.95, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.05}},
        {'name': 'probe masked cutout',
         'alphaMode': 'MASK', 'alphaCutoff': 0.2,
         'pbrMetallicRoughness': {'baseColorFactor': [0.9, 0.1, 0.7, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.6}},
    ]
    positions, normals, uvs, indices = uv_sphere(segments=32, rings=20)
    add_mesh, finish = _mesh_builder()
    primitives = []
    for index, x in enumerate([-3.0, -1.8, -0.6, 0.6, 1.8, 3.0]):
        scale = 0.55
        base = add_mesh([(px * scale + x, py * scale, pz * scale + scale) for px, py, pz in positions],
                        normals, uvs, indices,
                        tangents=[(1.0, 0.0, 0.0, 1.0)] * len(positions),
                        colors=[(1.0, 1.0, 1.0, 1.0)] * len(positions))
        primitives.append({'attributes': {'POSITION': base, 'NORMAL': base + 1,
                                         'TEXCOORD_0': base + 2, 'TANGENT': base + 4,
                                         'COLOR_0': base + 5},
                           'indices': base + 3, 'material': index})
    document = {
        'asset': {'version': '2.0', 'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'extensionsUsed': ['KHR_materials_transmission', 'KHR_materials_ior',
                           'KHR_materials_clearcoat', 'KHR_materials_sheen',
                           'KHR_materials_specular', 'KHR_materials_emissive_strength'],
        'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
        'meshes': [{'primitives': primitives}], 'materials': materials,
    }
    document, binary = finish(document)
    return _glb(document, binary, path)


def _png_data_uri(pixels: bytes, width: int = 4, height: int = 4) -> str:
    return 'data:image/png;base64,' + base64.b64encode(
        encoded_png(width, height, pixels)).decode('ascii')


def _solid(width, height, rgb):
    return bytes(rgb) * (width * height)


def material_semantics(path: Path) -> Path:
    """T22 authored-material fixture.

    Ten spheres isolate one authored input each: normal scale 0/0.45/1,
    MR-red-only versus an independent occlusion binding, an unsupported UV1
    reference, per-slot transforms/samplers, a masked cutout, and IOR 1 versus
    1.5 transmission. Every image is embedded as a data URI so the fixture is a
    single redistributable file.
    """
    # Non-flat tangent-space normal map: the R channel varies so normalScale
    # changes the resulting shading normal (diagnostic channel 3).
    normal_width = 4
    normal_pixels = bytearray()
    for y in range(normal_width):
        for x in range(normal_width):
            normal_pixels += bytes((64 + x * 48, 128, 255))
    normal_uri = _png_data_uri(bytes(normal_pixels))

    color_pixels = bytearray()
    for y in range(4):
        for x in range(4):
            color_pixels += bytes((230, 210, 190) if (x + y) % 2 else (40, 90, 160))
    color_uri = _png_data_uri(bytes(color_pixels))
    mr_uri = _png_data_uri(_solid(4, 4, (51, 128, 0)))   # R=0.2 is NOT AO
    ao_uri = _png_data_uri(_solid(4, 4, (51, 51, 51)))   # independent AO 0.2

    materials = [
        {'name': 'normal scale 0',
         'normalTexture': {'index': 2, 'scale': 0.0},
         'pbrMetallicRoughness': {'baseColorFactor': [0.55, 0.55, 0.55, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.55}},
        {'name': 'normal scale 045',
         'normalTexture': {'index': 2, 'scale': 0.45},
         'pbrMetallicRoughness': {'baseColorFactor': [0.55, 0.55, 0.55, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.55}},
        {'name': 'normal scale 1',
         'normalTexture': {'index': 2, 'scale': 1.0},
         'pbrMetallicRoughness': {'baseColorFactor': [0.55, 0.55, 0.55, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.55}},
        {'name': 'MR red is not AO',
         'pbrMetallicRoughness': {'baseColorFactor': [0.6, 0.6, 0.6, 1],
                                  'metallicFactor': 1.0, 'roughnessFactor': 1.0,
                                  'metallicRoughnessTexture': {'index': 3}}},
        {'name': 'independent occlusion',
         'occlusionTexture': {'index': 4, 'strength': 1.0},
         'pbrMetallicRoughness': {'baseColorFactor': [0.6, 0.6, 0.6, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.8}},
        {'name': 'uv1 unsupported',
         'pbrMetallicRoughness': {'baseColorFactor': [0.8, 0.3, 0.2, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.5,
                                  'baseColorTexture': {'index': 0, 'texCoord': 1}}},
        {'name': 'per slot transform',
         'normalTexture': {'index': 2, 'scale': 1.0, 'extensions': {
             'KHR_texture_transform': {'offset': [0.25, 0.1], 'scale': [2.0, 2.0],
                                       'rotation': 0.4}}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.7, 0.7, 0.7, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.5,
                                  'baseColorTexture': {'index': 0, 'extensions': {
                                      'KHR_texture_transform': {'offset': [0.5, 0.5],
                                                                'scale': [3.0, 3.0]}}}}},
        {'name': 'alpha cutout',
         'alphaMode': 'MASK', 'alphaCutoff': 0.2,
         'pbrMetallicRoughness': {'baseColorFactor': [0.9, 0.1, 0.7, 0.35],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.6}},
        {'name': 'ior 1 transmission suppressed',
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.0}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.72, 0.02, 0.02, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.4}},
        {'name': 'ior 15 transmission',
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 0.9},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.7, 0.9, 0.95, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.05}},
    ]
    positions, normals, uvs, indices = uv_sphere(segments=32, rings=20)
    add_mesh, finish = _mesh_builder()
    primitives = []
    locations = [(-4.5, 1.15), (-3.0, 1.15), (-1.5, 1.15), (0.0, 1.15), (1.5, 1.15),
                 (3.0, 1.15), (4.5, 1.15), (-2.25, -1.15), (0.0, -1.15), (2.25, -1.15)]
    for index, (x, y) in enumerate(locations):
        scale = 0.62
        base = add_mesh(
            [(px * scale + x, py * scale + y, pz * scale + scale) for px, py, pz in positions],
            normals, uvs, indices,
            tangents=[(1.0, 0.0, 0.0, 1.0)] * len(positions),
            colors=[(1.0, 1.0, 1.0, 1.0)] * len(positions))
        primitives.append({'attributes': {'POSITION': base, 'NORMAL': base + 1,
                                         'TEXCOORD_0': base + 2, 'TANGENT': base + 4,
                                         'COLOR_0': base + 5},
                           'indices': base + 3, 'material': index})
    # A separate mirrored sphere exercises the negative-determinant tangent
    # handedness path without instancing every probe sphere.
    mirrored_base = add_mesh(
        [(px * 0.8, py * 0.8, pz * 0.8) for px, py, pz in positions],
        normals, uvs, indices,
        tangents=[(1.0, 0.0, 0.0, 1.0)] * len(positions),
        colors=[(1.0, 1.0, 1.0, 1.0)] * len(positions))
    mirrored_primitive = {
        'attributes': {'POSITION': mirrored_base, 'NORMAL': mirrored_base + 1,
                       'TEXCOORD_0': mirrored_base + 2, 'TANGENT': mirrored_base + 4,
                       'COLOR_0': mirrored_base + 5},
        'indices': mirrored_base + 3, 'material': 6}
    document = {
        'asset': {'version': '2.0',
                  'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'extensionsUsed': ['KHR_texture_transform', 'KHR_materials_transmission',
                           'KHR_materials_ior'],
        'samplers': [
            {'wrapS': 10497, 'wrapT': 10497},
            {'wrapS': 33071, 'wrapT': 33648, 'magFilter': 9728},
            {'wrapS': 33648, 'wrapT': 33071},
        ],
        'images': [{'uri': color_uri}, {'uri': normal_uri},
                   {'uri': normal_uri}, {'uri': mr_uri}, {'uri': ao_uri}],
        'textures': [
            {'source': 0, 'sampler': 0}, {'source': 1, 'sampler': 1},
            {'source': 2, 'sampler': 0}, {'source': 3, 'sampler': 0},
            {'source': 4, 'sampler': 0},
        ],
        'scene': 0,
        'scenes': [{'nodes': [0, 1]}],
        'nodes': [{'mesh': 0},
                  {'mesh': 1, 'translation': [0.0, -3.1, 0.0],
                   'scale': [-1.0, 1.0, 1.0]}],
        'meshes': [{'primitives': primitives},
                   {'primitives': [mirrored_primitive]}],
        'materials': materials,
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
    'material-probe': (material_probe, 'material-probe.glb'),
    'material-semantics': (material_semantics, 'material-semantics.glb'),
    'stl-box': (stl_box, 'stl-box.stl'),
}

# Source-authoritative material values for the T21 probe. The viewer must report
# these through its validated wire snapshot; the test tooling reads this table
# (and, for external assets, the glTF JSON) and never asks the viewer to parse
# the document.
EXPECTED_MATERIALS = {
    'material-probe': [
        {'name': 'probe dielectric smooth', 'baseColor': [0.8, 0.2, 0.1, 1.0],
         'metallic': 0.0, 'roughness': 0.1, 'bucket': 0, 'alphaMode': 0},
        {'name': 'probe metal rough', 'baseColor': [0.2, 0.7, 0.3, 1.0],
         'metallic': 1.0, 'roughness': 0.8, 'bucket': 0, 'alphaMode': 0},
        {'name': 'probe clearcoat', 'baseColor': [0.1, 0.3, 0.9, 1.0],
         'metallic': 0.0, 'roughness': 0.4, 'bucket': 0, 'alphaMode': 0,
         'clearcoat': 0.75, 'clearcoatRoughness': 0.1},
        {'name': 'probe blend sheen emission', 'baseColor': [0.9, 0.8, 0.2, 0.5],
         'metallic': 0.5, 'roughness': 0.3, 'bucket': 2, 'alphaMode': 2,
         'emissive': [0.2, 0.1, 0.05], 'emissiveStrength': 2.0,
         'sheen': [0.4, 0.5, 0.6], 'sheenRoughness': 0.5,
         'specularFactor': 0.8, 'specularColor': [1.0, 0.9, 0.8], 'ior': 1.7},
        {'name': 'probe transmissive', 'baseColor': [0.7, 0.9, 0.95, 1.0],
         'metallic': 0.0, 'roughness': 0.05, 'bucket': 3, 'alphaMode': 2,
         'transmission': 0.9, 'ior': 1.33},
        {'name': 'probe masked cutout', 'baseColor': [0.9, 0.1, 0.7, 1.0],
         'metallic': 0.0, 'roughness': 0.6, 'bucket': 1, 'alphaMode': 1,
         'alphaCutoff': 0.2},
    ],
    # T22: authored inputs and the renderer policy each one must produce.
    'material-semantics': [
        {'name': 'normal scale 0', 'baseColor': [0.55, 0.55, 0.55, 1.0],
         'metallic': 0.0, 'roughness': 0.55, 'bucket': 0, 'alphaMode': 0,
         'normalScale': 0.0, 'occlusionBound': False, 'transmission': 0.0},
        {'name': 'normal scale 045', 'baseColor': [0.55, 0.55, 0.55, 1.0],
         'metallic': 0.0, 'roughness': 0.55, 'bucket': 0, 'alphaMode': 0,
         'normalScale': 0.45, 'occlusionBound': False, 'transmission': 0.0},
        {'name': 'normal scale 1', 'baseColor': [0.55, 0.55, 0.55, 1.0],
         'metallic': 0.0, 'roughness': 0.55, 'bucket': 0, 'alphaMode': 0,
         'normalScale': 1.0, 'occlusionBound': False, 'transmission': 0.0},
        {'name': 'MR red is not AO', 'baseColor': [0.6, 0.6, 0.6, 1.0],
         'metallic': 1.0, 'roughness': 1.0, 'bucket': 0, 'alphaMode': 0,
         'occlusionBound': False, 'occlusionExpectation': 1.0},
        {'name': 'independent occlusion', 'baseColor': [0.6, 0.6, 0.6, 1.0],
         'metallic': 0.0, 'roughness': 0.8, 'bucket': 0, 'alphaMode': 0,
         'occlusionBound': True, 'occlusionExpectation': 0.2},
        {'name': 'uv1 unsupported', 'baseColor': [0.8, 0.3, 0.2, 1.0],
         'metallic': 0.0, 'roughness': 0.5, 'bucket': 0, 'alphaMode': 0,
         'baseColorBound': False},
        {'name': 'per slot transform', 'baseColor': [0.7, 0.7, 0.7, 1.0],
         'metallic': 0.0, 'roughness': 0.5, 'bucket': 0, 'alphaMode': 0,
         'normalScale': 1.0, 'baseColorBound': True},
        {'name': 'alpha cutout', 'baseColor': [0.9, 0.1, 0.7, 0.35],
         'metallic': 0.0, 'roughness': 0.6, 'bucket': 1, 'alphaMode': 1,
         'alphaCutoff': 0.2},
        {'name': 'ior 1 transmission suppressed', 'baseColor': [0.72, 0.02, 0.02, 1.0],
         'metallic': 0.0, 'roughness': 0.4, 'bucket': 0, 'alphaMode': 0,
         'transmission': 0.0, 'ior': 1.0},
        {'name': 'ior 15 transmission', 'baseColor': [0.7, 0.9, 0.95, 1.0],
         'metallic': 0.0, 'roughness': 0.05, 'bucket': 3, 'alphaMode': 0,
         'transmission': 0.9, 'ior': 1.5},
    ],
}


def _sanitize(value):
    if isinstance(value, float):
        return value
    if isinstance(value, list):
        return [_sanitize(item) for item in value]
    if isinstance(value, dict):
        return {key: _sanitize(item) for key, item in value.items()}
    return value


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
                    'material-probe': [item['name'] for item in EXPECTED_MATERIALS['material-probe']],
                    'material-semantics': [item['name'] for item in EXPECTED_MATERIALS['material-semantics']],
                    'stl-box': ['hard-edged STEP/STL-like surface', 'ground contact'],
                }[name],
            },
        }
        if name in EXPECTED_MATERIALS:
            generated[name]['expectedMaterials'] = _sanitize(EXPECTED_MATERIALS[name])
    write_json(output / 'fixtures.json', {'schema': 1, 'fixtures': generated})
    return generated
