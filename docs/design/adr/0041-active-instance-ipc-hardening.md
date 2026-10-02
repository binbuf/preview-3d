# 0041 — Active-instance IPC hardening: minimum rights, peer identity, squatter rejection

## Status
accepted

## Context
The per-session singleton objects (`Local\Binbuf.Preview3D.<session>.<SID-hash>` mutex, its `.Ready`
event, and `\\.\pipe\Binbuf.Preview3D.<session>.<SID-hash>`) granted the interactive user
`GENERIC_ALL`, which includes `WRITE_DAC`/`WRITE_OWNER`; client authentication checked only the
impersonated token's user SID and session, so any same-user process — including a low-integrity or
AppContainer one — could connect and forward a model path. A process that pre-created or abandoned the
named objects could become the "primary", and a pre-created pipe could capture a forwarded path.

## Decision
- **Minimum-rights security descriptors, no `GENERIC_ALL`.** `SecurityForObject` builds one descriptor
  per object kind: mutex `SYNCHRONIZE|MUTEX_MODIFY_STATE|READ_CONTROL` (`0x120001`), event
  `SYNCHRONIZE|EVENT_MODIFY_STATE|READ_CONTROL` (`0x120002`), pipe `FR|FW`. Each is `D:P` with
  `SY|user` ACEs and carries a medium mandatory label (`S:(ML;;NW;;;LW)`) so a low-integrity same-user
  process cannot open the object for write. `PIPE_REJECT_REMOTE_CLIENTS` is retained.
- **Peer identity beyond SID/session.** After `ImpersonateNamedPipeClient` the server requires the
  client token's integrity RID to be at least medium (a lower-integrity peer is accepted only when it
  is our own child), rejects `TokenIsAppContainer`, and requires the client process image to equal this
  process's image (`GetModuleFileNameW`, so the portable layout works with no hard-coded path). The
  client opens the process before impersonating and reads the image from that handle, so a recycled
  PID cannot substitute another image.
- **Fail closed on pre-existing objects.** Mutex/ready-event creation uses the least privilege and, on
  `ERROR_ALREADY_EXISTS`, `ObjectSecurityMatches` compares the existing owner+DACL against the
  descriptor this process would have created; a mismatch fails `Initialize` instead of adopting a
  squatter's object. The abandoned-mutex recovery path is preserved because a legitimate predecessor
  wrote the same descriptor.
- **The pipe name stays reserved.** The first pipe instance is created with
  `FILE_FLAG_FIRST_PIPE_INSTANCE` (fails closed if a squatter already published the name); each next
  instance is created before the previous one is released, so the reservation never lapses between
  requests. A secondary also verifies the pipe server's image before writing a path.
- **`READY` is manual-reset.** `CreateEventExW` uses `CREATE_EVENT_MANUAL_RESET` (the `CreateEventW`
  `bManualReset=TRUE` semantics it replaced).

## Consequences
- A low-integrity or AppContainer same-user process can no longer connect and forward; a same-user
  medium process must still reproduce the exact minimum DACL and the viewer image to be trusted.
- Residual: object names remain deterministic (`session + SID hash`), so a *medium-integrity*
  same-user process that reads the algorithm can still race to pre-create. It must reproduce the exact
  descriptor and viewer image to pass, and `FILE_FLAG_FIRST_PIPE_INSTANCE` closes the pipe-capture
  window; a per-logon/boot secret was considered but rejected as not reachable by a medium same-user
  peer, so this residual is documented rather than mitigated further here.
- `Tests.Unit [activation]` adds the peer-identity rejection matrix, a minimum-rights DACL assertion,
  and a pre-created-untrusted-mutex rejection; the Debug activation smoke still passes end to end.