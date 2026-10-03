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
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_