# T17 — Provider pipeline fuzz + surrogate soak

## Goal
Each thumbnail family boundary and the CPU rasterizer are fuzzed, and an Explorer-surrogate soak
proves that hostile input cannot crash, hang, or leak the host across many parallel requests. This
completes the provider-owned scope of `docs/tasks/mvp/43-adversarial-hardening.md` (T43); if T43 is
executed instead, it must include the items below.

## Context (read first)
- `docs/tasks/mvp/43-adversarial-hardening.md` — the planned lane (currently unchecked).
- `thumbnail-provider/StreamSource.cpp` (`BoundedStreamSource`), `FamilyAdapterRegistry.cpp`,
  the eight family adapters, `GeometrySampler.cpp`, `CpuRasterizer.cpp`, `ThumbnailPipeline.cpp`.
- `tests/fuzz/README.md` conventions; `tests/provider-host` for a real COM host;
  `packaging/smoke/ProviderSmokeHost.cpp` and `thumbnail-spike/surrogate-probe` for surrogate
  hosting patterns.
- SEC-06 (containment) and SEC-08 (AV/stack/OCCT policy) define the failure behavior the soak must
  assert.

## Scope
- [ ] Add a fuzz target per family boundary: bounded stream source → adapter → sampler (no COM, no
      window, no GPU), following the existing libFuzzer+ASan envelope with an immutable seed set.
- [ ] Include the CPU rasterizer in the target or a second target (clipping math, degenerate
      transforms, NaN/Inf coordinates, extreme aspect ratios) now that SEC-07 clamps conversions.
- [ ] Add the surrogate soak: many concurrent apartments in a real dllhost, repeated load/unload,
      thumbnail-cache churn, asserting no crash/hang, no persistent thread, no monotonic
      GDI/User/private-byte growth.
- [ ] Re-run the real AppContainer/Job restriction suite for every wired family, not once globally.
- [ ] Minimize every finding into a regression fixture; fix small findings here, file tasks for the
      rest with the minimized input attached.
- [ ] Document the targets and soak commands in `tests/fuzz/README.md` and/or the provider design
      doc; record measured runtimes and peak RSS.

## Out of scope
- Worker-format fuzzing (→ SEC-15/16).
- Fixing upstream library bugs that need a version bump (record and escalate via ADR).
- Performance qualification (→ T51).

## Design notes
- The surrogate is crash containment, not a security boundary (ADR-0005); the soak's value is
  Explorer stability and leak detection, not escape testing.
- Keep seeds immutable and output under an ignored directory; libFuzzer mutates the corpus it is
  given.
- A contained AV or stack overflow that kills the surrogate must be recorded as the SEC-08 policy
  says, not masked as a passing run.

## Done when
- [ ] Every family has a bounded ASan smoke that passes, plus the rasterizer.
- [ ] The soak reports no crash/hang/leak/persistent thread over the agreed duration and
      concurrency, with raw numbers in the Hand-off.
- [ ] Findings are minimized, filed, and (where fixed) covered by regression fixtures.
- [ ] Hand-off filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_