# Preview 3D Thumbnail Provider Roadmap

Implement the original-MVP Explorer thumbnail provider in full: the shared COM/sampler/rasterizer
foundation, one bounded stream-only adapter per supported family (STL, PLY, OBJ, glTF, FBX, 3MF, USD,
STEP), Shell registration and packaging, adversarial hardening, and release qualification.

The plan is ordered as **vertical slices** so a production-shaped result exists early and every later
family lands as its own testable increment. The first slice — STL rendering in real Windows Explorer —
is assembled by T22. Each subsequent family adapter is validated end-to-end with the same smoke
procedure, and the full eight-CLSID installer, signing and clean-machine acceptance come after the
breadth is proven. That ordering exists to surface remediation or a pivot while it is still cheap.

Design docs live in [`design/`](design/). The provider contract is
[`design/05-thumbnail-provider.md`](design/05-thumbnail-provider.md) and the program map is
[`design/overview.md`](design/overview.md). The original design baseline and family plans are archived
under [`legacy/`](legacy/). This program restores the installed original-MVP scope
([ADR-0007](design/adr/0007-provider-program-scope-and-installer.md)): registration and packaging ship
through the project's per-machine NSIS installer, and the eventual MSI adopts the same identities.

## Milestones

The tasks marked **E2E slice review** close a substantial end-to-end slice. When one completes, that
is the natural moment for a fresh independent end-to-end review. These are **non-blocking**: the
pipeline continues to the next task without waiting, and the review can happen in parallel or before
the next milestone. A review that finds a problem is handled the same way any task is — fix forward,
`split`, or `replan`. The `T03` row is an earlier feasibility checkpoint rather than an end-to-end
slice; it is listed because the rasterizer and surrogate spikes can invalidate the shared foundation
before it is built.

| After | Slice completed | Review focus |
| --- | --- | --- |
| T03 | Feasibility spikes complete (CPU rasterizer + Shell surrogate) | Mesh/point and bounded compressed-glTF prototype paths are measured inside the isolated surrogate; handler registration resolves with another default app and a per-user association. Timing, commit, DPI and isolation evidence determine whether foundation work proceeds or the contract is replanned. |
| T17 | Shared foundation integrated | The synthetic pipeline produces correct, deterministic bitmaps through the real COM path, meets the deadline, and unloads without leaks. |
| **T22** | **First production-shaped slice: STL in Windows Explorer** | Build Release → register → a real `.stl` thumbnail; surrogate hosting, no `DisableProcessIsolation`, dependency closure, thumbnail-cache refresh. |
| T25 | Tier A breadth complete (STL, PLY, OBJ, glTF) | Point clouds, the OBJ no-sidecar policy, and compressed glTF (Draco/meshopt/KTX2/WebP) all hold under provider budgets. |
| T34 | Tier B breadth closed (FBX, 3MF, USD, STEP qualified) | FBX static pose, 3MF OPC/lattice policy, USD composition exclusion, and a model-derived STEP thumbnail within its measured targets. A STEP shortfall pauses this full-scope roadmap for an ADR and replan. |
| T44 | Production hardening and full registration | All eight CLSIDs load in the isolated surrogate on a clean machine with no isolation opt-out; signing/SBOM; fuzz and surrogate soak clean. |

## Phase 1 — Prerequisites and de-risking

- [x] T01 — Freeze the provider specification and eight-family roster → [tasks/01-freeze-provider-spec.md](tasks/01-freeze-provider-spec.md)
- [x] T02 — SPIKE-8a: prototype the mesh and point CPU rasterizer → [tasks/02-spike-rasterizer-prototype.md](tasks/02-spike-rasterizer-prototype.md)
- [x] T03 — SPIKE-8b: prove isolated Shell surrogate hosting → [tasks/03-spike-surrogate-hosting.md](tasks/03-spike-surrogate-hosting.md)
- [x] T04 — Freeze provider interfaces and the shared model-core subset → [tasks/04-freeze-provider-interfaces.md](tasks/04-freeze-provider-interfaces.md)
- [ ] T05 — Scaffold the provider build and test integration → [tasks/05-provider-build-test-scaffold.md](tasks/05-provider-build-test-scaffold.md)
- [ ] T06 — Define budgets, deadlines, and HRESULT mapping → [tasks/06-budgets-deadlines-hresults.md](tasks/06-budgets-deadlines-hresults.md)
- [ ] T07 — Extract the provider-shared parser source subset → [tasks/07-extract-provider-shared-source.md](tasks/07-extract-provider-shared-source.md)

## Phase 2 — Shared foundation

