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
- [~] T06a — SEC-03b Fix user-chosen asset-root canonicalization → [06a-asset-root-canonicalization.md](06a-asset-root-canonicalization.md) ⟵ running

## Phase 2 — Provider, process, and viewer hardening

- [ ] T07 — SEC-07 Provider stream/raster/accounting robustness → [07-provider-stream-raster-robustness.md](07-provider-stream-raster-robustness.md)
- [ ] T08 — SEC-08 Provider containment policy (AV, stack, OCCT) → [08-provider-containment-policy.md](08-provider-containment-policy.md)
- [ ] T09 — SEC-09 Child-process loader/plugin hardening → [09-child-process-loader-hardening.md](09-child-process-loader-hardening.md)
- [ ] T10 — SEC-10 Build and process mitigation hardening → [10-build-and-process-mitigations.md](10-build-and-process-mitigations.md)
- [ ] T11 — SEC-11 Viewer local attack-surface reduction → [11-viewer-attack-surface.md](11-viewer-attack-surface.md)
- [ ] T12 — SEC-12 Active-instance IPC hardening → [12-active-instance-ipc.md](12-active-instance-ipc.md)

## Phase 3 — CI, supply chain, and fuzzing

- [ ] T13 — SEC-13 CI test gate for PRs and releases → [13-ci-test-gate.md](13-ci-test-gate.md)
- [ ] T14 — SEC-14 Release and supply-chain hardening → [14-ci-release-supply-chain.md](14-ci-release-supply-chain.md)
- [ ] T15 — SEC-15 Fuzz targets: STL, PLY, OBJ → [15-fuzz-fast-path-parsers.md](15-fuzz-fast-path-parsers.md)
- [ ] T16 — SEC-16 Fuzz targets: glTF + compressed codecs → [16-fuzz-gltf-and-codecs.md](16-fuzz-gltf-and-codecs.md)
- [ ] T17 — SEC-17 Provider pipeline fuzz + surrogate soak → [17-fuzz-provider-and-soak.md](17-fuzz-provider-and-soak.md)

## Phase 4 — Documentation and release metadata

- [ ] T18 — SEC-18 Reconcile design docs with implemented controls → [18-docs-reconciliation.md](18-docs-reconciliation.md)
- [ ] T19 — SEC-19 License/SBOM/dependency metadata → [19-license-sbom-metadata.md](19-license-sbom-metadata.md)

<!-- symphony:status -->
**Pipeline status** — updated 2026-10-02T08:16:06Z · 5/19 done

- Completed: T01, T02, T03, T04, T05
- Blocked: none
- Failed: T06
- Remaining: T07, T08, T09, T10, T11, T12, T13, T14, T15, T16, T17, T18, T19
- Last finished: T06 — failed · verify failed (exit 42): npm test — > test > x64\Release\Tests.Unit.exe Randomness seeded to: 4259318499 ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Tests.Unit.exe is a Catch2 v3.16.0 host application. Run with -? for options -------------…
<!-- /symphony:status -->
