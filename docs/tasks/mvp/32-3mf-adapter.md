---
verify: x64\Release\Tests.Unit.exe
---
# T32 — Implement the 3MF thumbnail adapter

## Goal
Render the supported static `.3mf` preview subset in Explorer from a stream, including contained
colors/textures and a bounded Beam Lattice representation, under provider ceilings.

## Context (read first)
- `docs/design/adapters/3mf-thumbnail.md` — the "3MF-008 — Explorer thumbnail adapter" section.
- `docs/design/05-thumbnail-provider.md` — stream ingestion (3MF contained entries) and limits.
- `docs/design/03-file-formats-and-ingestion.md` — 3MF preflight/policy.
- `docs/legacy/design/` 3MF task history and `3MF-001-SPIKE-RESULTS.md` — lib3mf behavior notes.
- `docs/tasks/17-provider-test-harness.md`.

## Scope
- [x] Route `.3mf` to this adapter via its fixed CLSID; read exclusively from the stream and launch no worker.
- [x] Reuse the product OPC/ZIP preflight and required-extension policy under provider limits: 256 MiB stream, 128 MiB aggregate expansion, 100:1 ratio, 192 MiB accounted scratch and 384 MiB measured process-commit increase target. Record lib3mf allocations outside allocator accounting.
- [x] Link the approved bounded 3MF reader (lib3mf) only into the thumbnail DLL; never the viewer.
- [x] Sample the complete root build deterministically across build items and spatial regions; include a bounded representation of supported lattice geometry and standard materials/colors.
- [x] Add fixtures and goldens: Core mesh, Production multi-part, Materials color/texture, bounded Beam Lattice, and unsupported-required-extension/over-budget/malformed cases.

## Out of scope
- Slicer-private plate metadata (not needed for a thumbnail).
- Viewer 3MF qualification (3MF-007, viewer program).

## Design notes
- Unsupported required extensions or over-budget content fall back to the generic icon; never render a partial required scene as success.
- Contained PNG/JPEG attachments only; use the shared bounded image decoders.
- Keep lib3mf compatible-mode usage behind the product OPC/XML boundary as in the viewer adapter.

## Done when
- [x] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes in Debug and Release for supported and fallback cases.
- [x] The DLL's dependency closure includes lib3mf's runtime but no viewer/worker binary.
- [x] The family is smoke-checked in Explorer using the T22 procedure.
- [x] Hand-off below filled in.

## Hand-off
Landed: `thumbnail-provider/ThreeMfFamilyAdapter.{h,cpp}` (`ThreeMfAdapter`), registered for
`Family::ThreeMf`; compiled PCH/COM/GDI-free into the DLL, `Tests.Unit.exe` and `Tests.ProviderHost.exe`;
the worker's `import-worker/src/ThreeMfOpcPreflight.{h,cpp}` compiled source-not-state into all three;
coverage `tests/unit/ProviderThreeMfAdapterTests.cpp` (`[provider][3mf]`, 16 cases) over the committed
`tests/fixtures/3mf-spike/*.3mf.base64` corpus; host fixtures/goldens
`three-mf-core-256.pam`/`three-mf-lattice-256.pam`; 3MF added to the local smoke
(register/unregister/host `--mf`/orchestrator + committed `fixtures/smoke-cube.3mf`); docs
`design/05-thumbnail-provider.md`, `design/adapters/3mf-thumbnail.md` (3MF-008), `adr/0025-3mf-adapter-opc-and-lib3mf.md`,
`PROGRESS.md`, smoke `README.md`. The two tests that used the (previously unlinked) 3MF family as their
"no adapter" branch moved to USD (T33).

Deviations: (1) contained textures are structurally validated (PNG/JPEG sniff + encoded/aggregate-pixel
budget) but not decoded, because the frozen `MaterialPayload` has no texture slot and the rasterizer
samples no texture (recorded in ADR-0025); (2) the adapter always tessellates a lattice and does not
prefer an authored `representationmesh`, still bounded and recognizable; (3) `outside`/non-box clipping
without a representation mesh returns `UnsupportedRequiredFeature`; (4) lib3mf allocations are outside
the product ledger (recorded for T51); (5) lib3mf is statically linked, so the dependency closure
contains lib3mf's reader code *inside* the DLL rather than a separate runtime import.

Check results (x64): Release `Tests.Unit.exe` 299 cases / 134 766 assertions green; Debug 299 / 134 844
green; Release+Debug `Tests.ProviderHost.exe` 5 cases / 85 assertions green; dependency closure OK
(20 modules, no `preview3d*` imports); `Invoke-ProviderSmoke.ps1` exit 0 with 3MF reference-vs-Shell
`meanAbs=0.0000 maxAbs=0` in `dllhost.exe` and no `DisableProcessIsolation`. Commands:
`msbuild tests\unit\Tests.Unit.vcxproj /p:Configuration=<C> /p:Platform=x64 /p:SolutionDir=<repo>\`,
`x64\<C>\Tests.Unit.exe`, `x64\<C>\Tests.ProviderHost.exe`, plus
`x64\Release\Tests.ProviderHost.exe "[write-host-goldens]"` to regenerate goldens and
`pwsh -File packaging\smoke\Invoke-ProviderSmoke.ps1`.

Next task must know: T41 must register `.3mf` with the real installer (the smoke AppID `{...84BA}` is a
smoke-only identity); the `ProviderThumbnailTests.cpp` "no adapter" branch and the host `[parallel]` branch
now use `Family::Usd` and must move to STEP when T33 links USD; the required-extension scan reads only the
root `<model>` start tag; T51 must measure lib3mf peak commit outside the ledger; a future texture-sampling
task must promote structural image validation to a bounded decoder and revisit ADR-0025.
