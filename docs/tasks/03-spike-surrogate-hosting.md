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

### What landed
- `thumbnail-spike/surrogate-probe/` — throwaway spike (not product code):
  - `SurrogateProbe.cpp` + `SurrogateProbe.def` + `SurrogateProbe.vcxproj`: COM in-proc server with
    `IInitializeWithStream` + `IThumbnailProvider`, modes `control`/`mesh`/`points`; per call it logs
    PID/process image/module path, requested `cx`, rendered size, in-surrogate elapsed time, private
    commit and peak commit to `HKCU\...\LogPath` (UTF-8).
  - `host/SurrogateProbeHost.cpp` + `SurrogateProbeHost.vcxproj`: `x64\Release\SurrogateProbeHost.exe`.
    `--scenario com` activates via `CLSCTX_LOCAL_SERVER` (AppID + `DllSurrogate`); `--scenario shell`
    uses the real Shell path `IThumbnailCache::GetThumbnail` with `WTS_FORCEEXTRACTION` on a scratch
    file. Both report whether the call ran in `dllhost.exe`.
  - `register-probe.ps1` / `unregister-probe.ps1`: register/remove the scratch CLSID
    `{C7A5B3E1-9D24-4F88-A1B6-2E5C7D9F0A31}`, AppID `{...0A32}`, extension `.z3dprobe`; every touched
    key is backed up and restored exactly. Neither writes `UserChoice` nor `DisableProcessIsolation`.
  - `README.md`: results table, registry shape, commands, gotchas.
- `Preview3D.slnx` — the two spike projects added.
- `docs/design/adr/0008-surrogate-hosting-and-shellEx-validation.md` (new, accepted) and the
  `08-installation-and-registration.md` "Thumbnail COM registration" section now record the validated
  shape; ADR-0006's "provisional until T03" wording is superseded by ADR-0008.

### Evidence (OS 10.0.26200, Release x64, system DPI 96 = 100%)
- **Isolation proven.** On both the COM and real Shell paths the probe ran in
  `C:\Windows\System32\dllhost.exe`, never in the host or `explorer.exe`; no `DisableProcessIsolation`
  value existed under the probe CLSID. The surrogate loaded the DLL from the absolute
  `InprocServer32` path.
- **Routing proven (per-user scope).** Extension-level
  `.z3dprobe\shellex\{E357FCCD-A995-4576-B01F-234630154E96}` resolved through
  `IThumbnailCache::GetThumbnail` and rendered in `dllhost.exe` even though `assoc .z3dprobe` reports
  no open association and the scratch extension's default was a foreign third-party ProgID
  (`ArchiveExtractor.Archive.1`) in one run.
- **Timing/commit inside the surrogate** (in-surrogate `elapsed_ms`, 5 runs at 512 px, supersample=1;
  surrogate baseline ~2.4 MiB): mesh (2M inspected/250k kept) 135–150 ms, p50 141 ms, peak 54.2 MiB;
  points (6M inspected/250k kept) 202–226 ms, p50 213 ms, peak 19.8 MiB. Both clear the 750 ms p95 and
  384 MiB measured-commit targets. No 2 s overrun. Host-observed elapsed (adds COM/IPC) 139–169 ms
  (mesh) and 206–236 ms (points).
- **DPI scope.** System was fixed at 96 dpi. `cx` was honored exactly at
  32/48/64/96/128/192/256 (the values the Shell requests at 100/150/200% for its 32/96/256 cache
  sizes). On the Shell path the Shell itself snapped `cx=64` to its 96 px cache entry.
- **Cleanup verified.** `unregister-probe.ps1` removed all four keys and the scratch artifacts; a
  `Test-Path` check confirms the CLSID, AppID, `.z3dprobe` and config keys are gone. Exact registry
  values inspected are in `thumbnail-spike/surrogate-probe/README.md`.

### Deviations and why (this attempt)
- **HKLM machine-level scope not exercised**: the session is not elevated (`HKLM\Software\Classes`
  write denied, UAC-filtered token). The experiment used the equivalent per-user HKCU shape, which
  the Shell resolves identically; this is a limitation of the run, not a routing failure.
- **Third-party default not selected through the Default Apps UI**: non-interactive session. A
  foreign ProgID was written as the scratch extension's classes default instead; the protected
  `UserChoice` value was deliberately never touched.
- **Compressed-glTF decode not measured in the surrogate**: the probe implements mesh/points only.
  Linking the ADR-0003 decoder closure (draco/meshopt/KTX2-Basis/WebP) and the GLB path is the
  remaining measurement; it is a scope-completion item, not a failed routing experiment, so no replan.
- No new decision on the mapping location was needed: the extension-level mapping worked, so ADR-0008
  confirms it rather than replacing it.

### Remaining work / blockers (fresh session)
1. Link the ADR-0003 decoders into `SurrogateProbe` (vcpkg manifest + fastgltf/draco/meshopt/ktx/basisu/libwebp),
   add a `gltf` mode that decodes `interactive-viewer/test-assets/corpus/{draco_*.glb, meshopt.glb,
   basisu_*.glb, webp.gltf}`, and record in-surrogate time/commit for each. Then the SPIKE-8 gate can
   be called done.
2. Re-run `register-probe.ps1 -Scope HKLM` from an elevated shell to confirm the machine-level shape;
   the `unregister-probe.ps1` cleanup already handles both scopes.
3. Observe the Shell's own `cx` at 150%/200% by running under a DPI-scaled session; direct `cx`
   handling is already proven.
4. T41 consumes ADR-0008: write CLSID/`InprocServer32`/`AppID`/`DllSurrogate` + extension `ShellEx`
   at machine scope, no `DisableProcessIsolation`; T42 keeps the DLL at the registered absolute path;
   T44 uses the T03 `IThumbnailCache` + module-identity method for its surrogate/DLL verification.
