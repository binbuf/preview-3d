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

**Status:** implemented and verified.

### Assimp version / features

- `assimp:x64-windows-static-md@6.0.4#3` from the pinned vcpkg baseline
  `04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4`; default importer set only (no
  tools, tests, plug-in loading or file writes). Added to root `vcpkg.json`.
- Transitive closure added to the license/SBOM mapping: `jhasse-poly2tri`,
  `kubazip`, `minizip`, `polyclipping`, `pugixml`, `rapidjson`, `stb`,
  `utfcpp`. `pugixml` is also a direct root dependency for XML admission.
- Overlay port `packaging/vcpkg-ports/assimp/` carries one product finding
  fix: `fix-x3d-empty-node-list.patch` (Assimp 6.0.4 called `front()` on an
  empty node list when its X3D tokenizer rejected a document pugixml accepted).
- Static Release closure: no upstream DLL is deployed or imported by the
  worker (dumpbin shows only system/CRT imports). Worker binary is ~13.2 MiB.

### IOSystem policy

`BrokerIOSystem` is the only path boundary. `Exists` accepts only a relative
image reference and never calls the broker; `Open` accepts only `rb`, rejects
absolute/rooted, UNC/device, ADS/URL/drive-relative, `..`, control bytes and
non-image extensions, then requests the sidecar through the broker once.
Successful and failed requests are cached per reference, so Assimp's own opens
and material texture resolution share one sidecar request. Embedded textures
are read from scene memory under the encoded/decoded texture budgets. External
textures use the png/jpg/jpeg/bmp/tif/tiff/webp/ktx2 allowlist and the same
size/containment checks as OBJ/FBX.

### Format matrix

| Format | Status | Notes |
| --- | --- | --- |
| DAE (COLLADA) | supported | static meshes, transforms, normals/UVs/colors, materials, approved/embedded textures |
| 3DS | supported | static triangle meshes, material groups/colors, approved textures |
| OFF | supported | ASCII triangle/polygon meshes, full product token preflight |
| AMF | supported | static mesh volumes/materials; no slicing/print semantics |
| X3D | supported, limited | static IndexedFaceSet geometry; no scripting/sensors/animation |
| DirectX `.x` | supported, limited | static text mesh geometry/materials where Assimp supports it |
| DXF | supported, limited | Assimp's 3DFACE/polyline subset only, not full DXF |

All seven extensions are in the catalog, Open dialog/Open As, single-instance
acceptance, `--list-formats`, the NSIS install/uninstall registration and the
README/FORMAT-SUPPORT docs. No thumbnail family is claimed.

### Fixture licenses

`tests/fixtures/assimp-spike/` is generated by `generate_fixtures.py` from
repository-owned primitives (unit box, triangle grid, hand-written tetrahedron).
No third-party model, texture or exporter output is copied; no third-party
license applies. Included: DAE/3DS/OFF/AMF/X3D/X/DXF positives,
`missing-texture.dae`, `traversal-texture.dae`, truncated DAE/3DS, malformed
OFF/AMF, and `over-budget.off` (300 triangles; fails under the tiny-cap seam).

### Fuzz targets

- `AssimpFuzz` (`tests/fuzz/AssimpFuzz.vcxproj`): one dispatcher target; the
  first input byte selects one of the seven closed format hints.
- Seed preparation: `tests/fuzz/prepare_assimp_seeds.py`; checked-in seeds in
  `tests/fuzz/corpus/assimp/`; CI `fuzz-smoke` matrix entry `Assimp`.
- 30 s smoke run executed 85,671 units with no crash. Findings fixed before
  landing: the X3D empty-node-list crash and a malformed `coordIndex`
  double-free (both fixed by the overlay patch where applicable and by the X3D
  index/value admission), plus a DAE malformed-attribute/OFF count-graph
  unbounded-allocation/hang class (product admission).

### ADR amendment

ADR-006 is narrowly amended by
[`docs/design/adr/0065-bounded-assimp-compatibility-adapter.md`](../../design/adr/0065-bounded-assimp-compatibility-adapter.md):
Assimp is a compatibility-only worker importer for formats with no native
adapter, never a fallback for glTF/OBJ/FBX/STL/PLY/USD/STEP.

### Checks

```powershell
x64\Release\Tests.ImportIsolation.exe "[assimp]"          # 7 cases / 85 assertions, pass
x64\Release\Tests.ImportIsolation.exe "[obj],[fbx],[3mf],[format-catalog]"
                                                          # 43 cases / 48,779 assertions + 1 skip, pass
x64\Release\Tests.Unit.exe "[format-catalog],[format-installer],[presentation],[release-metadata]"
                                                          # 17 cases / 984 assertions, pass
.\tests\fuzz\Invoke-FuzzSmoke.ps1 -Target Assimp ...      # 85,671 units, exit 0
```

Changed docs: `docs/design/01-product-scope.md`,
`docs/design/03-file-formats-and-ingestion.md`,
`docs/design/11-decisions-and-risks.md`, new ADR-0065,
`docs/FORMAT-SUPPORT.md`, `THIRD-PARTY-LICENSES.md`, `README.md`.

