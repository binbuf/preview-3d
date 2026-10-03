# Developer/QA-local installed Release smoke (T22/T23/T25/T31/T32/T33/T34)

Proves the production-shaped end-to-end slice on this machine: a Release build,
staged provider DLL, all eight CLSIDs (STL, PLY, glTF, FBX, 3MF, USD, STEP) + extension `ShellEx`
registered, and a real `.stl`/`.ply`/`.glb`/`.fbx`/`.3mf`/`.usda`/`.stp` rendered to a
model-derived thumbnail through the same Shell path Explorer uses.

This is a **local developer/QA smoke, not the release artifact**. It writes per-user
(`HKCU`) keys, stages a scratch copy of the DLL and CRT, and cleans up after itself. It
does not replace the machine-level NSIS registration (T41), signing/SBOM (T42), or
clean-machine/DPI acceptance (T44). See `docs/design/adr/0020-developer-local-installed-smoke.md`.

## Layout

| Path | Purpose |
| --- | --- |
| `Invoke-ProviderSmoke.ps1` | Orchestrates build → stage → clear thumbnail cache → register → verify → unregister. Exit 0 means every check passed. |
| `Stage-ProviderSmoke.ps1` | Copies `Preview3DThumbnailProvider.dll` and its non-system runtime closure (app-local MSVC CRT) into `artifacts\smoke\stage\<Config>` and verifies the closure with `dumpbin`. |
| `Register-ProviderSmoke.ps1` | Writes each family's `CLSID`/`InprocServer32`/`ThreadingModel=Apartment`, AppID (`DllSurrogate=""`) and extension-level `ShellEx` mapping (STL + PLY + glTF `.glb` + FBX `.fbx` + 3MF `.3mf` + USD `.usd`/`.usda`/`.usdc`/`.usdz` + STEP `.step`/`.stp`). Backs up every touched key first; never writes `DisableProcessIsolation`. |
| `Unregister-ProviderSmoke.ps1` | Removes the owned keys, restores pre-existing ones, stops any `DllHost` still holding the DLL, and prunes keys this smoke created. |
| `ProviderSmokeHost.cpp` / `.vcxproj` | `ProviderSmokeHost.exe`: the verifier (see below). Part of `Preview3D.slnx`. |
| `fixtures/smoke-cube.stl` | A small, recognizable binary STL cube (12 facets) used as the STL smoke model. |
| `fixtures/smoke-cube.ply` | A small, recognizable colored ASCII PLY cube used as the PLY smoke model. |
| `fixtures/smoke-cube.glb` | A small, recognizable embedded GLB cube (one BIN chunk, no sidecar) used as the glTF smoke model. |
| `fixtures/smoke-cube.fbx` | The small binary FBX cube from the FBX corpus, used as the FBX smoke model. |
| `fixtures/smoke-cube.3mf` | The small Core 3MF unit cube from the 3MF corpus, used as the 3MF smoke model. |
| `fixtures/smoke-cube.usda` | An original Preview3D ASCII USDA unit cube (12 triangles), used as the USD smoke model. |
| `fixtures/smoke-cube.stp` | The committed AP214 analytic part with a through-hole and shape color, used as the STEP smoke model. |
| `fixtures/hostile-truncated.stl` | A truncated binary STL (a declared facet count with no facet data) used as the SEC-08 hostile-input soak model; the provider must refuse it without crashing. |

## Run

From the repository root:

```powershell
pwsh -File packaging\smoke\Invoke-ProviderSmoke.ps1
```

Useful switches: `-SkipBuild`, `-KeepRegistered` (leave the HKCU keys in place),
`-SkipThumbnailCacheClear`, `-StlPath <file>`, `-PlyPath <file>`, `-GltfPath <file>`,
`-FbxPath <file>`, `-MfPath <file>`, `-UsdPath <file>`, `-StepPath <file>`, `-Configuration Debug`. The orchestrator runs the verifier once per family
(STL, PLY, glTF, FBX, 3MF, USD, STEP) and exits 0 only when all match.

