# T44 — Verify the actual surrogate, DPI, and clean-machine install

> **E2E slice review point.** Completing this task closes the production-hardening and full-registration slice. Run a fresh end-to-end review now; the pipeline may continue to the next task without waiting.

## Goal
Prove the release-blocking Gate 6 exit criteria on a clean installed machine: every registered CLSID
loads into the isolated Shell thumbnail surrogate rather than `explorer.exe`, no value sets
`DisableProcessIsolation`, and thumbnails render at 100%/150%/200% DPI.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — the release-blocking surrogate check and DPI tests.
- `docs/design/08-installation-and-registration.md` — install/uninstall lifecycle.
- `docs/design/adr/0005-safety-bounded-parsing-not-surrogate.md` — why the check is non-negotiable.
- `docs/tasks/03-spike-surrogate-hosting.md` — the detection technique established in the spike.
- `docs/tasks/41-shell-registration.md`, `docs/tasks/42-payload-closure-signing-sbom.md`.

## Scope
- [ ] Install the signed candidate on a clean Windows 11 x64 VM with no developer tooling/ACLs and confirm each of the eight CLSIDs activates from Explorer.
- [ ] Confirm each CLSID loads into the isolated surrogate (e.g. `DllHost.exe`) and not `explorer.exe`, and that no installed registry value sets `DisableProcessIsolation`.
- [ ] Render real files from every family at 100%, 150% and 200% DPI and record the requested `cx` and result.
- [ ] Exercise conflict, NSIS same-version installer rerun as repair, upgrade and uninstall with third-party defaults and per-user associations; confirm unrelated handlers and user defaults survive.
- [ ] Record evidence (process lists, registry exports, screenshots/bitmaps) linked to the candidate binary hashes.

## Out of scope
- Performance numbers (→ T51).
- Viewer-side install lifecycle (viewer program).

## Design notes
- A CLSID found loading in-process in `explorer.exe`, or any `DisableProcessIsolation` value, is a release blocker, not a waivable performance tradeoff.
- If no signing certificate is available, record the unsigned-engineering state and keep this task blocked for the release candidate rather than marking it done.
- Never terminate or restart Explorer as part of the test.

## Done when
- [ ] Clean-machine evidence shows all eight CLSIDs in the isolated surrogate with no isolation opt-out.
- [ ] DPI results for every family are recorded.
- [ ] Conflict/repair/upgrade/uninstall leave unrelated state intact.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
