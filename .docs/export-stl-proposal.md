# Export as STL post-MVP work plan

Status: **proposal — not approved; export is currently an explicit exclusion**
Prepared: 2026-09-19
Design authority: [design/README.md](design/README.md)
Affected scope: [design/01-product-scope.md](design/01-product-scope.md) (FR/§Explicit exclusions),
[design/03-file-formats-and-ingestion.md](design/03-file-formats-and-ingestion.md) (normalized scene),
[design/07-user-experience.md](design/07-user-experience.md) (commands)

## Decision

Add a user-initiated **Export as STL** command that writes a **binary STL**
triangle mesh from a model already opened in the viewer, including from
non-STL sources. The export is deliberately **geometry-only and lossy**: it
reproduces the previewed fine-detail triangles in a chosen unit and orientation,
and does not attempt to carry materials, colors, textures, hierarchy, or
animation.

This is a scope change. `01-product-scope.md` currently lists
"export/conversion" among explicit exclusions, and `README.md` states that
models "are never modified." Approving this proposal requires updating those
statements, adding a new functional requirement, and accepting a narrow,
explicitly lossy output contract. Until that approval, this document is a
design study only.

STL is chosen as the first — and initially only — export target because binary
STL is the format whose correct output is least ambiguous to produce: an
80-byte header, a `uint32` facet count, and a fixed 50-byte record per facet.
There is no material graph, texture reference, compression scheme, or
vendor-extension surface to get subtly wrong.

