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
- [ ] Check `generationDeadline` at the top of the mid-generation loop so a queued backlog cannot be
      serviced indefinitely; expiry terminates the Job and returns the existing typed failure.
- [ ] Use the absolute deadline (not the raw `replyTimeout`) for every read in the progressive-detail
      replay loop, and bound the `nextDetail()==0` wait.
- [ ] Make the `textureBytes`/`texturePixels` accumulation overflow-checked and return `false` on wrap.
- [ ] Regression: a hostile worker that floods buffered messages still terminates at the generation
      deadline (extend the existing `--batches-unbounded` deadline case).

## Out of scope
- The per-category message caps (unchanged); remote-path handling (→ T26).

## Design notes
- Keep `ImportStage::GenerationDeadline` / `ResourceLimit` distinct from `ReplyTimedOut`.
- Do not make `WaitForBufferedBytes` fail a complete buffered message at the exact deadline; check
  the budget at the loop boundary instead, so a legitimate last message is still read.

## Done when
- [ ] `x64\Release\Tests.ImportIsolation.exe` passes with the new case; Debug shows no new failure.
- [ ] Hand-off filled in.

## Hand-off
_(filled in by the implementing session)_