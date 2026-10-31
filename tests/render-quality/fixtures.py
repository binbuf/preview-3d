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


FIXTURES = {
    'sphere-grid': (sphere_grid, 'sphere-grid.glb'),
    'glass-reference': (glass_reference, 'glass-reference.glb'),
    'material-probe': (material_probe, 'material-probe.glb'),
    'brdf-probe': (brdf_probe, 'brdf-probe.glb'),
    'material-semantics': (material_semantics, 'material-semantics.glb'),
    'textured-lobes': (textured_lobes, 'textured-lobes.glb'),
    'texture-reuse': (texture_reuse, 'texture-reuse.glb'),
    'stl-box': (stl_box, 'stl-box.stl'),
    'layered-glass': (layered_glass, 'layered-glass.glb'),
    'layered-glass-permuted': (
        lambda path: layered_glass(path, permuted=True), 'layered-glass-permuted.glb'),
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
                    'material-semantics': [item['name'] for item in EXPECTED_MATERIALS['material-semantics']],
                    'textured-lobes': [item['name'] for item in EXPECTED_TEXTURED_LOBES],
                    'texture-reuse': ['one reused sRGB color map', 'one linear data map'],
                    'stl-box': ['hard-edged STEP/STL-like surface', 'ground contact'],
                    'layered-glass': [item['name'] for item in EXPECTED_LAYERED_GLASS],
                    'layered-glass-permuted': [item['name'] for item in EXPECTED_LAYERED_GLASS],
                }[name],
            },
        }
        if name in EXPECTED_MATERIALS:
            generated[name]['expectedMaterials'] = _sanitize(EXPECTED_MATERIALS[name])
        if name == 'textured-lobes':
            generated[name]['expectedLobes'] = _sanitize(EXPECTED_TEXTURED_LOBES)
        if name in ('layered-glass', 'layered-glass-permuted'):
            generated[name]['expectedLayeredGlass'] = _sanitize(EXPECTED_LAYERED_GLASS)
    write_json(output / 'fixtures.json', {'schema': 1, 'fixtures': generated})
    return generated
