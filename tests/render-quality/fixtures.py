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
import zlib

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


def brdf_probe(path: Path) -> Path:
    """T24 material-BRDF fixture: isolated lobes and layering invariants.

    Two rows of seven opaque/transmissive probes sweep metallic, roughness,
    IOR, specular factor/color, clearcoat (with an uncoated twin), coated
    metal, emission and a thin transmissive surface. Every probe carries
    TANGENT and COLOR_0 so the Debug diagnostic channels and numeric HDR
    readback are available. Each base color is distinct enough for the
    locator to identify the sphere from the base-color channel alone.
    """
    materials = [
        {'name': 'brdf white dielectric',
         'pbrMetallicRoughness': {'baseColorFactor': [0.95, 0.95, 0.95, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.5}},
        {'name': 'brdf black dielectric',
         'pbrMetallicRoughness': {'baseColorFactor': [0.08, 0.05, 0.18, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.3}},
        {'name': 'brdf white metal',
         'pbrMetallicRoughness': {'baseColorFactor': [1.0, 1.0, 1.0, 1.0],
                                  'metallicFactor': 1.0, 'roughnessFactor': 0.2}},
        {'name': 'brdf rough metal',
         'pbrMetallicRoughness': {'baseColorFactor': [0.85, 0.65, 0.25, 1.0],
                                  'metallicFactor': 1.0, 'roughnessFactor': 0.9}},
        {'name': 'brdf ior 15',
         'extensions': {'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.7, 0.2, 0.2, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.1}},
        {'name': 'brdf ior 20',
         'extensions': {'KHR_materials_ior': {'ior': 2.0}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.2, 0.7, 0.2, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.1}},
        {'name': 'brdf specular zero',
         'extensions': {'KHR_materials_specular': {'specularFactor': 0.0}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.2, 0.3, 0.8, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.2}},
        {'name': 'brdf specular tinted',
         'extensions': {'KHR_materials_specular': {
             'specularFactor': 1.0, 'specularColorFactor': [1.0, 0.5, 0.25]}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.8, 0.3, 0.2, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.2}},
        {'name': 'brdf coat zero',
         'pbrMetallicRoughness': {'baseColorFactor': [0.10, 0.30, 0.90, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.4}},
        {'name': 'brdf coat one',
         'extensions': {'KHR_materials_clearcoat': {
             'clearcoatFactor': 1.0, 'clearcoatRoughnessFactor': 0.05}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.12, 0.32, 0.92, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.4}},
        {'name': 'brdf coat half',
         'extensions': {'KHR_materials_clearcoat': {
             'clearcoatFactor': 0.5, 'clearcoatRoughnessFactor': 0.2}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.10, 0.50, 0.90, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.4}},
        {'name': 'brdf coated metal',
         'extensions': {'KHR_materials_clearcoat': {
             'clearcoatFactor': 1.0, 'clearcoatRoughnessFactor': 0.1}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.9, 0.75, 0.3, 1.0],
                                  'metallicFactor': 1.0, 'roughnessFactor': 0.3}},
        {'name': 'brdf emission',
         'emissiveFactor': [0.9, 0.6, 0.2],
         'extensions': {'KHR_materials_emissive_strength': {'emissiveStrength': 8.0}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.6, 0.1, 0.6, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.6}},
        {'name': 'brdf coat emission',
         'emissiveFactor': [0.9, 0.6, 0.2],
         'extensions': {
             'KHR_materials_emissive_strength': {'emissiveStrength': 8.0},
             'KHR_materials_clearcoat': {
                 'clearcoatFactor': 1.0, 'clearcoatRoughnessFactor': 0.1}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.6, 0.2, 0.6, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.6}},
        {'name': 'brdf transmissive',
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.6, 0.8, 0.9, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.05}},
        {'name': 'brdf coverage blend', 'alphaMode': 'BLEND',
         'pbrMetallicRoughness': {'baseColorFactor': [0.55, 0.55, 0.55, 0.5],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.5}},
    ]
    positions, normals, uvs, indices = uv_sphere(segments=32, rings=20)
    add_mesh, finish = _mesh_builder()
    primitives = []
    # 4 x 4 grid: 16 probes, spaced so the framed camera sees every sphere
    # away from the ground highlight (which the numeric lane disables).
    locations = []
    for row, y in enumerate([2.4, 0.8, -0.8, -2.4]):
        for column in range(4):
            locations.append((-3.0 + column * 2.0, y))
    for index, (x, y) in enumerate(locations):
        scale = 0.55
        base = add_mesh(
            [(px * scale + x, py * scale + y, pz * scale + scale)
             for px, py, pz in positions],
            normals, uvs, indices,
            tangents=[(1.0, 0.0, 0.0, 1.0)] * len(positions),
            colors=[(1.0, 1.0, 1.0, 1.0)] * len(positions))
        primitives.append({'attributes': {'POSITION': base, 'NORMAL': base + 1,
                                         'TEXCOORD_0': base + 2, 'TANGENT': base + 4,
                                         'COLOR_0': base + 5},
                           'indices': base + 3, 'material': index})
    document = {
        'asset': {'version': '2.0',
                  'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'extensionsUsed': ['KHR_materials_transmission', 'KHR_materials_ior',
                           'KHR_materials_clearcoat', 'KHR_materials_specular',
                           'KHR_materials_emissive_strength'],
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
    1.5 transmission. A separate mirrored sphere and a double-sided back-facing
    quad exercise tangent handedness and front-face orientation. Every image is
    embedded as a data URI so the fixture is a single redistributable file.
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
    # R=0.2 is deliberately not AO; G=0.502 roughness and B=1 metallic keep the
    # metal/rough channels independently observable.
    mr_uri = _png_data_uri(_solid(4, 4, (51, 128, 255)))
    ao_uri = _png_data_uri(_solid(4, 4, (51, 51, 51)))   # independent AO 0.2

    materials = [
        {'name': 'normal scale 0',
         'normalTexture': {'index': 2, 'scale': 0.0},
         'pbrMetallicRoughness': {'baseColorFactor': [0.55, 0.55, 0.55, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.55}},
        {'name': 'normal scale 045',
         'normalTexture': {'index': 2, 'scale': 0.45},
         'pbrMetallicRoughness': {'baseColorFactor': [0.55, 0.52, 0.58, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.55}},
        {'name': 'normal scale 1',
         'normalTexture': {'index': 2, 'scale': 1.0},
         'pbrMetallicRoughness': {'baseColorFactor': [0.58, 0.55, 0.52, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.55}},
        {'name': 'MR red is not AO',
         'pbrMetallicRoughness': {'baseColorFactor': [0.6, 0.6, 0.6, 1],
                                  'metallicFactor': 1.0, 'roughnessFactor': 1.0,
                                  'metallicRoughnessTexture': {'index': 3}}},
        {'name': 'independent occlusion',
         'occlusionTexture': {'index': 4, 'strength': 1.0},
         'pbrMetallicRoughness': {'baseColorFactor': [0.62, 0.56, 0.52, 1],
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
        {'name': 'mirrored tangent handedness',
         'normalTexture': {'index': 2, 'scale': 1.0},
         'pbrMetallicRoughness': {'baseColorFactor': [0.35, 0.75, 0.45, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.5}},
        {'name': 'double sided backface', 'doubleSided': True,
         'normalTexture': {'index': 2, 'scale': 0.0},
         'pbrMetallicRoughness': {'baseColorFactor': [0.45, 0.65, 0.85, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.6}},
    ]
    positions, normals, uvs, indices = uv_sphere(segments=32, rings=20)
    add_mesh, finish = _mesh_builder()
    primitives = []
    # Each probe gets a distinct base color (the three normal-scale probes differ
    # only by a few hundredths) so the diagnostic locator can identify every
    # sphere from the isolated base-color channel rather than by draw order.
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
        'indices': mirrored_base + 3, 'material': 10}
    # A double-sided quad whose authoring normal points away from the camera
    # (back face visible). Normal scale 0 isolates the geometric-normal flip
    # from any tangent-space perturbation.
    quad = [(-0.45, -0.45, 0.0), (0.45, -0.45, 0.0), (-0.45, 0.45, 0.0), (0.45, 0.45, 0.0)]
    quad_positions = [(4.2 + x, -1.15 + y, 1.0 + z) for x, y, z in quad]
    quad_base = add_mesh(
        quad_positions,
        [(0.0, 0.0, -1.0)] * 4,
        [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)],
        [0, 2, 1, 1, 2, 3],
        tangents=[(1.0, 0.0, 0.0, 1.0)] * 4,
        colors=[(1.0, 1.0, 1.0, 1.0)] * 4)
    backface_primitive = {
        'attributes': {'POSITION': quad_base, 'NORMAL': quad_base + 1,
                       'TEXCOORD_0': quad_base + 2, 'TANGENT': quad_base + 4,
                       'COLOR_0': quad_base + 5},
        'indices': quad_base + 3, 'material': 11}
    document = {
        'asset': {'version': '2.0',
                  'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'extensionsUsed': ['KHR_texture_transform', 'KHR_materials_transmission',
                           'KHR_materials_ior'],
        'samplers': [
            {'wrapS': 10497, 'wrapT': 10497},
            {'wrapS': 33071, 'wrapT': 33648, 'magFilter': 9728},
            {'wrapS': 33648, 'wrapT': 33071, 'magFilter': 9728},
        ],
        'images': [{'uri': color_uri}, {'uri': normal_uri},
                   {'uri': normal_uri}, {'uri': mr_uri}, {'uri': ao_uri}],
        # Distinct samplers per slot: base color wraps, the normal map mirrors U
        # and clamps V with nearest filtering, and the occlusion map clamps U
        # and mirrors V with nearest filtering.
        'textures': [
            {'source': 0, 'sampler': 0}, {'source': 1, 'sampler': 1},
            {'source': 2, 'sampler': 2}, {'source': 3, 'sampler': 0},
            {'source': 4, 'sampler': 1},
        ],
        'scene': 0,
        'scenes': [{'nodes': [0, 1, 2]}],
        'nodes': [{'mesh': 0},
                  {'mesh': 1, 'translation': [0.0, -3.1, 0.0],
                   'scale': [-1.0, 1.0, 1.0]},
                  {'mesh': 2}],
        'meshes': [{'primitives': primitives},
                   {'primitives': [mirrored_primitive]},
                   {'primitives': [backface_primitive]}],
        'materials': materials,
    }
    document, binary = finish(document)
    return _glb(document, binary, path)


def texture_reuse(path: Path) -> Path:
    """T23: authored texture reuse and distinct color/data semantics.

    Two glTF image entries name the *same* 128x128 color map, and two materials
    use both entries. A third image is a distinct 64x64 data map used as
    metallicRoughness by both materials. The adapter must emit one sRGB color
    chunk and one linear data chunk for the whole model, not one per slot or
    per image entry. The lane asserts the viewer reports exactly two resident
    model textures.
    """
    color_width = 128
    color_pixels = bytearray()
    for y in range(color_width):
        for x in range(color_width):
            color_pixels += bytes((32 + (x * 160 // (color_width - 1)),
                                   200 - (y * 120 // (color_width - 1)), 96))
    color_uri = _png_data_uri(bytes(color_pixels), color_width, color_width)
    data_uri = _png_data_uri(_solid(64, 64, (51, 128, 255)), 64, 64)
    materials = [
        {'name': 'reused color and data',
         'pbrMetallicRoughness': {'baseColorTexture': {'index': 0},
                                  'metallicRoughnessTexture': {'index': 2}}},
        {'name': 'second use of the same color',
         'pbrMetallicRoughness': {'baseColorTexture': {'index': 1},
                                  'metallicRoughnessTexture': {'index': 2}}},
    ]
    positions, normals, uvs, indices = uv_sphere(segments=24, rings=16)
    add_mesh, finish = _mesh_builder()
    base = add_mesh(positions, normals, uvs, indices)
    document = {
        'asset': {'version': '2.0',
                  'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'scene': 0, 'scenes': [{'nodes': [0]}],
        'nodes': [{'mesh': 0}],
        'meshes': [{'primitives': [
            {'attributes': {'POSITION': base, 'NORMAL': base + 1, 'TEXCOORD_0': base + 2},
             'indices': base + 3, 'material': 0},
            {'attributes': {'POSITION': base, 'NORMAL': base + 1, 'TEXCOORD_0': base + 2},
             'indices': base + 3, 'material': 1},
        ]}],
        # Entries 0 and 1 are byte-identical color maps; entry 2 is distinct.
        'images': [{'uri': color_uri}, {'uri': color_uri}, {'uri': data_uri}],
        'textures': [{'source': 0}, {'source': 1}, {'source': 2}],
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


def textured_obj(path: Path) -> Path:
    """T34 generated textured OBJ/MTL fixture (portable, redistributable).

    The OBJ adapter must resolve the sidecar MTL and the referenced PNG through
    the brokered sidecar path. A cube (not a single quad) keeps back-face,
    wrapping and lighting review possible in all three modes.
    """
    width = height = 8
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            bright = (x + y) % 2 == 0
            pixels += bytes((225, 205, 170) if bright else (35, 95, 165))
    path.with_suffix('.png').write_bytes(encoded_png(width, height, bytes(pixels)))
    path.with_suffix('.mtl').write_text(
        'newmtl textured-cube\n'
        'Ka 0.1 0.1 0.1\n'
        'Kd 1.0 1.0 1.0\n'
        'Ks 0.2 0.2 0.2\n'
        'Ns 24\n'
        'd 1.0\n'
        'illum 2\n'
        f'map_Kd {path.with_suffix(".png").name}\n', encoding='utf-8')
    corners = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
               (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]
    faces = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
             (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    lines = [f'mtllib {path.with_suffix(".mtl").name}', 'o textured_cube']
    for x, y, z in corners:
        lines.append(f'v {x} {y} {z}')
    for u, v in ((0, 0), (1, 0), (1, 1), (0, 1)):
        lines.append(f'vt {u} {v}')
    lines += ['usemtl textured-cube']
    for a, b, c, d in faces:
        lines.append(f'f {a+1}/1 {b+1}/2 {c+1}/3 {d+1}/4')
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return path


def line_point_scene(path: Path) -> Path:
    """Mixed line/point OBJ for the Clay mode round-trip.

    The glTF LINES path is rejected by the bounded section validator, so the
    mixed-topology control uses OBJ, whose adapter already emits LineList and
    PointList chunks. A line/point-only document is required because ufbx only
    exposes OBJ lines/points through the adapter's bounded fallback scan.
    """
    lines = [
        '# preview-3d T29 generated mixed topology fixture',
        'v -1.6 0.0 0.0',
        'v 0.0 1.2 0.0',
        'v 1.6 0.0 0.0',
        'v -1.2 -0.9 0.0',
        'v -0.4 -0.9 0.0',
        'v 0.4 -0.9 0.0',
        'v 1.2 -0.9 0.0',
        'v -0.9 0.5 0.35',
        'v 0.0 0.5 0.35',
        'v 0.9 0.5 0.35',
        'l 1 2 3',
        'l 4 5 6 7',
        'p 8 9 10',
        '',
    ]
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def step_assembly(path: Path) -> Path:
    """Copy the checked-in AP214 STEP assembly used by the STEP-host lane.

    The external Voron assembly exceeds the bounded STEP host limit inside the
    task timeout, so the Clay CAD review uses this small in-repository assembly.
    """
    source = Path(__file__).resolve().parent.parent / 'fixtures' / 'stp-spike' / 'assembly_ap214.stp'
    path.write_bytes(source.read_bytes())
    return path


def _camera_facing_quad(x: float, y: float, width: float = 1.35,
                        height: float = 1.0):
    """A glTF front-facing (-Z normal) quad with UV0 left-to-right.

    glTF is Y-up; the viewer's front view looks along the model Z axis, so a
    constant-Z XY quad faces the camera. Winding is chosen for a -Z front face.
    """
    left, right = x - width * 0.5, x + width * 0.5
    bottom, top = y - height * 0.5, y + height * 0.5
    positions = [(left, bottom, 0.0), (right, bottom, 0.0),
                 (left, top, 0.0), (right, top, 0.0)]
    normals = [(0.0, 0.0, -1.0)] * 4
    uvs = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)]
    indices = [0, 2, 1, 1, 2, 3]
    tangents = [(1.0, 0.0, 0.0, 1.0)] * 4
    return positions, normals, uvs, indices, tangents


def _gradient_png(width, height, channel_values):
    """RGB PNG; channel_values returns (r, g, b)."""
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            pixels += bytes(channel_values(x, y))
    return _png_data_uri(bytes(pixels), width, height)


def _rgba_png_data_uri(width, height, channel_values):
    """RGBA PNG for maps whose authored channel is alpha (specular/sheen)."""
    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data
                + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows += bytes(channel_values(x, y))
    data = (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(bytes(rows), 6))
            + chunk(b'IEND', b''))
    return 'data:image/png;base64,' + base64.b64encode(data).decode('ascii')


def textured_lobes(path: Path) -> Path:
    """T25 textured extension-lobe fixture.

    Nine camera-facing quads isolate one authored map each: clearcoat factor,
    clearcoat roughness, clearcoat normal, specular factor/color (one shared
    image decoded into an sRGB color chunk and a linear data chunk), sheen
    color/roughness, transmission factor, an unsupported UV1 clearcoat map and
    a KHR_texture_transform clearcoat map. One plain quad is the no-extension
    baseline. The maps have deliberately mismatched dimensions and every
    channel varies left-to-right so a single probe can prove per-texel support.
    """
    coat_uri = _gradient_png(64, 64, lambda x, y: (30 + x * 225 // 63, 0, 0))
    rough_uri = _gradient_png(32, 16, lambda x, y: (0, 20 + x * 235 // 31, 0))
    normal_uri = _gradient_png(16, 8, lambda x, y: (
        96 + x * 96 // 15, 128, 255))
    # The shared image carries an RGB color gradient and an independent alpha
    # factor gradient: the same source is decoded once as sRGB color and once
    # as a linear data texture.
    shared_uri = _rgba_png_data_uri(32, 32, lambda x, y: (
        40 + x * 180 // 31, 70 + y * 150 // 31, 210,
        30 + x * 220 // 31))
    sheen_uri = _gradient_png(8, 8, lambda x, y: (20 + x * 200 // 7, 120, 60))
    # RGBA alpha carries the gradient; _gradient_png appends alpha=255, so build
    # the transmission map explicitly.
    transmission_pixels = bytearray()
    for y in range(16):
        for x in range(16):
            transmission_pixels += bytes((x * 255 // 15, 0, 0, 255))
    transmission_uri = _png_data_uri(bytes(transmission_pixels), 16, 16)
    # Sheen roughness uses the alpha channel of a 64x8 map.
    sheen_rough_uri = _rgba_png_data_uri(
        64, 8, lambda x, y: (0, 0, 0, 10 + x * 230 // 63))

    materials = [
        {'name': 'no extension baseline',
         'pbrMetallicRoughness': {'baseColorFactor': [0.72, 0.72, 0.72, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.7}},
        {'name': 'clearcoat factor map',
         'pbrMetallicRoughness': {'baseColorFactor': [0.85, 0.25, 0.25, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.6},
         'extensions': {'KHR_materials_clearcoat': {
             'clearcoatFactor': 0.9, 'clearcoatTexture': {'index': 0}}}},
        {'name': 'clearcoat roughness map',
         'pbrMetallicRoughness': {'baseColorFactor': [0.25, 0.85, 0.25, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.6},
         'extensions': {'KHR_materials_clearcoat': {
             'clearcoatFactor': 0.5, 'clearcoatRoughnessFactor': 0.8,
             'clearcoatRoughnessTexture': {'index': 1}}}},
        {'name': 'clearcoat normal map',
         'pbrMetallicRoughness': {'baseColorFactor': [0.25, 0.35, 0.9, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.5},
         'extensions': {'KHR_materials_clearcoat': {
             'clearcoatFactor': 1.0,
             'clearcoatNormalTexture': {'index': 2, 'scale': 1.0}}}},
        {'name': 'specular maps',
         'pbrMetallicRoughness': {'baseColorFactor': [0.9, 0.8, 0.2, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.4},
         'extensions': {'KHR_materials_specular': {
             'specularFactor': 0.9, 'specularColorFactor': [0.9, 0.8, 0.7],
             'specularTexture': {'index': 3},
             'specularColorTexture': {'index': 4}}}},
        {'name': 'sheen maps',
         'pbrMetallicRoughness': {'baseColorFactor': [0.8, 0.3, 0.85, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.5},
         'extensions': {'KHR_materials_sheen': {
             'sheenColorFactor': [0.7, 0.6, 0.5], 'sheenRoughnessFactor': 0.7,
             'sheenColorTexture': {'index': 5},
             'sheenRoughnessTexture': {'index': 6}}}},
        {'name': 'transmission map',
         'pbrMetallicRoughness': {'baseColorFactor': [0.2, 0.75, 0.85, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.2},
         'extensions': {
             'KHR_materials_transmission': {
                 'transmissionFactor': 0.8, 'transmissionTexture': {'index': 7}},
             'KHR_materials_ior': {'ior': 1.5}}},
        {'name': 'uv1 clearcoat map',
         'pbrMetallicRoughness': {'baseColorFactor': [0.95, 0.55, 0.3, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.6},
         'extensions': {'KHR_materials_clearcoat': {
             'clearcoatFactor': 0.6,
             'clearcoatTexture': {'index': 0, 'texCoord': 1}}}},
        {'name': 'transformed clearcoat map',
         'pbrMetallicRoughness': {'baseColorFactor': [0.55, 0.95, 0.45, 1],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.6},
         'extensions': {'KHR_materials_clearcoat': {
             'clearcoatFactor': 0.8, 'clearcoatTexture': {'index': 0,
                 'extensions': {'KHR_texture_transform': {
                     'offset': [0.5, 0.0], 'scale': [2.0, 1.0],
                     'rotation': 0.0}}}}}},
    ]
    # The probes are reviewed from either side of the flat plate; double-sided
    # rendering keeps the authored normal map orientation deterministic under
    # the viewer's orbit conventions.
    for material in materials:
        material['doubleSided'] = True
    add_mesh, finish = _mesh_builder()
    locations = [(-3.2, 3.9), (0.0, 3.9), (3.2, 3.9),
                 (-3.2, 2.6), (0.0, 2.6), (3.2, 2.6),
                 (-3.2, 1.3), (0.0, 1.3), (3.2, 1.3)]
    primitives = []
    for material_index, (x, y) in enumerate(locations):
        positions, normals, uvs, indices, tangents = _camera_facing_quad(x, y)
        base = add_mesh(positions, normals, uvs, indices, tangents=tangents,
                        colors=[(1.0, 1.0, 1.0, 1.0)] * 4)
        primitives.append({
            'attributes': {'POSITION': base, 'NORMAL': base + 1,
                           'TEXCOORD_0': base + 2, 'TANGENT': base + 4,
                           'COLOR_0': base + 5},
            'indices': base + 3, 'material': material_index})
    document = {
        'asset': {'version': '2.0',
                  'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'extensionsUsed': ['KHR_texture_transform', 'KHR_materials_clearcoat',
                           'KHR_materials_sheen', 'KHR_materials_specular',
                           'KHR_materials_transmission', 'KHR_materials_ior'],
        'samplers': [
            {'wrapS': 10497, 'wrapT': 10497},
            # Nearest/clamped sampler proves the per-slot sampler metadata is
            # carried independently of the shared linear default.
            {'wrapS': 33071, 'wrapT': 33648, 'magFilter': 9728},
        ],
        'images': [{'uri': coat_uri}, {'uri': rough_uri}, {'uri': normal_uri},
                   {'uri': shared_uri}, {'uri': sheen_uri},
                   {'uri': sheen_rough_uri}, {'uri': transmission_uri}],
        # Texture 3 and 4 share image 3 but are used as linear (A) and sRGB
        # (RGB) semantics respectively.
        'textures': [
            {'source': 0, 'sampler': 0}, {'source': 1, 'sampler': 1},
            {'source': 2, 'sampler': 0}, {'source': 3, 'sampler': 0},
            {'source': 3, 'sampler': 1}, {'source': 4, 'sampler': 0},
            {'source': 5, 'sampler': 1}, {'source': 6, 'sampler': 0},
        ],
        'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
        'materials': materials,
        'meshes': [{'primitives': primitives}],
    }
    document, binary = finish(document)
    return _glb(document, binary, path)


def _quad_at(x, y, z, width=1.2, height=1.2, color=None):
    left, right = x - width * 0.5, x + width * 0.5
    bottom, top = y - height * 0.5, y + height * 0.5
    positions = [(left, bottom, z), (right, bottom, z), (left, top, z), (right, top, z)]
    normals = [(0.0, 0.0, -1.0)] * 4
    uvs = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)]
    indices = [0, 2, 1, 1, 2, 3]
    # T26 needs the complete-vertex textured pipeline (OIT, alpha cutout,
    # emissive and diagnostic channels), not the compact legacy shader.
    tangents = [(1.0, 0.0, 0.0, 1.0)] * 4
    colors = [color if color is not None else (1.0, 1.0, 1.0, 1.0)] * 4
    return positions, normals, uvs, indices, tangents, colors


def layered_glass(path: Path, permuted: bool = False) -> Path:
    """T26 layered transparency fixture.

    Six isolated roles inside one scene: one primitive with two crossing
    triangles whose index order is the regression control, a two-primitive
    shared-material overlap, four nested front/back glass sheets, an alpha decal
    over opaque paint, an alpha-cutout panel over paint, and emissive blended
    geometry. The permuted twin keeps every world position identical and only
    changes primitive/index/node order, so an order-independent path must render
    the same image while a sorted path changes.
    """
    cutout_uri = _rgba_png_data_uri(16, 16, lambda x, y: (
        40, 220, 235, 0 if 4 <= x <= 11 and 4 <= y <= 11 else 255))
    materials = [
        {'name': 'regression shared', 'alphaMode': 'BLEND', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [1.0, 1.0, 1.0, 0.55],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.35}},
        {'name': 'intersection shared', 'alphaMode': 'BLEND', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [1.0, 1.0, 1.0, 0.5],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.3}},
        {'name': 'nested amber', 'alphaMode': 'BLEND', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.9, 0.25, 0.12, 0.4],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.08}},
        {'name': 'nested green', 'alphaMode': 'BLEND', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.12, 0.72, 0.25, 0.4],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.08}},
        {'name': 'nested blue', 'alphaMode': 'BLEND', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.12, 0.3, 0.92, 0.4],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.08}},
        {'name': 'nested yellow', 'alphaMode': 'BLEND', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.92, 0.8, 0.12, 0.4],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.08}},
        {'name': 'paint opaque', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.22, 0.23, 0.25, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.55}},
        {'name': 'alpha decal', 'alphaMode': 'BLEND', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.85, 0.1, 0.6, 0.3],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.7}},
        {'name': 'alpha cutout', 'alphaMode': 'MASK', 'alphaCutoff': 0.5,
         'doubleSided': True,
         'pbrMetallicRoughness': {
             'baseColorFactor': [1.0, 1.0, 1.0, 1.0],
             'baseColorTexture': {'index': 0},
             'metallicFactor': 0.0, 'roughnessFactor': 0.6}},
        {'name': 'emissive blend', 'alphaMode': 'BLEND', 'doubleSided': True,
         'emissiveFactor': [0.9, 0.18, 0.05],
         'extensions': {'KHR_materials_emissive_strength': {'emissiveStrength': 4.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.12, 0.1, 0.08, 0.35],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.8}},
        {'name': 'hidden glass', 'alphaMode': 'BLEND', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.2, 0.4, 0.95, 0.5],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.1}},
    ]
    add_mesh, finish = _mesh_builder()
    meshes = []
    nodes = []

    def add_quad(x, y, z, width, height, color=None):
        positions, normals, uvs, indices, tangents, colors = _quad_at(
            x, y, z, width=width, height=height, color=color)
        return add_mesh(positions, normals, uvs, indices,
                        tangents=tangents, colors=colors)

    def attributes(base):
        return {'POSITION': base, 'NORMAL': base + 1, 'TEXCOORD_0': base + 2,
                'TANGENT': base + 4, 'COLOR_0': base + 5}

    # 1. Shared-material regression: the same world quad twice with different
    # vertex colours. Only primitive order changes in the permuted twin.
    regression = []
    for color in ((0.9, 0.1, 0.1, 1.0), (0.1, 0.9, 0.1, 1.0)):
        base = add_quad(-2.0, 1.15, 0.0, width=1.5, height=1.4, color=color)
        regression.append({'attributes': attributes(base),
                           'indices': base + 3, 'material': 0})
    if permuted:
        regression.reverse()
    meshes.append({'name': 'regression overlap', 'primitives': regression})
    regression_mesh = len(meshes) - 1

    # 2. Two crossing triangles in one primitive. The permuted twin reverses
    # the triangle order inside the same draw.
    left, right = 1.0, 3.0
    bottom, top = 0.55, 1.75
    positions = [
        (left, bottom, 0.0), (right, bottom, 0.0), ((left + right) * 0.5, top, 0.0),
        (left, top, 0.02), (right, top, 0.02), ((left + right) * 0.5, bottom, 0.02),
    ]
    normals = [(0.0, 0.0, -1.0)] * 6
    uvs = [(0.0, 0.0), (1.0, 0.0), (0.5, 1.0)] * 2
    colors = [(0.95, 0.5, 0.1, 1.0)] * 3 + [(0.1, 0.4, 0.95, 1.0)] * 3
    tangents = [(1.0, 0.0, 0.0, 1.0)] * 6
    indices = [0, 1, 2, 3, 4, 5]
    if permuted:
        indices = [3, 4, 5, 0, 1, 2]
    base = add_mesh(positions, normals, uvs, indices, tangents=tangents, colors=colors)
    meshes.append({'name': 'crossing triangles', 'primitives': [
        {'attributes': attributes(base),
         'indices': base + 3, 'material': 1}]})
    crossing_mesh = len(meshes) - 1

    # 3. Four nested front/back glass sheets, each its own mesh/node.
    nested_layers = [(0.2, 2), (0.35, 3), (0.5, 4), (0.65, 5)]
    nested_meshes = []
    for z, material in nested_layers:
        base = add_quad(0.0, 1.15, z, width=1.25, height=1.25)
        meshes.append({'name': f'nested glass {z}', 'primitives': [
            {'attributes': attributes(base),
             'indices': base + 3, 'material': material}]})
        nested_meshes.append(len(meshes) - 1)

    # 4. Alpha decal over opaque paint, with hidden glass behind the paint.
    paint_base = add_quad(-2.0, -1.15, 0.0, width=1.55, height=1.45)
    meshes.append({'name': 'decal paint', 'primitives': [
        {'attributes': attributes(paint_base),
         'indices': paint_base + 3, 'material': 6}]})
    paint_mesh = len(meshes) - 1
    decal_base = add_quad(-2.0, -1.15, 0.12, width=0.95, height=0.9)
    meshes.append({'name': 'alpha decal', 'primitives': [
        {'attributes': attributes(decal_base),
         'indices': decal_base + 3, 'material': 7}]})
    decal_mesh = len(meshes) - 1
    hidden_base = add_quad(-2.0, -1.15, -0.12, width=1.35, height=1.25)
    meshes.append({'name': 'hidden glass behind paint', 'primitives': [
        {'attributes': attributes(hidden_base),
         'indices': hidden_base + 3, 'material': 10}]})
    hidden_mesh = len(meshes) - 1

    # 5. Alpha cutout over opaque paint.
    cut_paint = add_quad(0.0, -1.15, 0.0, width=1.55, height=1.45)
    meshes.append({'name': 'cutout paint', 'primitives': [
        {'attributes': attributes(cut_paint),
         'indices': cut_paint + 3, 'material': 6}]})
    cut_paint_mesh = len(meshes) - 1
    cut_base = add_quad(0.0, -1.15, 0.12, width=1.2, height=1.15)
    meshes.append({'name': 'alpha cutout', 'primitives': [
        {'attributes': attributes(cut_base),
         'indices': cut_base + 3, 'material': 8}]})
    cut_mesh = len(meshes) - 1

    # 6. Emissive blended geometry over paint.
    emit_paint = add_quad(2.0, -1.15, 0.0, width=1.55, height=1.45)
    meshes.append({'name': 'emissive paint', 'primitives': [
        {'attributes': attributes(emit_paint),
         'indices': emit_paint + 3, 'material': 6}]})
    emit_paint_mesh = len(meshes) - 1
    emit_base = add_quad(2.0, -1.15, 0.12, width=1.1, height=1.0)
    meshes.append({'name': 'emissive blend', 'primitives': [
        {'attributes': attributes(emit_base),
         'indices': emit_base + 3, 'material': 9}]})
    emit_mesh = len(meshes) - 1

    nodes.extend([
        {'mesh': regression_mesh, 'name': 'regression overlap'},
        {'mesh': crossing_mesh, 'name': 'crossing triangles'},
        {'mesh': paint_mesh, 'name': 'decal paint'},
        {'mesh': decal_mesh, 'name': 'alpha decal'},
        {'mesh': hidden_mesh, 'name': 'hidden glass'},
        {'mesh': cut_paint_mesh, 'name': 'cutout paint'},
        {'mesh': cut_mesh, 'name': 'alpha cutout'},
        {'mesh': emit_paint_mesh, 'name': 'emissive paint'},
        {'mesh': emit_mesh, 'name': 'emissive blend'},
    ])
    for index in nested_meshes:
        nodes.append({'mesh': index, 'name': f'nested glass {index}'})
    if permuted:
        nodes.reverse()

    document = {
        'asset': {'version': '2.0',
                  'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'scene': 0, 'scenes': [{'nodes': list(range(len(nodes)))}],
        'nodes': nodes, 'meshes': meshes, 'materials': materials,
        'extensionsUsed': ['KHR_materials_emissive_strength'],
        'samplers': [{'wrapS': 10497, 'wrapT': 10497}],
        'images': [{'uri': cutout_uri}],
        'textures': [{'source': 0, 'sampler': 0}],
    }
    document, binary = finish(document)
    return _glb(document, binary, path)


def transmission_glass(path: Path) -> Path:
    """T27 transmitted-scene fixture.

    A high-frequency opaque checker and an open-front interior box sit behind a
    row of transmissive panels: neutral, orange-tinted, dark, rough, two BLEND
    coverage x transmission sweeps, an IOR=1 no-interface surface, a MASK
    cutout, a two-layer overlap, a glass sheet over real interior geometry and
    a double-sided thin plane. Every panel carries TANGENT/COLOR_0 so the
    complete-vertex material shader (and its diagnostic channels) is selected.
    """
    cutout_uri = _rgba_png_data_uri(16, 16, lambda x, y: (
        40, 220, 235, 0 if 4 <= x <= 11 and 4 <= y <= 11 else 255))
    materials = [
        {'name': 'checker light', 'extensions': {'KHR_materials_unlit': {}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.6, 0.45, 0.25, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.9}},
        {'name': 'checker dark', 'extensions': {'KHR_materials_unlit': {}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.012, 0.012, 0.015, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.9}},
        {'name': 'interior red', 'extensions': {'KHR_materials_unlit': {}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.85, 0.06, 0.04, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.9}},
        {'name': 'interior blue', 'extensions': {'KHR_materials_unlit': {}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.04, 0.15, 0.85, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.9}},
        {'name': 'glass neutral',
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.9, 0.95, 1.0, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.03}},
        {'name': 'glass tinted orange',
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.95, 0.35, 0.08, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.03}},
        {'name': 'glass dark',
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.02, 0.04, 0.12, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.03}},
        {'name': 'glass rough',
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.45, 0.62, 0.85, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.75}},
        {'name': 'blend quarter transmission', 'alphaMode': 'BLEND',
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 0.25},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [1.0, 0.62, 0.34, 0.4],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.03}},
        {'name': 'blend full transmission', 'alphaMode': 'BLEND',
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.7, 0.35, 1.0, 0.4],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.03}},
        {'name': 'ior one no interface',
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.0}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.2, 0.8, 0.4, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.03}},
        {'name': 'mask cutout transmission', 'alphaMode': 'MASK', 'alphaCutoff': 0.5,
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.9, 0.9, 0.2, 1.0],
                                  'baseColorTexture': {'index': 0},
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.2}},
        {'name': 'overlap front',
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.2, 0.8, 1.0, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.03}},
        {'name': 'overlap back',
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [1.0, 0.5, 0.7, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.03}},
        {'name': 'interior glass',
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.85, 0.95, 1.0, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.05}},
        {'name': 'double sided thin glass', 'doubleSided': True,
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.55, 0.85, 0.55, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.05}},
    ]
    add_mesh, finish = _mesh_builder()
    meshes = []
    nodes = []

    def attributes(base):
        return {'POSITION': base, 'NORMAL': base + 1, 'TEXCOORD_0': base + 2,
                'TANGENT': base + 4, 'COLOR_0': base + 5}

    def add_quad(x, y, z, width=1.35, height=1.2, material=4):
        positions, normals, uvs, indices, tangents, colors = _quad_at(
            x, y, z, width=width, height=height)
        # _quad_at is authored CW because the T26 fixture relies on
        # doubleSided materials. Camera-facing single-sided probes need the
        # opposite winding so the textured pass does not cull them, and the
        # vertex normal must match that winding (+Z) so the interface Fresnel
        # sees a front-facing surface (NoV = 1, not 0).
        indices = [0, 1, 2, 2, 1, 3]
        normals = [(0.0, 0.0, 1.0)] * len(positions)
        base = add_mesh(positions, normals, uvs, indices,
                        tangents=tangents, colors=colors)
        meshes.append({'name': f'{materials[material]["name"]} {len(meshes)}',
                       'primitives': [{'attributes': attributes(base),
                                       'indices': base + 3, 'material': material}]})
        nodes.append({'mesh': len(meshes) - 1})

    # Opaque checker wall: two primitives (light/dark) so the scene behind the
    # glass has real high-frequency geometry and depth at z=-0.9.
    light_positions, light_indices, light_uvs = [], [], []
    dark_positions, dark_indices, dark_uvs = [], [], []
    columns, rows = 18, 10
    for row in range(rows):
        for column in range(columns):
            x = -5.4 + column * 0.6
            y = -2.4 + row * 0.6
            target = light_positions if (row + column) % 2 == 0 else dark_positions
            target_indices = light_indices if (row + column) % 2 == 0 else dark_indices
            target_uvs = light_uvs if (row + column) % 2 == 0 else dark_uvs
            start = len(target) // 3
            for dx, dy in ((-0.3, -0.3), (0.3, -0.3), (-0.3, 0.3), (0.3, 0.3)):
                target.extend((x + dx, y + dy, -0.9))
            target_uvs.extend(((0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)))
            target_indices.extend((start, start + 1, start + 2,
                                   start + 2, start + 1, start + 3))
    for name, material, positions, uvs, indices in (
            ('checker light wall', 0, light_positions, light_uvs, light_indices),
            ('checker dark wall', 1, dark_positions, dark_uvs, dark_indices)):
        normals = [(0.0, 0.0, 1.0)] * (len(positions) // 3)
        base = add_mesh(list(zip(positions[0::3], positions[1::3], positions[2::3])),
                        normals, uvs, indices,
                        tangents=[(1.0, 0.0, 0.0, 1.0)] * normals.__len__(),
                        colors=[(1.0, 1.0, 1.0, 1.0)] * normals.__len__())
        meshes.append({'name': name, 'primitives': [
            {'attributes': attributes(base), 'indices': base + 3, 'material': material}]})
        nodes.append({'mesh': len(meshes) - 1})

    # Open-front interior box: two opaque wall halves at one depth and a small
    # object at a nearer depth occlude the checker, so transmitted radiance is
    # sampled over actual interior geometry rather than only the checker.
    box_x, box_y, box_z = 1.35, -1.2, -0.78
    add_quad(box_x - 0.38, box_y, box_z, width=0.72, height=1.5, material=2)
    add_quad(box_x + 0.38, box_y, box_z, width=0.72, height=1.5, material=3)
    add_quad(box_x, box_y, -0.40, width=0.5, height=0.5, material=2)

    add_quad(-4.2, 2.1, 0.0, material=4)      # neutral
    add_quad(-2.5, 2.1, 0.0, material=5)      # tinted orange
    add_quad(-0.8, 2.1, 0.0, material=6)      # dark
    add_quad(0.9, 2.1, 0.0, material=7)       # rough
    add_quad(2.6, 2.1, 0.0, material=8)       # blend 0.25 transmission
    add_quad(4.3, 2.1, 0.0, material=9)       # blend 1.0 transmission
    add_quad(-3.4, -1.2, 0.16, width=1.2, height=1.2, material=12)  # overlap front
    add_quad(-3.4, -1.2, 0.0, width=1.2, height=1.2, material=13)   # overlap back
    add_quad(-0.9, -1.2, 0.0, material=10)    # IOR 1 no interface
    add_quad(1.35, -1.2, -0.15, material=14)  # interior glass in front of box
    add_quad(3.2, -1.2, 0.0, material=11)     # MASK cutout transmission
    add_quad(4.9, -1.2, 0.0, width=1.6, height=1.2, material=15)   # double sided

    document = {
        'asset': {'version': '2.0',
                  'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'extensionsUsed': ['KHR_materials_transmission', 'KHR_materials_ior',
                           'KHR_materials_unlit'],
        'scene': 0, 'scenes': [{'nodes': list(range(len(nodes)))}],
        'nodes': nodes, 'meshes': meshes, 'materials': materials,
        'samplers': [{'wrapS': 10497, 'wrapT': 10497}],
        'images': [{'uri': cutout_uri}],
        'textures': [{'source': 0, 'sampler': 0}],
    }
    document, binary = finish(document)
    return _glb(document, binary, path)


def lighting_probe(path: Path) -> Path:
    """T28 Studio calibration fixture: twelve isolated material families.

    Nine spheres cover white plastic, neutral gray, black leather/enamel, red
    paint with clearcoat, brass, chrome, emission and white CAD; three
    camera-facing patches provide reference white/gray/black diffuse response.
    Glass is covered by the shared glass-reference/transmission-glass fixtures
    so this probe stays a pure exposure/highlight/material-identity target.
    Every primitive carries TANGENT/COLOR_0 so the complete-vertex material
    shader (and its diagnostic channels) is selected.
    """
    materials = [
        {'name': 'light white plastic',
         'pbrMetallicRoughness': {'baseColorFactor': [0.94, 0.92, 0.90, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.35}},
        {'name': 'light neutral gray',
         'pbrMetallicRoughness': {'baseColorFactor': [0.16, 0.16, 0.16, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.50}},
        {'name': 'light black leather',
         'pbrMetallicRoughness': {'baseColorFactor': [0.100, 0.060, 0.045, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.55}},
        {'name': 'light red paint',
         'extensions': {'KHR_materials_clearcoat': {
             'clearcoatFactor': 1.0, 'clearcoatRoughnessFactor': 0.04}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.55, 0.02, 0.015, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.30}},
        {'name': 'light brass',
         'pbrMetallicRoughness': {'baseColorFactor': [0.88, 0.64, 0.28, 1.0],
                                  'metallicFactor': 1.0, 'roughnessFactor': 0.25}},
        {'name': 'light chrome',
         'pbrMetallicRoughness': {'baseColorFactor': [0.92, 0.93, 0.95, 1.0],
                                  'metallicFactor': 1.0, 'roughnessFactor': 0.14}},
        {'name': 'light emission',
         'emissiveFactor': [1.0, 0.55, 0.18],
         'extensions': {'KHR_materials_emissive_strength': {'emissiveStrength': 4.0}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.10, 0.08, 0.06, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.50}},
        {'name': 'light black enamel',
         'extensions': {'KHR_materials_clearcoat': {
             'clearcoatFactor': 1.0, 'clearcoatRoughnessFactor': 0.03}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.045, 0.060, 0.110, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.06}},
        {'name': 'light white cad',
         'pbrMetallicRoughness': {'baseColorFactor': [0.76, 0.78, 0.80, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.65}},
        {'name': 'light reference white', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.86, 0.86, 0.86, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.85}},
        {'name': 'light reference gray', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.36, 0.36, 0.36, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.85}},
        {'name': 'light reference black', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.140, 0.140, 0.140, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.85}},
    ]
    positions, normals, uvs, indices = uv_sphere(segments=32, rings=20)
    add_mesh, finish = _mesh_builder()
    primitives = []
    locations = [(-3.3, 2.4), (-1.1, 2.4), (1.1, 2.4), (3.3, 2.4),
                 (-3.3, 0.0), (-1.1, 0.0), (1.1, 0.0), (3.3, 0.0)]
    for index, (x, y) in enumerate(locations):
        scale = 0.58
        base = add_mesh(
            [(px * scale + x, py * scale + y, pz * scale + scale)
             for px, py, pz in positions],
            normals, uvs, indices,
            tangents=[(1.0, 0.0, 0.0, 1.0)] * len(positions),
            colors=[(1.0, 1.0, 1.0, 1.0)] * len(positions))
        primitives.append({'attributes': {'POSITION': base, 'NORMAL': base + 1,
                                          'TEXCOORD_0': base + 2, 'TANGENT': base + 4,
                                          'COLOR_0': base + 5},
                           'indices': base + 3, 'material': index})
    # White CAD sphere plus the three reference patches on the bottom row.
    cad_base = add_mesh(
        [(px * 0.58 - 3.3, py * 0.58 - 2.6, pz * 0.58 + 0.58)
         for px, py, pz in positions],
        normals, uvs, indices,
        tangents=[(1.0, 0.0, 0.0, 1.0)] * len(positions),
        colors=[(1.0, 1.0, 1.0, 1.0)] * len(positions))
    primitives.append({'attributes': {'POSITION': cad_base, 'NORMAL': cad_base + 1,
                                      'TEXCOORD_0': cad_base + 2, 'TANGENT': cad_base + 4,
                                      'COLOR_0': cad_base + 5},
                       'indices': cad_base + 3, 'material': 8})
    for offset, material in enumerate((9, 10, 11)):
        x = -1.1 + offset * 2.2
        positions_quad, normals_quad, uvs_quad, indices_quad, tangents_quad = \
            _camera_facing_quad(x, -2.6, width=1.1, height=1.1)
        base = add_mesh(positions_quad, normals_quad, uvs_quad, indices_quad,
                        tangents=tangents_quad,
                        colors=[(1.0, 1.0, 1.0, 1.0)] * 4)
        primitives.append({'attributes': {'POSITION': base, 'NORMAL': base + 1,
                                          'TEXCOORD_0': base + 2, 'TANGENT': base + 4,
                                          'COLOR_0': base + 5},
                           'indices': base + 3, 'material': material})
    document = {
        'asset': {'version': '2.0',
                  'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'extensionsUsed': ['KHR_materials_clearcoat',
                           'KHR_materials_emissive_strength'],
        'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
        'meshes': [{'primitives': primitives}], 'materials': materials,
    }
    document, binary = finish(document)
    return _glb(document, binary, path)


def clay_review(path: Path) -> Path:
    """T29 Clay geometry-review fixture.

    The layout isolates the mode contract, not PBR appearance:

    * a coplanar identity row whose four materials differ in authored colour,
      metal/roughness, clearcoat, sheen, emission, specular and normal map. Clay
      must render them as the same neutral matte;
    * a MASK sheet with real texture-alpha holes, so cutouts survive even though
      the shading is replaced;
    * a BLEND/transmissive sheet over an opaque sphere, so an opaque override
      must hide the interior instead of showing it through authored alpha;
    * two overlapping BLEND sheets (formerly a blend-order seam);
    * a mirrored node, three small curved parts and mixed line/point primitives.

    Every quad carries TANGENT/COLOR_0 so the complete-vertex material shader
    (and its diagnostic channels) is selected.
    """
    normal_pixels = bytearray()
    for y in range(4):
        for x in range(4):
            normal_pixels += bytes((64 + x * 48, 128, 255))
    normal_uri = _png_data_uri(bytes(normal_pixels))
    cutout_uri = _rgba_png_data_uri(32, 32, lambda x, y: (
        210, 210, 210, 0 if 10 <= x <= 21 and 10 <= y <= 21 else 255))

    materials = [
        # Identity row. Authored values are deliberately extreme so a mode that
        # leaks any surface decoration is visible in one capture.
        {'name': 'clay red metal coat', 'doubleSided': True,
         'extensions': {'KHR_materials_clearcoat': {
             'clearcoatFactor': 1.0, 'clearcoatRoughnessFactor': 0.03},
             'KHR_materials_sheen': {'sheenColorFactor': [0.9, 0.2, 0.1],
                                     'sheenRoughnessFactor': 0.2}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.78, 0.05, 0.04, 1.0],
                                  'metallicFactor': 1.0, 'roughnessFactor': 0.08}},
        {'name': 'clay blue emission', 'doubleSided': True,
         'emissiveFactor': [0.95, 0.25, 0.05],
         'extensions': {'KHR_materials_emissive_strength': {'emissiveStrength': 5.0},
                        'KHR_materials_specular': {'specularColorFactor': [0.9, 0.1, 0.2],
                                                   'specularFactor': 1.0},
                        'KHR_materials_ior': {'ior': 2.1}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.04, 0.06, 0.72, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.35}},
        {'name': 'clay green normal', 'doubleSided': True,
         'normalTexture': {'index': 1, 'scale': 1.0},
         'pbrMetallicRoughness': {'baseColorFactor': [0.05, 0.62, 0.10, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.25}},
        {'name': 'clay neutral matte', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.55, 0.50, 0.46, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.6}},
        # Geometry row.
        {'name': 'clay cutout sheet', 'alphaMode': 'MASK', 'alphaCutoff': 0.5,
         'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [1.0, 1.0, 1.0, 1.0],
                                  'baseColorTexture': {'index': 0},
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.7}},
        {'name': 'clay glass sheet', 'alphaMode': 'BLEND', 'doubleSided': True,
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.92, 0.55, 0.10, 0.28],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.08}},
        {'name': 'clay hidden interior', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.05, 0.85, 0.95, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.55}},
        {'name': 'clay overlap near', 'alphaMode': 'BLEND', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.75, 0.15, 0.75, 0.45],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.5}},
        {'name': 'clay overlap far', 'alphaMode': 'BLEND', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.15, 0.75, 0.25, 0.45],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.5}},
        {'name': 'clay small one',
         'pbrMetallicRoughness': {'baseColorFactor': [0.80, 0.25, 0.20, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.5}},
        {'name': 'clay small two',
         'pbrMetallicRoughness': {'baseColorFactor': [0.20, 0.55, 0.80, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.5}},
        {'name': 'clay small three',
         'pbrMetallicRoughness': {'baseColorFactor': [0.25, 0.80, 0.35, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.5}},
        {'name': 'clay mirrored', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.55, 0.30, 0.85, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.5}},
        {'name': 'clay opaque transmissive', 'doubleSided': True,
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 0.8},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.90, 0.90, 0.20, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.1}},
    ]
    add_mesh, finish = _mesh_builder()
    position_bounds = []

    def attributes(base):
        return {'POSITION': base, 'NORMAL': base + 1, 'TEXCOORD_0': base + 2,
                'TANGENT': base + 4, 'COLOR_0': base + 5}

    def add_quad(x, y, z, width, height, material):
        positions, normals, uvs, indices, tangents, colors = _quad_at(
            x, y, z, width=width, height=height)
        base = add_mesh(positions, normals, uvs, indices,
                        tangents=tangents, colors=colors)
        position_bounds.append((base, positions))
        return base

    def add_sphere(x, y, z, radius, material, node=None):
        positions, normals, uvs, indices = uv_sphere(segments=20, rings=12)
        positions = [(px * radius + x, py * radius + y, pz * radius + z)
                     for px, py, pz in positions]
        base = add_mesh(
            positions,
            normals, uvs, indices,
            tangents=[(1.0, 0.0, 0.0, 1.0)] * len(positions),
            colors=[(1.0, 1.0, 1.0, 1.0)] * len(positions))
        position_bounds.append((base, positions))
        return base

    meshes = []
    nodes = []
    identity = [(-2.55, 0), (-0.85, 1), (0.85, 2), (2.55, 3)]
    for x, material in identity:
        base = add_quad(x, 1.05, 0.4, 1.1, 0.9, material)
        meshes.append({'name': f'clay identity {material}',
                       'primitives': [{'attributes': attributes(base),
                                       'indices': base + 3, 'material': material}]})
        nodes.append({'mesh': len(meshes) - 1})

    cut_base = add_quad(-2.15, 0.0, 0.4, 1.3, 1.1, 4)
    meshes.append({'name': 'clay cutout sheet',
                   'primitives': [{'attributes': attributes(cut_base),
                                   'indices': cut_base + 3, 'material': 4}]})
    nodes.append({'mesh': len(meshes) - 1})

    glass_base = add_quad(-0.55, 0.0, 0.45, 1.3, 1.1, 5)
    meshes.append({'name': 'clay glass sheet',
                   'primitives': [{'attributes': attributes(glass_base),
                                   'indices': glass_base + 3, 'material': 5}]})
    nodes.append({'mesh': len(meshes) - 1})

    interior_base = add_sphere(-0.55, 0.0, -0.10, 0.25, 6)
    meshes.append({'name': 'clay hidden interior',
                   'primitives': [{'attributes': attributes(interior_base),
                                   'indices': interior_base + 3, 'material': 6}]})
    nodes.append({'mesh': len(meshes) - 1})

    for x, z, material in ((0.95, 0.40, 7), (1.25, 0.48, 8)):
        over_base = add_quad(x, 0.0, z, 1.1, 1.0, material)
        meshes.append({'name': f'clay overlap {material}',
                       'primitives': [{'attributes': attributes(over_base),
                                       'indices': over_base + 3, 'material': material}]})
        nodes.append({'mesh': len(meshes) - 1})

    opaque_glass_base = add_quad(2.2, 0.0, 0.4, 0.7, 0.85, 13)
    meshes.append({'name': 'clay opaque transmissive',
                   'primitives': [{'attributes': attributes(opaque_glass_base),
                                   'indices': opaque_glass_base + 3, 'material': 13}]})
    nodes.append({'mesh': len(meshes) - 1})

    # Three small curved parts test that the matte treatment keeps sub-pixel-ish
    # curvature readable instead of clipping to a flat white blob.
    for x, radius, material in ((-2.25, 0.08, 9), (-1.85, 0.11, 10),
                                (-1.40, 0.16, 11)):
        small_base = add_sphere(x, -1.0, 0.4, radius, material)
        meshes.append({'name': f'clay small {material}',
                       'primitives': [{'attributes': attributes(small_base),
                                       'indices': small_base + 3, 'material': material}]})
        nodes.append({'mesh': len(meshes) - 1})

    # A mirrored instance: positions stay local so the node's negative X scale
    # exercises the determinant/cull/tangent path in the overridden bucket.
    mirror_positions, mirror_normals, mirror_uvs, mirror_indices = uv_sphere(
        segments=20, rings=12)
    mirror_base = add_mesh(
        [(px * 0.38, py * 0.38, pz * 0.38) for px, py, pz in mirror_positions],
        mirror_normals, mirror_uvs, mirror_indices,
        tangents=[(1.0, 0.0, 0.0, 1.0)] * len(mirror_positions),
        colors=[(1.0, 1.0, 1.0, 1.0)] * len(mirror_positions))
    position_bounds.append((mirror_base,
                            [(px * 0.38, py * 0.38, pz * 0.38)
                             for px, py, pz in mirror_positions]))
    meshes.append({'name': 'clay mirrored',
                   'primitives': [{'attributes': attributes(mirror_base),
                                   'indices': mirror_base + 3, 'material': 12}]})
    nodes.append({'mesh': len(meshes) - 1,
                  'translation': [1.75, -1.0, 0.4], 'scale': [-1.0, 1.0, 1.0]})

    document = {
        'asset': {'version': '2.0',
                  'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'extensionsUsed': ['KHR_materials_clearcoat', 'KHR_materials_sheen',
                           'KHR_materials_emissive_strength',
                           'KHR_materials_specular', 'KHR_materials_ior',
                           'KHR_materials_transmission'],
        'scene': 0, 'scenes': [{'nodes': list(range(len(nodes)))}],
        'nodes': nodes, 'meshes': meshes, 'materials': materials,
        'samplers': [{'wrapS': 33071, 'wrapT': 33071}],
        'images': [{'uri': cutout_uri}, {'uri': normal_uri}],
        'textures': [{'source': 0, 'sampler': 0}, {'source': 1, 'sampler': 0}],
    }
    document, binary = finish(document)
    # _mesh_builder writes a placeholder [-1,1] POSITION range because most
    # fixtures are small. This fixture spans a wide layout and the viewer's
    # automatic framing uses these ranges, so publish the real per-mesh bounds
    # (world-baked for quads/spheres, local for the mirrored node).
    for base, positions in position_bounds:
        accessor = document['accessors'][base]
        accessor['min'] = [min(point[axis] for point in positions) for axis in range(3)]
        accessor['max'] = [max(point[axis] for point in positions) for axis in range(3)]
    return _glb(document, binary, path)


