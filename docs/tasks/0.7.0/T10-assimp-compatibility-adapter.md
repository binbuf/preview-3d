---
timeoutMin: 300
verify: x64\Release\Tests.ImportIsolation.exe "[assimp]"
---
# T10 — Bounded Assimp compatibility adapter

## Goal
`.dae`, `.3ds`, `.dxf`, `.x`, `.off`, `.amf` and `.x3d` open through a new
worker-only Assimp adapter, with a broker-backed `IOSystem`, hard budgets,
cancellation and a fuzz lane. Assimp is a compatibility importer for formats
with no native adapter, not a replacement for the existing glTF/OBJ/FBX/STL/PLY/
USD/STEP paths.

## Context (read first)
- `docs/proposal.md` §3.4 — the Assimp tradeoff and scope.
- `docs/design/11-decisions-and-risks.md` ADR-006 — the current “no universal
  Assimp” decision; amend it narrowly.
- `import-worker/src/ObjAdapter.cpp` — the ufbx broker-callback pattern
  (`open_file_cb`, sidecar client, allocation/progress limits).
- `import-worker/src/UfbxMaterialConversion.{h,cpp}` — material normalization
  pattern to mirror.
- `shared/import-broker/include/import_broker/ImportSession.h` and
  `SidecarFileClient` — how a worker requests a sidecar by relative reference.
- `vcpkg.json` — the worker dependency manifest; Assimp is added here.
- F3D reference: `plugins/assimp/CMakeLists.txt:42-118` and
  `vtkF3DAssimpImporter.cxx` for format declarations and content hints, and
  `doc/user/14-LIMITATIONS_AND_TROUBLESHOOTING.md` for known quality limits.

## Scope
- [ ] Add `assimp` to the root `vcpkg.json` with exact pinning and a minimal
      feature set. Confirm the static Release closure and license/SBOM entries.
- [ ] Add `import-worker/src/AssimpAdapter.{h,cpp}` and a worker dispatch route
      for `Dae`, `ThreeDs`, `Dxf`, `DirectX`, `Off`, `Amf`, `X3d`.
- [ ] Implement `Assimp::IOSystem` over the existing broker sidecar client:
      - no `CreateFile`/path opens;
      - deny absolute, `..`, UNC/device, ADS, URL and unknown extensions;
      - embedded textures are read from memory within the existing texture
        budgets;
      - external textures use the same allowlist/size checks as FBX/OBJ.
- [ ] Configure the importer conservatively:
      - `aiProcess_Triangulate`, `JoinIdenticalVertices` only if it preserves
        material/UV seams, `GenSmoothNormals` only when missing, no animation,
        no geometry caches, no file writes, no plug-in loading;
      - explicit node/mesh/material/texture/vertex/triangle caps before/while
        converting;
      - progress callback and cancellation between bounded units.
- [ ] Normalize Assimp meshes/materials into `BoundedChunkWriter` and
      `MaterialPayload`. Reuse `UfbxMaterialConversion` concepts or factor a
      shared product-owned material mapper; do not expose `aiMaterial*` to the
      viewer.
- [ ] Add catalog entries and T03-driven installer/UI/docs entries for the new
      extensions. Do not claim them until fixtures and tests pass.
- [ ] Add fixtures (small, licensed/generated):
      `duck.dae` or equivalent, `iflamigm.3ds`/small 3DS, `teapot.off` or
      equivalent, `cube-with-hole.amf`/small AMF, a simple DXF, a simple `.x`,
      and an X3D. Include malformed/truncated/over-budget cases and one with a
      missing texture.
- [ ] Add a fuzz target/corpus for each new container or a single dispatcher
      target with per-format seeds; wire it into the existing fuzz lane.
- [ ] Add `[assimp]` cases asserting open/degraded/fail outcomes, chunk counts,
      material/texture counts and no path escape. Include a sidecar-request cap
      case.
- [ ] Update `docs/design/01-product-scope.md`, `docs/design/03-file-formats-and-ingestion.md`,
      ADR-006, `docs/FORMAT-SUPPORT.md`, `THIRD-PARTY-LICENSES.md` and the
      README.

## Out of scope
- Using Assimp for glTF, OBJ, FBX, STL, PLY, USD or STEP. Those keep their
  native adapters.
- Native-adapter fallback routing (FBX/3MF) on `UnsupportedRequiredFeature`;
  file a follow-up task after this adapter is proven.
- Animation playback, camera/light import, or file export.
- DXF full support: Assimp’s DXF is limited to polylines/3D faces; document it.

## Design notes
- Assimp is a large historical CVE surface. The AppContainer/Job Object is
  necessary but not sufficient: fuzz every new container, keep the broker
  sidecar limit, and never expose an Assimp object outside the adapter.
- Do not allow Assimp to load plug-ins or use its default `IOSystem`. The
  custom IOSystem is the path boundary.
- Assimp can produce degenerate/duplicate geometry. Accept it only after
  finite/count/range validation and the existing normalized-scene invariants.
- If a format cannot be represented in the normalized contract (e.g. a weird
  line-only primitive before T17), fail typed rather than fabricate triangles.
- Keep the adapter deterministic; sort/deduplicate materials by a stable key.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe "[assimp]"` passes.
- [ ] At least DAE, 3DS, OFF and AMF positive fixtures open; malformed and
  over-budget fixtures fail typed.
- [ ] No path/network/file-write/child-process access is possible through the
  adapter; the sidecar cap test passes.
- [ ] The new dependency is pinned, in the SBOM/license file, and the Release
  closure remains free of unapproved DLLs.
- [ ] Fuzz target(s) run clean on the seed corpus.
- [ ] Installer/open dialog/docs list the new extensions; no claim is made for
  formats without fixtures.
- [ ] Hand-off lists supported/limited formats, fixture licenses, fuzz target
  names and the ADR amendment.

## Hand-off
_(filled in by the implementing session: Assimp version/features, IOSystem
policy, format matrix, fixture licenses, fuzz commands and packaging size.)_
