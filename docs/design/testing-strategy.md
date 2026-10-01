# Thumbnail provider testing strategy

## How the provider is built

- Build through the solution, never a single project: `msbuild Preview3D.slnx /p:Configuration=<Debug|Release> /p:Platform=x64`.
  A project-level build leaves `$(SolutionDir)` undefined and breaks app-local dependency deployment.
- The provider builds `Preview3DThumbnailProvider.dll` with the hardening flags in
  `05-thumbnail-provider.md` (`/guard:cf`, `/CETCOMPAT`, `/DYNAMICBASE`, `/NXCOMPAT`, `/sdl`, high
  warning level, warnings-as-errors at the product boundary).
- The DLL links its own copy of the bounded fast-path parsers; it must not import the viewer, the
  import worker, either import host, the old `Preview3D` project or the persistent cache. Prove the
  closure with `dumpbin /dependents` and the packaging allowlist (T42). The `[provider][scaffold]`
  cases in `Tests.Unit.exe` assert this automatically (no import whose name starts with `Preview3D`)
  and pin the two-symbol export surface, so the baseline verify command exercises it; the
  `tests/unit/check-provider-dependency-closure.ps1` script is the literal `dumpbin` evidence view.
- The provider's COM in-proc server exports exactly `DllGetClassObject` and `DllCanUnloadNow` from a
  `.def`, both `PRIVATE` (ADR-0010). Provider tests therefore load the built DLL at runtime rather
  than link it, which is also how the Shell and the COM host harness activate it.

## Test layers

| Layer | Where | What it proves |
| --- | --- | --- |
| Provider unit tests | `tests/unit/` (Catch2, `Tests.Unit.exe`) | COM identity/refcount/unload, stream backing, checked arithmetic, budget ledger, sampler determinism, rasterizer math and HRESULT mapping |
| Golden images | `tests/unit/` / fixtures | Deterministic perceptual output at 32, 64, 256 and 512 px for representative and malformed inputs |
| COM host harness | new target `Tests.ProviderHost.exe` (name frozen by T05/ADR-0010) | `IClassFactory`/`IThumbnailProvider` exercised through the real COM activation path, per CLSID, plus the tolerant golden comparator, the fixture registry, parallel-apartment and load/unload leak soak |
| Isolation tests | provider test group in `Tests.Unit.exe` | Stream-only ingestion: no sidecar/path/network/process/cache access; adversary corpora per adapter |
| Fuzz | `tests/fuzz/` (ASan/libFuzzer) | Each adapter's parse/sample boundary; no crash, hang, overflow or unbounded allocation |
| Surrogate soak | new / `tests/app-smoke/` | Parallel apartments, repeated load/unload in the real Shell surrogate, GDI/User/private-byte leak checks |
| Local installed smoke | `packaging/smoke/` | Per-user registered CLSID + extension `ShellEx`, a real `.stl` thumbnail through `IThumbnailCache`, module-identity proof of `DllHost` hosting, and no `DisableProcessIsolation` (T22; ADR-0020) |
| Install verification | `packaging/` + clean VM | Each CLSID loads into the isolated surrogate, not `explorer.exe`; no `DisableProcessIsolation`; conflict/repair/uninstall |

