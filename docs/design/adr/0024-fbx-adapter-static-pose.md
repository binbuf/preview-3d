# 0024 — FBX adapter: deterministic static pose under a preflight-bounded ufbx call

## Status
accepted

## Context
T31 must render binary and ASCII `.fbx` in Explorer from the Shell's `IInitializeWithStream` stream
using the provider-local pinned ufbx copy, with path, external-file, geometry-cache, plug-in, script
and environment-codec access disabled (FBX-008). Two properties of ufbx 0.23.0 shape the decision:
`ufbx_evaluate_scene()` has **no progress callback** (FBX-001), so the cooperative deadline cannot
interrupt it; and a static preview must bake supported skin/blend deterministically rather than play
animation. External geometry caches and texture sidecars must never be opened, and the frozen
`VertexSample`/`MaterialPayload` carry no texture sampling, so a decoded embedded image cannot change
the thumbnail.

## Decision
1. **Pose:** evaluate at the first animation stack's `time_begin`, or `scene->anim` at time zero when
   no stack exists (FBX-001). `evaluate_skinning = true`, `evaluate_caches = false`. Use the evaluated
   `mesh.skinned_position`/`skinned_normal`; when `skinned_is_local` is set, transform position by
   `node.geometry_to_world` and normal by its inverse-transpose. Rejected: subsampling or ignoring
   deformers, which would misrepresent the start pose.
2. **Bounded evaluation without a callback:** preflight node/mesh/material/anim-stack/skin/bone counts
   and total triangles/vertices (2 M / 6 M) *before* evaluation and fail to the generic icon above
   them; additionally cap the evaluation temp/result allocators (96 MiB each, 1 M allocations). Never
   allow an unbounded library call merely because Shell uses a surrogate.
3. **External access:** `load_external_files = false` plus `ignore_missing_external_files` and a deny
   `open_file_cb`. The encoding (ASCII/binary) is detected from content only; extension detection is
   disabled and a non-FBX `metadata.file_format` is rejected. NURBS/subdivision/procedural/cache-only
   content with no supported polygon returns `UnsupportedRequiredFeature`; when supported polygons
   remain, the unsupported features are omitted.
4. **Images:** embedded allowlisted containers (PNG/JPEG/GIF/BMP/WebP) are structurally validated and
   bounded (32 MiB encoded, 32 MP aggregate) then discarded. External textures use the neutral
   fallback. Rejected: decoding via WIC/COM, which the provider must not use, and which could not
   affect the product-owned material anyway.

## Consequences
- The provider stays import-clean (ufbx static, no viewer/worker/host/core imports) and never opens a
  path; the crash-containment boundary is unchanged.
- FBX thumbnails are deterministic and stream-only, but a pathological file whose *evaluation* alone
  exceeds the allocator caps fails closed rather than being interrupted mid-call. Later work that
  needs true evaluation cancellation must route it through the worker/Job path (FBX-001), not here.
- `wall_clock` bounds are preflight-based: a file within the count caps can still take longer than the
  750 ms p95 target on a slow machine; T51 measures the actual distribution.
- Embedded-image structural validation is intentionally not a decode; if a later task adds texture
  sampling it must promote this to a real bounded decoder (and revisit the COM/WIC boundary).