def directional_probe(path: Path) -> Path:
    """T30 world-anchored Directional sun fixture.

    A wall of flat panels faces -Y (the front camera at yaw 0 / pitch 90)
    and a horizontal panel faces +Z. Every panel has one constant normal, so
    the direct-diffuse channel isolates the light transform:

    * front-lit / back-lit azimuth sweeps change the wall response while the
      camera stays still;
    * orbiting the camera around the vertical axis must leave the wall's
      direct response unchanged when the sun is world-anchored;
    * the floor panel's response grows with sun elevation;
    * ``directional lambert`` disables specular (ior 1, specularFactor 0) so
      channel 14 is the pure Lambert term;
    * black paint, gold metal and thin glass keep the ambient reflection
      floor and separation roles;
    * the emission panel must survive an intensity-zero frame.
    """
    materials = [
        {'name': 'directional lambert',
         'extensions': {'KHR_materials_ior': {'ior': 1.0},
                        'KHR_materials_specular': {'specularFactor': 0.0}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.82, 0.82, 0.82, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.9}},
        {'name': 'directional coat',
         'extensions': {'KHR_materials_clearcoat': {
             'clearcoatFactor': 1.0, 'clearcoatRoughnessFactor': 0.22}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.12, 0.32, 0.92, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.4}},
        {'name': 'directional black paint',
         'extensions': {'KHR_materials_clearcoat': {
             'clearcoatFactor': 1.0, 'clearcoatRoughnessFactor': 0.05}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.16, 0.04, 0.08, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.25}},
        {'name': 'directional gold metal',
         'pbrMetallicRoughness': {'baseColorFactor': [1.0, 0.72, 0.28, 1.0],
                                  'metallicFactor': 1.0, 'roughnessFactor': 0.3}},
        {'name': 'directional glass',
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.35, 0.75, 0.9, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.05}},
        {'name': 'directional emission',
         'emissiveFactor': [0.9, 0.15, 0.65],
         'extensions': {'KHR_materials_emissive_strength': {'emissiveStrength': 4.0}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.55, 0.08, 0.48, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.6}},
        {'name': 'directional floor lambert',
         'extensions': {'KHR_materials_ior': {'ior': 1.0},
                        'KHR_materials_specular': {'specularFactor': 0.0}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.6, 0.65, 0.72, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.9}},
    ]
    add_mesh, finish = _mesh_builder()
    primitives = []
    position_bounds = []

    def add_panel(positions, indices, normal, material_index):
        uvs = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)]
        tangents = [(1.0, 0.0, 0.0, 1.0)] * 4
        colors = [(1.0, 1.0, 1.0, 1.0)] * 4
        base = add_mesh(positions, [normal] * 4, uvs, indices,
                        tangents=tangents, colors=colors)
        primitives.append({'attributes': {'POSITION': base, 'NORMAL': base + 1,
                                          'TEXCOORD_0': base + 2, 'TANGENT': base + 4,
                                          'COLOR_0': base + 5},
                           'indices': base + 3, 'material': material_index})
        position_bounds.append((base, positions))

    # glTF is authored Y-up; the viewer's import correction maps source (x,y,z)
    # to world (x, -z, y). Author the source fixtures in that frame:
    #   source XY plane, normal +Z  -> world XZ wall at y=0, normal -Y
    #   source XZ plane at y=-1, normal +Y -> world XY floor at z=-1, normal +Z
    wall_centers = [-3.0, -1.8, -0.6, 0.6, 1.8, 3.0]
    for column, x in enumerate(wall_centers):
        left, right = x - 0.48, x + 0.48
        bottom, top = 0.25, 1.35
        positions = [(left, bottom, 0.0), (right, bottom, 0.0),
                     (left, top, 0.0), (right, top, 0.0)]
        add_panel(positions, [0, 1, 2, 1, 3, 2], (0.0, 0.0, 1.0), column)

    # World floor at z=-1: source y=-1. Source z becomes world -Y, so positive
    # source z places the panel in front of the wall (toward the camera).
    floor = [(-1.2, -1.0, 0.3), (1.2, -1.0, 0.3),
             (-1.2, -1.0, 1.7), (1.2, -1.0, 1.7)]
    add_panel(floor, [0, 2, 1, 1, 2, 3], (0.0, 1.0, 0.0), 6)

    document = {
        'asset': {'version': '2.0',
                  'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'extensionsUsed': ['KHR_materials_transmission', 'KHR_materials_ior',
                           'KHR_materials_clearcoat', 'KHR_materials_specular',
                           'KHR_materials_emissive_strength'],
        'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
        'meshes': [{'primitives': primitives}], 'materials': materials,
    }
    document, binary = finish(document)
    for base, positions in position_bounds:
        accessor = document['accessors'][base]
        accessor['min'] = [min(point[axis] for point in positions) for axis in range(3)]
        accessor['max'] = [max(point[axis] for point in positions) for axis in range(3)]
    return _glb(document, binary, path)


