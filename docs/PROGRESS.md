# Progress notes

<!-- symphony:digest:start -->
## Key facts (maintained by symphony — do not edit)

- **Program context (pre-run)**: This roadmap implements the **Explorer thumbnail provider** end to end. The provider was a stub:; Design authority is `docs/design/`. It contains reviewed copies of the original baseline
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

## Follow-ups

- Family viewer release qualification remains open in the viewer program; it does not block provider
  work but must be true before any public claim pairs manager thumbnails with qualified viewer support.
- The original WiX/MSI (Gate 7) is unbuilt; T41 registers through the current installer path and the
  MSI must later adopt the same identities and rules (ADR-0006).
