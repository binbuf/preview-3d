---
verify: x64\Release\Tests.ProviderHost.exe
---

# T08 — Provider containment policy (AV, stack, OCCT)

## Goal
The provider has an explicit, tested policy for structured exceptions and process-global library
state, so a contained crash cannot leave the surrogate running on corrupted state and concurrent
STEP requests cannot race OCCT's process singleton.

## Context (read first)
- `thumbnail-provider/Containment.cpp:71-81` — `EXCEPTION_ACCESS_VIOLATION` is contained and the
  surrogate keeps servicing requests; a heap-corrupting parser bug can therefore poison later
  requests. Stack overflow/breakpoint/single-step are re-raised (kill the process), and `__fastfail`
  cannot be caught (documented in `docs/PROGRESS.md`).
- `thumbnail-provider/StepFamilyAdapter.cpp:389,512` — `XCAFApp_Application::GetApplication()` is a
  process singleton used from COM apartments that may run concurrently (ThreadingModel=Apartment
  isolates objects, not surrogates). No mutex protects it.
- `docs/design/05-thumbnail-provider.md` — "Security and robustness"; ADR-0005 fixes the surrogate's
  role as crash containment, not a security boundary.
- Related: SEC-06 (exception path), SEC-17 (soak/fuzz evidence).

## Scope
- [ ] Decide the contained-AV policy and implement it: after an access violation either (a)
      fail-fast the surrogate, or (b) mark the surrogate unhealthy and stop accepting new calls
      while draining, so later requests never run on suspected-corrupt state. Record the decision
      and rationale in the provider design doc/ADR.
- [ ] Make the stack-overflow/`__fastfail` behavior explicit in diagnostics and documentation; add
      the expected crash to the soak's allowed-failure list rather than treating it as a pass.
- [ ] Serialize OCCT process singletons for the provider (mutex around
      `GetApplication`/`NewDocument`/`Close` and any other shared statics), or give each request a
      documented isolated context if OCCT supports it. Prove it with a concurrent STEP test.
- [ ] Add a provider-host concurrency test (two STEP requests in one surrogate) and an AV-injection
      test that asserts the chosen policy.

## Out of scope
- Making the surrogate a security boundary (it is not; that is ADR-0005).
- Upstream OCCT fixes (record and escalate via ADR if needed).
- Fuzz corpus expansion (→ SEC-17).

## Design notes
- Crash containment exists to protect Explorer; continuing after a contained AV trades that
  protection for availability. Prefer the conservative option unless evidence justifies otherwise.
- The policy must be observable in diagnostics (a stage/code), not a silent behavior.
- If OCCT's singleton is documented as thread-safe for the calls used, cite that and close the item
  with a test rather than adding a lock blindly.

## Done when
- [ ] The policy is written in the provider design doc and implemented with tests.
- [ ] `Tests.ProviderHost` passes, including the concurrent-STEP and AV-injection cases.
- [ ] Hand-off filled in.

## Hand-off

**What landed.**
- **Contained-AV policy = quarantine (option b).** `Containment.cpp` now records the first contained
  structured exception in a process-global quarantine
  (`MarkContainmentQuarantined`/`ContainmentQuarantined`/`ContainmentQuarantineCode`).
  `RunThumbnailPipeline` refuses every later request with `DecoderFailure` (`E_FAIL`, no bitmap)
  before any adapter/parser runs, while in-flight requests drain. The transition and each refusal are
  observable: new `DiagnosticStage::Containment`, new `DiagnosticEvent.quarantined` field (and
  `quarantined=%u` in `FormatDiagnostic`). `ResetContainmentQuarantineForTest` clears it for tests.
  Decision + rationale: ADR-0037 and `docs/design/05-thumbnail-provider.md` ("Threading and unload",
  "Security and robustness").
- **OCCT serialized.** `StepFamilyAdapter.cpp` holds one process-global `SRWLOCK` across
  `Impl::Reset` (Close), `LoadDocument` (GetApplication/NewDocument/reader/transfer) and
  `BuildGeometry` (meshing). The guard is acquired in frames above the containment SEH handler, so a
  contained AV cannot leave it locked; `SRWLOCK` because the sites are `noexcept`.
- **Stack-overflow/`__fastfail` made explicit.** design/05 and design/09 state that a re-raised stack
  overflow or an uncatchable `__fastfail`/stack-cookie fault kills the surrogate and is an SEC-08
  *allowed* soak failure that must be recorded, not a pass. The soak itself is SEC-17 (follow-up
  recorded in `docs/security/PROGRESS.md`).
- **Tests.** `Tests.Unit`: `[provider][threading][quarantine]` and `[provider][pipeline][quarantine]`.
  `Tests.ProviderHost`: `[host][containment][quarantine]` (real injected AV through the shipped
  boundary, then a refused fixture) and `[host][step][concurrency]` (two STEP requests in one
  surrogate).

**Deviations.** None from Scope. Chose quarantine over fail-fast: same safety property (no later
request on suspect state) without discarding unrelated in-flight work, and it is positively testable
in the in-process harness (ADR-0037 records the rejected fail-fast option).

**Check results.**
- `x64\Release\Tests.Unit.exe`: 135005 assertions / 357 cases, all pass (exit 0).
- `x64\Debug\Tests.Unit.exe`: 135080 assertions / 357 cases, all pass.
- `x64\Release\Tests.ProviderHost.exe`: 142 assertions / 8 cases, all pass.
- `x64\Debug\Tests.ProviderHost.exe`: 142 assertions / 8 cases, all pass.
- Rebuilt `Preview3DThumbnailProvider.dll` Release (0 warnings/errors). Commands in
  `docs/security/PROGRESS.md` ("T08"), including the LTCG `/t:Rebuild` gotcha.

**What the next task must know.** SEC-17 owns the soak allowed-failure classification (av/stack/
fastfail). The quarantine is process-global, so any new unit/host case that injects an SEH fault must
call `ResetContainmentQuarantineForTest()` before finishing. `occurrences_` ledger accounting (T51)
is still open.

**Remaining work / blockers.** None for this task.