def contact_shadow_probe(path: Path) -> Path:
    """T31 geometry-derived ground contact fixture (source Y-up).

    All objects sit above a nominal ground plane at source y = 0. The layout
    separates casters so the GPU lane can label connected shadow regions:

    * a suspended torus (strap-loop analogue) whose projected shadow keeps a
      bright hole instead of filling with an ellipse;
    * two squashed spheres touching the ground (tire-like contacts);
    * a suspended sphere with empty space directly below it;
    * equal opaque and clear-transmission horizontal panels, so the glass
      shadow can be compared with the opaque one and must stay weaker;
    * a MASK cutout sheet whose authored alpha holes survive the caster pass;
    * a mirrored node proving cull-free casting.

    Every mesh carries the complete vertex layout so the textured material and
    MASK alpha test are available to both the visible pass and the caster pass.
    """
    cutout_uri = _rgba_png_data_uri(32, 32, lambda x, y: (
        220, 220, 220, 0 if 9 <= x <= 22 and 9 <= y <= 22 else 255))

    materials = [
        {'name': 'contact ring opaque', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.78, 0.72, 0.60, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.55}},
        {'name': 'contact tire opaque', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.10, 0.11, 0.13, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.75}},
        {'name': 'contact suspended sphere', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.20, 0.42, 0.85, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.5}},
        {'name': 'contact opaque panel', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.86, 0.86, 0.84, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.6}},
        {'name': 'contact clear panel', 'doubleSided': True,
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.88, 0.94, 1.0, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.04}},
        {'name': 'contact cutout sheet', 'alphaMode': 'MASK', 'alphaCutoff': 0.5,
         'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [1.0, 1.0, 1.0, 1.0],
                                  'baseColorTexture': {'index': 0},
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.7}},
        {'name': 'contact mirrored part', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.55, 0.25, 0.80, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.5}},
    ]
    add_mesh, finish = _mesh_builder()
    position_bounds = []
    meshes = []
    nodes = []

    def attributes(base):
        return {'POSITION': base, 'NORMAL': base + 1, 'TEXCOORD_0': base + 2,
                'TANGENT': base + 4, 'COLOR_0': base + 5}

    def add_complete(positions, normals, uvs, indices, material, name, node=None):
        base = add_mesh(positions, normals, uvs, indices,
                        tangents=[(1.0, 0.0, 0.0, 1.0)] * len(positions),
                        colors=[(1.0, 1.0, 1.0, 1.0)] * len(positions))
        position_bounds.append((base, positions))
        meshes.append({'name': name,
                       'primitives': [{'attributes': attributes(base),
                                       'indices': base + 3, 'material': material}]})
        nodes.append(node if node is not None else {'mesh': len(meshes) - 1})

    def torus(major, minor, segments=48, sides=16, center=(0.0, 0.0, 0.0)):
        positions, normals, uvs, indices = [], [], [], []
        cx, cy, cz = center
        for i in range(segments):
            u = 2.0 * math.pi * i / segments
            for j in range(sides):
                v = 2.0 * math.pi * j / sides
                radial = major + minor * math.cos(v)
                positions.append((cx + radial * math.cos(u),
                                  cy + minor * math.sin(v),
                                  cz + radial * math.sin(u)))
                normals.append((math.cos(v) * math.cos(u), math.sin(v),
                                math.cos(v) * math.sin(u)))
                uvs.append((i / segments, j / sides))
        for i in range(segments):
            for j in range(sides):
                a = i * sides + j
                b = ((i + 1) % segments) * sides + j
                c = i * sides + (j + 1) % sides
                d = ((i + 1) % segments) * sides + (j + 1) % sides
                indices += [a, b, c, c, b, d]
        return positions, normals, uvs, indices

    def squashed_sphere(center, scale, segments=28, rings=18):
        positions, normals, uvs, indices = uv_sphere(segments=segments, rings=rings)
        cx, cy, cz = center
        sx, sy, sz = scale
        transformed = [(px * sx + cx, py * sy + cy, pz * sz + cz)
                       for px, py, pz in positions]
        return transformed, normals, uvs, indices

    def horizontal_panel(center, width, depth):
        cx, cy, cz = center
        left, right = cx - width * 0.5, cx + width * 0.5
        near, far = cz - depth * 0.5, cz + depth * 0.5
        positions = [(left, cy, near), (right, cy, near),
                     (left, cy, far), (right, cy, far)]
        normals = [(0.0, 1.0, 0.0)] * 4
        uvs = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)]
        indices = [0, 2, 1, 1, 2, 3]
        return positions, normals, uvs, indices

    ring = torus(0.78, 0.24, center=(-2.30, 2.00, 0.0))
    add_complete(*ring, 0, 'contact ring')

    for index, x in enumerate((-0.05, 1.05)):
        tire = squashed_sphere((x, 0.34, -1.10), (0.50, 0.34, 0.50))
        add_complete(*tire, 1, f'contact tire {index}')

    suspended = squashed_sphere((-1.00, 1.70, 1.10), (0.32, 0.32, 0.32))
    add_complete(*suspended, 2, 'contact suspended sphere')

    add_complete(*horizontal_panel((0.80, 1.50, 0.35), 0.95, 0.95),
                 3, 'contact opaque panel')
    add_complete(*horizontal_panel((1.85, 1.50, 0.35), 0.95, 0.95),
                 4, 'contact clear panel')
    add_complete(*horizontal_panel((-1.00, 2.15, 1.75), 1.25, 1.25),
                 5, 'contact cutout sheet')

    mirrored = squashed_sphere((0.0, 0.30, 1.70), (0.30, 0.30, 0.30))
    add_complete(*mirrored, 6, 'contact mirrored part',
                 node={'mesh': len(meshes), 'translation': [2.55, 0.0, 1.70],
                       'scale': [-1.0, 1.0, 1.0]})

    document = {
        'asset': {'version': '2.0',
                  'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'extensionsUsed': ['KHR_materials_transmission', 'KHR_materials_ior'],
        'scene': 0, 'scenes': [{'nodes': list(range(len(nodes)))}],
        'nodes': nodes, 'meshes': meshes, 'materials': materials,
        'samplers': [{'wrapS': 33071, 'wrapT': 33071}],
        'images': [{'uri': cutout_uri}],
        'textures': [{'source': 0, 'sampler': 0}],
    }
    document, binary = finish(document)
    for base, positions in position_bounds:
        accessor = document['accessors'][base]
        accessor['min'] = [min(point[axis] for point in positions) for axis in range(3)]
        accessor['max'] = [max(point[axis] for point in positions) for axis in range(3)]
    return _glb(document, binary, path)


def ao_probe(path: Path, scale: float = 1.0) -> Path:
    """T32 scale-aware screen-space AO fixture (source Y-up).

    The layout gives the AO lane labelable regions without any per-asset
    shading exception:

    * an inside corner (floor + two walls) plus a black body panel;
    * a sphere resting on a plate (local contact);
    * two narrow parallel plates whose gap must stay a gap;
    * a thin vertical ring (camera-strap analogue) whose hole stays bright;
    * a deeply recessed emissive panel that AO must not darken;
    * a transmissive window in front of background (no opaque AO sheet);
    * a MASK cutout sheet with a hole (no AO sheet through the hole);
    * a mirrored instance of the corner assembly.

    ``scale`` multiplies the whole assembly through a root node, so the same
    fixture can prove the radius is scene-relative rather than model-fixed.
    """
    cutout_uri = _rgba_png_data_uri(32, 32, lambda x, y: (
        230, 230, 230, 0 if 10 <= x <= 21 and 10 <= y <= 21 else 255))

    materials = [
        {'name': 'ao neutral wall', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.72, 0.72, 0.70, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.55}},
        {'name': 'ao floor', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.56, 0.58, 0.60, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.65}},
        {'name': 'ao contact sphere', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.28, 0.48, 0.86, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.42}},
        {'name': 'ao black body', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.018, 0.019, 0.022, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.38}},
        {'name': 'ao emissive panel', 'doubleSided': True,
         'emissiveFactor': [1.0, 0.42, 0.12],
         'extensions': {'KHR_materials_emissive_strength': {'emissiveStrength': 2.4}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.02, 0.02, 0.02, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.6}},
        {'name': 'ao clear window', 'doubleSided': True,
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.90, 0.95, 1.0, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.04}},
        {'name': 'ao cutout sheet', 'alphaMode': 'MASK', 'alphaCutoff': 0.5,
         'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [1.0, 1.0, 1.0, 1.0],
                                  'baseColorTexture': {'index': 0},
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.7}},
        {'name': 'ao thin ring', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.80, 0.74, 0.58, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.45}},
    ]
    add_mesh, finish = _mesh_builder()
    position_bounds = []
    meshes = []
    nodes = []

    def attributes(base):
        return {'POSITION': base, 'NORMAL': base + 1, 'TEXCOORD_0': base + 2,
                'TANGENT': base + 4, 'COLOR_0': base + 5}

    def add_complete(positions, normals, uvs, indices, material, name, node=None):
        base = add_mesh(positions, normals, uvs, indices,
                        tangents=[(1.0, 0.0, 0.0, 1.0)] * len(positions),
                        colors=[(1.0, 1.0, 1.0, 1.0)] * len(positions))
        position_bounds.append((base, positions))
        meshes.append({'name': name,
                       'primitives': [{'attributes': attributes(base),
                                       'indices': base + 3, 'material': material}]})
        nodes.append(node if node is not None else {'mesh': len(meshes) - 1})

    def quad(center, right, up):
        cx, cy, cz = center
        rx, ry, rz = right
        ux, uy, uz = up
        positions = [
            (cx - rx - ux, cy - ry - uy, cz - rz - uz),
            (cx + rx - ux, cy + ry - uy, cz + rz - uz),
            (cx - rx + ux, cy - ry + uy, cz - rz + uz),
            (cx + rx + ux, cy + ry + uy, cz + rz + uz),
        ]
        normals = [(0.0, 0.0, 1.0)] * 4
        uvs = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)]
        indices = [0, 2, 1, 1, 2, 3]
        return positions, normals, uvs, indices

    def box(center, half):
        cx, cy, cz = center
        hx, hy, hz = half
        positions, normals, uvs, indices = [], [], [], []
        faces = [
            ((1, 0, 0), [(hx, -hy, -hz), (hx, hy, -hz), (hx, -hy, hz), (hx, hy, hz)]),
            ((-1, 0, 0), [(-hx, -hy, hz), (-hx, hy, hz), (-hx, -hy, -hz), (-hx, hy, -hz)]),
            ((0, 1, 0), [(-hx, hy, -hz), (hx, hy, -hz), (-hx, hy, hz), (hx, hy, hz)]),
            ((0, -1, 0), [(-hx, -hy, hz), (hx, -hy, hz), (-hx, -hy, -hz), (hx, -hy, -hz)]),
            ((0, 0, 1), [(-hx, -hy, hz), (hx, -hy, hz), (-hx, hy, hz), (hx, hy, hz)]),
            ((0, 0, -1), [(hx, -hy, -hz), (-hx, -hy, -hz), (hx, hy, -hz), (-hx, hy, -hz)]),
        ]
        for normal, corners in faces:
            base = len(positions)
            for corner in corners:
                positions.append((cx + corner[0], cy + corner[1], cz + corner[2]))
                normals.append(normal)
                uvs.append((0.0, 0.0))
            indices += [base, base + 1, base + 2, base + 2, base + 1, base + 3]
        return positions, normals, uvs, indices

    def torus(major, minor, segments=48, sides=12, center=(0.0, 0.0, 0.0)):
        positions, normals, uvs, indices = [], [], [], []
        cx, cy, cz = center
        for i in range(segments):
            u = 2.0 * math.pi * i / segments
            for j in range(sides):
                v = 2.0 * math.pi * j / sides
                radial = major + minor * math.cos(v)
                # Ring lies in the XY plane, so the hole is camera-facing in
                # the default front view (a strap-like loop).
                positions.append((cx + radial * math.cos(u),
                                  cy + radial * math.sin(u),
                                  cz + minor * math.sin(v)))
                normals.append((math.cos(v) * math.cos(u),
                                math.cos(v) * math.sin(u), math.sin(v)))
                uvs.append((i / segments, j / sides))
        for i in range(segments):
            for j in range(sides):
                a = i * sides + j
                b = ((i + 1) % segments) * sides + j
                c = i * sides + (j + 1) % sides
                d = ((i + 1) % segments) * sides + (j + 1) % sides
                indices += [a, b, c, c, b, d]
        return positions, normals, uvs, indices

    # Inside corner: floor, back wall and side wall.
    add_complete(*box((-1.35, 0.06, -0.10), (0.75, 0.06, 0.75)), 1, 'ao corner floor')
    add_complete(*box((-1.35, 0.70, -0.82), (0.75, 0.64, 0.06)), 0, 'ao corner back wall')
    add_complete(*box((-2.07, 0.70, -0.10), (0.06, 0.64, 0.75)), 0, 'ao corner side wall')
    add_complete(*quad((-1.35, 0.70, -0.73), (0.22, 0.0, 0.0), (0.0, 0.30, 0.0)),
                 3, 'ao black body panel')
    add_complete(*quad((-1.35, 0.66, -0.70), (0.18, 0.0, 0.0), (0.0, 0.24, 0.0)),
                 4, 'ao emissive panel')

    # Open recess (a five-sided box with the opening toward the camera). This
    # is the main cavity: the back and side walls are all inside one another's
    # screen-space horizon, so a restrained AO mask must darken the interior
    # while the outer silhouette stays unchanged.
    recess_center = (-0.20, 1.05, -0.15)
    recess_half = (0.35, 0.35, 0.25)
    add_complete(*quad((recess_center[0], recess_center[1],
                        recess_center[2] - recess_half[2]),
                       (recess_half[0], 0.0, 0.0), (0.0, recess_half[1], 0.0)),
                 0, 'ao recess back')
    add_complete(*quad((recess_center[0] - recess_half[0], recess_center[1],
                        recess_center[2]),
                       (0.0, 0.0, recess_half[2]), (0.0, recess_half[1], 0.0)),
                 0, 'ao recess left')
    add_complete(*quad((recess_center[0] + recess_half[0], recess_center[1],
                        recess_center[2]),
                       (0.0, 0.0, recess_half[2]), (0.0, recess_half[1], 0.0)),
                 0, 'ao recess right')
    add_complete(*quad((recess_center[0], recess_center[1] - recess_half[1],
                        recess_center[2]),
                       (recess_half[0], 0.0, 0.0), (0.0, 0.0, recess_half[2])),
                 0, 'ao recess bottom')
    add_complete(*quad((recess_center[0], recess_center[1] + recess_half[1],
                        recess_center[2]),
                       (recess_half[0], 0.0, 0.0), (0.0, 0.0, recess_half[2])),
                 0, 'ao recess top')
    # A shelf across the recess opening: the back wall below it is in the
    # shelf's screen-space horizon, which produces the broad cavity darkening
    # a wall-only corner cannot.
    add_complete(*box((recess_center[0], recess_center[1] + 0.02,
                       recess_center[2] + recess_half[2] + 0.10),
                      (recess_half[0] + 0.02, 0.035, 0.12)),
                 0, 'ao recess shelf')

    # Contact sphere on its own small plate.
    positions, normals, uvs, indices = uv_sphere(segments=32, rings=20)
    sphere = [(x * 0.38 + 0.55, y * 0.38 + 0.38, z * 0.38 + 0.0)
              for x, y, z in positions]
    add_complete(sphere, normals, uvs, indices, 2, 'ao contact sphere')
    add_complete(*box((0.55, 0.04, 0.0), (0.48, 0.04, 0.48)), 1, 'ao contact plate')

    # Two narrow parallel plates: the gap must remain a gap.
    add_complete(*box((1.55, 0.62, 0.0), (0.30, 0.56, 0.035)), 0, 'ao gap plate left')
    add_complete(*box((1.86, 0.62, 0.0), (0.30, 0.56, 0.035)), 0, 'ao gap plate right')
    add_complete(*box((1.705, 0.04, 0.0), (0.42, 0.04, 0.42)), 1, 'ao gap floor')

    # A clearly visible emissive panel in front of the assembly. AO must leave
    # emission untouched, so this is the lane's warm-pixel invariant.
    add_complete(*quad((0.10, 1.62, 0.55), (0.20, 0.0, 0.0), (0.0, 0.16, 0.0)),
                 4, 'ao front emissive panel')

    # Thin ring whose hole stays bright (camera strap analogue).
    add_complete(*torus(0.52, 0.055, center=(1.30, 1.70, 0.30)), 7, 'ao thin ring')

    # Transmissive window in front of background, above the assembly.
    add_complete(*quad((2.55, 1.55, 0.55), (0.42, 0.0, 0.0), (0.0, 0.52, 0.0)),
                 5, 'ao clear window')
    # MASK cutout sheet in front of background, beside the window.
    add_complete(*quad((3.45, 1.55, 0.55), (0.42, 0.0, 0.0), (0.0, 0.52, 0.0)),
                 6, 'ao cutout sheet')

    # Mirrored copy of the contact assembly proves two-sided/mirrored handling.
    mirrored_sphere = [(-(x * 0.26) - 0.55, y * 0.26 + 0.26, z * 0.26 - 0.55)
                       for x, y, z in positions]
    add_complete(mirrored_sphere, normals, uvs, indices, 2,
                 'ao mirrored sphere',
                 node={'mesh': len(meshes), 'translation': [2.30, 0.0, -0.55],
                       'scale': [-1.0, 1.0, 1.0]})

    document = {
        'asset': {'version': '2.0',
                  'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'extensionsUsed': ['KHR_materials_transmission', 'KHR_materials_ior',
                           'KHR_materials_emissive_strength'],
        'scene': 0, 'scenes': [{'nodes': [0]}],
        'nodes': [{'children': list(range(1, len(nodes) + 1)),
                   'scale': [scale, scale, scale]}],
        'meshes': meshes, 'materials': materials,
        'samplers': [{'wrapS': 33071, 'wrapT': 33071}],
        'images': [{'uri': cutout_uri}],
        'textures': [{'source': 0, 'sampler': 0}],
    }
    # The assembly nodes are children; each mesh node keeps its own transform.
    document['nodes'].extend(nodes)
    document['scenes'][0]['nodes'] = [0]
    document, binary = finish(document)
    for base, positions in position_bounds:
        accessor = document['accessors'][base]
        accessor['min'] = [min(point[axis] for point in positions) for axis in range(3)]
        accessor['max'] = [max(point[axis] for point in positions) for axis in range(3)]
    return _glb(document, binary, path)


