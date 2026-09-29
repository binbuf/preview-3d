# 0005 — Safety rests on bounded parsing, not the Shell surrogate

## Status
accepted

## Context
Windows loads thumbnail handlers into an isolated per-handler COM surrogate (normally `DllHost.exe`)
by default, but that surrogate still runs with the invoking user's ordinary token and file/network
access. It is crash containment for Explorer, not a zero-capability AppContainer boundary. The
provider is therefore hostile-input code running in a sensitive host with no sandbox guarantee.

## Decision
- The provider never sets `DisableProcessIsolation` (or any per-handler opt-out) in the installer, the
  registry, or support guidance. Enabling in-process execution later requires a new ADR, a revised
  threat model and re-justification of every isolation-dependent claim.
- Safety depends on bounded stream reads, checked integer/range helpers, per-family admission and
  accountable allocation limits, a monotonic cooperative deadline, and exception/structured-exception
  containment at the third-party boundary — not on a claim that the surrogate is a sandbox.
  Preflight size limits cannot bound execution time or unaccounted allocation inside an opaque parser
  call. The 384 MiB process-commit increase and 2 s time-to-return are measured qualification targets;
  exceeding either requires narrowing or disabling the affected family/subset before release.
- The provider writes no persistent model data, opens no path or sidecar, makes no network request,
  launches no process, and creates no GPU device. A failed parse returns a precise HRESULT with a null
  bitmap so Explorer falls back to the generic icon; a fabricated success is prohibited.

## Consequences
- T44 is a release-blocking post-install check: every installed CLSID must be observed loading into
  the isolated surrogate, and no installed value may set `DisableProcessIsolation`.
- Parser crashes are fixed by fuzzing and hardening (T43); SEH containment is a last-resort HRESULT
  boundary, never a correctness mechanism.
