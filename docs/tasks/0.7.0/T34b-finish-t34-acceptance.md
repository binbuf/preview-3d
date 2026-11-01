---
timeoutMin: 240
verify: python tests/render-quality/verify.py --task T34 --configuration Debug --output TestResults/render-quality/T34
---
# T34b — Finish T34 acceptance: full-corpus gate, docs and hand-off

## Goal

Turn T34's already-committed implementation into a truthful accepted task: one green full-corpus gate, the one corpus-root accounting fix it still needs, and the T34 hand-off/progress/doc updates that were never written.

## Context (read first)

- T34 never reached acceptance. Two runs exist (local harness logs, not committed): the first hit the 9.7 h wall-clock timeout after sitting idle for roughly seven hours before it launched the final gate; the second was interrupted after 17 s. The first run's commit **6ea6b5a** carried the implementation (2,140 insertions across 22 files), so the code is not the missing piece — the passing run and the bookkeeping are.
- Evidence that already passes:
  - `TestResults/render-quality/T34-dev/report.json` — focused `--fast` run, 47/47 checks, portable 3MF/FBX/USD/OBJ/STEP, hostile inputs, lifecycle, Debug-only controls. `--fast` skips the external corpus and the review gate, so it is **not** acceptance evidence.
  - `TestResults/render-quality/T34-capture/report.json` — capture-only external run, 230 cases, contract self-tests pass, F3D references `available`.
- Evidence that has never existed: `TestResults/render-quality/T34/report.json`. The directory stops after the capture images, so the front-matter gate has never been evaluated end to end.
- Known blocker: the local corpus root contains `desktop.ini`. `tests/render-quality/corpus.py` skips only names starting with `.` (added for `.git`), so `desktop.ini` enters `rootAssets`; `full_corpus._external_audit` then reports `unaccounted corpus root entries: desktop.ini` and fails the gate. The current inventory has 25 entries: 19 profile assets, 5 size-capped `dispositions`, and `desktop.ini`.
- Implementation under test: `tests/render-quality/profiles/T34.json`, `tests/render-quality/full_corpus.py`, `tests/render-quality/verify.py`, `tests/compat/manifest.json`, `tests/render-quality/review/T34-review.json`, plus the 3MF/FBX/USDZ fixtures and the hostile UV/material/texture tests.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [x] Fix corpus-root accounting for OS metadata. Treat `desktop.ini`, `Thumbs.db` and equivalent directory metadata as non-corpus in one shared helper so `corpus.inventory` and `full_corpus._external_audit` agree; keep the existing `.git` behaviour. Add a focused check that a root containing metadata plus real entries still reports zero unaccounted entries and still counts the real entries. Do not add per-asset-name exceptions or widen `maxEntryBytes`.
- [x] Run the front-matter gate exactly as written, in the foreground with an explicit long timeout (900000 ms or more), after the changed binaries are built. Do not background-then-poll and do not repeat it without a change. The gate repeats Debug/Release builds and the serial import lanes.
- [x] Require a passing `TestResults/render-quality/T34/report.json` with `status: pass`, `fast: false`, `captureOnly: false`, `failures: []`, `externalQualification.status: available`, `fullCorpus.status: pass`, `fullCorpus.external.unaccountedRootEntries: []`, `review.status: recorded`, and `reference.f3d.status: available`.
- [x] If the gate finds a real failure, fix the cause and rerun once; keep the first-failure log. Never convert a failure into a capture-only/`--fast` pass, a skip, a self-created golden or a relaxed threshold.
- [x] Confirm the accepted matrix actually covers: all 19 external assets captured or typed-dispositioned, the five size-capped dispositions, Car windshield/paint and Camera lens/lettering in all three modes, portable FBX binary/hierarchy, USD crease/USDZ, 3MF, textured OBJ and STEP, hostile oversized-image/NaN-Inf-UV rejection before GPU upload, and the lifecycle/device-loss/low-budget/Release-probe/dependency-closure lanes.
- [x] Complete the T34 task file: tick the six **Scope** and three **Done when** boxes that the passing report proves, leave anything unproven unticked with a named reason, and replace the `## Hand-off` "Pending" text with what landed in 6ea6b5a, deviations, exact build/verify commands and results, image/review manifest paths, measured-cost evidence and remaining defects. Note that "T23 verification" in T34's Done-when is a typo for T34.
- [x] Write `docs/0.7.0/progress/T34.md` (reusable facts, gotchas, exact commands) with a `## Follow-ups` section for the carry-forward defects.
- [x] Update the docs T34's Done-when requires, preserving history: `docs/FORMAT-SUPPORT.md` (extended portable compatibility manifest and generated fixtures plus typed local-corpus dispositions), `docs/WINDOWS-SECURITY.md` (hostile-input pre-GPU rejection, device-loss/low-budget recovery, parser/codec dependency closure), the rendering record (`docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`) with the observed matrix and known defects, and the task-set README/ROADMAP status markers. Do not commit restricted models or screenshots, and do not hand-edit `docs/0.7.0/PROGRESS.md` (harness-maintained).
- [x] Record the carry-forward dispositions for T35: mint-swimsuit FBX typed `MalformedData` failure, food-collection fitted framing, Car window/trim authored speckle, and the F3D 3.5.0 reference limitation (pale Camera lens). These are recorded defects, not successful images.