def edge_probe(path: Path) -> Path:
    """T33 edge/detail/sampling fixture (source Y-up).

    Roles are geometric and material, never asset names:
    * sub-pixel horizontal and diagonal wires;
    * a high-frequency metal grille with a dark backing;
    * an engraved digit strip that must stay legible after edge AA;
    * a polished chrome sphere;
    * a low-roughness metal panel with a high-frequency normal map, so mip and
      anisotropic filtering can be inspected without a post blur;
    * a clear transmissive lens.
    The presentation lane additionally turns the ground grid on for a glancing
    procedural-line case. MASK cutout coverage is exercised on the established
    ao-probe fixture so this probe keeps one map per visible semantic.
    """
    digits = {
        '0': ('01110', '10001', '10011', '10101', '11001', '10001', '01110'),
        '1': ('00100', '01100', '00100', '00100', '00100', '00100', '01110'),
        '2': ('01110', '10001', '00001', '00010', '00100', '01000', '11111'),
        '3': ('11110', '00001', '00001', '01110', '00001', '00001', '11110'),
        '4': ('00010', '00110', '01010', '10010', '11111', '00010', '00010'),
        '5': ('11111', '10000', '11110', '00001', '00001', '10001', '01110'),
        '6': ('00110', '01000', '10000', '11110', '10001', '10001', '01110'),
        '7': ('11111', '00001', '00010', '00100', '01000', '01000', '01000'),
        '8': ('01110', '10001', '10001', '01110', '10001', '10001', '01110'),
        '9': ('01110', '10001', '10001', '01111', '00001', '00010', '01100'),
    }
    text_w, text_h = 256, 96
    text_pixels = bytearray(bytes((24, 26, 30)) * (text_w * text_h))
    for index, glyph in enumerate('0123456789'):
        pattern = digits[glyph]
        origin_x = 16 + index * 23
        origin_y = 34
        for row, line in enumerate(pattern):
            for column, bit in enumerate(line):
                if bit != '1':
                    continue
                for sy in range(3):
                    for sx in range(3):
                        x = origin_x + column * 3 + sx
                        y = origin_y + row * 3 + sy
                        if 0 <= x < text_w and 0 <= y < text_h:
                            at = (y * text_w + x) * 3
                            text_pixels[at:at + 3] = bytes((238, 236, 228))
    text_uri = _png_data_uri(bytes(text_pixels), text_w, text_h)

    normal_size = 128
    normal_pixels = bytearray()
    for y in range(normal_size):
        for x in range(normal_size):
            nx = 0.34 * math.sin(x * 0.71) + 0.16 * math.cos(y * 1.31)
            ny = 0.34 * math.cos(y * 0.83) + 0.16 * math.sin(x * 1.17)
            nz = math.sqrt(max(0.05, 1.0 - nx * nx - ny * ny))
            normal_pixels += bytes((int((nx * 0.5 + 0.5) * 255),
                                    int((ny * 0.5 + 0.5) * 255),
                                    int((nz * 0.5 + 0.5) * 255)))
    normal_uri = _png_data_uri(bytes(normal_pixels), normal_size, normal_size)

    mr_size = 64
    mr_pixels = bytearray()
    for y in range(mr_size):
        for x in range(mr_size):
            rough = 0.06 + 0.14 * (0.5 + 0.5 * math.sin(x * 1.9 + y * 1.3))
            mr_pixels += bytes((0, int(rough * 255), 255))
    mr_uri = _png_data_uri(bytes(mr_pixels), mr_size, mr_size)

    materials = [
        {'name': 'edge dark backing', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.035, 0.038, 0.042, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.55}},
        {'name': 'edge engraved text', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [1.0, 1.0, 1.0, 1.0],
                                  'baseColorTexture': {'index': 0},
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.62}},
        {'name': 'edge grille metal', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.72, 0.74, 0.78, 1.0],
                                  'metallicFactor': 1.0, 'roughnessFactor': 0.34}},
        {'name': 'edge thin wire', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.86, 0.87, 0.90, 1.0],
                                  'metallicFactor': 1.0, 'roughnessFactor': 0.22}},
        {'name': 'edge chrome', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.92, 0.94, 0.98, 1.0],
                                  'metallicFactor': 1.0, 'roughnessFactor': 0.025}},
        {'name': 'edge normal detail', 'doubleSided': True,
         'normalTexture': {'index': 1},
         'pbrMetallicRoughness': {'baseColorFactor': [0.74, 0.76, 0.80, 1.0],
                                  'metallicRoughnessTexture': {'index': 2},
                                  'metallicFactor': 1.0, 'roughnessFactor': 0.15}},
        {'name': 'edge clear lens', 'doubleSided': True,
         'extensions': {'KHR_materials_transmission': {'transmissionFactor': 1.0},
                        'KHR_materials_ior': {'ior': 1.5}},
         'pbrMetallicRoughness': {'baseColorFactor': [0.94, 0.97, 1.0, 1.0],
                                  'metallicFactor': 0.0, 'roughnessFactor': 0.03}},
    ]

    add_mesh, finish = _mesh_builder()
    position_bounds = []
    meshes = []

    def add_complete(positions, normals, uvs, indices, material, name, node=None):
        base = add_mesh(positions, normals, uvs, indices,
                        tangents=[(1.0, 0.0, 0.0, 1.0)] * len(positions),
                        colors=[(1.0, 1.0, 1.0, 1.0)] * len(positions))
        position_bounds.append((base, positions))
        meshes.append({'name': name,
                       'primitives': [{'attributes': {
                           'POSITION': base, 'NORMAL': base + 1, 'TEXCOORD_0': base + 2,
                           'TANGENT': base + 4, 'COLOR_0': base + 5},
                           'indices': base + 3, 'material': material}]})
        node_out = node if node is not None else {'mesh': len(meshes) - 1}
        return node_out

    def quad(center, half_width, half_height, z_offset=0.0):
        cx, cy, cz = center
        positions = [
            (cx - half_width, cy - half_height, cz + z_offset),
            (cx + half_width, cy - half_height, cz + z_offset),
            (cx - half_width, cy + half_height, cz + z_offset),
            (cx + half_width, cy + half_height, cz + z_offset),
        ]
        normals = [(0.0, 0.0, 1.0)] * 4
        uvs = [(0.0, 1.0), (1.0, 1.0), (0.0, 0.0), (1.0, 0.0)]
        indices = [0, 1, 2, 2, 1, 3]
        return positions, normals, uvs, indices

    def box(center, half):
        cx, cy, cz = center
        hx, hy, hz = half
        positions, normals, uvs, indices = [], [], [], []
        faces = [
            ((1, 0, 0), [(hx, -hy, -hz), (hx, hy, -hz), (hx, -hy, hz), (hx, hy, hz)]),
            ((-1, 0, 0), [(-hx, -hy, hz), (-hx, hy, hz), (-hx, -hy, -hz), (-hx, hy, -hz)]),
            ((0, 1, 0), [(-hx, hy, -hz), (hx, hy, -hz), (-hx, hy, hz), (hx, hy, hz)]),
            ((0, -1, 0), [(-hx, -hy, hz), (hx, -hy, hz), (-hx, -hy, -hz), (hx, -hy, -hz)]),
            ((0, 0, 1), [(-hx, -hy, hz), (hx, -hy, hz), (-hx, hy, hz), (hx, hy, hz)]),
            ((0, 0, -1), [(hx, -hy, -hz), (-hx, -hy, -hz), (hx, hy, -hz), (-hx, hy, -hz)]),
        ]
        for normal, corners in faces:
            base = len(positions)
            for corner in corners:
                positions.append((cx + corner[0], cy + corner[1], cz + corner[2]))
                normals.append(normal)
                uvs.append((0.0, 0.0))
            indices += [base, base + 1, base + 2, base + 2, base + 1, base + 3]
        return positions, normals, uvs, indices

    nodes = []
    nodes.append(add_complete(*box((0.0, 1.25, -0.18), (2.15, 1.32, 0.05)),
                              0, 'edge backing panel'))
    nodes.append(add_complete(*quad((-1.22, 1.92, 0.50), 0.94, 0.31),
                              1, 'edge engraved digit strip'))
    # Grille: fixed-count vertical bars plus a frame, all complete-vertex.
    for bar in range(13):
        x = 0.42 + bar * 0.105
        nodes.append(add_complete(*box((x, 1.92, 0.50), (0.011, 0.30, 0.018)),
                                  2, f'edge grille bar {bar}'))
    nodes.append(add_complete(*box((1.05, 2.24, 0.50), (0.66, 0.018, 0.020)),
                              2, 'edge grille top rail'))
    nodes.append(add_complete(*box((1.05, 1.60, 0.50), (0.66, 0.018, 0.020)),
                              2, 'edge grille bottom rail'))
    # Sub-pixel wires, including one diagonal.
    for wire in range(5):
        y = 1.20 + wire * 0.028
        nodes.append(add_complete(*box((0.0, y, 0.55), (1.95, 0.003, 0.009)),
                                  3, f'edge wire {wire}'))
    nodes.append(add_complete(
        *box((0.35, 1.34, 0.56), (0.0028, 0.24, 0.009)),
        3, 'edge diagonal wire'))
    positions, normals, uvs, indices = uv_sphere(segments=40, rings=24)
    chrome = [(x * 0.31 - 1.38, y * 0.31 + 0.58, z * 0.31 + 0.20)
              for x, y, z in positions]
    nodes.append(add_complete(chrome, normals, uvs, indices, 4, 'edge chrome sphere'))
    nodes.append(add_complete(*quad((-0.38, 0.58, 0.50), 0.40, 0.40),
                              5, 'edge normal detail panel'))
    glass = [(x * 0.30 + 0.52, y * 0.30 + 0.58, z * 0.30 + 0.22)
             for x, y, z in positions]
    nodes.append(add_complete(glass, normals, uvs, indices, 6, 'edge clear lens'))

    document = {
        'asset': {'version': '2.0',
                  'generator': 'preview-3d tests/render-quality/fixtures.py'},
        'extensionsUsed': ['KHR_materials_transmission', 'KHR_materials_ior'],
        'scene': 0, 'scenes': [{'nodes': [0]}],
        'nodes': [{'children': list(range(1, len(nodes) + 1))}],
        'meshes': meshes, 'materials': materials,
        'samplers': [{'magFilter': 9729, 'minFilter': 9987,
                      'wrapS': 33071, 'wrapT': 33071}],
        'images': [{'uri': text_uri}, {'uri': normal_uri}, {'uri': mr_uri}],
        'textures': [{'source': 0, 'sampler': 0}, {'source': 1, 'sampler': 0},
                     {'source': 2, 'sampler': 0}],
    }
    document['nodes'].extend(nodes)
    document['scenes'][0]['nodes'] = [0]
    document, binary = finish(document)
    for base, positions in position_bounds:
        accessor = document['accessors'][base]
        accessor['min'] = [min(point[axis] for point in positions) for axis in range(3)]
        accessor['max'] = [max(point[axis] for point in positions) for axis in range(3)]
    return _glb(document, binary, path)