The orchestrator builds the provider and the smoke host directly (with
`/p:SolutionDir=<repo>\`) so it is not blocked by unrelated projects. The full-solution
Release build (`msbuild Preview3D.slnx /p:Configuration=Release /p:Platform=x64`) builds
the same DLL and `ProviderSmokeHost.exe`; in this environment it still fails only at the
pre-existing, unrelated `compatibility-host-step` OCCT `x64-windows-static-md` gap
(`C1083 BRepBndLib.hxx`).

Evidence is written to `artifacts\smoke\evidence\<Config>\`:

- `reference.pam` — the DLL's in-process render of the family model (written per family; the PLY run overwrites the STL one);
- `shell.pam` — the Shell `IThumbnailCache::GetThumbnail` render;
- `report.txt` — the pass/fail lines (surrogate, opt-out, image diff) for the last family run.

## What the verifier checks

1. No `DisableProcessIsolation` value under the CLSID or AppID, in either hive.
2. The staged DLL renders the family model in-process through its PRIVATE `DllGetClassObject`
   (an `HBITMAP` cannot be marshaled across a surrogate, so this is the reference image).
3. The DLL is not left loaded in the smoke host.
4. `CoCreateInstance(CLSCTX_LOCAL_SERVER)` succeeds and a module-identity scan finds
   `Preview3DThumbnailProvider.dll` mapped in `dllhost.exe`, not in the caller.
5. `IThumbnailCache::GetThumbnail` (with `WTS_FORCEEXTRACTION`) returns a bitmap and its
   pixels match the reference within tolerance — so the Shell thumbnail is model-derived
   and produced by this provider, not a generic icon.

## Cache, registration and cleanup notes

- The Shell caches thumbnails aggressively. The smoke deletes
  `%LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db` (best effort; locked files are
  reported) and requests `WTS_FORCEEXTRACTION`, which bypasses the cache.
- Default scope is `HKCU\Software\Classes`; T03 showed the Shell resolves the per-user shape
  identically to `HKLM`. Pass `-Scope HKLM` from an elevated shell for the machine shape.
- The AppID with an empty `REG_SZ` `DllSurrogate` is what routes an explicit
  `CLSCTX_LOCAL_SERVER` activation into `DllHost.exe`; the Shell's own thumbnail path isolates
  by default (ADR-0008). `DisableProcessIsolation` is never set (ADR-0005).
- `Unregister-ProviderSmoke.ps1` restores exactly the keys that pre-existed and removes only
  the values/keys this smoke added, so the machine's existing `.stl` associations are kept.

## Adding a family

Add the family's CLSID, AppID and extension to the `$families` array in both register/unregister
scripts, add its `--<family>` case and CLSID/key constants to `ProviderSmokeHost.cpp`, add the model
to `Invoke-ProviderSmoke.ps1`, commit a small non-degenerate fixture, and re-run the orchestrator.
The verifier itself is family-agnostic; the CLSID and model path are the only family-specific values.
T34 completed the eighth family (STEP/STP); T41 must still move these smoke identities to the real
installer rules. The STEP DLL statically contains OCCT, so it stages no extra runtime DLL.

## Surrogate soak (SEC-17)

`ProviderSmokeHost.exe --soak` is the Explorer-stability soak. It repeatedly
renders the smoke fixtures through the real Shell path
(`IThumbnailCache::GetThumbnail` with `WTS_FORCEEXTRACTION`) from several
concurrent STA apartments, so the provider is loaded, exercised and torn down in
the real `DllHost.exe` surrogate many times while the thumbnail cache is churned.
It asserts:

- no crash/hang: every valid call must return a bitmap (a surrogate crash makes
  the next `GetThumbnail` fail and is counted as a failure, never a pass);
- no persistent surrogate after the last release: the `DllHost` hosting
  `Preview3DThumbnailProvider.dll` must disappear within 30 s;
- no monotonic growth on the surrogate process(es): GDI `+64`, User `+64`,
  handles `+256`, threads `+8`, private bytes `+32 MiB` are the tolerances
  (raw baseline/final numbers are written to `soak-report.txt`).

SEC-17/T25 adds the SEC-08 hostile-input classification. The soak keeps a
handle on every `dllhost` surrogate that hosts the provider and reads its exit
code when it dies, so a fault is classified instead of reported as an anonymous
crash:

- a valid request refused while no surrogate died is a **contained access
  violation / quarantine** — recorded as a failure-with-reason and the soak
  exits nonzero (the process-global quarantine refuses later requests, so any
  valid-phase failure is evidence of it);
- a surrogate death with a re-raised stack overflow (`0xC00000FD`) or an
  uncatchable `__fastfail`/stack-cookie fault (`0xC0000409`, `0xC0000602`) is
  the documented SEC-08 **allowed** failure — recorded with its exit code, and
  the run reports `ALLOWED`, not `PASS`;
- any other surrogate death is an unexpected fault and fails the soak.

```powershell
# Stage and register exactly as the smoke above, then:
x64\Release\ProviderSmokeHost.exe --soak ^
  --dll artifacts\smoke\stage\Release\Preview3DThumbnailProvider.dll ^
  --stl packaging\smoke\fixtures\smoke-cube.stl ^
  --ply packaging\smoke\fixtures\smoke-cube.ply ^
  --gltf packaging\smoke\fixtures\smoke-cube.glb ^
  --fbx packaging\smoke\fixtures\smoke-cube.fbx ^
  --mf packaging\smoke\fixtures\smoke-cube.3mf ^
  --usd packaging\smoke\fixtures\smoke-cube.usda ^
  --step packaging\smoke\fixtures\smoke-cube.stp ^
  --hostile packaging\smoke\fixtures\hostile-truncated.stl ^
  --apartments 4 --iterations 100 --cx 256 --out artifacts\smoke\evidence\Release\soak
```

`--hostile <path>` is repeatable and adds an expected-fail-closed lane: a
refused thumbnail on a hostile input is counted as `rejected`, not a crash,
while a valid input failing afterwards still surfaces a quarantine. The
exit-code classifier has a deterministic self-check:

```powershell
x64\Release\ProviderSmokeHost.exe --soak-classify-selftest
```

The SoakHost is built with the verifier
(`packaging\smoke\ProviderSmokeHost.vcxproj`, `ProviderSoak.{h,cpp}`). It is
crash containment, not a security boundary (ADR-0005): the value is Explorer
stability and leak detection.

Measured Release reference run on the development machine (SEC-17: 7 families, 4
apartments x 100 iterations + 4 warm-up = 404 thumbnails): 0 failed, one
`dllhost.exe` surrogate, GDI 0 -> 0, User 6 -> 8, handles 189 -> 207, threads
10 -> 11, private bytes 4,665,344 -> 9,818,112 (+5.15 MiB), teardown in 5.1 s,
no persistent surrogate.

T25 classification run (7 valid families, 2 apartments x 20 iterations, plus the
hostile lane at 2 x 20): valid 40/40 pass, hostile `rejected=40`
(`first_hostile_failure_hr=0x8004B200`), `quarantined=0`, `allowed_faults=0`,
`unexpected_faults=0`, growth within tolerance, exit 0; elapsed 7.0 s, host peak
working set 30.8 MB. `--soak-classify-selftest` passes all six exit-code cases.