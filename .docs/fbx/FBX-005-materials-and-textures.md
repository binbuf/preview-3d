# FBX-005: unified materials and texture dependencies

Status: complete
Depends on: FBX-004  
Unblocks: FBX-006

## Objective

Complete the FBX unified PBR subset using the existing normalized material and
image contract, including embedded media and broker-approved local image
sidecars under the common decode budgets.

## Context to load

- `import-worker/src/ObjAdapter.cpp`
- `import-worker/src/{TextureTranscodeAdapter,WicImageDecodeAdapter,WebpDecodeAdapter,SidecarFileClient}.{h,cpp}`
- `shared/model-core/include/model_core/{MaterialPayload,PixelFormats,WireFormat}.h`
- `shared/import-broker/src/{ImportSession,SharedSectionValidator}.cpp`
- `interactive-viewer/src/app/D3D12ImportBridge.cpp`
- `interactive-viewer/src/app/D3D12ViewerPath.cpp`
- pinned `ufbx.h` material/texture/content structures

## Work

1. Extract a tested, format-neutral `ufbx` material conversion helper from the
   working OBJ path where behavior is truly shared. Preserve OBJ output exactly.
2. Map `ufbx` unified PBR/FBX fallback properties into base color/opacity,
   metallic, roughness, emissive, normal/bump, alpha mode/cutoff, double-sided,
   and UV transform fields. Clamp/validate non-finite values and emit bounded
   warnings for approximated optional lobes.
3. Decode embedded image blobs without routing them through a path. Resolve
   external image names only through `RequestSidecarFile`; keep pinned replay,
   source-directory containment, request count/byte caps, MIME sniffing, codec
   allowlist, color-space semantics, and aggregate decoded pixel/byte budgets.
4. Do not request geometry caches, video, remote URLs, absolute paths, ADS,
   traversal, or environment-selected resources. Missing optional textures use
   deterministic fallbacks and warnings; unsafe references remain hard errors.
   A reference that stores the authoring machine's absolute path is reduced to
   its file name first; the broker may then resolve exactly one matching name
   under the model package root (the primary file's parent directory, covering
   the common `<model>/source/<file>.fbx` + `<model>/textures/` layout). That
   package lookup is shared by every local format, is depth/entry bounded,
   skips reparse points, and only opens the matched file after the ordinary
   canonical containment and size checks; the authored absolute path is never
   opened. Traversal, UNC/device, ADS, URL, and forward-slash drive text keep
   failing closed through the resolver.
5. Preserve geometry sharing when instances bind different materials: geometry
   is uploaded once, while instance draw records select the applicable material.
   Validate every cross-batch geometry/node/instance/material/image dependency
   before terminal success.
6. Handle layered/procedural textures and unsupported material models according
   to the FBX-001 policy. Never invoke arbitrary installed WIC codecs or silently
   discard an unsupported feature required to make visible geometry meaningful.
7. Add tests for factor mapping, alpha, normal/bump, emissive, UV transforms,
   embedded PNG/JPEG/WebP as allowed, local sidecars, differing per-instance
   materials, missing/corrupt fallbacks, path attacks, aggregate limits,
   progressive dependencies, cancellation, and OBJ regression parity.

## Constraints

- All image parsing/decoding remains in the AppContainer worker.
- Do not broaden the global texture allowlist as an incidental FBX change.
- Keep warning/status payloads bounded and path-free.

## Verification

- Pixel/golden tests demonstrate the supported PBR subset and color-space
  behavior for embedded and brokered images.
- Unsafe external references are rejected by the trusted broker, and direct
  worker filesystem/network probes remain denied.
- Missing/corrupt optional textures preserve geometry with a bounded warning;
  required unsafe data never falls back permissively.
- OBJ/MTL material and texture regression tests are byte/semantically unchanged.
- Debug/Release Unit and full ImportIsolation suites pass.

## Completion (2026-09-17)

- The apparent external-image worker crash was a harness defect, not an FBX
  protocol defect: the FBX request helper left both sidecar limits at their
  zero defaults, so the trusted broker correctly stopped the first request at
  `SidecarRequestLimit`. The helper now supplies the same bounded nonzero shape
  as product callers. Untouched upstream external-texture FBX reproduced the
  limit before the fix; no containment or worker-failure rule was weakened.
- Embedded PNG/JPEG/WebP and broker-approved local JPEG sidecars decode through
  byte sniffing and the existing explicit codec paths. Tests cover sRGB/linear
  role semantics, full raster mip chains, factors, transparency/alpha mode,
  emissive, normal and bump fallback, UV transforms, deterministic missing,
  corrupt and byte-capped fallbacks, and hard traversal/absolute/UNC/remote/ADS
  rejection.
- Aggregate decoded texture pressure now remains a typed `ResourceLimit`
  instead of being mistaken for an optional corrupt texture. A test-only
  request flag reduces that aggregate budget to exercise failure and
  same-worker recovery without allocating the production 128 MiB allowance;
  production limits are unchanged.
- Pinned upstream `max_instanced_material_7700_ascii.fbx` proves one geometry
  upload can serve three instances with distinct material IDs. Pinned upstream
  `maya_texture_layers_7500_ascii.fbx` proves ambiguous layered graphs preserve
  supported visible geometry and emit bounded warnings. Progressive material /
  image dependencies validate across batches, and existing cancellation and
  pooled recovery coverage remains green.
- Debug and Release solution builds pass. Focused FBX materials pass 12 cases /
  284 assertions; the complete FBX subset passes 25 / 47,968; Unit passes 98 /
  7,609 Debug and 7,521 Release assertions; full ImportIsolation passes 249 /
  100,693 assertions in each configuration. The unchanged OBJ regression subset
  passes 7 / 82 in both configurations. FBX-006 is unblocked.
