# ADR-0065 — Bounded Assimp compatibility adapter

**Status:** Accepted (amends ADR-006 narrowly).

## Context

ADR-006 rejected Assimp as a *universal* runtime importer: the native glTF,
OBJ, FBX, STL, PLY, USD and STEP paths already have better validation, limits,
fidelity and memory behavior. However, `.dae`, `.3ds`, `.dxf`, `.x`, `.off`,
`.amf` and `.x3d` have no product adapter, and writing seven parsers plus their
fuzz lanes would be a disproportionate cost for compatibility coverage. Assimp
is a large historical CVE surface, so it may only run behind the existing
worker sandbox and a custom IOSystem.

## Decision

Add one worker-only Assimp compatibility adapter:

- Assimp is linked only into `Preview3DImportWorker.exe` (never the viewer,
  thumbnail provider, OpenUSD host or CAD host).
- The adapter is selected only for the seven compatibility extensions. Native
  formats never route through it, and it never replaces a native adapter.
- `Assimp::IOSystem` is implemented over the broker sidecar protocol. The
  default filesystem IOSystem, path opens, plug-in loading, file writes and
  network access are never used. Absolute, `..`, UNC/device, ADS, URL,
  drive-relative and unknown-extension references are rejected by the adapter
  and again by the broker.
- The importer uses a closed post-process set (triangulate, join identical
  vertices, generate missing normals, validate, sort by primitive type), no
  animation evaluation, no geometry caches, no file writes and no plug-ins.
- Tier-B object/material/triangle/vertex caps and the existing scratch,
  texture, sidecar, chunk and generation budgets apply. Unsupported primitive
  types are skipped with a compatibility warning; a scene with no triangles
  fails typed rather than fabricating geometry.
- A single `AssimpFuzz` dispatcher target covers all seven container hints.
- Formats stay unregistered until their fixture and test land; DXF is
  documented as Assimp's limited 3DFACE/polyline subset.

ADR-006's "Assimp as a universal runtime importer" rejection remains in force:
this is a compatibility-only adapter, not a fallback for a native format.

## Consequences

The viewer gains seven low-frequency formats at the cost of one large static
dependency and a new attack surface that must be fuzzed and kept inside the
worker. Assimp's scene objects never cross the adapter boundary; only normalized
wire chunks do. A future native adapter for any of these formats can replace
the compatibility route without changing the wire contract.
