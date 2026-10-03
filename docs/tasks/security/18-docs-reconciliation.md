---
verify: x64\Release\Tests.Unit.exe "~[graphics]"
---

# T18 — Reconcile design docs with implemented controls

## Goal
The design documents always describe what the code actually does, so future work and the audit
surface stop relying on claims that are not true. Where code is the intended behavior, the docs
change; where a doc claim is the required behavior, the code gets a task (listed below) instead.

## Context (read first)
Audit-confirmed divergences (doc line → code reality):
- `docs/design/08-installation-and-registration.md:64` claims per-generation pipe/event/section
  ACLs; `shared/import-broker` uses anonymous objects with default security descriptors and relies on
  handle possession (`WorkerPool.cpp`, `ImportSession.cpp`, `SharedSection.cpp`). Either document the
  handle-only model or implement ACLs (→ SEC-03).
- `docs/design/02-system-architecture.md:88` and NFR-14 (`01-product-scope.md:66`) claim CPU-time
  Job limits; `SandboxLauncher.cpp:16-24` sets none (→ SEC-03).
- `docs/design/02-system-architecture.md:48,88` say a fresh AppContainer token/Job pairing per
  generation; the worker pool reuses one profile/job per session (`ImportSession.cpp:537-559`,
  `WorkerPool.cpp:77-96`). Correct the wording.
- `docs/design/09-quality-performance-and-security.md:216` claims safe DLL search for every import
  process; the general worker does not set it (→ SEC-09).
- `docs/design/02-system-architecture.md:5,174` and `09:174` list DirectXTex/WIC; the tree uses WIC
  only, with no DirectXTex dependency in `vcpkg.json` (decide: drop the claim or add the plan).
- `docs/design/06-application-lifecycle-and-ipc.md:27` says developer switches are compiled out or
  rejected by production registration; child binaries accept them (→ SEC-09).
- `docs/design/05-thumbnail-provider.md`/ADR-0008 should note that `InprocServer32` remains
  activatable in-process by any local process; the surrogate routing is Shell-path-specific.
- `docs/design/09...md:177-192` promises fuzz targets (glTF/STL/PLY/OBJ/codecs/provider) that do not
  exist (→ SEC-15/16/17); update the doc to the tracked state until they land.
- `THIRD-PARTY-LICENSES.md`/SBOM are manual and drifting (→ SEC-19).

## Scope
- [ ] Make the doc corrections above (or link the implementing task where the code will change), in
      the design docs themselves rather than only in this task.
- [ ] Add the new security invariants this audit proposes to the threat model/controls sections
      (NUL/control rejection in references, validate-before-decode, provider containment policy,
      mandatory CI gate) so they are testable requirements.
- [ ] Update `SECURITY.md` scope if any new class becomes reportable (e.g. sidecar disclosure in
      the model directory, provider surrogate crash), and keep the out-of-scope list accurate.
- [ ] Add an ADR for any decision that changes a guarantee or limit (CPU limits, per-format sidecar
      policy, provider AV policy) instead of folding it into prose.
- [ ] Cross-check `docs/INDEX.md`/`docs/design/README.md` links for the new `docs/security/` tree if
      the docs index is meant to list task lanes.

## Out of scope
- Implementing the code changes (each has its own task); this task only fixes or links docs.
- Rewriting the legacy `docs/legacy/` baseline.

## Design notes
- Keep the docs' present-tense normative voice; do not add "TODO" markers where a task link is
  better.
- When behavior is intentional but undocumented, document it with the reason, not just the fact.

## Done when
- [ ] Every bullet in Context is either fixed in the referenced doc or annotated with the task that
      will change the code.
- [ ] `SECURITY.md` and the threat model reflect the audit's added controls.
- [ ] Hand-off lists each doc changed and each code claim intentionally left to a task.

## Hand-off

Docs-only; no product code or tests changed. New ADR:
`docs/design/adr/0048-handle-only-broker-objects-worker-reuse-and-directxtex-drop.md`.

Documents changed and what changed:

- `docs/design/08-installation-and-registration.md` — replaced the false "per-generation pipe,
  event, and shared-section ACLs" claim with the handle-only model (anonymous, unnamed objects with
  default security descriptors; access is handle possession plus the AppContainer token via
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`). Dropped DirectXTex from the payload list. Added the
  SEC-19/T19 note for the drifting license/SBOM metadata.
- `docs/design/02-system-architecture.md` — corrected the worker restriction wording to one
  AppContainer profile/Job Object per session (was "fresh job/sandbox" / "fresh AppContainer
  token/Job Object pairing" per generation); replaced the blanket CPU-time claim with the ADR-0031
  model (pool bounded by the per-generation wall-clock deadline; one-shot worker and hosts get the
  cumulative CPU cap). Replaced DirectXTex/WIC with WIC in the component text, topology diagram,
  dependency table, viewer-link list, and repo layout.
- `docs/design/01-product-scope.md` — NFR-14 now states the time bound precisely (ADR-0031) and
  drops DirectXTex/WIC.
- `docs/design/03-file-formats-and-ingestion.md` — WIC-only process-boundary list; texture policy
  now states TGA/HDR/DDS are not decoded (DirectXTex dropped); removed the DirectXTex link.
- `docs/design/05-thumbnail-provider.md` and
  `docs/design/adr/0008-surrogate-hosting-and-shellEx-validation.md` — documented that
  `InprocServer32` remains activatable in-process by any local process via `CLSCTX_INPROC_SERVER`,
  so the DllHost surrogate routing is Shell-path-specific and safety rests on bounded parsing
  (ADR-0005).
- `docs/design/09-quality-performance-and-security.md` — added a "tracked state" paragraph to
  Fuzzing naming the implemented targets and the still-unimplemented ones; added security-control
  bullets for NUL/control + invalid-UTF-8 reference rejection (ADR-0032) and validate-before-decode
  preflights (ADR-0030/ADR-0046); added the provider AV quarantine control (ADR-0037); made the CI
  gate explicitly mandatory (ADR-0042); dropped DirectXTex from the dependency-update list.
- `docs/design/10-delivery-plan.md`, `docs/design/11-decisions-and-risks.md` — DirectXTex dropped
  from the raster-texture plan/decision.
- `docs/design/README.md` — added a "Security hardening program" pointer to `docs/security/`.
- `SECURITY.md` — in scope now names NUL/control and invalid-UTF-8 reference handling and
  provider-surrogate escapes; out-of-scope link fixed from `.docs/FORMAT-SUPPORT.md` to
  `docs/FORMAT-SUPPORT.md` and TGA/HDR/DDS named.

Code claims intentionally left to a task (not re-asserted):

- Context bullet 4 (safe DLL search for every import child) and bullet 6 (compiled-out developer/
  fault switches) are already true after SEC-09; the documents already described the intended
  behavior, so no code task remains.
- Fuzz targets are implemented by SEC-15/16/16b/17; `09` now lists the remaining unimplemented
  lanes (protocol/shared-section descriptor, broker dependency request, cache manifests, IPC
  frame/JSON, image-metadata boundary) as not shipped.
- CPU limits, per-format sidecar policy, and provider AV policy already have ADR-0031, ADR-0032,
  and ADR-0037; no new ADR was needed for those.
- License/SBOM drift is annotated to SEC-19/T19; `THIRD-PARTY-LICENSES.md`/SBOM themselves are out
  of scope for this task.

Checks (foreground): `x64\Release\Tests.Unit.exe "~[graphics]"` →
`All tests passed (131557 assertions in 312 test cases)`. Docs-only, so no new regression coverage
was required.