FIXTURES = {
    'sphere-grid': (sphere_grid, 'sphere-grid.glb'),
    'glass-reference': (glass_reference, 'glass-reference.glb'),
    'material-probe': (material_probe, 'material-probe.glb'),
    'brdf-probe': (brdf_probe, 'brdf-probe.glb'),
    'directional-probe': (directional_probe, 'directional-probe.glb'),
    'material-semantics': (material_semantics, 'material-semantics.glb'),
    'textured-lobes': (textured_lobes, 'textured-lobes.glb'),
    'texture-reuse': (texture_reuse, 'texture-reuse.glb'),
    'stl-box': (stl_box, 'stl-box.stl'),
    'layered-glass': (layered_glass, 'layered-glass.glb'),
    'layered-glass-permuted': (
        lambda path: layered_glass(path, permuted=True), 'layered-glass-permuted.glb'),
    'transmission-glass': (transmission_glass, 'transmission-glass.glb'),
    'lighting-probe': (lighting_probe, 'lighting-probe.glb'),
    'clay-review': (clay_review, 'clay-review.glb'),
    'contact-shadow-probe': (contact_shadow_probe, 'contact-shadow-probe.glb'),
    'ao-probe': (ao_probe, 'ao-probe.glb'),
    'ao-probe-large': (lambda path: ao_probe(path, 8.0), 'ao-probe-large.glb'),
    'edge-probe': (edge_probe, 'edge-probe.glb'),
    'line-point-obj': (line_point_scene, 'line-point-scene.obj'),
    'textured-obj': (textured_obj, 'textured-obj.obj'),
    'step-assembly': (step_assembly, 'assembly_ap214.stp'),
}

