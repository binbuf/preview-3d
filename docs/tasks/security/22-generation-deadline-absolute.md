---
verify: x64\Release\Tests.ImportIsolation.exe
---

# T22 — Make the broker generation wall-clock deadline absolute (SEC-03 completion)

## Goal
A worker cannot spend unbounded host time past the per-generation budget by pre-buffering messages
or by stretching the progressive-detail replay, and the texture byte accounting cannot wrap.

## Context (read first)
- `docs/tasks/security/03-sandbox-and-broker-limits.md` — the deadline and `boundedReplyWait`.
- `shared/import-broker/src/ControlChannelWait.cpp:26-28` returns `Ready` for an already-buffered
  message before the deadline check at `:32`; a worker that pipelines valid messages is serviced
  past `generationDeadline` until the per-category caps trip.
- `shared/import-broker/src/ImportSession.cpp:820-832` computes `boundedReplyWait`, but the
  progressive-detail replay loop `:1531-1597` uses the full `replyTimeout` at `:1551` and never
  consults `generationDeadline`; `:1545` spins with `Sleep(20)` while `nextDetail()` returns 0.
- `ImportSession.cpp:373-379` `BatchAcceptance::Record` `memcpy`s the header and adds
  `header.pixelDataByteSize` to `textureBytes` without an overflow check (the validator caps the
  aggregate today, so this is latent).

## Scope
- [x] Check `generationDeadline` at the top of the mid-generation loop so a queued backlog cannot be
      serviced indefinitely; expiry terminates the Job and returns the existing typed failure.
- [x] Use the absolute deadline (not the raw `replyTimeout`) for every read in the progressive-detail
      replay loop, and bound the `nextDetail()==0` wait.
- [x] Make the `textureBytes`/`texturePixels` accumulation overflow-checked and return `false` on wrap.
- [x] Regression: a hostile worker that floods buffered messages still terminates at the generation
      deadline (extend the existing `--batches-unbounded` deadline case).

## Out of scope
- The per-category message caps (unchanged); remote-path handling (→ T26).

## Design notes
- Keep `ImportStage::GenerationDeadline` / `ResourceLimit` distinct from `ReplyTimedOut`.
- Do not make `WaitForBufferedBytes` fail a complete buffered message at the exact deadline; check
  the budget at the loop boundary instead, so a legitimate last message is still read.

## Done when
- [x] `x64\Release\Tests.ImportIsolation.exe` passes with the new case; Debug shows no new failure.
- [x] Hand-off filled in.

## Hand-off

### What landed
- `shared/import-broker/src/ImportSession.cpp`
  - The mid-generation reply loop now enforces `generationDeadline` at its top for every *non-terminal*
    message before servicing it. `ReadControlMessageBounded`/`WaitForBufferedBytes` returns `Ready`
    for a message already buffered in the pipe before it consults the deadline, so without this a
    worker pipelining valid mid-generation messages (without waiting for their replies) could be
    serviced past the budget until a per-category cap happened to trip. A timeout past the deadline
    still maps to `ImportStage::GenerationDeadline` / `ErrorCode::ResourceLimit` (the pre-existing
    typed failure). Terminal `ChunksReady`/`GenerationError` replies are deliberately exempt so a
    legitimate last message is still read, per the task's design note.
  - The progressive-detail replay loop is now bounded by the same absolute deadline: a
    `generationDeadline` check at the top of the loop bounds the `nextDetail()==0` spin, and the
    `RequestDetail` reply read uses `boundedReplyWait()` instead of the raw `replyTimeout`. A read
    that times out after the deadline maps to `GenerationDeadline` rather than `AwaitReply`.
  - `BatchAcceptance::Record` accumulates `textureBytes`/`texturePixels` through
    `platform::CheckedAdd` and returns `false` on wrap (which the caller already maps to
    `MalformedData`), instead of an unchecked `+=` that relied on the validator's aggregate cap.
- `tests/hostile-worker/src/{AttackModes.h,AttackModes.cpp,main.cpp}`
  - New `--batches-buffered-flood` mode: writes one well-formed zero-chunk section once, drains the
    host's acks on a helper thread, and then sends zero-chunk `ChunkBatchReady` notices as fast as it
    can without waiting for them. Because the bytes never change there is no writer/validator race,
    and because the chunk total stays zero the chunk caps cannot fire — the absolute deadline is the
    only limit that can stop it.
- `tests/import-isolation/ChunkBatchTests.cpp`
  - New `[chunk-batch]` case "A worker that pipelines buffered batches still stops at the generation
    deadline": the flood worker with the batch cap at 1,000,000, `maxChunksPerGeneration` at
    1,000,000 and a 400 ms generation budget, asserting `GenerationDeadline` / `ResourceLimit` and
    `elapsed < 20 s`.

### Tests
- `x64\Release\Tests.ImportIsolation.exe "[chunk-batch]"`: 99 assertions / 21 cases, exit 0.
- `x64\Release\Tests.ImportIsolation.exe` full: 405 cases, 400 passed, 5 skipped, 0 failed, exit 0.
- `x64\Debug\Tests.ImportIsolation.exe` full: 405 cases, 404 passed, 1 skipped, 0 failed, exit 0.
- No new Debug failure. The documented pre-existing failures did not reproduce in this checkout's
  Debug run; either way nothing was added.

### Deviations / notes
- The regression uses a new `--batches-buffered-flood` hostile mode rather than reusing
  `--batches-unbounded`, because the latter waits for each batch's ack and therefore never keeps a
  message buffered. The new mode is the minimal worker that exhibits the audited condition; the two
  cases share `MakeBatchRequest`, the deadline value and the assertions.
- The terminal-reply exemption is the explicit resolution of the scope's "check at the loop boundary"
  against the design note's "a legitimate last message is still read": a terminal reply buffered at
  or just past the deadline still completes the generation, while any further mid-generation message
  after the deadline fails closed.
- The `nextDetail()==0` spin is bounded only by the deadline check at the loop top; there is no
  separate iteration cap, matching the design's single absolute budget.

### Docs changed
- `docs/tasks/security/22-generation-deadline-absolute.md` (this hand-off).
- `docs/security/PROGRESS.md` (T22 reusable facts).
- No ADR: this completes the enforcement ADR 0031 already records (the generation wall-clock
  deadline). ADR 0031's `min(replyTimeout, ceil(remaining))` rule was not yet applied to the
  progressive-detail replay read; the code now matches it. No new limit or public constant was
  introduced.