# 0081 — Texture-budget accounting and complete-load readiness

- **Status:** Accepted
- **Date:** 2026-10-30
- **Task:** T23

## Context

The microscope became visible and then reported `ResourceLimit` minutes later.
An instrumented Debug import/capture measured the first failure as
`ImportStage::GenerationDeadline` (17) with `ImportErrorCode::ResourceLimit` (2)
at ~301 s. The model was already complete: 3–4 × 2048 px textures with 12 mips,
134,742,016 GPU bytes against a 1,023,410,176-byte detail target. No decoded
byte/pixel, mip-chain, catalog or GPU-residency cap was implicated.

The limiter was the broker's absolute 300 s generation wall clock applied to the
background *detail replay*. Once the viewer stopped asking for visible regions,
the broker's replay loop called `nextDetail()` (which returned 0) until the
deadline, then surfaced the expiry as a terminal failure after a valid image.
The microscope also names the same diffuse sidecar through two distinct glTF
image entries, so it decoded and uploaded that image twice.

Separately, the capture contract accepted a frame once it was bright and quiet;
it did not require the terminal handoff, geometry/texture residency, or the
absence of a late error.

## Decision

1. **One immutable image source, one decode.** glTF image entries with the same
   source identity (URI, bufferView, or content hash) and the same semantic and
   color space reuse one pending image. sRGB color and linear data maps remain
   distinct chunks; distinct sources never alias. The aggregate byte and pixel
   totals are charged once.
2. **The detail replay is a bounded background session, not the forward load.**
   The broker ends it cleanly with a new `ControlOpcode::EndDetail` after a
   bounded idle grace (a generous initial grace before the first request, a
   short grace between requests) or a bounded request count. The worker leaves
   `AwaitDetail`, returns to the pool, and the generation completes. An
   outstanding detail reply that never arrives is still terminated at the
   existing absolute deadline. A detail replay never re-decodes the texture set
   that the scan batch already delivered.
3. **Complete-load readiness is terminal and resident.** A capture requires the
   expected generation, `Ready`/complete (or an explicit degraded completion),
   resident geometry and texture counters, no pending detail request, settled
   pixels and quiet counters. A terminal error observed before or after the
   first visible geometry is a failure, never a qualified capture.
4. **Diagnostics are bounded and typed.** A first texture-budget refusal records
   the limiting resource and requested/allowed quantities. Debug builds emit one
   value-only line without paths or model data; Release logs stay silent.
5. **No aggregate limit is raised and no isolation is weakened.** The worker
   Job/AppContainer/broker limits, feature level 11_0 and the finite texture
   budgets are unchanged.

## Consequences

- The microscope opens within the existing envelope and stays complete while
  held; its duplicate diffuse map costs one decode, and resident model textures
  drop from four to three.
- After the replay has ended, newly visible regions use the already-resident
  coarse proxy until the model is reopened. Re-opening the detail service on a
  later view change is a follow-up, not a correctness dependency.
- The new idle-end and reused-image cases are import-isolation regressions; the
  T23 render-quality lane holds the microscope past the former failure window
  and inspects the reused-image fixture, Camera and Car.
- An unexpected late terminal error is reported honestly as a failed load, so a
  previously visible model is never mistaken for a qualified image.
