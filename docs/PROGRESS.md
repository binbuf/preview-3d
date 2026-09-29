# Progress notes

<!-- symphony:digest:start -->
## Key facts (maintained by symphony — do not edit)

- **Program context (pre-run)**: This roadmap implements the **Explorer thumbnail provider** end to end. The provider was a stub:; Design authority is `docs/design/`. It contains reviewed copies of the original baseline
- **T01 — Freeze the provider specification and eight-family roster**: Specification authority is frozen. Eight CLSIDs, one per family, in; `design/05-thumbnail-provider.md` now has a "Frozen specification" section. It records: roster
- **Follow-ups**: Family viewer release qualification remains open in the viewer program; it does not block provider; The original WiX/MSI (Gate 7) is unbuilt; T41 registers through the current installer path and the
<!-- symphony:digest:end -->

Shared notebook for the symphony run. Each task session appends a "## Txx — title" section with what
later tasks need to know: real paths, commands that work, contract deviations, gotchas. Facts, not
narrative. The harness keeps a generated "Key facts" digest at the top (between the symphony:digest
markers); sessions are pointed at this file and read it themselves.

## Program context (pre-run)

- This roadmap implements the **Explorer thumbnail provider** end to end. The provider was a stub:
  `thumbnail-provider/dllmain.cpp` is 21 lines and exports nothing.
- Design authority is `docs/design/`. It contains reviewed copies of the original baseline
  (`01`–`11`), plus `overview.md`, `testing-strategy.md`, per-family briefs under
  `design/adapters/`, and decisions under `design/adr/` (0001–0007).
- The original planning tree and family plans were archived to `docs/legacy/`. Family viewer
  qualification (FBX-007, 3MF-007, USD-009, STEP-008) stays with the viewer program and is referenced,
  never claimed, here.
- Build only through the solution: `msbuild Preview3D.slnx /p:Configuration=<Debug|Release> /p:Platform=x64`.
  Catch2 suites run from the repository root as `x64\Release\Tests.Unit.exe` and
  `x64\Release\Tests.ImportIsolation.exe`. There is no CI.
- Verification: the harness runs each task's front-matter `verify:` command, falling back to the
  global `verifyCommand` / root `package.json` `test` script (`x64\Release\Tests.Unit.exe`). Every code
  task must set `verify:` to its already-built test binary; spike/installer/VM/perf tasks leave it
  unset and record evidence.
- Baseline gotcha: the full `Tests.ImportIsolation.exe` suite is currently red on pre-existing USD
  protocol/spike and hidden large-scan cases. Do not gate on the unfiltered suite; use a scoped
  Catch2 filter (e.g. `[stl-import],[ply-import],[gltf-import]`) or `Tests.Unit.exe`.
- Eight families are in scope (ADR-0001): glTF, STL, PLY, OBJ, FBX, 3MF, USD, STEP. OCCT links only
  into a separate bounded adapter in the DLL (ADR-0002); decoder scope is ADR-0003.

## T01 — Freeze the provider specification and eight-family roster

- Specification authority is frozen. Eight CLSIDs, one per family, in
  `design/05-thumbnail-provider.md` (CLSID table) and `design/overview.md` (target roster). STEP =
  `{6EE961AC-AC3B-4958-A898-E30523FEE79D}`. The seven pre-existing CLSIDs are product identities;
  never regenerate or rename any of them.
- `design/05-thumbnail-provider.md` now has a "Frozen specification" section. It records: roster
  (ADR-0001), in-DLL constrained OCCT adapter (ADR-0002), decoder scope (ADR-0003), NSIS-now/MSI-later
  registration with provisional extension-level `ShellEx` until T03 (ADR-0006/0007).
- Limit/HRESULT source of truth for T06: the *Thumbnail host* column of
  `design/03-file-formats-and-ingestion.md` plus the HRESULT table in `design/05-thumbnail-provider.md`.
  Hard accountable caps: 256 MiB stream max, 128 MiB contiguous backing, 192 MiB scratch, per-component
  decode caps, 384 MiB product-owned ledger. The 384 MiB *total process private commit above the idle,
  loaded surrogate baseline* is a measured release target (T51), not a ledger-enforced ceiling. The 2 s
  point is a cooperative stop check, not an interruptible call timeout.
- ADR-0001–0007 are all `accepted` (verified, unchanged). The `design/` copies already agreed with them;
  T01 only fixed residual wording: `02` (ADR-0002 supersession note), `11` TSK-209 (ADR-0003 supersedes
  its "does not link either decoder" sentence), `08` (freeze + provisional ShellEx note).
- No normative "seven CLSID"/"seven stable" statement remains; only historical quotes in `overview.md`,
  `adr/0001` and `adr/0007`. Verify with
  `Select-String -Path docs/design/*.md,docs/design/adr/*.md -Pattern 'seven'`.

## Follow-ups

- Family viewer release qualification remains open in the viewer program; it does not block provider
  work but must be true before any public claim pairs manager thumbnails with qualified viewer support.
- The original WiX/MSI (Gate 7) is unbuilt; T41 registers through the current installer path and the
  MSI must later adopt the same identities and rules (ADR-0006).
