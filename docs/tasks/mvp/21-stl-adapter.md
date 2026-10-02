---
verify: x64\Release\Tests.Unit.exe
---
# T21 — Implement the STL thumbnail adapter

## Goal
Render ASCII and binary `.stl` models in Explorer using the product STL parser under provider limits,
with neutral shading.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — stream ingestion (STL is fully stream-contained).
- `docs/design/03-file-formats-and-ingestion.md` — STL subset and budget column.
- `import-worker/src/StlAdapter.*` — the viewer parser to reuse as source (see ADR-0004).
- `docs/tasks/14-geometry-sampler.md`, `docs/tasks/17-provider-test-harness.md`.

## Scope
- [x] Route `.stl` to this adapter via its fixed CLSID; detect ASCII vs binary on the stream.
- [x] Emit bounded triangles with computed/neutral normals and the neutral material.
- [x] Reuse the checked triangle-count and record bounds so an adversary cannot force an unbounded read or allocation; split/stream the read against the deadline.
- [x] Add fixtures and goldens: ASCII mesh, binary mesh, large triangle-count input, truncated file, fabricated count, non-finite vertices — the last four asserting typed failures.

## Out of scope
- PLY parsing (→ T23), regardless of shared point/mesh paths.
- Viewer STL streaming/LOD behavior (viewer program).

## Design notes
- STL is stream-contained and needs no sidecar policy.
- Prefer a single bounded pass with the sampler able to stop early at the inspect cap.
- Keep the ASCII tokenizer bounded and locale-independent.

## Done when
- [x] `x64\Release\Tests.Unit.exe` (the `verify:` command) passes in Debug and Release for ASCII/binary and every malformed case.
- [x] A fabricated-count input is rejected before allocation.
- [x] Hand-off below filled in.

## Hand-off

### What landed
- `thumbnail-provider/StlFamilyAdapter.{h,cpp}` — the `Family::Stl` adapter
  behind the frozen `IFamilyAdapter` lifecycle. `Initialize` adopts the caller's
  bounded source/limits/deadline/ledger; `Parse` copies the checked 84-byte
  prefix (contiguous view when available, otherwise one checked `ReadAt`) and
  classifies binary vs ASCII with the shared `parser_core::IsAsciiStl`;
  `EnumerateMaterials` emits the neutral material as index 1;
  `EnumerateGeometry` streams facets; `Reset` clears every per-call resource.
- `thumbnail-provider/FamilyAdapterRegistry.cpp` — `Family::Stl` now returns
  `std::make_unique<StlAdapter>()` (in a `try/catch` so the `noexcept` factory
  cannot terminate on allocation failure). The other seven families stay
  `nullptr`.
- `tests/unit/ProviderStlAdapterTests.cpp` — 16 `[provider][stl]` cases:
  binary mesh (direct + through the routed real pipeline, with and without a
  contiguous view), ASCII mesh, a binary header whose free-form text starts
  with `solid`, over-cap declared count, fabricated count, truncated file,
  mixed/only non-finite facets, sink cap stop, expired deadline, pipeline error
  mapping, a real rendered bitmap, and CLSID routing.
- `tests/provider-host/FixtureRegistry.cpp` — registered the first real family
  fixture `stl-cube` (`Family::Stl`, a 684-byte binary cube built in-memory) and
  committed `tests/provider-host/goldens/stl-cube-256.pam`.
- Build wiring: `StlFamilyAdapter.cpp` and the shared `AsciiTokenizer.cpp`/
  `StlParserCore.cpp` compile into `Preview3DThumbnailProvider.dll`,
  `Tests.Unit.exe` and `Tests.ProviderHost.exe` (PCH-free).
- Host changes needed by a real adapter: `ProviderHostSupport.h`'s
  `MemorySource` now serves the fixture bytes through `ReadAt`/`ContiguousView`
  instead of only reporting a size; `ProviderHostTests.cpp`'s `[host][com]`
  branches on `CreateFamilyAdapter(family) != nullptr` so a linked adapter's
  typed parse failure (garbage stream → no bitmap) is accepted alongside its
  success path, while an unlinked family still requires
  `ERROR_NOT_SUPPORTED`/null.

### Decisions / deviations
- **Files are named `StlFamilyAdapter.{h,cpp}`, not `StlAdapter.{h,cpp}`.**
  `Tests.Unit.vcxproj` puts `import-worker/src` on the include path, which also
  contains `StlAdapter.h`; an identical header name silently resolved to the
  worker's `import_worker` parser and broke `preview3d::provider::StlAdapter`.
  Every later family task that has a same-named worker adapter (PLY, OBJ, ...)
  needs the same distinct provider file name.
- **Binary reads are streamed in 4096-facet (200 KiB) blocks** through
  `BoundedSource::ReadAt`, charged to the T06 ledger before allocation, with a
  deadline checkpoint per block. The declared count is validated against
  `kMaxStlFacets` and the actual source extent in `Parse`, before any facet
  buffer exists, so a large/fabricated count never reaches an allocation.
- **ASCII requires the whole source as one span.** It is served from the
  frozen 128 MiB contiguous backing view; a larger ASCII source fails closed
  with `ResourceLimit` rather than allocating an unbounded buffer.
- **Detection matches the worker exactly:** a structurally consistent binary
  shape wins even when the 80-byte header starts with `solid`; only a non-binary
  shape with a leading `solid` takes the ASCII tokenizer.
- No ADR: the adapter uses only the frozen contracts and the T07 shared source.

### Check results (Release x64 unless noted)
```
msbuild tests\unit\Tests.Unit.vcxproj /p:Configuration=Release|Debug /p:Platform=x64 /p:SolutionDir=<root>\   -> clean, 0 warnings (/W4 /WX)
x64\Release\Tests.Unit.exe "[stl]"                  -> All tests passed (73 assertions in 16 test cases)
x64\Release\Tests.Unit.exe                          -> All tests passed (134400 assertions in 214 test cases)
x64\Debug\Tests.Unit.exe                            -> All tests passed (134485 assertions in 214 test cases)
x64\Release\Tests.ProviderHost.exe                  -> All tests passed (55 assertions in 5 test cases)
x64\Debug\Tests.ProviderHost.exe                    -> All tests passed (55 assertions in 5 test cases)
x64\Release\Tests.ProviderHost.exe "[write-host-goldens]" -> stl-cube-256.pam written
pwsh -File tests\unit\check-provider-dependency-closure.ps1 -Configuration Release -> OK: no viewer/worker/host/core imports
```

### Next task must know
- T22 (first installed Release smoke) can register the STL CLSID and expect a
  real thumbnail; the DLL now links the adapter and the host renders `stl-cube`.
- T23 (PLY) must use a provider-distinct file name (e.g. `PlyFamilyAdapter.*`)
  for the same include-path collision reason, and can reuse this task's
  fixture/MemorySource/host-`[com]` pattern.