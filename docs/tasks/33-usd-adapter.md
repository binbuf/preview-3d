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
- [x] Route `.usd`/`.usda`/`.usdc`/`.usdz` to this adapter via its fixed CLSID `{E938BC70-4C08-4446-A15D-EE31576BFB48}`.
- [x] Byte-sniff `.usd`; parse USDA/USDC and contained USDZ layers/textures with TinyUSDZ only.
- [x] Apply the same static scene/material policy within provider limits and feed finite sampled triangles to the shared CPU rasterizer.
- [x] Return the free generic-icon fallback for external references/payloads/sublayers/textures, any composition arc, malformed or over-budget input; never recover a path, start the compatibility host, read the cache or reach the network.
- [x] Add fixtures and goldens: USDA mesh, USDC mesh, USDZ with a contained layer/texture, and external-reference/composition/malformed/oversized cases asserting fallback.

## Out of scope
- OpenUSD and the compatibility host (never in the provider).
- USD-009 viewer qualification (viewer program).

## Design notes
- Only `UnsupportedComposition` is a viewer concept; the provider has no retry path — composition-dependent input is simply the generic icon.
- TinyUSDZ has no allocation/progress callback; preflight counts and rely on the provider's own checked reads and commit cap.
- Never claim success for partial required geometry.

## Done when
- [x] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes in Debug and Release for all four extensions and the fallback cases.
- [x] The isolation test confirms no external layer/texture open and no host launch.
- [x] The family is smoke-checked in Explorer using the T22 procedure.
- [x] Hand-off below filled in.

## Hand-off
Landed: `thumbnail-provider/UsdFamilyAdapter.{h,cpp}` (`UsdAdapter`) registered for `Family::Usd` (CLSID
`{E938BC70-4C08-4446-A15D-EE31576BFB48}`); compiled PCH/COM/GDI-free into the DLL, `Tests.Unit.exe` and
`Tests.ProviderHost.exe`; the worker's `import-worker/src/UsdZipPreflight.{h,cpp}` compiled
source-not-state into all three; coverage `tests/unit/ProviderUsdAdapterTests.cpp` (`[provider][usd]`, 20
cases) over the committed `tests/fixtures/usd-spike` corpus; host fixtures/goldens
`usd-{mesh,crate,zip}-256.pam`; USD added to the local smoke (register/unregister/host `--usd`/orchestrator
+ committed `fixtures/smoke-cube.usda`); docs `design/05-thumbnail-provider.md`,
`design/adapters/usd-010-thumbnail.md` (USD-010), `adr/0026-usd-adapter-pinned-tinyusdz.md`, `PROGRESS.md`,
smoke `README.md`. The two tests that used the previously unlinked USD family as their "no adapter" branch
moved to STEP (T34).

Deviations: (1) contained textures are recorded but not decoded, because the frozen `MaterialPayload` has
no texture slot and the rasterizer samples no texture (recorded in ADR-0026); (2) the provider has no
compatibility-host path, so every composition arc is the generic icon rather than composed geometry
(USD-010 is deliberately stream-only); (3) skeletal bindings are stripped and the authored rest pose is
previewed; (4) the `fast_float` ABI collision with lib3mf was fixed in the dependency (tinyusdz port
`0.9.1#3`) rather than with a consumer guard; (5) TinyUSDZ allocations are outside the product ledger
(recorded for T51).

Check results (x64): Release `Tests.Unit.exe` 316 cases / 134 834 assertions green; Debug 316 / 134 912
green; `Tests.ProviderHost.exe` 5 cases / 99 assertions green; provider Debug+Release builds zero
warnings; `Invoke-ProviderSmoke.ps1` exit 0 with USD reference-vs-Shell `meanAbs=0.0000 maxAbs=0` in
`dllhost.exe` and no `DisableProcessIsolation`. Commands:
`vcpkg install --triplet x64-windows-static-md --x-manifest-root=<repo> --x-install-root=<repo>\vcpkg_installed\x64-windows-static-md`
(to rebuild tinyusdz against the vcpkg `fast_float`);
`msbuild tests\unit\Tests.Unit.vcxproj /p:Configuration=<C> /p:Platform=x64 /p:SolutionDir=<repo>\`;
`x64\<C>\Tests.Unit.exe`; `x64\<C>\Tests.ProviderHost.exe`;
`x64\Release\Tests.ProviderHost.exe "[write-host-goldens]"`; `pwsh -File packaging\smoke\Invoke-ProviderSmoke.ps1`.

Next task must know: T34 (STEP) inherits the "no adapter" branches in `ProviderThumbnailTests.cpp` and the
host `[parallel]` case. T41 must register `.usd/.usda/.usdc/.usdz` with the real installer (the smoke AppID
`{...FB49}` is smoke-only). T51 must measure TinyUSDZ/lib3mf peak commit outside the ledger. Advancing the
TinyUSDZ pin must keep `use-vcpkg-fast-float.patch` aligned with the vcpkg `fast_float` revision lib3mf
uses, or the ASCII parse crashes again. A future texture-sampling task must add a bounded decoder and
revisit ADR-0026.