# T23: unique decoded textures the viewer must report for a generated fixture.
# The reused color map is one sRGB chunk; the data map is one linear chunk.
EXPECTED_TEXTURE_COUNTS = {'texture-reuse': 2}

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
    # T24: isolated BRDF/environment/layering roles. `bucket`/`alphaMode` come
    # from the normalized material contract; the numeric lane additionally
    # reads the GPU diagnostic channels for each role.
    'brdf-probe': [
        {'name': 'brdf white dielectric', 'baseColor': [0.95, 0.95, 0.95, 1.0],
         'metallic': 0.0, 'roughness': 0.5, 'bucket': 0, 'alphaMode': 0},
        {'name': 'brdf black dielectric', 'baseColor': [0.08, 0.05, 0.18, 1.0],
         'metallic': 0.0, 'roughness': 0.3, 'bucket': 0, 'alphaMode': 0},
        {'name': 'brdf white metal', 'baseColor': [1.0, 1.0, 1.0, 1.0],
         'metallic': 1.0, 'roughness': 0.2, 'bucket': 0, 'alphaMode': 0},
        {'name': 'brdf rough metal', 'baseColor': [0.85, 0.65, 0.25, 1.0],
         'metallic': 1.0, 'roughness': 0.9, 'bucket': 0, 'alphaMode': 0},
        {'name': 'brdf ior 15', 'baseColor': [0.7, 0.2, 0.2, 1.0],
         'metallic': 0.0, 'roughness': 0.1, 'bucket': 0, 'alphaMode': 0, 'ior': 1.5},
        {'name': 'brdf ior 20', 'baseColor': [0.2, 0.7, 0.2, 1.0],
         'metallic': 0.0, 'roughness': 0.1, 'bucket': 0, 'alphaMode': 0, 'ior': 2.0},
        {'name': 'brdf specular zero', 'baseColor': [0.2, 0.3, 0.8, 1.0],
         'metallic': 0.0, 'roughness': 0.2, 'bucket': 0, 'alphaMode': 0,
         'specularFactor': 0.0},
        {'name': 'brdf specular tinted', 'baseColor': [0.8, 0.3, 0.2, 1.0],
         'metallic': 0.0, 'roughness': 0.2, 'bucket': 0, 'alphaMode': 0,
         'specularFactor': 1.0, 'specularColor': [1.0, 0.5, 0.25]},
        {'name': 'brdf coat zero', 'baseColor': [0.10, 0.30, 0.90, 1.0],
         'metallic': 0.0, 'roughness': 0.4, 'bucket': 0, 'alphaMode': 0,
         'clearcoat': 0.0},
        {'name': 'brdf coat one', 'baseColor': [0.12, 0.32, 0.92, 1.0],
         'metallic': 0.0, 'roughness': 0.4, 'bucket': 0, 'alphaMode': 0,
         'clearcoat': 1.0, 'clearcoatRoughness': 0.05},
        {'name': 'brdf coat half', 'baseColor': [0.10, 0.50, 0.90, 1.0],
         'metallic': 0.0, 'roughness': 0.4, 'bucket': 0, 'alphaMode': 0,
         'clearcoat': 0.5, 'clearcoatRoughness': 0.2},
        {'name': 'brdf coated metal', 'baseColor': [0.9, 0.75, 0.3, 1.0],
         'metallic': 1.0, 'roughness': 0.3, 'bucket': 0, 'alphaMode': 0,
         'clearcoat': 1.0, 'clearcoatRoughness': 0.1},
        {'name': 'brdf emission', 'baseColor': [0.6, 0.1, 0.6, 1.0],
         'metallic': 0.0, 'roughness': 0.6, 'bucket': 0, 'alphaMode': 0,
         'emissive': [0.9, 0.6, 0.2], 'emissiveStrength': 8.0},
        {'name': 'brdf coat emission', 'baseColor': [0.6, 0.2, 0.6, 1.0],
         'metallic': 0.0, 'roughness': 0.6, 'bucket': 0, 'alphaMode': 0,
         'emissive': [0.9, 0.6, 0.2], 'emissiveStrength': 8.0,
         'clearcoat': 1.0, 'clearcoatRoughness': 0.1},
        {'name': 'brdf transmissive', 'baseColor': [0.6, 0.8, 0.9, 1.0],
         'metallic': 0.0, 'roughness': 0.05, 'bucket': 3, 'alphaMode': 0,
         'transmission': 1.0, 'ior': 1.5},
        {'name': 'brdf coverage blend', 'baseColor': [0.55, 0.55, 0.55, 0.5],
         'metallic': 0.0, 'roughness': 0.5, 'bucket': 2, 'alphaMode': 2},
    ],
    # T30: one flat panel per role with a fixed world normal. The locator
    # reads the base-colour channel, so every role gets a distinct hue.
    'directional-probe': [
        {'name': 'directional lambert', 'baseColor': [0.82, 0.82, 0.82, 1.0],
         'metallic': 0.0, 'roughness': 0.9, 'bucket': 0, 'alphaMode': 0,
         'ior': 1.0, 'specularFactor': 0.0},
        {'name': 'directional coat', 'baseColor': [0.12, 0.32, 0.92, 1.0],
         'metallic': 0.0, 'roughness': 0.4, 'bucket': 0, 'alphaMode': 0,
         'clearcoat': 1.0, 'clearcoatRoughness': 0.22},
        {'name': 'directional black paint', 'baseColor': [0.16, 0.04, 0.08, 1.0],
         'metallic': 0.0, 'roughness': 0.25, 'bucket': 0, 'alphaMode': 0,
         'clearcoat': 1.0, 'clearcoatRoughness': 0.05},
        {'name': 'directional gold metal', 'baseColor': [1.0, 0.72, 0.28, 1.0],
         'metallic': 1.0, 'roughness': 0.3, 'bucket': 0, 'alphaMode': 0},
        {'name': 'directional glass', 'baseColor': [0.35, 0.75, 0.9, 1.0],
         'metallic': 0.0, 'roughness': 0.05, 'bucket': 3, 'alphaMode': 0,
         'transmission': 1.0, 'ior': 1.5},
        {'name': 'directional emission', 'baseColor': [0.55, 0.08, 0.48, 1.0],
         'metallic': 0.0, 'roughness': 0.6, 'bucket': 0, 'alphaMode': 0,
         'emissive': [0.9, 0.15, 0.65], 'emissiveStrength': 4.0},
        {'name': 'directional floor lambert', 'baseColor': [0.6, 0.65, 0.72, 1.0],
         'metallic': 0.0, 'roughness': 0.9, 'bucket': 0, 'alphaMode': 0,
         'ior': 1.0, 'specularFactor': 0.0},
    ],
    # T22: authored inputs and the renderer policy each one must produce.
    'material-semantics': [
        {'name': 'normal scale 0', 'baseColor': [0.55, 0.55, 0.55, 1.0],
         'metallic': 0.0, 'roughness': 0.55, 'bucket': 0, 'alphaMode': 0,
         'normalScale': 0.0, 'occlusionBound': False, 'transmission': 0.0},
        {'name': 'normal scale 045', 'baseColor': [0.55, 0.52, 0.58, 1.0],
         'metallic': 0.0, 'roughness': 0.55, 'bucket': 0, 'alphaMode': 0,
         'normalScale': 0.45, 'occlusionBound': False, 'transmission': 0.0},
        {'name': 'normal scale 1', 'baseColor': [0.58, 0.55, 0.52, 1.0],
         'metallic': 0.0, 'roughness': 0.55, 'bucket': 0, 'alphaMode': 0,
         'normalScale': 1.0, 'occlusionBound': False, 'transmission': 0.0},
        {'name': 'MR red is not AO', 'baseColor': [0.6, 0.6, 0.6, 1.0],
         'metallic': 1.0, 'roughness': 1.0, 'bucket': 0, 'alphaMode': 0,
         'occlusionBound': False, 'occlusionExpectation': 1.0},
        {'name': 'independent occlusion', 'baseColor': [0.62, 0.56, 0.52, 1.0],
         'metallic': 0.0, 'roughness': 0.8, 'bucket': 0, 'alphaMode': 0,
         'occlusionBound': True, 'occlusionExpectation': 0.2,
         'slotSampler': {4: {'addressU': 2, 'addressV': 1, 'nearest': True}}},
        {'name': 'uv1 unsupported', 'baseColor': [0.8, 0.3, 0.2, 1.0],
         'metallic': 0.0, 'roughness': 0.5, 'bucket': 0, 'alphaMode': 0,
         'baseColorBound': False},
        {'name': 'per slot transform', 'baseColor': [0.7, 0.7, 0.7, 1.0],
         'metallic': 0.0, 'roughness': 0.5, 'bucket': 0, 'alphaMode': 0,
         'normalScale': 1.0, 'baseColorBound': True,
         'baseUv': {'offset': [0.5, 0.5], 'scale': [3.0, 3.0], 'rotation': 0.0},
         'slotUv': {2: {'offset': [0.25, 0.1], 'scale': [2.0, 2.0],
                        'rotation': 0.4}},
         'slotSampler': {2: {'addressU': 1, 'addressV': 2, 'nearest': True}},
         # The textured probe cannot be located by its flat factor; this is
         # factor * linear(base-color texel) for the fixture's two colors.
         'locatorColors': [[0.55391, 0.45114, 0.36044],
                           [0.01485, 0.07157, 0.24607]]},
        {'name': 'alpha cutout', 'baseColor': [0.9, 0.1, 0.7, 0.35],
         'metallic': 0.0, 'roughness': 0.6, 'bucket': 1, 'alphaMode': 1,
         'alphaCutoff': 0.2},
        {'name': 'ior 1 transmission suppressed', 'baseColor': [0.72, 0.02, 0.02, 1.0],
         'metallic': 0.0, 'roughness': 0.4, 'bucket': 0, 'alphaMode': 0,
         'transmission': 0.0, 'ior': 1.0},
        {'name': 'ior 15 transmission', 'baseColor': [0.7, 0.9, 0.95, 1.0],
         'metallic': 0.0, 'roughness': 0.05, 'bucket': 3, 'alphaMode': 0,
         'transmission': 0.9, 'ior': 1.5},
        {'name': 'mirrored tangent handedness', 'baseColor': [0.35, 0.75, 0.45, 1.0],
         'metallic': 0.0, 'roughness': 0.5, 'bucket': 0, 'alphaMode': 0,
         'normalScale': 1.0, 'occlusionBound': False, 'transmission': 0.0},
        {'name': 'double sided backface', 'baseColor': [0.45, 0.65, 0.85, 1.0],
         'metallic': 0.0, 'roughness': 0.6, 'bucket': 0, 'alphaMode': 0,
         'normalScale': 0.0, 'occlusionBound': False, 'transmission': 0.0,
         'backFace': True},
    ],
    # T28 Studio exposure/highlight calibration roles. Base colors are distinct
    # enough for the base-color locator to isolate every probe.
    'lighting-probe': [
        {'name': 'light white plastic', 'baseColor': [0.94, 0.92, 0.90, 1.0],
         'metallic': 0.0, 'roughness': 0.35, 'bucket': 0, 'alphaMode': 0},
        {'name': 'light neutral gray', 'baseColor': [0.16, 0.16, 0.16, 1.0],
         'metallic': 0.0, 'roughness': 0.50, 'bucket': 0, 'alphaMode': 0},
        {'name': 'light black leather', 'baseColor': [0.100, 0.060, 0.045, 1.0],
         'metallic': 0.0, 'roughness': 0.55, 'bucket': 0, 'alphaMode': 0},
        {'name': 'light red paint', 'baseColor': [0.55, 0.02, 0.015, 1.0],
         'metallic': 0.0, 'roughness': 0.30, 'bucket': 0, 'alphaMode': 0,
         'clearcoat': 1.0, 'clearcoatRoughness': 0.04},
        {'name': 'light brass', 'baseColor': [0.88, 0.64, 0.28, 1.0],
         'metallic': 1.0, 'roughness': 0.25, 'bucket': 0, 'alphaMode': 0},
        {'name': 'light chrome', 'baseColor': [0.92, 0.93, 0.95, 1.0],
         'metallic': 1.0, 'roughness': 0.14, 'bucket': 0, 'alphaMode': 0},
        {'name': 'light emission', 'baseColor': [0.10, 0.08, 0.06, 1.0],
         'metallic': 0.0, 'roughness': 0.50, 'bucket': 0, 'alphaMode': 0,
         'emissive': [1.0, 0.55, 0.18], 'emissiveStrength': 4.0},
        {'name': 'light black enamel', 'baseColor': [0.045, 0.060, 0.110, 1.0],
         'metallic': 0.0, 'roughness': 0.06, 'bucket': 0, 'alphaMode': 0,
         'clearcoat': 1.0, 'clearcoatRoughness': 0.03},
        {'name': 'light white cad', 'baseColor': [0.76, 0.78, 0.80, 1.0],
         'metallic': 0.0, 'roughness': 0.65, 'bucket': 0, 'alphaMode': 0},
        {'name': 'light reference white', 'baseColor': [0.86, 0.86, 0.86, 1.0],
         'metallic': 0.0, 'roughness': 0.85, 'bucket': 0, 'alphaMode': 0},
        {'name': 'light reference gray', 'baseColor': [0.36, 0.36, 0.36, 1.0],
         'metallic': 0.0, 'roughness': 0.85, 'bucket': 0, 'alphaMode': 0},
        {'name': 'light reference black', 'baseColor': [0.140, 0.140, 0.140, 1.0],
         'metallic': 0.0, 'roughness': 0.85, 'bucket': 0, 'alphaMode': 0},
    ],
}

