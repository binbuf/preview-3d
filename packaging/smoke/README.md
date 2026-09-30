# Developer/QA-local installed Release smoke (T22/T23/T25)

Proves the production-shaped end-to-end slice on this machine: a Release build,
staged provider DLL, the STL, PLY and glTF CLSIDs + extension `ShellEx` registered, and a real
`.stl`/`.ply`/`.glb` rendered to a model-derived thumbnail through the same Shell path Explorer uses.

This is a **local developer/QA smoke, not the release artifact**. It writes per-user
(`HKCU`) keys, stages a scratch copy of the DLL and CRT, and cleans up after itself. It
does not replace the machine-level NSIS registration (T41), signing/SBOM (T42), or
clean-machine/DPI acceptance (T44). See `docs/design/adr/0020-developer-local-installed-smoke.md`.

## Layout

| Path | Purpose |
| --- | --- |
| `Invoke-ProviderSmoke.ps1` | Orchestrates build → stage → clear thumbnail cache → register → verify → unregister. Exit 0 means every check passed. |
| `Stage-ProviderSmoke.ps1` | Copies `Preview3DThumbnailProvider.dll` and its non-system runtime closure (app-local MSVC CRT) into `artifacts\smoke\stage\<Config>` and verifies the closure with `dumpbin`. |
| `Register-ProviderSmoke.ps1` | Writes each family's `CLSID`/`InprocServer32`/`ThreadingModel=Apartment`, AppID (`DllSurrogate=""`) and extension-level `ShellEx` mapping (STL + PLY + glTF `.glb`). Backs up every touched key first; never writes `DisableProcessIsolation`. |
| `Unregister-ProviderSmoke.ps1` | Removes the owned keys, restores pre-existing ones, stops any `DllHost` still holding the DLL, and prunes keys this smoke created. |
| `ProviderSmokeHost.cpp` / `.vcxproj` | `ProviderSmokeHost.exe`: the verifier (see below). Part of `Preview3D.slnx`. |
| `fixtures/smoke-cube.stl` | A small, recognizable binary STL cube (12 facets) used as the STL smoke model. |
| `fixtures/smoke-cube.ply` | A small, recognizable colored ASCII PLY cube used as the PLY smoke model. |
| `fixtures/smoke-cube.glb` | A small, recognizable embedded GLB cube (one BIN chunk, no sidecar) used as the glTF smoke model. |

## Run

From the repository root:

```powershell
pwsh -File packaging\smoke\Invoke-ProviderSmoke.ps1
```

Useful switches: `-SkipBuild`, `-KeepRegistered` (leave the HKCU keys in place),
`-SkipThumbnailCacheClear`, `-StlPath <file>`, `-PlyPath <file>`, `-GltfPath <file>`,
`-Configuration Debug`. The orchestrator runs the verifier once per family (STL, PLY, glTF) and
exits 0 only when all match.

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

## For later family tasks (T24–T34)

Add the family's CLSID, AppID and extension to the `$families` array in both register/unregister
scripts, add its `--<family>` case and CLSID/key constants to `ProviderSmokeHost.cpp`, add the model
to `Invoke-ProviderSmoke.ps1`, commit a small non-degenerate fixture, and re-run the orchestrator.
The verifier itself is family-agnostic; the CLSID and model path are the only family-specific values.