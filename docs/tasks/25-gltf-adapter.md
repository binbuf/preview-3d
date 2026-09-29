---
verify: x64\Release\Tests.Unit.exe
---
# T25 — Implement the glTF/GLB thumbnail adapter

> **E2E slice review point.** Completing this task closes the Tier A breadth slice. Run a fresh end-to-end review now; the pipeline may continue to the next task without waiting.

## Goal
Render stream-contained `.glb` and `.gltf` models in Explorer, including embedded compressed geometry
and images, without ever resolving a sidecar.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — stream ingestion (glTF rules) and limits.
- `docs/design/adr/0003-provider-decoder-scope.md` — Draco/meshopt/KTX2/WebP scope in the provider.
- `docs/design/05-thumbnail-provider.md` and `docs/design/03-file-formats-and-ingestion.md` — the Tier A adapter contract (Tier A has no separate family brief; see `design/README.md`).
- `import-worker/src/GltfAdapter.*`, `import-worker/src/DracoDecodeAdapter.*`, `import-worker/src/TextureTranscodeAdapter.*` — implemented viewer adapters used as policy references.
- `docs/tasks/13-family-routing-adapter-interface.md`, `docs/tasks/14-geometry-sampler.md`, `docs/tasks/17-provider-test-harness.md`.

## Scope
- [ ] Route `.glb`/`.gltf` to this adapter via its fixed CLSID; parse GLB or embedded-data-URI glTF only.
- [ ] Emit bounded triangles with transforms, normals/UVs/colors as available, and product-owned material values (base color, metallic/roughness, emissive, unlit, alpha mode/cutoff, double-sided).
- [ ] Decode bounded Draco and `EXT_meshopt_compression` geometry; decode embedded KTX2/Basis and WebP images; each under provider limits, with optional-image fallback to the default material.
- [ ] Treat a missing required geometry buffer/extension or any external `.bin`/image sidecar as the safe generic-icon fallback; never open a path.
- [ ] Add fixtures and goldens: embedded GLB mesh, instanced/transformed scene, Draco and meshopt sample, KTX2/WebP textured sample, `.gltf` data-URI sample, and sidecar-dependent/malformed/over-budget cases asserting null-bitmap errors.

## Out of scope
- Local `.bin`/image sidecar resolution (never in the provider; viewer-only).
- Viewer-side WebP/meshopt worker budget changes (owned by TSK-209 in the archived plan).

## Design notes
- No path recovery from `IInitializeWithStream`; a `.gltf` that depends on external files gets no thumbnail.
- Keep a hard per-primitive decode cap so one Draco/meshopt primitive cannot exceed scratch/commit budgets.
- Reuse `model_core` material fallbacks for deterministic neutral shading.

## Done when
- [ ] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes for the listed fixtures in Debug and Release.
- [ ] A sidecar-dependent glTF returns the generic-icon HRESULT with no filesystem access (verified by the isolation test).
- [ ] The family is smoke-checked in Explorer using the T22 procedure.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_