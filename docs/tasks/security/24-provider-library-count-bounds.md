---
verify: x64\Release\Tests.ProviderHost.exe
---

# T24 — Bound provider allocations fed by library-supplied counts (3MF/USD)

## Goal
A hostile 3MF/USD cannot make the provider allocate from an authored count before the product cap
runs, so the 384 MiB ledger is honest and an oversized library result is rejected, not absorbed.

## Context (read first)
- Follow-up audit evidence:
  - `thumbnail-provider/ThreeMfFamilyAdapter.cpp:815-819` `composite->GetComposite(...)` allocates
    then checks `values.size() > 4096`; `:859-862` `GetMultiProperty` similarly; `:1552-1558`
    `GetBeams`/`GetBalls` allocate before the `kLatticeTrianglesMax` check; `:1653-1658`
    `ClipInside` can grow generated triangles past its estimate before the cap is applied.
  - `thumbnail-provider/UsdFamilyAdapter.cpp:1100-1136` loads instancer `protoIndices`/`positions`/
    `ids`/`orientations`/`scales`/`invisibleIds` and builds an `unordered_set` proportional to
    file-controlled lengths with no pre-cap.
- `docs/tasks/security/07-provider-stream-raster-robustness.md` — the accounting model.

## Scope
- [ ] For each library call whose output is file-count-driven, check the authored count against the
      product cap before the call where the library exposes it, or bound the result immediately and
      fail `ErrorCode::ResourceLimit` (not `OutOfMemory`) on overrun.
- [ ] Bound the USD instancer arrays before loading them.
- [ ] Add `ProviderFuzz` seeds and/or `ProviderHost` regressions for the oversized cases.

## Out of scope
- The allocating-`noexcept` holes (→ T20); OCCT/STEP (SEC-17 soak, → T25).

## Design notes
- Where the library exposes no count accessor, the accepted shape is: allocate under the ledger,
  check the size, fail `ResourceLimit`, and keep the peak charged and bounded.
- Do not change the public 384 MiB target without T51 evidence.

## Done when
- [ ] `x64\Release\Tests.ProviderHost.exe` passes with the new cases.
- [ ] Hand-off records the chosen bound and rationale per site.

## Hand-off

Landed in this session (attempt 1):

- **3MF (`thumbnail-provider/ThreeMfFamilyAdapter.cpp`).**
  - `ResolveProperty` now returns `ErrorCode`; its callers propagate the typed
    code instead of flattening every failure to `MalformedData`.
  - Composite constituents: `GetComposite` still sizes from the authored count
    (lib3mf exposes no per-property count), but the result is capped at
    `kCompositeConstituentsMax` = 4096 immediately after the call and overrun is
    `ResourceLimit`.
  - Multi-property layers: `GetLayerCount()` is checked against
    `kMultiPropertyLayersMax` = 16 **before** `GetMultiProperty`;
    `indices.size() != layers` stays `MalformedData`.
  - Beam/ball lattice: `GetBeamCount()`/`GetBallCount()` are checked against
    `kLatticeTrianglesMax` = 262 144 (individually and combined) **before**
    `GetBeams`/`GetBalls`; a size mismatch stays `MalformedData`.
  - `ClipInside` now returns `bool` and takes the lattice cap; it returns false
    as soon as the clipped output would exceed the cap, and the caller maps that
    to `ResourceLimit`. This bounds the live `generated` peak, not just the final
    count.
- **USD (`thumbnail-provider/UsdFamilyAdapter.cpp`).** After `Evaluate` (TinyUSDZ
  exposes no pre-copy count), `protoIndices`/`positions` are capped at
  `kMaxPrimCount` = 10 000 and overrun is `ResourceLimit`; `invisibleIds` is capped
  the same way before the `unordered_set` is built. `protoIndices.size() !=
  positions.size()` and the per-array `!= positions.size()` checks stay
  `MalformedData`.
- **Tests.** `tests/provider-host/ProviderHostTests.cpp`:
  - `an oversized USD point-instancer array is rejected as ResourceLimit` (10001
    instances).
  - `an oversized 3MF composite material is rejected as ResourceLimit`, built
    through the pinned lib3mf writer with one base material per constituent
    (lib3mf merges constituents that share a material).
  `tests/fuzz/prepare_provider_seeds.py` gains
  `pipeline-usd-huge-instancer.pipeline.seed`.
- **Docs.** ADR-0051; `docs/design/05-thumbnail-provider.md` records the caps.

Chosen bound and rationale per site:
- 3MF composite: 4096 (`kCompositeConstituentsMax`) — the existing cap, matching
  `ProviderLimits::kMaterialsMax`; no count accessor, so allocate-then-check.
- 3MF multi-property: 16 (`kMultiPropertyLayersMax`) — the pre-existing cap,
  now enforced before the call.
- 3MF beam/ball: 262 144 (`kLatticeTrianglesMax`) — the per-lattice preview
  ceiling; pre-cap via `GetBeamCount`/`GetBallCount`.
- 3MF clip: the same 262 144 cap enforced during clipping.
- USD instancer arrays: 10 000 (`kMaxPrimCount`) — a point instancer can
  contribute at most `kMaxPrimCount` instances and every array is indexed by
  instance, so anything larger cannot succeed; allocate-then-check because
  TinyUSDZ exposes no element count before `get`.

Check results (foreground, this machine):
- `MSBuild tests\provider-host\Tests.ProviderHost.vcxproj /p:Configuration=Release
  /p:Platform=x64 /p:SolutionDir=D:\repos\binbuf\preview-3d\` → OK.
- `x64\Release\Tests.ProviderHost.exe` → All tests passed (162 assertions /
  11 cases). New cases: USD 3 assertions, 3MF 4 assertions.

Remaining work / next task must know:
- Tests.Unit was not rebuilt/rerun in this session; the changed provider sources
  are also compiled into Tests.Unit, but the 3MF `beam-lattice` golden and the
  other provider-host goldens exercise the changed `ClipInside`/lattice paths
  and pass.
- The public 384 MiB ledger target is unchanged (needs T51 evidence).
- lib3mf/TinyUSDZ still own the initial authored allocation; the cap bounds the
  provider's copy/set and fails `ResourceLimit`, it does not shrink the library's
  own footprint (T51 measurement).