Catch2 binaries are run by hand out of `x64\<Config>\` today (there is no CI); routine provider
checks run `Tests.Unit.exe` and scoped `Tests.ImportIsolation.exe` filters in Debug and Release.

## Golden-image policy

- Render at fixed isometric framing with a fixed seed; compare with a tolerant perceptual metric
  (mean absolute error per channel plus a max-outlier guard), not byte equality.
- Every family contributes at least: a canonical small mesh, a point cloud (PLY), an instanced/
  transformed scene, an embedded-texture scene where supported, and a malformed/over-budget case that
  must return a null bitmap with a typed HRESULT.
- A fabricated "success" bitmap for a failed parse poisons the Shell cache and is prohibited; a golden
  that encodes a fallback must assert the null-bitmap error path instead.

The COM host harness (`Tests.ProviderHost.exe`, T17) owns the reusable
comparator (`tests/provider-host/GoldenImage.h`: mean absolute error per channel
plus a max-outlier guard, `meanAbs <= 2.0`, `maxAbs <= 48` by default) and a
build-time fixture registry (`tests/provider-host/FixtureRegistry.cpp`). Each
family task links its adapter into the host, registers one `GoldenFixture`
(family, small committed source, golden path, tolerance, `cx`), regenerates the
committed PAM with the hidden `[write-host-goldens]` case and verifies both
configurations. The host compiles the shipped pipeline, sampler and rasterizer
rather than a copy, so a fixture is rendered through the production
orchestration; the `Family::Unknown` placeholder scene proves the harness before
the first real adapter exists. The full workflow is in
`tests/provider-host/README.md` and `design/adr/0019-provider-com-host-harness.md`.

The local installed smoke (`packaging/smoke/`, T22; ADR-0020) is the first registered,
Shell-resolved proof: it stages the Release DLL and its CRT closure, registers one family's
CLSID and extension `ShellEx` per-user, then renders a real `.stl` twice — in-process through the
DLL's PRIVATE `DllGetClassObject` (the reference image) and through the real Shell
`IThumbnailCache::GetThumbnail` path. It passes only when the two images match, which proves the
Explorer thumbnail is model-derived and produced by this provider, and it confirms `DllHost`
hosting with no `DisableProcessIsolation`. Every later family task re-runs it for its family.

## Containment and hostile input

- The provider is treated as hostile-input code in a sensitive host. Bounded reads, checked parsing
  and deadlines — not the surrogate process — are its actual safety boundary (ADR-0005).
- Place third-party parser calls behind exception/structured-exception containment at the COM
  boundary where legally safe, while still fixing ordinary memory faults rather than masking them.
- Re-run the isolation/adversary corpus against each newly wired adapter; re-running alone is not
  enough when a change creates new attack surface.

## Performance and memory method

- Deadline: 750 ms p95 target and a cooperative 2 s stop point. Record actual maximum elapsed time,
  including overruns inside calls without progress callbacks; input preflight is not a time bound.
  T03 measures the prototype inside the real surrogate and T51 qualifies each implemented family.
- Memory: 256 MiB stream / 128 MiB contiguous backing / 192 MiB accounted parser scratch; the
  ledger reserves product-owned allocations across concurrent calls. Measure the actual process
  private-commit increase against the 384 MiB qualification target from an idle, loaded surrogate,
  including third-party allocations, module loading and GDI that the ledger does not control.
  Sample GDI/User handles and thread count across the corpus.
- Determinism: a stable source-derived seed and fixed framing; identical input must yield a
  bit-identical raster across repeated runs on the same machine and build, and a raster within the
  golden metric's tolerance across different machines/CPUs. Bit-exact cross-machine equality is not
  required (CPU floating-point results may differ); the DPI/size request may change resolution only.

## Definition of done for a task

A task is done only when product code, its tests, the malformed/cancellation path, documentation and
dependency/licence effects land together. Compilation is not completion; the named commands must be
run and their real output recorded in the task's Hand-off.

## Harness verification

The harness runs an independent verification command after a task reports done; a non-zero exit means
the task is not done. Resolution order is: the task's front-matter `verify:` field, then the harness
global `verifyCommand`, then the root `package.json` `test` script. Both the global `verifyCommand`
and the committed root `package.json` `test` script run `x64\Release\Tests.Unit.exe` as the baseline
regression guard; they are not a substitute for a task's own check. **Every code task MUST
set its own `verify:` front matter** to the exact already-built executable it produces, run from the
repository root - for example `x64\Release\Tests.Unit.exe`, `x64\Release\Tests.ProviderHost.exe`, or a
scoped Catch2 filter such as `x64\Release\Tests.ImportIsolation.exe "[stl-import],[ply-import],[gltf-import]"`.
A task's `Done when` must name that same command. Because `msbuild` is only guaranteed inside a Visual
Studio developer environment, verify commands run an already-built test binary; they never invoke the
build directly. Spike, installer, clean-VM, packaging, performance and acceptance tasks that have no
reliable automated command must leave `verify:` unset, state their evidence in `Done when`, and record
raw results in the Hand-off; the global baseline still runs.

The full `Tests.ImportIsolation.exe` suite currently retains pre-existing failures unrelated to the
thumbnail program (USD protocol/spike cases and the hidden large-scan fixture that requires generated
inputs). Those are baselined by the owning viewer program; provider tasks use a scoped filter as their
pass/fail gate. T52 also records a full diagnostic run with exact known failure names and compares
it to the baseline. Any new failure, including a new provider-relevant case, blocks acceptance.