## Verification

The front-matter command is T34's gate, reused deliberately: T34b completes the same lane, so it has no separate profile and must not create a parallel `T34b-review.json` that could drift from `tests/render-quality/review/T34-review.json`. The wrapper builds changed binaries first, runs the contract negative self-tests, the restricted external corpus, the portable fixture lane, serial unit/import/security lanes, the review-record gate and F3D references, and writes the report to `TestResults/render-quality/T34`.

### Verification economy

- Run the gate once for acceptance, in the foreground with a long explicit timeout (900000 ms or more). The last T34 failure was an inactivity stall, not a slow gate; launch the gate promptly and wait for completion.
- Use `--fast` (and `--no-build` once the binary is current) only for iteration; it is not acceptance evidence.
- If the gate fails, change the cause and rerun at most once more; keep the first-failure log instead of counting a later passing rerun.
- Do not re-promote references or rebuild the corpus baseline per fix.

## Done when

- [x] The metadata root-entry fix is covered by a focused check, and the full audit reports zero unaccounted corpus root entries.
- [x] `TestResults/render-quality/T34/report.json` exists with the passing fields listed in Scope, produced by the exact front-matter command.
- [x] T34's checkboxes and Hand-off, `docs/0.7.0/progress/T34.md`, and the compatibility/security/rendering/task-set docs are updated with commands, results and unresolved defects.
- [x] Hand-off below is filled.

## Out of scope

- New format families, parser/sandbox changes, or weakening broker/budget limits.
- Rendering-quality fixes themselves (T35 owns release-blocking visual defects) and baseline promotion.
- Resetting or rewriting T34 history, or accepting missing external evidence.

## Design notes

Keep worker-only parsing/decoding, brokered sidecars, validated bounded wire data, generation/fence retirement, GPU budget and feature-level 11_0 unchanged. The metadata filter belongs beside the existing `.git` filter in the corpus inventory/audit path, not in a per-profile disposition list. Preserve the distinction between local corpus evidence (not committed) and the checked-in portable manifest. Do not add Release test-control authority or per-asset-name rendering hacks.

## Hand-off

T34 is accepted. The passing report is
`TestResults/render-quality/T34/report.json` (`status: pass`, `failures: []`,
`fast: false`, `captureOnly: false`, `externalQualification.status: available`,
`fullCorpus.status: pass`, `fullCorpus.external.unaccountedRootEntries: []`,
`review.status: recorded`, `reference.f3d.status: available`; 74/74 checks,
226 captures), produced by the exact front-matter command:

```powershell
python tests/render-quality/verify.py --task T34 --configuration Debug --output TestResults/render-quality/T34
```

### Changes

- `tests/render-quality/corpus.py`: one shared `is_corpus_entry(name)` rule with
  `OS_METADATA_NAMES` (`desktop.ini`, `Thumbs.db`, `ehthumbs*.db`,
  `Thumbs.db:encryptable`, `$RECYCLE.BIN`, `System Volume Information`) beside
  the existing leading-dot (`.git`) rule. `inventory()` now uses it.
- `tests/render-quality/full_corpus.py`: `_external_audit()` filters root names
  through the same helper and derives `rootEntryCount` from the filtered set.
  Added `_corpus_accounting_check()` with three focused checks: metadata is not
  an asset entry; a metadata-plus-real-entry root reports zero unaccounted
  entries and counts the real entry; an audit that omits the real entry still
  flags it. No per-asset-name exceptions and no `maxEntryBytes` change.
- `full_corpus._release_probe()` fixed: it had launched the Release viewer with
  the Debug-only `--app-smoke` flag and therefore always saw `viewer exited 2`.
  It now requires `Release/Preview3D.exe --app-smoke` to exit nonzero and then
  launches Release normally, requiring control fields 0/105/107/300 to return
  0. This was a real T34 defect found by the first acceptance run.

### Gate history

The first run failed only on the Release probe above. The cause was fixed and
the gate rerun once; no failure was converted to a `--fast`/capture-only pass,
skip, self-created golden or relaxed threshold. First-failure evidence:
`TestResults/render-quality/T34-gate-first-failure.log` and
`TestResults/render-quality/T34-first-failure-report.json`.

### Matrix confirmed

24 corpus root entries: 19 normal assets captured or typed (18 captured; mint
FBX is a typed `MalformedData` open-failure) plus the five size-capped
`resource-budget` dispositions. Car windshield/paint and Camera lens/lettering
have all three modes (12/12 mandatory checks). Portable FBX binary/hierarchy,
USDZ, USD crease, 3MF, textured OBJ and STEP captured. Oversized image falls
back before upload; NaN/Inf UV is rejected with code 3 at stage 15. Lifecycle,
device loss, 32 MiB low-budget `ResourceLimit`, Release control rejection and
clean dependency closure all pass. F3D 3.5.0 references rendered (qualitative).

### Docs and evidence

Updated: `docs/tasks/0.7.0/T34-full-corpus-render-regressions.md` (checkboxes
and full hand-off, with the T23→T34 typo noted), `docs/0.7.0/progress/T34.md`,
`docs/0.7.0/progress/T34b.md`, `docs/FORMAT-SUPPORT.md` (portable manifest/
fixtures and typed local-corpus dispositions), `docs/WINDOWS-SECURITY.md`
(pre-GPU hostile rejection, recovery and dependency closure),
`docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md` (observed matrix and defects),
`docs/tasks/0.7.0/README.md`, `docs/0.7.0/README.md` and
`docs/design/adr/0091-corpus-root-metadata-accounting.md`. The
harness-owned `ROADMAP.md` markers and `PROGRESS.md` were not edited.
Evidence: `T34/images/`, `captures.json`, `full-corpus-report.md`,
`diagnostics/full-corpus.json`, `external/matrix.json`, `corpus-inventory.json`,
`reference/` and `tests/render-quality/review/T34-review.json`. No restricted
model or screenshot was committed.

### Remaining work for T35

Mint-swimsuit FBX typed `MalformedData` (recorded failure image), food-collection
fitted framing, Car window/trim authored speckle, and the F3D 3.5.0 DPI-aware
preset/pale Camera lens limitation. The five size-capped corpus assets stay
typed render-lane dispositions; do not raise the cap or add per-asset exceptions.
