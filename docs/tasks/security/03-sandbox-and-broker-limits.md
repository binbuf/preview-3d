---
verify: x64\Release\Tests.ImportIsolation.exe
---

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
- [x] Add CPU-time and/or job-time limits to `SandboxLimits`/`CreateConfiguredJob` and set them for
      all three import processes; pick values from the measured budgets, not guesses.
- [x] Enforce an absolute per-generation wall-clock deadline that progress messages cannot extend
      (keep the per-reply timeout as well).
- [x] Reject a null `sid` inside `LaunchSuspendedSandboxedWithSid`.
- [x] Check every `SetHandleInformation` and fail the launch/session when it fails.
- [x] Replace the optional dereference in texture accounting with a checked path.
- [x] Reject ADS on the primary source (and add a `SourceFileAccessTests` case), matching the
      sidecar policy.
- [x] Validate `RequestSidecarFileNotice.generationId` against the current generation before
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
- [x] `Tests.ImportIsolation` passes in Debug and Release; new cases cover the Job limit, the
      progress-spam deadline, ADS rejection, and the stale-generation notice.
- [x] No doc-promised limit remains unimplemented for these paths.
- [x] Hand-off filled in.

## Hand-off

All seven Scope items landed. Files:
`shared/import-broker/{include/import_broker/{SandboxLauncher.h,ImportSession.h},src/{SandboxLauncher.cpp,ImportSession.cpp,WorkerPool.cpp,SourceFileAccess.cpp}}`,
`import-worker/src/{ContainmentProbes.h,ContainmentProbes.cpp,main.cpp}`,
`tests/hostile-worker/src/{AttackModes.h,AttackModes.cpp,main.cpp}`,
`tests/import-isolation/{SandboxLaunchTests.cpp,ChunkBatchTests.cpp,SourceFileAccessTests.cpp}`.

What landed:
- `SandboxLimits::processCpuTimeLimitMs` → `JOB_OBJECT_LIMIT_PROCESS_TIME`, and a Job violation in
  that flag now maps to `ResourceLimit`. Set on the one-shot worker path and on the compatibility /
  STEP hosts (`kImportProcessCpuTimeLimitMs`, 15 min, derived from the measured STEP Ready ≤180 s
  budget). Deliberately NOT set on the reused general worker pool: the limit is cumulative per
  process and would eventually kill a healthy worker; that pool is bounded by the new generation
  deadline instead. See ADR 0031.
- `ImportSessionRequest::generationWallClockBudgetMs` (default 300 s) caps every mid-generation
  read at `min(replyTimeout, ceil(remaining))`; expiry terminates the Job and fails with the new
  `ImportStage::GenerationDeadline` / `ResourceLimit` (kept distinct from `ReplyTimedOut`).
- `LaunchSuspendedSandboxedWithSid` rejects a null SID.
- Both `SetHandleInformation` results are checked; `WorkerPool::LaunchOne` and the one-shot session
  path fail closed.
- `BatchAcceptance::Record` now returns bool and re-derives the RGBA8 byte count via a checked
  `ComputeImagePixelBytes`; failure is `MalformedData` at `ValidateSection`.
- `OpenAndCanonicalizeSourceFile` rejects any `:` that is not the volume separator (ADS,
  drive-relative, URI), matching `ResolveSidecarPath`.
- `RequestSidecarFileNotice.generationId` is validated before the request is counted or resolved.

New tests: `SandboxLaunchTests` null-SID fail-closed and `--cpu-spin` Job CPU-time termination; `ChunkBatchTests`
progress-spam deadline (hostile `--batches-unbounded`, caps raised) and stale sidecar generation
(hostile `--sidecar-stale-generation`); `SourceFileAccessTests` ADS and drive-relative rejection.

Checks run:
- Debug build: `MSBuild tests\import-isolation\Tests.ImportIsolation.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:SolutionDir=<repo>\` → success. Full suite 387 cases / 381 passed / 6 failed; every failure is the documented pre-existing set (SidecarPathResolver user-asset-root ×3, UsdSpikeTests:230, ThreeMfSpikeTests 20 MB Job memory-pressure recovery, OpenUsdHostSpikeTests startup-timing). Confirmed pre-existing by `git stash` + rebuild (the OpenUsdHost case fails 1–3 assertions there too, run to run). No new failures.
- Release build: same command `/p:Configuration=Release` → success. Full suite 384/387; the 3 failures are the pre-existing `SidecarPathResolverTests` user-asset-root cases (489/504/518).
- New cases pass in both configurations (`[sandbox],[chunk-batch],[import-broker]`).
- `npm test` (Tests.Unit, 350 cases) passes.

Deviations / notes:
- The Job CPU-time limit is not applied to the reused general worker pool (cumulative semantics);
  that process is bounded by the generation wall-clock deadline. Only the compatibility/STEP hosts
  and the one-shot worker path carry the CPU cap.
- The generation deadline was mapped to `ResourceLimit` (a limit failure), not `Cancelled`, to keep
  limit failures distinct from cancellation per the task's own design note. The Job is still
  terminated/invalidated exactly as on cancellation because the caller returns and drops the lease.

Next task must know:
- `readFileImportRequestAndMapSection`-driven hostile modes select the one-shot path via
  `workerArgumentsOverride`; the hostile worker binary shares the worker output directory, so the
  long-lived `Binbuf.Preview3D.ImportWorker` profile's ACE covers it.
- `Tests.ImportIsolation` is not green at baseline (see `docs/security/PROGRESS.md` T01/T02 notes);
  judge this task by "no new failures", not by a clean run.