A GLB export (which could preserve the PBR/material/texture subset) is
**deferred**, not rejected; see [Deferred: GLB export](#deferred-glb-export).

## What "faithful" means here

For this feature, **faithful means geometric**, and must be defined precisely
so the UI and documentation do not over-promise:

- the exported triangle set matches the viewer's **fine-detail** normalized
  geometry, with node/instance world transforms applied and cluster origins
  restored;
- winding and facet normals are correct so the printed/rendered result is not
  inside-out;
- the exported model has a **defined unit and up-axis** so it opens at the
  intended size and orientation.

It explicitly does **not** mean appearance fidelity. STL cannot express the
material, texture, color, or hierarchy content that the normalized scene does
carry, and the UI must say so at export time rather than implying a round-trip.

## Why STL is the safe first target

| Property | Consequence |
| --- | --- |
| Fixed binary layout, no compression | Serializer is small and independently verifiable; "is this a well-formed STL" is easy to assert |
| No material/texture references | Nothing to remap or approximate; no false fidelity claim |
| No hierarchy or instancing | Instances are expanded to triangles by definition; no ambiguity about what a "node" means in STL |
| Geometry-only semantics match the normalizer | The one thing the normalized scene guarantees is verified triangles |
| Universal consumer support (slicers, DCCs) | High user value for the 3D-printing/preview audience already targeted |

## Current-state reuse

The following already exists and is reused rather than rebuilt:

- **Node-hierarchy world transforms are computed and validated host-side.**
  `shared/import-broker/src/SharedSectionValidator.cpp:769-776` resolves the
  parent chain and multiplies the affines; `D3D12ImportBridge.cpp:544-548`
  stamps the result onto `ImportedInstance::worldTransform`, with
  `resolvedVisible` and `mirrored` derived alongside. `D3D12ViewerPath.cpp:1630-1653`
  holds an equivalent fallback resolver. No new hierarchy math is required.
- **Instance identity, visibility, and mirroring are already carried**
  (`ImportedInstance`).
- **A non-batched accumulation path exists.** `RunImport` without `onBatch`
  returns the session's accumulated `ValidatedChunk` list and unpacks it into a
  full `ImportResult` (`D3D12ImportBridge.cpp:601-605`).
- **Facet normalization precedent.** The import-side `StlAdapter` already has
  bounded degenerate/non-finite drop and flat-normal fallback logic that the
  exporter's normal generation SHOULD mirror in behavior, even if the code is
  not shared.
- **The wire format and validator are unchanged.** Export adds no new chunk
  topology, no new protocol version, and no new worker behavior.

## What is *not* reusable: the rendered scene is not the export source

The exporter MUST NOT treat GPU residency as its source of truth:

- Transforms, cluster origin, and up-axis correction are applied on the GPU per
  frame; the renderer never materializes a transformed CPU triangle stream
  (`RenderThread.cpp:887`; `Model.h:99-103`).
- Instances are re-drawn from one geometry buffer with per-draw transforms
  (`D3D12ViewerPath.cpp:1798-1809`), not expanded.
- The progressive path leaves `ModelData::vertices` empty (`Model.h:75-79`), and
  `ImportedMesh::payload` is transient during upload.
- Fine detail is streamed and may be rejected under GPU pressure
  (`RenderThread.cpp:339-345`), so "what is resident" can be a coarse proxy or a
  partial set.

Export is therefore a distinct **gather → transform → assemble → serialize**
path, not a read-back.

## Data source

| Option | Description | Verdict |
| --- | --- | --- |
| A. Non-batched import accumulation | Run the existing importer without `onBatch`, force a full **fine-detail** pass, and transform the returned `ImportResult` on the CPU. Reuses all validation and format coverage. | **Recommended for the first slice.** |
| B. Dedicated worker export opcode | A new control message that emits STL-shaped output (or transformed triangle soup) from inside the worker. | Deferred. Adds protocol surface and a second serialization contract; only justified if A's memory cost proves unacceptable. |
| C. Read back GPU buffers | Copy vertex/index buffers to a readback heap and transform on CPU. | Rejected. Proxy/partial residency makes it incorrect; instance expansion and origin/axis still need a CPU pass anyway. |

Option A's cost is that the normalized geometry is held in RAM for the duration
of the export, which is contrary to the viewer's streaming posture. The first
slice MUST bound this: enforce a maximum export triangle/byte budget and fail
with a clear, actionable error above it rather than attempting an unbounded
in-memory gather. Out-of-core streaming export (Option B) is the follow-up if
demand justifies it.

Because the normal progressive import may only deliver coarse/proxy detail,
the export path MUST request and wait for the complete fine-detail set
(`kFineLod`) for every exported geometry chunk, and MUST NOT silently export
coarse, preview, or scan geometry.

## Export pipeline

1. **Gate.** Enabled only when a model is loaded and has at least one triangle
   chunk; disabled for point-only scenes (see [Point clouds](#point-clouds)).
2. **Gather.** Full fine-detail `ImportResult` via the non-batched import path
   for the current source, under the export budget.
3. **Select.** Consider only `ChunkTopology::TriangleList` chunks at
   `kFineLod`. Skip `kCoarseLod`, `kPreviewLod`, `kScanLod`, and non-triangle
   topologies. Geometry marked `kGeometryReusableInstanceSource` is emitted
   only through its instances, never directly.
4. **Resolve occurrences.** For each geometry chunk, if instances reference it,
   emit one transformed copy per visible instance and skip the bare geometry;
   otherwise emit the bare geometry once. Invisible instances are skipped.
5. **Transform.** For each vertex: `world = worldTransform * (localPosition +
   clusterOrigin)`, using the descriptor's double `origin` and the resolved
   world matrix. Composition stays in double precision; only the final STL
   facet positions are narrowed to `float`.
6. **Winding.** If the occurrence's transform has a negative determinant
   (`mirrored`), reverse the triangle winding so facet normals remain outward.
7. **Unit and up-axis.** Apply the chosen policy (see
   [Units and up-axis](#units-and-up-axis)).
8. **Assemble.** Expand `uint32` indices into triangles; drop degenerate
   (zero-area) and non-finite triangles with a bounded count reported to the UI.
9. **Normals.** Compute a per-facet normal from the (already transformed)
   triangle winding, normalized when credible; never reuse per-vertex normals,
   since STL facet normals are flat.
10. **Serialize.** Binary STL only for the first slice: 80-byte header (ASCII
    text, no `solid` requirement), `uint32` facet count, then 50 bytes/facet
    (12 floats normal+vertices, `uint16` attribute byte count = 0). Facet count
    MUST be validated against the `uint32` ceiling before writing.
11. **Write.** Stream facet records to the destination handle with bounded
    buffering; never materialize the whole file in memory.

## Units and up-axis

STL is **unitless** and defines no up-axis, so the exporter must choose and
state a convention. This is the largest source of user-visible "wrongness"
and is therefore an explicit product decision, not an implementation detail.

- **Units.** Default to **millimetres**, the dominant slicer convention.
  Convert using `SceneMetadata::metersPerUnit` when it is nonzero
  (`WireFormat.h:158-169`). When `metersPerUnit` is zero (STL/PLY/STEP authors
  may legitimately not declare units), do **not** assume metres; export the
  source coordinates 1:1 and report the chosen unit and the missing source unit
  in the export summary.
- **Up-axis.** Default to baking the viewer's **display** correction
  (`ModelData::upAxisCorrection`, `Model.h:99-109`) so the exported mesh is
  oriented as the user sees it. The user's live ground-axis *presentation*
  toggle is a view preference and is not part of the model, so it is not baked.
  A "preserve source axes" option is deferred.

Both choices MUST be surfaced in the export summary (unit, up-axis, any
applied scale factor) so a surprising size or orientation is diagnosable.

## Point clouds

STL has no point primitive. A scene with only `PointList` geometry MUST fail
the export with a specific, non-generic message rather than producing an empty
or bogus file. A scene mixing triangles and points exports the triangles and
reports the omitted point count. This is consistent with the viewer's existing
"no synthetic triangle expansion" policy for points.

## Security and the write path

The AppContainer import worker is a zero-capability, zero-write process with no
path authority (`03-file-formats-and-ingestion.md:33`). Export MUST NOT change
that. Consequences:

- All file writing happens in the **trusted** `Preview3D.exe` process, after the
  user picks a destination through a standard Save dialog.
- The destination is opened by the trusted process with explicit create/truncate
  semantics; no path is ever passed to a worker, and no worker opcode gains
  write capability.
- The source is never opened for writing and is never modified in place.
- The export writes a new file only; it does not overwrite the source unless the
  user explicitly chooses the same path, in which case the normal overwrite
  confirmation applies.
- Export performs no network access, consistent with NFR-11.

## User experience

- A **File ▸ Export as STL…** command, keyboard reachable, disabled until a
  triangle-bearing model is ready.
- A Save dialog defaulting to the source base name with `.stl`, with overwrite
  confirmation.
- A non-modal, cancellable progress indication for large exports; cancellation
  removes the partial file.
- A completion summary reporting facet count, applied unit, applied up-axis, and
  any dropped-degenerate or omitted-point counts.
- Clear disclosure that the export is **geometry only** — no materials, colors,
  textures, or hierarchy — shown before or with the Save dialog, not only in
  documentation.
- `.stl` remains an input type; the command must not be confused with the file
  association or with "save," which does not otherwise exist in this read-only
  viewer.

## Limits and failure modes

| Condition | Required behavior |
| --- | --- |
| Triangle count would exceed `uint32` facet ceiling | Fail with a named limit before writing |
| Export working set exceeds the export budget (Option A) | Fail with a named limit and a clear message; do not thrash |
| Source declares no unit | Export 1:1, report it |
| Point-only scene | Fail with a point-specific message |
| Fine detail unavailable (limit/cancel) | Fail; never silently export coarse/proxy geometry |
| Degenerate/non-finite triangles | Drop with a bounded, reported count |
| Destination unwritable / disk full | Fail with the OS error mapped to user text; leave no partial file |
| Export cancelled | Delete the partial file and report cancellation |

## Test plan

- Golden binary STL output for a known triangle mesh, byte-for-byte against an
  independently computed reference.
- **Round-trip:** re-import the exported STL through the existing importer and
  assert vertex/facet counts and verified bounds match the source's fine-detail
  values within tolerance.
- Instance expansion: one geometry chunk with multiple instances produces the
  correct multiplicity and world positions.
- Mirrored instance: winding is reversed and facet normals point outward.
- Visibility: hidden instances/nodes are absent.
- Units: a source with a known `metersPerUnit` exports at the expected scale; a
  unitless source exports 1:1 and reports it.
- Up-axis: a Z-up and a Y-up source both export in the documented orientation.
- Point-only and mixed point/triangle scenes.
- Degenerate/non-finite input drops and reports correctly.
- Budget and `uint32` ceiling failures occur before any write.
- Cancellation leaves no partial file.
- Hostile input: a malformed source that imports as a controlled error does not
  reach the exporter.
- The importer's isolation tests remain unchanged (no new worker surface).

## Delivery slices

| Slice | Deliverable |
| --- | --- |
| EXPORT-STL-001 | Scope approval; `01-product-scope.md` FR + exclusion updates; README wording |
| EXPORT-STL-002 | CPU transform/assembly core (gather → select → instance expand → origin/world → winding → normals) with unit tests, no UI |
| EXPORT-STL-003 | Binary STL serializer + golden/round-trip tests |
| EXPORT-STL-004 | Trusted-process write path, Save dialog, progress/cancel, error mapping |
| EXPORT-STL-005 | Unit/up-axis policy implementation and export summary |
| EXPORT-STL-006 | Point-cloud and limit failure behavior |
| EXPORT-STL-007 | End-to-end corpus run across every supported input family; documentation |

## Deferred: GLB export

GLB is the only plausible target that could preserve the viewer's
material/texture/hierarchy subset, and therefore the only one that could be
called "faithful" in an appearance sense. It is deferred because:

- the normalized scene carries a bounded PBR subset, not arbitrary glTF
  material graphs, so a GLB export would still be lossy and would need the same
  explicit disclosure;
- it requires a material/image re-serialization layer and a texture re-encode
  policy that STL does not;
- the export source is still normalized geometry, so the instance/origin/axis
  work is shared with STL and can land first.

## Open questions

1. **Units default for unitless sources.** Export 1:1, or assume millimetres
   with a warning? This proposal recommends 1:1 plus disclosure; confirm with
   the product owner.
2. **Up-axis default.** "Export as displayed" (recommended) versus "preserve
   source axes." Confirm.
3. **In-memory budget.** What maximum export working set is acceptable for the
   first slice, and is an out-of-core (Option B) follow-up required at launch?
4. **Scope of "any opened model."** Exporting an already-STL source is nearly
   identity but still re-normalized; confirm it is in scope or disable the
   command for STL input.
5. **Naming.** "Export as STL" versus "Save a copy as STL" — the latter may
   better signal that the source is untouched.

## References

- [design/01-product-scope.md](design/01-product-scope.md)
- [design/03-file-formats-and-ingestion.md](design/03-file-formats-and-ingestion.md)
- [design/07-user-experience.md](design/07-user-experience.md)
- `shared/model-core/include/model_core/WireFormat.h`
- `shared/model-core/include/model_core/VertexLayouts.h`
- `shared/import-broker/src/SharedSectionValidator.cpp`
- `interactive-viewer/src/app/D3D12ImportBridge.cpp`
- `interactive-viewer/src/app/D3D12ViewerPath.cpp`
- `interactive-viewer/src/app/RenderThread.cpp`
- `interactive-viewer/src/render/Model.h`
