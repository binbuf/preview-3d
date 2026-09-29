---
verify: x64\Release\Tests.ProviderHost.exe
---
# T17 — Build the provider COM host test harness

> **E2E slice review point.** Completing this task closes the shared-foundation slice. Run a fresh end-to-end review now; the pipeline may continue to the next task without waiting.

## Goal
Give every later adapter one repeatable harness: a COM host that activates the provider like the
Shell, renders and compares golden images, runs parallel apartments, and samples GDI/User/private-byte
leaks across repeated load/unload.

## Context (read first)
- `docs/design/testing-strategy.md` — test layers, golden-image policy, soak expectations.
- `docs/design/05-thumbnail-provider.md` — "Tests".
- `docs/tasks/03-spike-surrogate-hosting.md` — the observed surrogate activation and DPI behavior.
- `docs/tasks/15-cpu-tile-rasterizer.md` — the raster output the harness compares.

## Scope
- [ ] Implement the host test target `Tests.ProviderHost.exe` (name frozen in T05) that calls `DllGetClassObject` → `IClassFactory::CreateInstance` → `IInitializeWithStream` → `GetThumbnail`, matching the Shell sequence per CLSID, and wire it into `Preview3D.slnx`.
- [ ] Add a golden-image comparator (mean absolute error per channel plus a max-outlier guard) and a fixture/expectation registry used by family tasks.
- [ ] Add an STA parallel-host stress case driving multiple provider objects concurrently.
- [ ] Add a repeated load/unload loop that samples GDI object count, User handles, private bytes and thread count before/after, failing on growth beyond a fixed tolerance.
- [ ] Document the exact commands and how a family task registers its fixtures.

## Out of scope
- Running inside the real `DllHost.exe` surrogate (→ T44; T03 proves the mechanism).
- Family parsing (→ T21–T34).

## Design notes
- The harness must be deterministic and CI-runnable where possible; surrogate-only checks stay in T44.
- Keep golden fixtures small and additive per family; do not commit multi-gigabyte binaries.
- The comparator must tolerate platform/rasterizer rounding while catching real regressions.

## Done when
- [ ] `x64\Release\Tests.ProviderHost.exe` (the `verify:` command) activates the provider and renders a golden for at least one placeholder adapter from the repository root.
- [ ] Parallel-stress and leak-loop cases pass with stable handle/thread counts.
- [ ] The fixture workflow is documented for family tasks.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_