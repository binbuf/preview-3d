# Preview 3D Security Hardening Roadmap

Close the containment-resilience, defense-in-depth, and CI/supply-chain gaps found by the v0.5.0
security audit, plus the places where the code does less than `docs/design/` claims. The audit found
no critical escape from the AppContainer/Job design and no remotely reachable memory-corruption bug,
so this roadmap hardens rather than redesigns the boundary.

This plan is its own **task set**. Run it without touching the MVP provider pipeline:

```powershell
./.symphony/symphony.ps1 doctor --set security
./.symphony/symphony.ps1 run    --set security
./.symphony/symphony.ps1 status --set security
```

Tasks run in file order. Each is scoped to one session; a task that overruns should be split rather
than stretched. Every task carries its audit evidence (file:line) inline so sessions need no external
report. See [`README.md`](README.md) for the priority rationale, dependencies and per-area index.

## Phase 1 — Parser and containment hardening

- [x] T01 — SEC-01 Bound glTF traversal and fix worker limit ordering → [01-gltf-traversal-and-limit-ordering.md](01-gltf-traversal-and-limit-ordering.md)
- [x] T02 — SEC-02 Preflight compressed decoders before allocation → [02-compressed-decode-preflight.md](02-compressed-decode-preflight.md)
- [x] T03 — SEC-03 Sandbox limits and broker defensive checks → [03-sandbox-and-broker-limits.md](03-sandbox-and-broker-limits.md)
- [x] T04 — SEC-04 Sidecar reference validation (NUL/control, per-format) → [04-sidecar-reference-validation.md](04-sidecar-reference-validation.md)
- [x] T05 — SEC-05 3MF OPC preflight/library reconciliation → [05-3mf-opc-reconciliation.md](05-3mf-opc-reconciliation.md)
- [x] T06 — SEC-06 Provider adapter exception containment → [06-provider-exception-containment.md](06-provider-exception-containment.md) ⟵ accepted
- [x] T06a — SEC-03b Fix user-chosen asset-root canonicalization → [06a-asset-root-canonicalization.md](06a-asset-root-canonicalization.md)

## Phase 2 — Provider, process, and viewer hardening

- [x] T07 — SEC-07 Provider stream/raster/accounting robustness → [07-provider-stream-raster-robustness.md](07-provider-stream-raster-robustness.md)
- [x] T08 — SEC-08 Provider containment policy (AV, stack, OCCT) → [08-provider-containment-policy.md](08-provider-containment-policy.md)
- [x] T09 — SEC-09 Child-process loader/plugin hardening → [09-child-process-loader-hardening.md](09-child-process-loader-hardening.md)
- [x] T10 — SEC-10 Build and process mitigation hardening → [10-build-and-process-mitigations.md](10-build-and-process-mitigations.md)
- [x] T11 — SEC-11 Viewer local attack-surface reduction → [11-viewer-attack-surface.md](11-viewer-attack-surface.md)
- [x] T12 — SEC-12 Active-instance IPC hardening → [12-active-instance-ipc.md](12-active-instance-ipc.md)

## Phase 3 — CI, supply chain, and fuzzing

- [x] T13 — SEC-13 CI test gate for PRs and releases → [13-ci-test-gate.md](13-ci-test-gate.md)
- [x] T14 — SEC-14 Release and supply-chain hardening → [14-ci-release-supply-chain.md](14-ci-release-supply-chain.md)
- [x] T15 — SEC-15 Fuzz targets: STL, PLY, OBJ → [15-fuzz-fast-path-parsers.md](15-fuzz-fast-path-parsers.md)
- [x] T16 — SEC-16 Fuzz targets: glTF + compressed codecs → [16-fuzz-gltf-and-codecs.md](16-fuzz-gltf-and-codecs.md) ⟵ completed by T16b ⟵ accepted
- [x] T16b — SEC-16b KTX2/BasisLZ ETC1S decoder finding and GltfFuzz smoke promotion → [16b-fuzz-ktx-etc1s-finding.md](16b-fuzz-ktx-etc1s-finding.md)
- [x] T17 — SEC-17 Provider pipeline fuzz + surrogate soak → [17-fuzz-provider-and-soak.md](17-fuzz-provider-and-soak.md)

## Phase 4 — Documentation and release metadata

- [x] T18 — SEC-18 Reconcile design docs with implemented controls → [18-docs-reconciliation.md](18-docs-reconciliation.md)
- [x] T19 — SEC-19 License/SBOM/dependency metadata → [19-license-sbom-metadata.md](19-license-sbom-metadata.md)

## Phase 5 — Post-audit hardening follow-ups

A follow-up source audit of the completed security set (2026-10-03) verified each task and found
the items below. The two smallest were fixed inline and committed before this phase (medium
mandatory label in `ActiveInstance.cpp`; provider USD `StripSkeletonBindings` recursion bound) — do
not re-fix those. Each task below carries its audit evidence inline.

- [x] T20 — Close provider allocating-`noexcept` holes (SEC-06 completion) → [20-provider-allocating-noexcept-closure.md](20-provider-allocating-noexcept-closure.md)
- [x] T21 — Complete the SEC-01 sweep: USD primvar cap and provider glTF visit cap → [21-sec01-sweep-completion.md](21-sec01-sweep-completion.md)
- [x] T22 — Make the broker generation wall-clock deadline absolute (SEC-03 completion) → [22-generation-deadline-absolute.md](22-generation-deadline-absolute.md)
- [x] T23 — Fix release/CI NuGet credential lifecycle and durable-cache restore (SEC-14 follow-up) → [23-ci-credential-lifecycle.md](23-ci-credential-lifecycle.md)
- [x] T24 — Bound provider allocations fed by library-supplied counts (3MF/USD) → [24-provider-library-count-bounds.md](24-provider-library-count-bounds.md)
- [ ] T25 — Complete SEC-17: provider fuzz/soak promotion and hostile failure classification → [25-sec17-completion.md](25-sec17-completion.md)
- [ ] T26 — Normalize untrusted paths before opening (remote UNC + absolute primary) → [26-path-normalization.md](26-path-normalization.md)
- [ ] T27 — Bound 3MF ZIP64 extra-field reads to the sub-record (SEC-05 follow-up) → [27-3mf-zip64-extra-field.md](27-3mf-zip64-extra-field.md)
- [ ] T28 — Worker WIC native-copy buffer invariant → [28-wic-native-copy.md](28-wic-native-copy.md)

<!-- symphony:status -->
**Pipeline status** — updated 2026-10-03T02:41:35Z · 26/30 done

- Completed: T01, T02, T03, T04, T05, T06, T06a, T07, T08, T09, T10, T11, T12, T13, T14, T15, T16, T16b, T17, T18, T19, T20, T21, T22, T23, T24
- Blocked: none
- Failed: none
- Remaining: T25, T26, T27, T28
- Last finished: T24 — done · Bounded 3MF composite/multi/lattice and USD instancer count-driven allocations to ResourceLimit (ADR-0051), added ProviderHost 3MF+USD regressions and a ProviderFuzz seed; Release Tests.ProviderHost passes 162/11.
<!-- /symphony:status -->
