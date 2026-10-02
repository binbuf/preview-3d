# T12 — Active-instance IPC hardening

## Goal
A same-user, lower-integrity, or sandboxed process cannot authenticate to the activation pipe or
squat the singleton objects as easily, and a forwarded model path cannot be captured by a
pre-created pipe.

## Context (read first)
- `interactive-viewer/src/app/ActiveInstance.cpp:102` — SDDL `D:P(A;;GA;;;SY)(A;;GA;;;<user>)`
  grants `GENERIC_ALL` including `WRITE_DAC`/`WRITE_OWNER`; no mandatory label.
- `interactive-viewer/src/app/ActiveInstance.cpp:243-262` — client auth checks only user SID and
  session after `ImpersonateNamedPipeClient`; any same-user process, including low-integrity and
  AppContainer processes, passes.
- `interactive-viewer/src/app/ActiveInstance.cpp:79-91,356-358` — names derive from session + a
  truncated SID hash, and pre-created/abandoned objects are accepted (`:364-377`), so a squatter
  can become the "primary" and receive forwarded paths (`Forward`, `:460-510`).
- `docs/design/06-application-lifecycle-and-ipc.md` — intended singleton/pipe policy.
- Tests: `tests/unit` ActiveInstance cases, `tests/app-smoke` activation lane.

## Scope
- [ ] Tighten the DACL: grant only the rights each object needs (no `GENERIC_ALL`, no
      `WRITE_DAC`/`WRITE_OWNER` for the interactive user unless required), keep
      `PIPE_REJECT_REMOTE_CLIENTS`.
- [ ] Authenticate more than SID/session: verify the client token's integrity level (reject below
      medium unless it is the product's own child), presence of no package/AppContainer identity,
      and that the client process image is the installed viewer (path + optionally signer). Record
      why each check is safe against PID reuse.
- [ ] Fail closed when a named object already exists but is not owned/created by the expected
      identity (do not silently adopt a squatter's mutex/pipe); validate the security descriptor or
      owner before use, and re-create on mismatch.
- [ ] Consider a per-logon/boot random component stored in a current-user-only location for object
      names; if not feasible, document the residual squat risk and the owner checks that mitigate it.
- [ ] Tests: low-integrity client rejection, pre-created object rejection, and the existing
      activation smoke still passing.

## Out of scope
- Replacing named pipes with a different IPC mechanism.
- The viewer's local file writes (→ SEC-11).

## Design notes
- The pipe carries a local path and `activate`; the viewer revalidates the path by handle later, so
  the worst case is a confused-deputy open, not code execution — still worth closing.
- Process-image checks must handle the portable layout (viewer launched from an arbitrary
  directory) without hard-coding `Program Files`.
- Do not break the existing 1 s timeout, 16-command queue cap, or one-request-per-connection model.

## Done when
- [ ] `Tests.Unit` and the activation smoke pass with the new checks.
- [ ] A test proves a pre-created object or unauthenticated same-user client is rejected.
- [ ] Hand-off filled in with the chosen authentication set and residual risk.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_