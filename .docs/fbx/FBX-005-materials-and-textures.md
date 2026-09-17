# FBX-005: unified materials and texture dependencies

Status: in progress
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

## Progress (2026-09-17)

- Added the pinned upstream `synthetic_embedded_base64_7700_ascii.fbx` corpus
  as `tests/fixtures/fbx-spike/embedded-png-ascii.fbx`, with provenance in the
  fixture README. The sandbox integration test verifies its embedded 32x32 PNG
  becomes a normalized RGBA8 sRGB image with the full mip chain, is linked from
  the material dependency, and does not follow the fixture's absolute filename
  metadata.
- External-image qualification is blocked. A small no-embedded-content
  derivative issues the FBX external-texture request but the one-shot worker
  exits before a terminal reply for both an allowed local path and a traversal
  path. The same broker/client machinery passes the existing glTF sidecar
  suite. Reproduce against an untouched external-texture FBX first, then fix
  the FBX-specific interaction without weakening broker containment or turning
  unsafe paths into fallbacks. The failing candidate test is intentionally not
  retained.
- This slice's focused embedded-PNG test and the full tagged FBX
  ImportIsolation subset pass in both Debug and Release (14 cases / 47,703
  assertions per configuration). This is not the task's final full-suite
  qualification.
