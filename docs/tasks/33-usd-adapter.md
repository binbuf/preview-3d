---
verify: x64\Release\Tests.Unit.exe
---
# T33 — Implement the USD/USDZ thumbnail adapter

## Goal
Render stream-contained `.usd`, `.usda`, `.usdc` and `.usdz` models in Explorer through TinyUSDZ,
with no OpenUSD, no composition, and no external layer resolution.

## Context (read first)
- `docs/design/adapters/usd-010-thumbnail.md` — the "USD-010" section.
- `docs/design/05-thumbnail-provider.md` — stream ingestion (USD) and limits.
- `docs/design/03-file-formats-and-ingestion.md` — the USD static subset.
- `docs/legacy/` USD-001 spike results — the shared static-time and subset policy.
- `docs/tasks/17-provider-test-harness.md`.

## Scope
- [ ] Route `.usd`/`.usda`/`.usdc`/`.usdz` to this adapter via its fixed CLSID `{E938BC70-4C08-4446-A15D-EE31576BFB48}`.
- [ ] Byte-sniff `.usd`; parse USDA/USDC and contained USDZ layers/textures with TinyUSDZ only.
- [ ] Apply the same static scene/material policy within provider limits and feed finite sampled triangles to the shared CPU rasterizer.
- [ ] Return the free generic-icon fallback for external references/payloads/sublayers/textures, any composition arc, malformed or over-budget input; never recover a path, start the compatibility host, read the cache or reach the network.
- [ ] Add fixtures and goldens: USDA mesh, USDC mesh, USDZ with a contained layer/texture, and external-reference/composition/malformed/oversized cases asserting fallback.

## Out of scope
- OpenUSD and the compatibility host (never in the provider).
- USD-009 viewer qualification (viewer program).

## Design notes
- Only `UnsupportedComposition` is a viewer concept; the provider has no retry path — composition-dependent input is simply the generic icon.
- TinyUSDZ has no allocation/progress callback; preflight counts and rely on the provider's own checked reads and commit cap.
- Never claim success for partial required geometry.

## Done when
- [ ] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes in Debug and Release for all four extensions and the fallback cases.
- [ ] The isolation test confirms no external layer/texture open and no host launch.
- [ ] The family is smoke-checked in Explorer using the T22 procedure.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_