---
timeoutMin: 300
verify: x64\Release\Tests.Unit.exe
---
# T34 — Implement the STEP/STP thumbnail adapter

> **E2E slice review point.** Completing this task closes the Tier B breadth slice. Run a fresh end-to-end review now; the pipeline may continue to the next task without waiting.

## Goal
Render self-contained `.step`/`.stp` visual geometry in Explorer through a separately built,
constrained OCCT adapter linked only into the thumbnail DLL — the last and heaviest family — and
qualify it before the full eight-family registration/release claim.

## Context (read first)
- `docs/design/adr/0002-occt-linked-into-thumbnail-adapter.md` — the linkage decision, ceilings and contingency.
- `docs/design/adapters/step-009-thumbnail.md` — the "STEP-009" section.
- `docs/design/05-thumbnail-provider.md` — stream ingestion and limits.
- `docs/tasks/06-budgets-deadlines-hresults.md` — provider ceilings (T34 uses the stricter provider values, not the host's).
- `docs/tasks/17-provider-test-harness.md`.

## Scope
- [ ] Route `.step`/`.stp` through the frozen STEP CLSID `{6EE961AC-AC3B-4958-A898-E30523FEE79D}`.
- [ ] Build a separately constrained OCCT static-library adapter and link it only into the thumbnail DLL; confirm the DLL imports no viewer/worker/host executable or library. The existing STEP host retains its own separate OCCT closure.
- [ ] Consume only `IInitializeWithStream` through a bounded seekable stream; run product-owned Part-21 admission before OCCT, rejecting external declarations, unsupported content and over-budget input.
- [ ] Use a fixed low-detail deterministic tessellation/sample policy: 256 MiB stream, 192 MiB accounted parser/tessellation scratch, 384 MiB measured process-commit increase target, reduced triangle cap, cooperative 2 s stop point / 750 ms p95. Measure actual OCCT-call overruns and unaccounted allocation.
- [ ] Preserve bounded shape colors and complete-assembly spatial representation where possible; never launch `Preview3DStepHost.exe`.
- [ ] Add fixtures and goldens: self-contained AP203/AP214/AP242 mesh/assembly, colors, and external/unsupported/over-budget/malformed cases; measure time and commit.
- [ ] If qualification fails, record raw timing/commit evidence and keep T34 incomplete. Replan via a scope-changing ADR and update the roadmap, registration and public release claims before proceeding; do not treat an always-generic STEP handler as completed thumbnail support.

## Out of scope
- STEP-008 viewer/host qualification (viewer program).
- External STEP documents (out of scope by recorded product decision).

## Design notes
- OCCT throws `Standard_Failure`, not `std::exception`; catch it explicitly at the adapter boundary.
- The provider must not create OCCT global state that outlives a call; keep per-call state bounded.
- This is the highest-risk family. A failed feasibility result is useful evidence, but it does not satisfy this full-scope task.

## Done when
- [ ] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes for supported self-contained STEP renders and safe fallbacks on unsupported inputs.
- [ ] Measured STEP corpus meets the time and process-commit qualification targets; otherwise the task is not done and the program is replanned.
- [ ] The closure check proves the constrained thumbnail OCCT adapter enters only the provider DLL, while the existing STEP host keeps its separate OCCT closure; viewer/general worker and provider-to-host imports remain absent.
- [ ] The family renders a model-derived Explorer thumbnail using the T22 procedure.
- [ ] Hand-off below filled in.

## Hand-off

**What landed.** `.step`/`.stp` renders in Explorer through the frozen STEP CLSID
`{6EE961AC-AC3B-4958-A898-E30523FEE79D}` and a constrained OCCT 7.8 adapter
(`thumbnail-provider/StepFamilyAdapter.{h,cpp}`) linked only into the provider DLL,
`Tests.Unit.exe` and `Tests.ProviderHost.exe`. A dedicated **static** OCCT closure is
declared by the isolated manifest `thumbnail-provider/step-occt/` (built for
`x64-windows-static-md`; see PROGRESS T34 for the exact `vcpkg install` command and the
~27 min build). The provider imports no OCCT DLL and no product/worker/host binary, and it
never launches `Preview3DStepHost.exe`. Product-owned Part-21 admission (reused from the STEP
host, source-not-state) runs before any OCCT call and rejects external documents,
unsupported encodings and over-budget input; OCCT reads a bounded seekable stream over
`BoundedSource` and all opaque OCCT calls run under the T16 exception/SEH containment
boundary. The full assembly/instance hierarchy, checked double-precision transforms,
shape/instance colors and a fixed low-detail deterministic tessellation policy are preserved
under the stricter provider ceilings.

**Deviations.** (1) The environment shipped only a dynamic OCCT (the STEP host's), so a new
static closure was built and recorded in ADR-0027 rather than reusing it. (2) `faceted_invalid`
reports a typed failure at extraction time rather than at transfer (the provider reader does not
pre-validate authored indices), so the provider-host outcome is `BadFormat`/`DecoderFailure`
instead of the STEP-006 host's `MalformedData`; both are safe, non-fabricating fallbacks. (3)
Per-face subshape colors are not carried yet (shape/instance colors are). (4)
`XCAFApp_Application::GetApplication()` is a process singleton, but every document is per-call and
closed on `Reset`.

**Checks (x64).** `x64\Release\Tests.Unit.exe` 339 cases / 134 916 assertions green (Debug 339 /
134 994), including 23 `[provider][step]` cases. `x64\Release\Tests.ProviderHost.exe` 5 cases /
103 assertions green (Debug identical), with the new committed `step-part-256.pam` golden.
`tests/unit/check-provider-dependency-closure.ps1` OK Release (22 modules, incl. OCCT's
`WS2_32/ADVAPI32/USER32`, no `Preview3D*`/TK import) and Debug (12 modules).
`packaging\smoke\Invoke-ProviderSmoke.ps1 -SkipBuild` exit 0 for STL+PLY+glTF+FBX+3MF+USD+STEP;
STEP reference-vs-Shell `meanAbs=0.0000 maxAbs=0`, `WTSAT_ARGB` in `dllhost.exe`, no
`DisableProcessIsolation`, registration cleaned up. Hidden `[step-perf]` (Release, committed
fixtures): ~18–19 ms and ≤0.11 MiB commit delta per 256 px render, inside the 750 ms p95 / 384 MiB
targets.

**What the next task must know.** T41 must register both `.step` and `.stp` ShellEx for the STEP
CLSID with the real installer rules; T42 must include the static OCCT closure in the payload/SBOM
and T51 must measure a genuine large STEP corpus against the 384 MiB process-commit target (the
committed fixtures are small). A `__fastfail`/stack-cookie fault inside OCCT would still kill the
surrogate (SEH cannot catch it), so T43 must fuzz the STEP adapter. Do **not** put OCCT in the
repository-root `vcpkg.json`. Changed docs: new `design/adr/0027-step-adapter-constrained-occt.md`;
`design/05-thumbnail-provider.md`; `design/adr/0002-...md`; `design/adapters/step-009-thumbnail.md`.
