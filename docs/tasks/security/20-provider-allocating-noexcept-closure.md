---
verify: x64\Release\Tests.ProviderHost.exe
---

# T20 — Close the remaining provider allocating-`noexcept` holes (SEC-06 completion)

## Goal
A hostile 3MF or USD thumbnail can no longer call `std::terminate` through an allocating `noexcept`
helper; every product-owned allocation in the provider reaches the containment boundary and returns
a typed `OutOfMemory`.

## Context (read first)
- `docs/tasks/security/06-provider-exception-containment.md` — the chosen pattern and why it matters.
- `thumbnail-provider/ContainmentStage.h` / `Containment.cpp` — `RunContainedStage`; a throw from a
  `noexcept` function terminates before this boundary can catch it.
- Follow-up audit evidence, still `noexcept` while allocating:
  - `thumbnail-provider/ThreeMfFamilyAdapter.cpp:251` `AddSphere(...) noexcept` (builds
    `std::vector` rings), `:295` `AddBeam(...) noexcept`, `:419` `ClipInside(...) noexcept`,
    `:483` `ScanModelPart(...) noexcept` (file-sized `std::string`s).
  - `thumbnail-provider/UsdFamilyAdapter.cpp:517/535/548` `ResolveAsset`/`SizeAsset`/`ReadAsset`
    are `noexcept` but construct `std::string` / call `context.Find()`.
  - Latent: `thumbnail-provider/GeometrySampler.cpp:643` `StratifiedOffsets(...) noexcept`.
- `tests/provider-host/ProviderHostTests.cpp:220-279` — the fault-injection case; it deliberately
  excludes 3MF/USD/STEP, which is why these holes are untested.

## Scope
- [ ] Remove `noexcept` from every allocating helper/callback above, or route it through
      `RunContained`/`RunContainedStage`; keep genuinely non-allocating helpers `noexcept`.
- [ ] Re-audit `ThreeMfFamilyAdapter.cpp`, `UsdFamilyAdapter.cpp`, `GeometrySampler.cpp` for any
      other allocating `noexcept` function (grep `noexcept` plus `std::` allocations / `push_back`).
- [ ] Extend the provider-host fault-injection case to 3MF and USD and assert
      `ErrorCode::OutOfMemory` (not a process exit). If STEP still cannot be reached by the
      injector, record that in the Hand-off and leave it to the SEC-17 soak (→ T25).
- [ ] Update the design/ADR wording only if the containment rule is stated imprecisely.

## Out of scope
- Provider library-C-allocator paths (lib3mf/TinyUSDZ/OCCT) — SEC-17 soak (→ T25).
- Worker-side equivalents (→ T21, T24, T27).

## Design notes
- A `noexcept` function that can throw is a terminate; dropping `noexcept` is the minimal fix and
  matches the STL/PLY/OBJ/glTF/FBX adapters. Do not add a broad catch that hides logic errors.
- If a third-party callback signature forces `noexcept` (TinyUSDZ resolver), wrap the allocating
  body in `try/catch` and surface a typed error instead of letting it escape.

## Done when
- [ ] `x64\Release\Tests.ProviderHost.exe` and `x64\Debug\Tests.ProviderHost.exe` pass, including
      the new 3MF/USD fault cases.
- [ ] `x64\Release\Tests.Unit.exe "[provider]"` passes.
- [ ] Docs touched: `docs/design/05-thumbnail-provider.md` only if wording is imprecise.
- [ ] Hand-off filled in.

## Hand-off

**Pattern.** Kept the T06/ADR-0034 model: frozen adapter stages stay `noexcept` and run their
non-`noexcept` Impl body through `RunContainedStage`. The closure here is that no internal helper on
those paths may be `noexcept` while it allocates, or its `std::bad_alloc` terminates before the
boundary. A third-party-invoked callback (TinyUSDZ asset resolver) cannot let the throw unwind
through the vendored library, so it catches inside its own `noexcept` body and returns a typed
error.

**What landed.**
- `thumbnail-provider/ThreeMfFamilyAdapter.cpp`: removed `noexcept` from the four audited allocating
  helpers `AddSphere` (`:251`), `AddBeam` (`:295`), `ClipInside` (`:419`), `ScanModelPart` (`:483`).
  Re-audit found no others: `ReadCallback`/`SeekCallback`/`ProgressCallback` only `memset`/`memcpy`,
  and the `Vec3`/matrix/`ReadBe*`/`SniffImage`/`AllowedNamespace`/`Map*Error` helpers do not allocate.
- `thumbnail-provider/UsdFamilyAdapter.cpp`: `ResolveAsset`/`SizeAsset`/`ReadAsset` (`:523`/`:541`/
  `:554`) now build the `std::string`/`Find()` body under `try/catch` and set
  `UsdAssetContext::error = ErrorCode::OutOfMemory` (bad_alloc) or
  `ErrorCode::InternalImporterFailure` (other), returning `-2`. They stay `noexcept` because TinyUSDZ
  invokes the raw function pointer. Re-audit found no other allocating `noexcept` helper.
- `thumbnail-provider/GeometrySampler.cpp:643` + `GeometrySamplingPolicy.h:82`: removed `noexcept`
  from `StratifiedOffsets` (allocates). `AddTriangle`/`AddPoint`/`OfferCell` stay `noexcept` because
  `Begin` reserves the reservoir so no `push_back` can allocate; `BuildResult` already contains its
  own throw.
- `tests/provider-host/ProviderHostTests.cpp`: `[host][containment]` now includes `Family::ThreeMf`
  and `Family::Usd` and asserts `ErrorCode::OutOfMemory`.
- Docs: `docs/design/05-thumbnail-provider.md` containment paragraph clarified; no new ADR (rule is
  already ADR-0034).

**Deviation / limitation.** STEP remains excluded from the fault-injection case: its product-owned
allocations are interleaved with OCCT's own C allocator, which the in-executable global
`operator new` replacement cannot reach. Recorded as a Follow-up for the SEC-17 soak (→ T25), as the
task permitted.

**Check results (all foreground).**
- `MSBuild tests\provider-host\Tests.ProviderHost.vcxproj /p:Configuration=Debug /p:Platform=x64 "/p:SolutionDir=D:\repos\binbuf\preview-3d\\" /m` — succeeded (also rebuilt the provider DLL).
- `x64\Debug\Tests.ProviderHost.exe "[host][containment]"` — passed (44 assertions / 2 cases).
- `x64\Debug\Tests.ProviderHost.exe` — passed (152 assertions / 8 cases).
- Same Release build/run — passed (152 assertions / 8 cases).
- `MSBuild tests\unit\Tests.Unit.vcxproj /p:Configuration=Release ...` succeeded;
  `x64\Release\Tests.Unit.exe "[provider]"` — passed (60934 assertions / 221 cases).

**Next task must know.** Do not re-add `noexcept` to the four 3MF helpers or `StratifiedOffsets`.
The worker-side counterparts still need the same closure (→ T21/T24/T27). STEP library-allocator
exhaustion is still only proven by the SEC-17 soak (→ T25).