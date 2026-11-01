# Topology fixtures (T17)

Small, repository-owned fixtures for line/strip/fan/point normalization. Each
glTF embeds one 4-vertex quad (positions then `uint16` indices) as a base64
data URI, so there are no sidecar files and no third-party content.

| File | Primitive mode | Expected normalized result |
| --- | --- | --- |
| `triangle_strip.gltf` | 5 (TRIANGLE_STRIP) | `TriangleList`, 6 indices |
| `triangle_fan.gltf` | 6 (TRIANGLE_FAN) | `TriangleList`, 6 indices |
| `line_strip.gltf` | 3 (LINE_STRIP) | `LineList`, 6 indices (3 segments) |
| `line_loop.gltf` | 2 (LINE_LOOP) | `LineList`, 8 indices (4 segments) |
| `lines.gltf` | 1 (LINES) | `LineList`, 4 indices (2 segments) |
| `line_only.obj` | `l 1 2 3 4` | `LineList`, 6 indices (3 segments) |
| `point_only.obj` | `p 1 2 3` | `PointList`, 3 vertices, 0 indices |

`tests/import-isolation/TopologyTests.cpp` opens these through the real
sandboxed import session. Malformed variants are generated in-memory by the
test rather than committed.
