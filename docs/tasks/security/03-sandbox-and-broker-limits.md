# T03 — Sandbox limits and broker defensive checks

## Goal
The Job Object and broker enforce the limits the design claims: bounded CPU/wall time, no prolonged
generation from progress-message spam, fail-closed primitives, and no unchecked security-relevant
return values or doc-promised path checks missing in code.

## Context (read first)
- `shared/import-broker/src/SandboxLauncher.cpp:16-24` — Job sets only kill-on-close, active-process,
  optional process-memory. `docs/design/02-system-architecture.md:88` and NFR-14 claim CPU-time
  limits that do not exist.
- `shared/import-broker/src/ImportSession.cpp:1253-1278` — every accepted `StepProgress` re-arms the
  full 120 s reply timeout (8192 messages), so a compromised STEP host can evade the timeout.
- `shared/import-broker/src/SandboxLauncher.cpp:36-53` — `LaunchSuspendedSandboxedWithSid` assigns a
  caller-supplied SID with no null check; callers currently guard it, but this is the security
  primitive.
- `shared/import-broker/src/WorkerPool.cpp:30,40` and `ImportSession.cpp:734-735` —
  `SetHandleInformation` results unchecked.
- `shared/import-broker/src/ImportSession.cpp:368-371` — optional dereference in texture accounting;
  safe today only because the validator proved it, latent if the two ever diverge.
- `shared/import-broker/src/SourceFileAccess.cpp:7-69` — primary path rejects remote/UNC/device but
  not NTFS ADS (`:` stream), unlike the sidecar resolver at `SidecarPathResolver.cpp:364`.
- `shared/import-broker/src/ImportSession.cpp:1176-1181` — `RequestSidecarFileNotice` is not checked
  against the current generation (every other mid-generation message is).
- Tests: `tests/import-isolation/SandboxLaunchTests.cpp`, `ImportSessionTests.cpp`,
  `SourceFileAccessTests.cpp`, `WorkerPoolTests.cpp`.

## Scope
- [ ] Add CPU-time and/or job-time limits to `SandboxLimits`/`CreateConfiguredJob` and set them for
      all three import processes; pick values from the measured budgets, not guesses.
- [ ] Enforce an absolute per-generation wall-clock deadline that progress messages cannot extend
      (keep the per-reply timeout as well).
- [ ] Reject a null `sid` inside `LaunchSuspendedSandboxedWithSid`.
- [ ] Check every `SetHandleInformation` and fail the launch/session when it fails.
- [ ] Replace the optional dereference in texture accounting with a checked path.
- [ ] Reject ADS on the primary source (and add a `SourceFileAccessTests` case), matching the
      sidecar policy.
- [ ] Validate `RequestSidecarFileNotice.generationId` against the current generation before
      servicing it.

## Out of scope
- Per-format sidecar policy (→ SEC-04).
- Mitigation-policy process attributes (→ SEC-10).
- Repo-wide unchecked-Win32 sweep; fix only the security-relevant paths here.

## Design notes
- Job limits are a backstop for a compromised child; product callbacks remain the primary budget.
- A generation deadline should terminate the Job on expiry and produce the same typed failure as a
  cancellation, invalidating sections.
- Keep protocol failures (`ImportProtocolViolation`) distinct from limit failures (`ResourceLimit`).

## Done when
- [ ] `Tests.ImportIsolation` passes in Debug and Release; new cases cover the Job limit, the
      progress-spam deadline, ADS rejection, and the stale-generation notice.
- [ ] No doc-promised limit remains unimplemented for these paths.
- [ ] Hand-off filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_