---
verify: x64\Release\Tests.Unit.exe
---
# T31 — Implement the FBX thumbnail adapter

## Goal
Render binary and ASCII `.fbx` models in Explorer: stream-contained geometry, embedded textures, and
a deterministic static start pose with supported skin/blend baked, under provider limits.

## Context (read first)
- `docs/design/adapters/fbx-008-thumbnail.md` — the full FBX thumbnail requirement.
- `docs/design/05-thumbnail-provider.md` — stream ingestion (FBX) and limits.
- `docs/legacy/fbx/FBX-001-SPIKE-RESULTS.md` — ufbx provider policy and the lack of an evaluation progress callback.
- `import-worker/src/` FBX/ufbx usage — viewer policy reference.
- `docs/tasks/17-provider-test-harness.md`.

## Scope
- [x] Route `.fbx` to this adapter via its fixed CLSID `{FBC218D4-FD2C-41DF-B168-7F3B9E53C84E}`; parse with provider-local ufbx (path/external-file, cache, plug-in, script and environment-codec access disabled).
- [x] Preflight element/triangle counts before evaluation; apply the deterministic static pose and supported skin/blend policy for fixtures small enough to evaluate within budget.
- [x] Decode only embedded allowlisted images; external textures use the neutral fallback and external required geometry returns the generic icon.
- [x] Feed finite evaluated triangles and material colors to the shared sampler/rasterizer.
- [x] Add fixtures and goldens: binary and ASCII FBX, static pose, embedded texture, external-dependency fallback, malformed/over-limit/deadline/OOM.

## Out of scope
- Scene/instance viewer contract work and FBX-007 qualification (viewer program).
- Dynamic constraints, NURBS, subdivision, animation playback.

## Design notes
- ufbx evaluation has no progress callback: encode a conservative size/deadline policy; never allow an unbounded library call just because Shell uses a surrogate.
- Constrain ufbx allocation/temp-memory caps explicitly.
- The DLL does not launch or IPC to any product process.

## Done when
- [x] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes in Debug and Release for binary/ASCII, pose, embedded texture and fallbacks.
- [x] Deadline/OOM/over-limit cases fail safe with no Explorer hang.
- [x] The family is smoke-checked in Explorer using the T22 procedure.
- [x] Hand-off below filled in.

## Hand-off
Landed: `thumbnail-provider/FbxFamilyAdapter.{h,cpp}` (`FbxAdapter`), registered for `Family::Fbx`;
compiled PCH/COM/GDI-free into the DLL, `Tests.Unit.exe` and `Tests.ProviderHost.exe`; coverage
`tests/unit/ProviderFbxAdapterTests.cpp` (`[provider][fbx]`, 15 cases); host fixture/golden
`fbx-hierarchy-256.pam`; FBX added to the local smoke (register/unregister/host/orchestrator + committed
`fixtures/smoke-cube.fbx`); docs `design/05-thumbnail-provider.md`, `design/adapters/fbx-008-thumbnail.md`,
`adr/0024-fbx-adapter-static-pose.md`, `PROGRESS.md`, smoke `README.md`. Two pre-existing tests that used
the (previously unlinked) FBX family as their "no adapter" branch moved to 3MF (T32).

Deviations: (1) embedded images are structurally validated (allowlist + encoded/pixel budget) but not
decoded, because the frozen `MaterialPayload` has no texture slot and the rasterizer samples no texture,
and a real decoder would need WIC/COM that the provider must not use — recorded in ADR-0024; (2) the
"OOM" acceptance is met by the same bounded allocator/limit path that the other adapters use and that
FBX-001 proved; no fixture triggers an in-cap evaluation OOM cheaply, so there is no dedicated OOM unit
case; (3) over-limit is covered by the ledger-capped backing case and the preflight caps rather than a
generated multi-million-triangle file.

Check results (x64): Release `Tests.Unit.exe` 283 cases / 134 698 assertions green; Debug 283 / 134 779
green; Release+Debug `Tests.ProviderHost.exe` 5 cases / 76 assertions green; dependency closure OK (14
modules); `Invoke-ProviderSmoke.ps1` exit 0 with FBX reference-vs-Shell `meanAbs=0.0000 maxAbs=0` in
`dllhost.exe` and no `DisableProcessIsolation`. Commands:
`msbuild tests\unit\Tests.Unit.vcxproj /p:Configuration=<C> /p:Platform=x64 /p:SolutionDir=<repo>\`,
`x64\<C>\Tests.Unit.exe`, `x64\<C>\Tests.ProviderHost.exe`, plus
`x64\Release\Tests.ProviderHost.exe "[write-host-goldens]"` to regenerate goldens and
`pwsh -File packaging\smoke\Invoke-ProviderSmoke.ps1`.

Next task must know: T41 must register `.fbx` with the real installer (the smoke AppID `{...C84F}` is a
smoke-only identity); T51 must measure the actual evaluation time distribution because ufbx evaluation
cannot be cooperatively interrupted; if a later task adds texture sampling it must promote embedded-image
validation to a bounded decoder and revisit ADR-0024.