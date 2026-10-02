# 0031 — Import-process CPU-time and per-generation wall-clock limits

## Status
accepted

## Context
`docs/design/02-system-architecture.md:88` and NFR-14 claim every import process runs under
"process/job memory/time limits". Only the memory (commit) and active-process limits existed:
`SandboxLauncher::CreateConfiguredJob` set `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`,
`JOB_OBJECT_LIMIT_ACTIVE_PROCESS`, and an optional `JOB_OBJECT_LIMIT_PROCESS_MEMORY`, so the
doc-promised CPU-time limit was unimplemented.

Separately, the broker's reply loop re-armed the full 120 s per-message timeout on every accepted
mid-generation message (`RequestSidecarFile`, `ChunkBatchReady`, `StepProgress`). A compromised
child that keeps talking could therefore hold one generation — and the import thread — open
indefinitely without ever tripping the timeout.

## Decision
- Add `SandboxLimits::processCpuTimeLimitMs` and map a nonzero value to
  `JOB_OBJECT_LIMIT_PROCESS_TIME` (`PerProcessUserTimeLimit`, 100 ns units). A Job CPU-limit
  violation is surfaced as `ImportErrorCode::ResourceLimit`, alongside the existing memory-limit
  mapping.
- **Do not apply the cumulative CPU cap to a process reused across generations.**
  `JOB_OBJECT_LIMIT_PROCESS_TIME` is cumulative, so a long-lived worker would eventually be killed
  by its own legitimate work. It is set on the one-shot worker path and on the
  compatibility/STEP hosts (which exit after their generation), at
  `kImportProcessCpuTimeLimitMs` (15 min): a hard backstop above the measured STEP Ready ≤ 180 s
  budget.
- Add `ImportSessionRequest::generationWallClockBudgetMs` (default
  `kImportGenerationWallClockBudgetMs`, 300 s) and cap every mid-generation read's wait at
  `min(replyTimeout, ceil(remaining))`. When the absolute deadline expires the Job is terminated
  and the session fails with the new `ImportStage::GenerationDeadline` /
  `ImportErrorCode::ResourceLimit` — a limit failure, not a protocol violation and not an ordinary
  reply timeout. The per-reply timeout is kept as a separate fault.

## Consequences
- The reused general worker pool is bounded per generation by the broker's wall-clock deadline
  rather than by a Job CPU cap; its process is never killed for accumulating prior generations'
  CPU time.
- `SandboxLaunchTests` proves `JOB_OBJECT_LIMIT_PROCESS_TIME` terminates the `--cpu-spin` worker;
  `ChunkBatchTests` proves the deadline stops an endlessly batching worker that keeps re-arming
  the reply timeout.
- The constants are derived from the measured STEP budget in
  `docs/design/09-quality-performance-and-security.md:92`, not guessed; a future measured
  multi-gigabyte import budget must re-derive them.