# T25 textured extension-lobe probes. `channels` lists the effective-input
# diagnostic channels that must vary across the quad's U axis; `constant`
# records the values a factor-only or unsupported-slot probe must keep. Slot
# numbers are the fixed material slots from MaterialPayload.h.
# T26 layered transparency roles. `baseColor` is the authored factor used by
# the material-record locator; `alphaMode` is 0 OPAQUE, 1 MASK, 2 BLEND.
EXPECTED_LAYERED_GLASS = [
    {'name': 'regression shared', 'baseColor': [1.0, 1.0, 1.0, 0.55],
     'alphaMode': 2, 'role': 'primitive-order regression'},
    {'name': 'intersection shared', 'baseColor': [1.0, 1.0, 1.0, 0.5],
     'alphaMode': 2, 'role': 'intersecting triangles in one primitive'},
    {'name': 'nested amber', 'baseColor': [0.9, 0.25, 0.12, 0.4],
     'alphaMode': 2, 'role': 'nested front/back glass'},
    {'name': 'nested green', 'baseColor': [0.12, 0.72, 0.25, 0.4],
     'alphaMode': 2, 'role': 'nested front/back glass'},
    {'name': 'nested blue', 'baseColor': [0.12, 0.3, 0.92, 0.4],
     'alphaMode': 2, 'role': 'nested front/back glass'},
    {'name': 'nested yellow', 'baseColor': [0.92, 0.8, 0.12, 0.4],
     'alphaMode': 2, 'role': 'nested front/back glass'},
    {'name': 'paint opaque', 'baseColor': [0.22, 0.23, 0.25, 1.0],
     'alphaMode': 0, 'role': 'opaque paint'},
    {'name': 'alpha decal', 'baseColor': [0.85, 0.1, 0.6, 0.3],
     'alphaMode': 2, 'role': 'low-coverage decal over paint'},
    {'name': 'alpha cutout', 'baseColor': [1.0, 1.0, 1.0, 1.0],
     'alphaMode': 1, 'role': 'alpha cutout over paint'},
    {'name': 'emissive blend', 'baseColor': [0.12, 0.1, 0.08, 0.35],
     'alphaMode': 2, 'role': 'emissive blended geometry'},
    {'name': 'hidden glass', 'baseColor': [0.2, 0.4, 0.95, 0.5],
     'alphaMode': 2, 'role': 'transparent behind opaque'},
]

