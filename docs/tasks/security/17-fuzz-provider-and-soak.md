---
verify: x64\Release\Tests.Unit.exe "~[graphics]"
---

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

**Landed.**
- `tests/fuzz/ProviderFuzz.cpp` + `ProviderFuzz.vcxproj` + `prepare_provider_seeds.py`: a
  libFuzzer + ASan target over the real provider sources. A 24-byte envelope selects
  `Pipeline` (registry -> adapter -> sampler -> rasterizer over a memory-backed
  `BoundedStreamSource`), `Stream`, `Sampler`, or `Raster`; the stream flags select hostile
  STATSTG/seek/short-read behaviour. It covers all eight families (including STEP/OCCT) plus the
  rasterizer (clipping math, degenerate transforms, NaN/Inf, extreme aspect, `cx == 0`/`0xFFFFFFFF`).
  36 immutable seeds are generated from the committed per-family fixtures and hostile shapes.
- `packaging/smoke/ProviderSoak.{h,cpp}` compiled into the existing `ProviderSmokeHost.exe`, reached
  with `--soak`: concurrent STA apartments through the real `IThumbnailCache` path with
  `WTS_FORCEEXTRACTION`, repeated dllhost load/render/teardown with cache churn, asserting no failed
  thumbnail, no persistent surrogate after 30 s, and bounded GDI/User/handle/thread/private growth.
- `docs/design/adr/0047-provider-pipeline-fuzz-and-surrogate-soak.md`; design/05 and design/09
  updated; `tests/fuzz/README.md` and `packaging/smoke/README.md` document the commands and the
  measured numbers.

**Measured checks (Release x64).**
- `ProviderFuzz.exe <36 seeds> -max_total_time=60 -timeout=10 -rss_limit_mb=2048 -max_len=2100000`:
  788 executions, 447 MiB peak RSS, no crash/ASan report (30 s: 590 / 330 MiB).
- Surrogate soak, 7 families, 4 apartments x 100 iterations + 4 warm-up = 404 thumbnails: 0 failed,
  GDI 0->0, User 6->8, handles 189->207, threads 10->11, private bytes 4,665,344 -> 9,818,112
  (+5.15 MiB), one `dllhost.exe` surrogate, teardown 5.1 s, no persistent surrogate.
- `Tests.ImportIsolation.exe "[sandbox]"`: 17 cases / 15 passed / 2 skipped, 764 assertions.
- `Tests.ImportIsolation.exe` (Release): 403 cases / 398 passed / 5 skipped / 0 failed.
- Harness verify `Tests.Unit.exe "~[graphics]"`: 312 cases / 131557 assertions, all pass.

**Deviations.**
- The fuzz target links all eight adapters including STEP/OCCT, so a cold build needs **both** the
  root and `thumbnail-provider\step-occt` vcpkg manifests restored. It is deliberately **not** added
  to the scheduled `fuzz-smoke` matrix yet, because that job restores only the root manifest;
  promoting it is a recorded follow-up (ADR-0047). No product code changed, so no new regression
  fixture was needed (the seeds produced no finding).
- The soak classifies a surrogate crash/hang as a hard failure and is run on valid fixtures only, so
  it does not yet implement the SEC-08 allowed-failure classification for a real `__fastfail`/stack
  death or the quarantine of a contained AV (needs a hostile-input soak lane) - recorded as a
  follow-up.

**Next task must know.** Build the fuzz target with
`C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe` and both
vcpkg trees present; use the distinct `IntDir` in the project. The soak reuses the existing
`Stage-`/`Register-`/`Unregister-ProviderSmoke.ps1` flow. Remaining SEC-17 work: the `fuzz-smoke`
promotion and the hostile-input SEC-08 classification lane.