# 0037 — Provider containment policy: quarantine on a contained structured fault, and serialize OCCT

## Status
accepted

## Context
`thumbnail-provider/Containment.cpp` contains an `EXCEPTION_ACCESS_VIOLATION` and lets the surrogate
keep servicing requests. A contained fault can therefore have corrupted the process, and a later
request would run on that suspect state. Stack overflow, breakpoint and single-step are re-raised
(killing the process); a `__fastfail`/stack-cookie fault cannot be caught at all. Separately,
`StepFamilyAdapter.cpp` drives `XCAFApp_Application::GetApplication()`, a process singleton whose
lazy creation and `NewDocument`/`Close` document list were used from COM apartments that may run
concurrently (`ThreadingModel=Apartment` isolates objects, not process globals). ADR-0005 fixes the
surrogate as crash containment, not a security boundary.

## Decision
- **Quarantine on a contained structured fault.** On the first contained structured exception the
  boundary records the code and sets a process-global quarantine. `RunThumbnailPipeline` refuses
  every later request with `ProviderOutcome::DecoderFailure` (`E_FAIL`) before any adapter or parser
  runs, while requests already in flight drain normally. The transition and each refusal are emitted
  as a `DiagnosticStage::Containment` event with `quarantined=true`, and
  `ContainmentQuarantined()`/`ContainmentQuarantineCode()` expose the state. Chosen over fail-fast
  because it keeps the same safety property (a later request never runs on suspected-corrupt state)
  without discarding unrelated in-flight work, and it is observable and positively testable in the
  in-process harness; the Shell reclaims the surrogate on its next lifecycle. Stack
  overflow/breakpoint/single-step are not contained and never set the quarantine.
- **Serialize OCCT.** All provider OCCT work — the lazy `GetApplication()`, `NewDocument`, the
  reader/transfer pass, tessellation and the final `Close` — is serialized by one process-global
  `SRWLOCK` acquired in frames *above* the containment SEH handler, so a contained fault cannot
  leave it locked. `SRWLOCK` (not `std::mutex`) because the acquisition sites are `noexcept` and
  must not throw. This is more conservative than relying on OCCT thread-safety, which is not
  documented for a shared application/document list.
- **Stack-overflow/`__fastfail` explicit.** The re-raised codes and the uncatchable
  `__fastfail`/stack-cookie path are documented (design/05); the surrogate soak must classify the
  resulting process death as an SEC-08 allowed failure (recorded with its code), not a passing run.

## Consequences
- `Tests.Unit.exe` `[provider][threading][quarantine]` covers the boundary transition and the
  first-fault-wins/uncatchable-code rules; `[provider][pipeline][quarantine]` proves a quarantined
  pipeline refuses work before any adapter runs and fabricates no image. `Tests.ProviderHost.exe`
  `[host][containment][quarantine]` injects a real access violation through the shipped boundary and
  asserts the refusal; `[host][step][concurrency]` drives two STEP requests in one surrogate.
- A fault that quarantines the surrogate is still fuzzed and fixed (SEC-17/T43); containment and
  quarantine are last-resort resilience, never a correctness mechanism.
- The quarantine adds two process-global atomics and the OCCT lock adds no import; the two-symbol
  `PRIVATE` export surface and the dependency-closure rule are unchanged.
- Rejected: fail-fast (drops unrelated in-flight work and cannot be positively asserted in the
  in-process harness); a `std::mutex` (can throw at a `noexcept` acquisition site); leaving the
  OCCT singleton unserialized (races its document list).