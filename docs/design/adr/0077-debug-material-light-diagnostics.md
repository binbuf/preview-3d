# ADR-0077 — Debug material/light diagnostics use shader channels and bounded readback

- **Status:** accepted
- **Date:** 2026-10-30
- **Task:** T21

## Context

T20 established deterministic capture and comparison, but a screenshot cannot
say whether a region is wrong because of an authored factor, a bound texture, a
particular BRDF lobe or the environment. T22-T27 need an isolated way to see
imported values, actual GPU inputs and per-pass contributions before changing
exposure, lighting or material semantics. Ordinary release builds must not gain
a new message/path authority surface.

## Decision

1. The textured PBR pixel shader reads a diagnostic channel from
   `gPresentation.w` (already present in the frame constant buffer). Channel 0
   keeps the product render byte-for-byte. Channels 1-20 expose raw linear
   views: base colour, geometric/shading normal, UV0, metallic, roughness,
   effective AO, coverage alpha, transmission, IOR/F0, clearcoat, sheen,
   isolated direct/environment diffuse and specular, coat, emission and the
   pre-tone-map HDR colour. The resolve pass remains the only display clamp.
2. UV1 is not on the wire. Channel 5 returns an explicit magenta marker and the
   capability mask reports it unsupported. No neutral fallback is presented as
   authored data.
3. A bounded (at most 64x64) RGBA16F region of the HDR scene target is copied to
   a readback resource, fenced, and averaged on the CPU. Tests assert numeric
   values through that GPU readback rather than shader-token presence.
4. A bounded per-material draw record is published on model publication:
   source/primitive/material ids, validated wire values, texture slot identity
   (chunk id, pixel format, colour space, bound/unbound), sampler/UV flags and
   effective bucket, depth-write and cull state. The record mirrors the actual
   PSO selection. The viewer does not parse glTF; test tooling reads source
   metadata and matches it to the validated snapshot.
5. All new messages (WM_APP+104 fields 124-194) live inside the existing
   `PREVIEW3D_ENABLE_TEST_CONTROL` + `--app-smoke` guard. Release keeps no
   handler and no new path authority; T21 builds Release and asserts every field
   returns zero.

## Consequences

- Diagnostic data is GPU-authoritative and survives future BRDF changes better
  than source-string assertions.
- Temporary render-target copies and a second Debug-only material snapshot add
  bounded memory/work; they are not in the present path and cannot affect a
  Release binary.
- The channel numbering and material record fields are a test-tooling contract.
  Extending them requires updating `tests/render-quality/diagnostics.py`, the
  viewer doc and the T21 profile together.
- Naming these channels in the design record makes later tasks cite an isolated
  contribution instead of guessing a cause from a final image.
