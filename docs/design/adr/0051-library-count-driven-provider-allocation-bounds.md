# 0051 — Library-count-driven provider allocations are pre-capped or rejected as `ResourceLimit`

## Status
accepted

## Context
The T24 follow-up audit found provider calls that let a file-authored count
size a library-owned allocation before any product cap ran:

- `thumbnail-provider/ThreeMfFamilyAdapter.cpp` — `GetComposite` and
  `GetMultiProperty` resized their output from the authored constituent/layer
  count and only checked the cap afterwards, and `GetBeams`/`GetBalls` were
  called before the `kLatticeTrianglesMax` check. Overrun mapped to
  `MalformedData`, and `ClipInside` could grow the live `generated` vector past
  the lattice cap before the post-clip check.
- `thumbnail-provider/UsdFamilyAdapter.cpp` — a `PointInstancer`'s
  `protoIndices`, `positions`, `ids`, `orientations`, `scales` and
  `invisibleIds` arrays were copied from the authored lengths, and an
  `unordered_set` was built from `invisibleIds`, with no cap. The first cap was
  the per-instance `pointInstances` budget inside the placement loop, long after
  the arrays and the set existed.

lib3mf exposes `GetLayerCount`, `GetBeamCount` and `GetBallCount`, so those can
be checked before the allocating call. It exposes no per-property composite
constituent count, and TinyUSDZ exposes no element-count accessor before
`Animatable::get` copies the array, so those two sites can only be capped
immediately after the copy.

## Decision
- Where the library exposes the authored count, check it against the product cap
  **before** the allocating call: 3MF multi-property layers
  (`kMultiPropertyLayersMax`, 16) and beam/ball counts (`kLatticeTrianglesMax`,
  262 144).
- Where it does not, allocate under the provider boundary, check the result
  immediately, and fail `ErrorCode::ResourceLimit` on overrun: 3MF composite
  constituents (`kCompositeConstituentsMax`, 4 096) and the USD instancer arrays
  (`kMaxPrimCount`, 10 000). A mismatched library result (size != authored count
  for beams/balls, protoIndices != positions) stays `MalformedData`.
- `ClipInside` takes the lattice cap and returns `false` as soon as the clipped
  output would exceed it; the caller maps that to `ResourceLimit`.
- `ResolveProperty` returns `ErrorCode` (not `bool`) so the over-cap composite
  reaches the caller as `ResourceLimit` instead of being flattened to
  `MalformedData` at the triangle-emission site.

The public 384 MiB ledger target is unchanged.

## Consequences
- An oversized 3MF composite or USD instancer now fails closed with
  `ResourceLimit`, and the peak charged by the product boundary is bounded even
  though lib3mf/TinyUSDZ still own the initial authored allocation.
- `ClipInside` can fail a pathological clip whose *final* count would fit but
  whose intermediate clip count exceeds the lattice cap. That is a deliberate
  peak bound; the estimate that feeds the clipper is already `<=` the cap, so a
  normal bounded lattice is unaffected (the committed `beam-lattice` golden still
  renders).
- `ProviderFuzz` gains a hostile USD instancer seed, and `Tests.ProviderHost`
  gains an oversized USD instancer case and an oversized 3MF composite case
  built through the pinned lib3mf writer (lib3mf merges constituents that share
  a base material, so the fixture gives each constituent its own material).