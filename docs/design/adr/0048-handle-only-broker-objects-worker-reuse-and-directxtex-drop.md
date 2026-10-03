# 0048 — Handle-only broker objects, session-scoped worker reuse, and no DirectXTex

## Status
accepted

## Context
The v0.5.0 security audit found design documents that described controls the code does not
implement, or implementation choices the documents did not describe:

- `08-installation-and-registration.md` claimed per-generation pipe/event/section ACLs. The broker
  actually creates those objects anonymously with default security descriptors and relies on handle
  possession — `WorkerPool.cpp`, `ImportSession.cpp`, and `SharedSection.cpp` pass empty
  `SECURITY_ATTRIBUTES` and never build a per-object DACL.
- `02-system-architecture.md` described the worker as reused "under a fresh job/sandbox" /
  "a fresh AppContainer token/Job Object pairing" per generation. The pool reuses one AppContainer
  profile and one Job Object per session (`ImportSession.cpp`, `WorkerPool.cpp`), because
  `JOB_OBJECT_LIMIT_PROCESS_TIME` is cumulative and would eventually kill a healthy long-lived
  worker (ADR-0031 already chose the per-generation wall-clock deadline instead).
- Several documents listed DirectXTex/WIC as a shipped decoder. The tree uses WIC only; there is no
  DirectXTex dependency in `vcpkg.json`, and the unused DirectXTex paths were removed.

These need one binding record so later tasks do not re-add the untrue claims or "fix" the code to
match stale prose.

## Decision
- **Broker objects are handle-only.** The per-generation control pipes, cancellation event, and
  shared sections are anonymous, created with default security descriptors, and never opened by
  name. The child receives exactly the handles the broker duplicates into it via
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`; access is governed by handle possession plus the
  AppContainer token. No per-generation named-object ACL is written, so no two import SIDs can
  share an object. Rejected: adding per-object DACLs (the objects are unnameable, so an ACL adds no
  reachability control and would be dead policy).
- **The general worker pool is session-scoped.** The reused pool keeps the AppContainer profile and
  Job Object created at first Open; it takes no cumulative process CPU-time cap. Each generation is
  bounded by the broker's wall-clock deadline, and the one-shot worker path and per-generation
  compatibility/STEP hosts keep their `kImportProcessCpuTimeLimitMs` cap (ADR-0031).
- **DirectXTex is not shipped.** WIC plus libwebp are the raster decoders. TGA, HDR, and DDS stay
  unsupported as recorded in `docs/FORMAT-SUPPORT.md`; adding them later requires a new dependency
  review, not a reinstatement of the old prose.

## Consequences
- `docs/design/02`, `03`, `08`, `09`, `10`, and `11`, and `SECURITY.md`, now state the handle-only
  model, the session-scoped pool, and the WIC-only texture path.
- A future task that wants per-generation ACLs, a CPU cap on the reused worker, or DirectXTex must
  supersede this ADR and re-justify the change against ADR-0031 and ADR-0005.
- The provider's `InprocServer32` registration remains in-process activatable by any local process;
  surrogate routing is Shell-path-specific (ADR-0008), and safety rests on bounded parsing.