---
timeoutMin: 240
---
# T03 — SPIKE-8b: prove isolated Shell surrogate hosting

## Goal
Confirm experimentally that the T02 raster/decode prototype runs within its measured budget in the
real Windows Shell surrogate, that the handler loads outside `explorer.exe` without
`DisableProcessIsolation`, and that registration works while another app is the user's default.
This completes the Spike 8 feasibility precondition before foundation work.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — COM classes, `ThreadingModel=Apartment`, surrogate isolation, and the prohibition on `DisableProcessIsolation`.
- `docs/design/08-installation-and-registration.md` — the exact CLSID/ShellEx registry shape.
- `docs/design/adr/0005-safety-bounded-parsing-not-surrogate.md` — why surrogate isolation is crash containment, not a security boundary.
- `docs/design/testing-strategy.md` — the surrogate-soak and install-verification layers.
- `docs/tasks/02-spike-rasterizer-prototype.md` — reusable mesh/point raster and compressed-glTF spike path.

## Scope
- [ ] Build a throwaway probe handler that implements `IInitializeWithStream` + `IThumbnailProvider`, first returns a solid-color control image, then invokes the T02 mesh/point raster and bounded compressed-glTF decode path, with a fixed scratch CLSID.
- [ ] Register it under `HKLM\Software\Classes\CLSID` with a dedicated scratch extension and test the proposed extension `ShellEx` thumbnail-handler mapping. Run the same test with a third-party ProgID selected as the default and with a per-user association; inspect the effective Shell association and test ProgID registration if the proposed key does not route. Restore any pre-existing scratch-key values exactly.
- [ ] Observe and record which process actually loads the probe (`DllHost.exe` surrogate vs `explorer.exe`), including how to detect it tooling-wise (Process Explorer / ETW / `GetModuleFileName` probe).
- [ ] Verify the probe renders at 100%, 150% and 200% DPI and record the requested `cx` values.
- [ ] Measure time-to-bitmap p50/p95/max, peak private commit above an idle, loaded surrogate baseline, and decoder dependency load for the mesh, point and compressed-glTF cases inside that surrogate. Record any 2 s overrun, including time spent inside decoder calls.
- [ ] Freeze the registry location that actually works without changing the user's default. Update `design/08-installation-and-registration.md`, ADR-0006 and T41 if the proposed extension-only mapping fails; a failed routing experiment is a replan, not a passed spike.
- [ ] Clean up the probe registration and document the exact registry values inspected.

## Out of scope
- Production format parsing or the final CPU rasterizer (→ T15, T21–T34).
- Production registration (→ T41).
- Clean-machine verification (→ T44).

## Design notes
- Never set `DisableProcessIsolation=1`; record the default behavior explicitly.
- Select the third-party default through Windows Default Apps UI; do not write or synthesize the protected `UserChoice` value. Use a dedicated scratch extension and restore every test association on cleanup.
- Note the surrogate DLL search path behavior so T42 can guarantee the payload is found.
- Record the exact OS build used; surrogate behavior is version-sensitive.

## Done when
- [ ] Evidence shows the mesh/point/compressed-glTF prototype loaded and ran in the isolated surrogate, not `explorer.exe`, with no isolation opt-out set; raw timing and commit measurements are recorded.
- [ ] The qualified spike corpus meets the 750 ms p95 and 384 MiB measured process-commit targets with no unexplained 2 s overrun. If it does not, narrow the admission/prototype and rerun or replan; recorded failure alone does not complete this feasibility gate.
- [ ] The validated handler mapping resolves with both a third-party default and a per-user association, without changing the selected default.
- [ ] DPI results at 100/150/200% are recorded with the observed `cx` request sizes.
- [ ] Probe registration is fully removed and the cleanup commands are documented.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
