---
verify: x64\Release\Tests.Unit.exe
---
# T16 — Implement threading, deadline, and containment behavior

## Goal
Make the provider deterministic and crash-contained under Shell scheduling: deadline checks throughout
parse/sample/raster, an exception/SEH boundary at the third-party calls, and path-redacted diagnostics.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — "Threading and unload", "Security and robustness", "HRESULT mapping".
- `docs/design/adr/0005-safety-bounded-parsing-not-surrogate.md` — containment philosophy.
- `docs/design/09-quality-performance-and-security.md` — threat controls and diagnostic rules.
- `docs/tasks/06-budgets-deadlines-hresults.md` — the deadline type and mapping.

## Scope
- [ ] Perform `GetThumbnail` work on the calling thread; create no lasting pool and no process-global mutable caches.
- [ ] Add deadline checkpoints at bounded parser/sampler/raster intervals and make long product-owned adapter loops cancellable. Record actual elapsed time when an uninterruptible library call returns after the stop point.
- [ ] Wrap third-party parser calls in exception and structured-exception containment at the COM boundary where legally safe, returning the tabulated HRESULT without swallowing ordinary memory faults.
- [ ] Keep diagnostic events path-redacted and disabled unless troubleshooting is enabled.
- [ ] Add tests for deadline expiry, injected parser exceptions/SEH, and unload while a call is active.

## Out of scope
- Fixing specific parser defects (→ T43 fuzzing/hardening).
- AppContainer or job-object isolation (not applicable inside the Shell surrogate).

## Design notes
- The 2 s stop point is cooperative inside a non-interruptible third-party call. Preflight bounds input, not execution time or unaccounted allocation. A call already in progress is allowed to return before the request is rejected; do not claim an in-call interrupt or hard wall-clock cutoff.
- SEH containment is a last-resort HRESULT boundary, not a correctness mechanism.
- `DllCanUnloadNow` must account for active calls; thread-local scratch must not keep the module alive.
- Never start, message or wait on any product process.

## Done when
- [ ] Deadline, containment and unload-during-call tests pass in Debug and Release.
- [ ] Diagnostics contain no file path by default.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
