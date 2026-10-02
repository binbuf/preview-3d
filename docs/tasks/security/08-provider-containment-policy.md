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
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_