# T27 transmission roles. `baseColor` is the authored factor used by the
# material-record locator; `alphaMode` is 0 OPAQUE, 1 MASK, 2 BLEND.
EXPECTED_TRANSMISSION_GLASS = [
    {'name': 'glass neutral', 'baseColor': [0.9, 0.95, 1.0, 1.0],
     'alphaMode': 0, 'transmission': 1.0, 'ior': 1.5, 'role': 'neutral glass'},
    {'name': 'glass tinted orange', 'baseColor': [0.95, 0.35, 0.08, 1.0],
     'alphaMode': 0, 'transmission': 1.0, 'ior': 1.5, 'role': 'tinted glass'},
    {'name': 'glass dark', 'baseColor': [0.02, 0.04, 0.12, 1.0],
     'alphaMode': 0, 'transmission': 1.0, 'ior': 1.5, 'role': 'dark tinted glass'},
    {'name': 'glass rough', 'baseColor': [0.45, 0.62, 0.85, 1.0],
     'alphaMode': 0, 'transmission': 1.0, 'ior': 1.5, 'role': 'rough glass'},
    {'name': 'blend quarter transmission', 'baseColor': [1.0, 0.62, 0.34, 0.4],
     'alphaMode': 2, 'transmission': 0.25, 'ior': 1.5,
     'role': 'coverage x transmission sweep'},
    {'name': 'blend full transmission', 'baseColor': [0.7, 0.35, 1.0, 0.4],
     'alphaMode': 2, 'transmission': 1.0, 'ior': 1.5,
     'role': 'coverage x transmission sweep'},
    {'name': 'ior one no interface', 'baseColor': [0.2, 0.8, 0.4, 1.0],
     'alphaMode': 0, 'transmission': 0.0, 'ior': 1.0,
     'role': 'IOR 1 suppression'},
    {'name': 'mask cutout transmission', 'baseColor': [0.9, 0.9, 0.2, 1.0],
     'alphaMode': 1, 'transmission': 0.0, 'ior': 1.5,
     'role': 'MASK cutout wins over transmission'},
    {'name': 'overlap front', 'baseColor': [0.2, 0.8, 1.0, 1.0],
     'alphaMode': 0, 'transmission': 1.0, 'ior': 1.5, 'role': 'overlapping layers'},
    {'name': 'overlap back', 'baseColor': [1.0, 0.5, 0.7, 1.0],
     'alphaMode': 0, 'transmission': 1.0, 'ior': 1.5, 'role': 'overlapping layers'},
    {'name': 'interior glass', 'baseColor': [0.85, 0.95, 1.0, 1.0],
     'alphaMode': 0, 'transmission': 1.0, 'ior': 1.5, 'role': 'interior geometry'},
    {'name': 'double sided thin glass', 'baseColor': [0.55, 0.85, 0.55, 1.0],
     'alphaMode': 0, 'transmission': 1.0, 'ior': 1.5, 'doubleSided': True,
     'role': 'double-sided thin surface'},
]

EXPECTED_TEXTURED_LOBES = [
    {'name': 'no extension baseline', 'baseColor': [0.72, 0.72, 0.72, 1.0],
     'bucket': 0, 'alphaMode': 0, 'extensionSlots': [],
     'constant': {12: [0.0, 0.0, 0.0], 24: [0.0, 0.045, 0.0],
                  25: [1.0, 1.0, 1.0], 26: [0.04, 0.04, 0.04],
                  27: [0.0, 0.0, 0.0], 28: [0.0, 0.0, 0.0],
                  30: [0.0, 0.0, 0.0]}},
    {'name': 'clearcoat factor map', 'baseColor': [0.85, 0.25, 0.25, 1.0],
     'bucket': 0, 'alphaMode': 0,
     'extensionSlots': [{'slot': 5, 'bound': True, 'uvSet': 0,
                         'colorSpace': 0}],
     'channels': [12], 'ranges': {12: [0.1, 0.91]}},
    {'name': 'clearcoat roughness map', 'baseColor': [0.25, 0.85, 0.25, 1.0],
     'bucket': 0, 'alphaMode': 0,
     'extensionSlots': [{'slot': 6, 'bound': True, 'uvSet': 0,
                         'colorSpace': 0}],
     'channels': [24], 'ranges': {24: [0.06, 0.81]}},
    {'name': 'clearcoat normal map', 'baseColor': [0.25, 0.35, 0.9, 1.0],
     'bucket': 0, 'alphaMode': 0,
     'extensionSlots': [{'slot': 7, 'bound': True, 'uvSet': 0,
                         'colorSpace': 0}],
     'channels': [29], 'ranges': {29: [0.3, 1.0]},
     'clearcoatNormalScale': 1.0},
    {'name': 'specular maps', 'baseColor': [0.9, 0.8, 0.2, 1.0],
     'bucket': 0, 'alphaMode': 0,
     'extensionSlots': [{'slot': 8, 'bound': True, 'uvSet': 0,
                         'colorSpace': 0},
                        {'slot': 9, 'bound': True, 'uvSet': 0,
                         'colorSpace': 1}],
     'sharedImageSlots': [8, 9],
     'channels': [25, 26], 'ranges': {25: [0.09, 0.91], 26: [0.002, 0.02]},
     'deltas': {26: 0.004}},
    {'name': 'sheen maps', 'baseColor': [0.8, 0.3, 0.85, 1.0],
     'bucket': 0, 'alphaMode': 0,
     'extensionSlots': [{'slot': 10, 'bound': True, 'uvSet': 0,
                         'colorSpace': 1},
                        {'slot': 11, 'bound': True, 'uvSet': 0,
                         'colorSpace': 0}],
     'channels': [27, 30], 'ranges': {27: [0.01, 0.71], 30: [0.01, 0.71]}},
    {'name': 'transmission map', 'baseColor': [0.2, 0.75, 0.85, 1.0],
     'bucket': 3, 'alphaMode': 0,
     'extensionSlots': [{'slot': 12, 'bound': True, 'uvSet': 0,
                         'colorSpace': 0}],
     'channels': [28], 'ranges': {28: [0.0, 0.81]}},
    {'name': 'uv1 clearcoat map', 'baseColor': [0.95, 0.55, 0.3, 1.0],
     'bucket': 0, 'alphaMode': 0,
     'extensionSlots': [{'slot': 5, 'bound': False, 'uvSet': 1,
                         'colorSpace': 0}],
     'constant': {12: [0.6, 0.6, 0.6]}},
    {'name': 'transformed clearcoat map', 'baseColor': [0.55, 0.95, 0.45, 1.0],
     'bucket': 0, 'alphaMode': 0,
     'extensionSlots': [{'slot': 5, 'bound': True, 'uvSet': 0,
                         'colorSpace': 0, 'uvOffset': [0.5, 0.0],
                         'uvScale': [2.0, 1.0]}],
     'channels': [12], 'ranges': {12: [0.05, 0.81]}},
]

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
                    'brdf-probe': [item['name'] for item in EXPECTED_MATERIALS['brdf-probe']],
                    'directional-probe': [item['name'] for item in EXPECTED_MATERIALS['directional-probe']],
                    'material-semantics': [item['name'] for item in EXPECTED_MATERIALS['material-semantics']],
                    'textured-lobes': [item['name'] for item in EXPECTED_TEXTURED_LOBES],
                    'texture-reuse': ['one reused sRGB color map', 'one linear data map'],
                    'stl-box': ['hard-edged STEP/STL-like surface', 'ground contact'],
                    'layered-glass': [item['name'] for item in EXPECTED_LAYERED_GLASS],
                    'layered-glass-permuted': [item['name'] for item in EXPECTED_LAYERED_GLASS],
                    'transmission-glass': [item['name'] for item in EXPECTED_TRANSMISSION_GLASS],
                    'lighting-probe': [item['name'] for item in EXPECTED_MATERIALS['lighting-probe']],
                    'clay-review': ['identity row', 'mask cutout', 'glass over interior',
                                    'overlapping alpha sheets', 'opaque transmissive',
                                    'small curved parts', 'mirrored instance',
                                    'mixed line/point primitives'],
                    'contact-shadow-probe': ['suspended ring with a shadow hole',
                                             'ground-contact tires', 'suspended sphere',
                                             'opaque panel', 'clear transmission panel',
                                             'MASK cutout sheet', 'mirrored part'],
                    'ao-probe': ['inside corner cavity', 'contact sphere', 'narrow plate gap',
                                 'thin ring hole', 'recessed emission', 'transmissive window',
                                 'MASK cutout sheet', 'mirrored instance',
                                 'scene-scale duplicate'],
                    'ao-probe-large': ['inside corner cavity', 'contact sphere',
                                       'narrow plate gap', 'thin ring hole',
                                       'recessed emission', 'transmissive window',
                                       'MASK cutout sheet', 'mirrored instance',
                                       'scene-scale duplicate'],
                    'line-point-obj': ['polyline', 'point row'],
                    'textured-obj': ['checker texture', 'brokered MTL and PNG sidecars'],
                    'step-assembly': ['STEP assembly body', 'part separation'],
                    'edge-probe': ['sub-pixel wires', 'high-frequency grille',
                                   'engraved digit strip', 'chrome sphere',
                                   'normal-map detail', 'clear lens'],
                }[name],
            },
        }
        if name in EXPECTED_MATERIALS:
            generated[name]['expectedMaterials'] = _sanitize(EXPECTED_MATERIALS[name])
        if name == 'textured-lobes':
            generated[name]['expectedLobes'] = _sanitize(EXPECTED_TEXTURED_LOBES)
        if name in ('layered-glass', 'layered-glass-permuted'):
            generated[name]['expectedLayeredGlass'] = _sanitize(EXPECTED_LAYERED_GLASS)
        if name == 'transmission-glass':
            generated[name]['expectedTransmissionGlass'] = _sanitize(EXPECTED_TRANSMISSION_GLASS)
    write_json(output / 'fixtures.json', {'schema': 1, 'fixtures': generated})
    return generated
