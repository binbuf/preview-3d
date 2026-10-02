---
verify: x64\Release\Tests.Unit.exe "[activation]"
---

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
- [x] Tighten the DACL: grant only the rights each object needs (no `GENERIC_ALL`, no
      `WRITE_DAC`/`WRITE_OWNER` for the interactive user unless required), keep
      `PIPE_REJECT_REMOTE_CLIENTS`.
- [x] Authenticate more than SID/session: verify the client token's integrity level (reject below
      medium unless it is the product's own child), presence of no package/AppContainer identity,
      and that the client process image is the installed viewer (path + optionally signer). Record
      why each check is safe against PID reuse.
- [x] Fail closed when a named object already exists but is not owned/created by the expected
      identity (do not silently adopt a squatter's mutex/pipe); validate the security descriptor or
      owner before use, and re-create on mismatch.
- [x] Consider a per-logon/boot random component stored in a current-user-only location for object
      names; if not feasible, document the residual squat risk and the owner checks that mitigate it.
- [x] Tests: low-integrity client rejection, pre-created object rejection, and the existing
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
- [x] `Tests.Unit` and the activation smoke pass with the new checks.
- [x] A test proves a pre-created object or unauthenticated same-user client is rejected.
- [x] Hand-off filled in with the chosen authentication set and residual risk.

## Hand-off
Changed:
- `interactive-viewer/src/app/ActiveInstance.cpp` / `.h`:
  - `SecurityForObject` (replaces `SecurityForCurrentUser`) builds one minimum-rights descriptor per
    object kind — mutex `0x120001`, event `0x120002`, pipe `FRFW` — with `O:<user>D:P(SY,user)` and a
    medium mandatory label `S:(ML;;NW;;;LW)`. No `GENERIC_ALL`/`WRITE_DAC`/`WRITE_OWNER`.
  - `Initialize` uses `CreateMutexExW`/`CreateEventExW` with least privilege and, on
    `ERROR_ALREADY_EXISTS`, `ObjectSecurityMatches` compares the existing owner+DACL to the expected
    descriptor and fails closed on mismatch (abandoned-mutex recovery still proves out because a real
    predecessor wrote the same descriptor). `CreateEventExW` passes `CREATE_EVENT_MANUAL_RESET`.
  - `AuthenticateClient` now also enforces integrity >= medium (own-child exception), non-AppContainer,
    and client image == this process image; it opens the client process before impersonating so the
    PID cannot be recycled between check and use (`ClientIdentityAccepted` is a testable pure policy).
  - `ListenerMain` claims the pipe name with `FILE_FLAG_FIRST_PIPE_INSTANCE` and creates each next
    instance before releasing the previous, so a pre-created pipe cannot capture a forward and the
    reservation never lapses. `Forward` verifies the pipe server image before writing.
  - `Coordinator::SessionObjectNames` derives the names without creating objects (test hook).
- `tests/unit/ActiveInstanceTests.cpp`: peer-identity rejection matrix, minimum-rights DACL assertion,
  and a pre-created untrusted-mutex rejection test (all tagged `[activation]`).
- Docs: `docs/design/06-application-lifecycle-and-ipc.md` singleton/pipe policy updated; new
  `docs/design/adr/0041-active-instance-ipc-hardening.md`.

Deviations:
- "Installed viewer (path + optionally signer)": implemented path equality against the current image
  (works for the portable layout); signer verification is deferred to SEC-14/T10 (Authenticode).
- Per-logon/boot random name component not implemented. A low-integrity peer is blocked by the
  mandatory label plus the integrity check; a medium same-user process could still read any
  current-user secret, so a random component would not stop the realistic squatter. Residual squat
  risk is documented in ADR-0041 and mitigated by the exact owner+DACL match and pipe reservation.
- The mandatory label is set from the descriptor at creation but cannot be read back in the
  unprivileged test process (reading a SACL needs SeSecurityPrivilege), so the unit test asserts the
  minimum-rights DACL and the label is covered by design/ADR rather than a direct SACL read.

Check results:
- `MSBuild tests\unit\Tests.Unit.vcxproj /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=…"` — ok.
- `x64\Release\Tests.Unit.exe "[activation]"` — 7 cases / 66 assertions, all pass.
- `npm test` (`x64\Release\Tests.Unit.exe`) — 369 cases / 135121 assertions, all pass.
- `MSBuild interactive-viewer\Preview3D.vcxproj /p:Configuration=Debug …` then
  `python tests/app-smoke/activation.py --configuration Debug` — all 8 checks pass.

Next task must know: the object names are still deterministic, so the residual above holds; the
listener now holds the pipe name continuously (a second live instance is transient), and
`ClientIdentityAccepted` is the single place to extend the peer policy (e.g., signer checks).