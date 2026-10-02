# Security hardening task set

This is the `security` task set — a second, independent plan alongside the base/MVP provider roadmap.
It carries the hardening backlog from the v0.5.0 security audit. Run it without disturbing the MVP
pipeline:

```powershell
./.symphony/symphony.ps1 doctor --set security
./.symphony/symphony.ps1 run    --set security
./.symphony/symphony.ps1 status --set security
```

- Roadmap: [`ROADMAP.md`](ROADMAP.md) (execution is file order: T01 → T19).
- Task files: [`../tasks/security/`](../tasks/security/).
- Shared design docs: [`../design/`](../design/).
- Progress notebook: [`PROGRESS.md`](PROGRESS.md).

The base package (`docs/ROADMAP.md`) is the MVP provider plan and is also selectable as the `mvp`
task set (`--set mvp`). Task ids are per-set, so `T01` here is unrelated to `T01` in the MVP set.

## Tasks

| ID | Audit ref | Task | Area | Priority |
| --- | --- | --- | --- | --- |
| [T01](../tasks/security/01-gltf-traversal-and-limit-ordering.md) | SEC-01 | Bound glTF traversal and worker limit ordering | import-worker | 1 |
| [T02](../tasks/security/02-compressed-decode-preflight.md) | SEC-02 | Preflight compressed decoders before allocation | worker + provider | 6 |
| [T03](../tasks/security/03-sandbox-and-broker-limits.md) | SEC-03 | Sandbox limits and broker defensive checks | import-broker | 5 |
| [T04](../tasks/security/04-sidecar-reference-validation.md) | SEC-04 | Sidecar reference validation (NUL/control, per-format) | import-broker | 2 |
| [T05](../tasks/security/05-3mf-opc-reconciliation.md) | SEC-05 | 3MF OPC preflight/library reconciliation | import-worker | 4 |
| [T06](../tasks/security/06-provider-exception-containment.md) | SEC-06 | Provider adapter exception containment | thumbnail-provider | 3 |
| [T07](../tasks/security/07-provider-stream-raster-robustness.md) | SEC-07 | Provider stream/raster/accounting robustness | thumbnail-provider | 11 |
| [T08](../tasks/security/08-provider-containment-policy.md) | SEC-08 | Provider containment policy (AV, stack, OCCT) | thumbnail-provider | 12 |
| [T09](../tasks/security/09-child-process-loader-hardening.md) | SEC-09 | Child-process loader/plugin hardening | all import processes | 7 |
| [T10](../tasks/security/10-build-and-process-mitigations.md) | SEC-10 | Build and process mitigation hardening | build + launcher | 8 |
| [T11](../tasks/security/11-viewer-attack-surface.md) | SEC-11 | Viewer local attack-surface reduction | interactive-viewer | 9 |
| [T12](../tasks/security/12-active-instance-ipc.md) | SEC-12 | Active-instance IPC hardening | interactive-viewer | 10 |
| [T13](../tasks/security/13-ci-test-gate.md) | SEC-13 | CI test gate for PRs and releases | CI | 13 |
| [T14](../tasks/security/14-ci-release-supply-chain.md) | SEC-14 | Release/supply-chain hardening | CI + packaging | 14 |
| [T15](../tasks/security/15-fuzz-fast-path-parsers.md) | SEC-15 | Fuzz targets: STL, PLY, OBJ | tests/fuzz | 15 |
| [T16](../tasks/security/16-fuzz-gltf-and-codecs.md) | SEC-16 | Fuzz targets: glTF + compressed codecs | tests/fuzz | 16 |
| [T17](../tasks/security/17-fuzz-provider-and-soak.md) | SEC-17 | Fuzz provider pipeline + surrogate soak | tests/fuzz, provider | 17 |
| [T18](../tasks/security/18-docs-reconciliation.md) | SEC-18 | Reconcile design docs with implemented controls | docs | 18 |
| [T19](../tasks/security/19-license-sbom-metadata.md) | SEC-19 | License/SBOM/dependency metadata | packaging | 19 |

The harness executes T01 → T19 in order, which is intentionally close to the priority column:
parser and sidecar fixes first, provider/sandbox/viewer next, CI and fuzzing last. Tasks 13–17 pay
risk down continuously and can be reordered with `--from`/`--only` once earlier work lands. Soft
dependencies (e.g. T02 benefits from T01/T06; T14 builds on T13) are noted in each task's Context.

## Conventions

- Structure follows [`../tasks/mvp/TEMPLATE.md`](../tasks/mvp/TEMPLATE.md): Goal / Context (read
  first) / Scope / Out of scope / Design notes / Done when / Hand-off. Check a scope item only when
  the evidence exists; fill the Hand-off before reporting done.
- Standard verification, Debug then Release (from `CONTRIBUTING.md`):
  `msbuild Preview3D.slnx /t:Preview3D,Tests_Unit,Tests_ImportIsolation /p:Configuration=Debug /p:Platform=x64 /m`
  then `& ./x64/Debug/Tests.Unit.exe` and `& ./x64/Debug/Tests.ImportIsolation.exe`.
  Provider work additionally builds and runs `Tests.ProviderHost`; fuzz work follows
  [`tests/fuzz/README.md`](../../tests/fuzz/README.md) and the harness conventions in
  [`.symphony/README.md`](../../.symphony/README.md).
- The harness's inferred verify runs the repo's `x64\Release\Tests.Unit.exe` (root `package.json`).
  Build Release before reporting done. Add task front matter `verify:` for a suite-specific check.
- Any changed limit, protocol field, or guarantee is an ADR change; record it (see
  [`../design/11-decisions-and-risks.md`](../design/11-decisions-and-risks.md)) rather than silently
  updating code. T18 tracks the doc reconciliation.
- Prefer fail-closed defaults: reject, cap, and surface a typed error over best-effort recovery.