- [ ] T11 — Implement the COM core and lifetime exports → [tasks/11-com-core-lifetime.md](tasks/11-com-core-lifetime.md)
- [ ] T12 — Implement bounded stream backing over IInitializeWithStream → [tasks/12-bounded-stream-backing.md](tasks/12-bounded-stream-backing.md)
- [ ] T13 — Implement family routing and the adapter interface → [tasks/13-family-routing-adapter-interface.md](tasks/13-family-routing-adapter-interface.md)
- [ ] T14 — Implement the deterministic geometry sampler → [tasks/14-geometry-sampler.md](tasks/14-geometry-sampler.md)
- [ ] T15 — Implement the CPU tile rasterizer and bitmap output → [tasks/15-cpu-tile-rasterizer.md](tasks/15-cpu-tile-rasterizer.md)
- [ ] T16 — Implement threading, deadline, and containment behavior → [tasks/16-threading-deadline-containment.md](tasks/16-threading-deadline-containment.md)
- [ ] T17 — Build the provider COM host test harness — E2E slice review → [tasks/17-provider-test-harness.md](tasks/17-provider-test-harness.md)

## Phase 3 — First end-to-end slice: STL in Explorer

- [ ] T21 — Implement the STL thumbnail adapter → [tasks/21-stl-adapter.md](tasks/21-stl-adapter.md)
- [ ] T22 — First installed Release smoke in Windows Explorer — E2E slice review → [tasks/22-first-installed-release-smoke.md](tasks/22-first-installed-release-smoke.md)

## Phase 4 — Tier A breadth

- [ ] T23 — Implement the PLY thumbnail adapter → [tasks/23-ply-adapter.md](tasks/23-ply-adapter.md)
- [ ] T24 — Implement the OBJ thumbnail adapter → [tasks/24-obj-adapter.md](tasks/24-obj-adapter.md)
- [ ] T25 — Implement the glTF/GLB thumbnail adapter — E2E slice review → [tasks/25-gltf-adapter.md](tasks/25-gltf-adapter.md)

## Phase 5 — Tier B breadth

- [ ] T31 — Implement the FBX thumbnail adapter → [tasks/31-fbx-adapter.md](tasks/31-fbx-adapter.md)
- [ ] T32 — Implement the 3MF thumbnail adapter → [tasks/32-3mf-adapter.md](tasks/32-3mf-adapter.md)
- [ ] T33 — Implement the USD/USDZ thumbnail adapter → [tasks/33-usd-adapter.md](tasks/33-usd-adapter.md)
- [ ] T34 — Implement the STEP/STP thumbnail adapter — E2E slice review → [tasks/34-step-adapter.md](tasks/34-step-adapter.md)

## Phase 6 — Production hardening and full registration

- [ ] T41 — Register all eight CLSIDs and ShellEx handlers → [tasks/41-shell-registration.md](tasks/41-shell-registration.md)
- [ ] T42 — Package, sign, and audit the provider payload closure → [tasks/42-payload-closure-signing-sbom.md](tasks/42-payload-closure-signing-sbom.md)
- [ ] T43 — Harden with fuzz, ASan, and hostile corpora → [tasks/43-adversarial-hardening.md](tasks/43-adversarial-hardening.md)
- [ ] T44 — Verify the actual surrogate, DPI, and clean-machine install — E2E slice review → [tasks/44-surrogate-dpi-install-verification.md](tasks/44-surrogate-dpi-install-verification.md)

## Phase 7 — Qualification and release

- [ ] T51 — Qualify performance and memory against the provider budgets → [tasks/51-performance-memory-qualification.md](tasks/51-performance-memory-qualification.md)
- [ ] T52 — Complete release acceptance and support documentation → [tasks/52-release-acceptance-docs.md](tasks/52-release-acceptance-docs.md)

<!-- symphony:status -->
**Pipeline status** — updated 2026-09-29T21:51:15Z · 4/29 done

- Completed: T01, T02, T03, T04
- Blocked: none
- Failed: none
- Remaining: T05, T06, T07, T11, T12, T13, T14, T15, T16, T17, T21, T22, T23, T24, T25, T31, T32, T33, T34, T41, T42, T43, T44, T51, T52
- Last finished: T04 — done · Froze adapter/sampler/raster contracts + CLSID routing in five provider headers compiled via ProviderContracts.cpp, documented signatures/routing and the T07 move/duplicate/exclude source list in design/interfaces.md (ADR-0009), and filled hand-off/PROGRESS; provider Debug+Release builds clean and Tests.Unit 123 cases green. Only pre-existing gap: compatibility-host-step cannot build here because its OCCT x64-windows-static-md vcpkg triplet is missing (unrelated to T04).
<!-- /symphony:status -->
