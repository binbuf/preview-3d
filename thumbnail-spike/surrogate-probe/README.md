# SPIKE-8b — isolated Shell surrogate hosting (T03)

Throwaway probe proving that the T02 CPU rasterizer runs inside the Windows Shell's out-of-process
thumbnail surrogate (`DllHost.exe`), that the extension-level `ShellEx` mapping resolves, and that the
provider meets its time/commit targets there. Not product code; registration is removed again by
`unregister-probe.ps1`.

## Layout

| Path | Purpose |
| --- | --- |
| `SurrogateProbe.cpp` | COM in-proc server: `IInitializeWithStream` + `IThumbnailProvider`, modes `control`/`mesh`/`points`/`gltf`/`image`, per-call logging of process identity, `cx`, time, commit and decoder. |
| `GltfSpikeDecode.{h,cpp}` | Throwaway bounded compressed-glTF decode over the ADR-0003 closure (fastgltf + draco + meshoptimizer + KTX2/Basis + libwebp). `gltf` mode decodes the corpus fixtures; `image` mode decodes a standalone `.ktx2`/`.webp`. |
| `SurrogateProbe.def` | Exports `DllGetClassObject` / `DllCanUnloadNow`. |
| `host/SurrogateProbeHost.cpp` | `SurrogateProbeHost.exe`: activates the probe via `CLSCTX_LOCAL_SERVER` (`com`) or the real Shell path `IThumbnailCache::GetThumbnail` (`shell`), drives the `cx` matrix, reads back the probe records. |
| `register-probe.ps1` | Writes the probe CLSID/AppID/`InprocServer32` + scratch-extension `ShellEx` mapping; backs up every touched key first. Never writes `UserChoice`, never sets `DisableProcessIsolation`. |
| `unregister-probe.ps1` | Removes owned keys and restores the exact pre-registration state. |

Scratch identities: CLSID `{C7A5B3E1-9D24-4F88-A1B6-2E5C7D9F0A31}`, AppID
`{C7A5B3E1-9D24-4F88-A1B6-2E5C7D9F0A32}`, extension `.z3dprobe` (all throwaway).

## Build and run

```
msbuild thumbnail-spike\surrogate-probe\SurrogateProbe.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild thumbnail-spike\surrogate-probe\host\SurrogateProbeHost.vcxproj /p:Configuration=Release /p:Platform=x64

pwsh -File thumbnail-spike\surrogate-probe\register-probe.ps1 -DllPath x64\Release\SurrogateProbe.dll
x64\Release\SurrogateProbeHost.exe --scenario shell --mode control --file %TEMP%\probe.z3dprobe --cx 32,64,256
x64\Release\SurrogateProbeHost.exe --scenario com --mode mesh --cx 512 --runs 5
x64\Release\SurrogateProbeHost.exe --scenario com --mode points --cx 512 --runs 5
x64\Release\SurrogateProbeHost.exe --scenario com --mode gltf --gltf interactive-viewer\test-assets\corpus\draco_triangle.glb --cx 512 --runs 5
x64\Release\SurrogateProbeHost.exe --scenario com --mode gltf --gltf interactive-viewer\test-assets\corpus\basisu_textured_triangle.glb --cx 512 --runs 5
x64\Release\SurrogateProbeHost.exe --scenario com --mode image --gltf interactive-viewer\test-assets\basisu_sample.ktx2 --cx 512 --runs 5
pwsh -File thumbnail-spike\surrogate-probe\unregister-probe.ps1
```

Building the probe directly (not through the solution) leaves `$(SolutionDir)` unset, so pass
`/p:SolutionDir=<repo root>\` to keep `x64\Release\` as the output directory.

## Results (Windows 11 build 10.0.26200, Release x64, system DPI 96 = 100%)

Isolation, both activation paths: the probe always executed in `C:\Windows\System32\dllhost.exe`,
never in `SurrogateProbeHost.exe` and never in `explorer.exe`, with no `DisableProcessIsolation`
value present under the probe CLSID. The surrogate DLL was loaded from the absolute path written to
`InprocServer32` and `DllSurrogate` was the empty `REG_SZ`.

Registry shape used (per-user HKCU because the run was not elevated; `HKLM` is the production scope):

```
HKCU\Software\Classes\CLSID\{C7A5B3E1-...-0A31}
    (Default)      = "Preview 3D Surrogate Probe (T03 spike)"
    AppID          = "{C7A5B3E1-...-0A32}"
  \InprocServer32
    (Default)      = <absolute path>\SurrogateProbe.dll
    ThreadingModel = "Apartment"
HKCU\Software\Classes\AppID\{C7A5B3E1-...-0A32}
    (Default)      = "Preview 3D Surrogate Probe (T03 spike)"
    DllSurrogate   = ""            ; empty REG_SZ routes CLSCTX_LOCAL_SERVER into DllHost
HKCU\Software\Classes\.z3dprobe\shellex\{E357FCCD-A995-4576-B01F-234630154E96}
    (Default)      = "{C7A5B3E1-...-0A31}"
```

Per-call timing and commit, measured inside the surrogate (probe `elapsed_ms`; commit from
`GetProcessMemoryInfo` with the surrogate baseline of ~2.4 MiB):

| Mode | cx | in-surrogate ms (5 runs) | p50 | max | peak commit |
| --- | ---: | --- | ---: | ---: | ---: |
| control | 32..256 | 0.03–0.17 | — | 0.17 | ~2.4 MiB |
| mesh (2M inspected / 250k rasterized) | 512 | 141.2, 141.4, 135.4, 150.3, 139.7 | 141.2 | 150.3 | 54.2 MiB |
| points (6M inspected / 250k rasterized) | 512 | 209.5, 215.4, 226.1, 213.4, 202.3 | 213.4 | 226.1 | 19.8 MiB |

Both cap cases clear the 750 ms p95 target and the 384 MiB measured-commit target with wide margin.
Host-observed elapsed (includes COM/IPC and activation) was 139–169 ms (mesh) and 206–236 ms (points).

Compressed-glTF decode, same surrogate and session (512 px, 5 runs):

| Fixture | Mode | Decoder | in-surrogate ms | peak commit | image decoded |
| --- | --- | --- | --- | ---: | --- |
| `draco_triangle.glb` | gltf | draco | 15.1–15.9 | 8.8 MiB | — |
| `draco_position_only.glb` | gltf | draco | 15.6–16.5 | 8.9 MiB | — |
| `meshopt.glb` | gltf | meshopt | 14.5–14.8 | 9.0 MiB | — |
| `basisu_textured_triangle.glb` | gltf | plain + ktx2 | 14.5–28.6 | 9.0 MiB | 8×8 KTX2/Basis |
| `webp.gltf` | gltf | plain + webp | 14.7–15.0 | 9.1 MiB | 1×1 WebP |
| `basisu_sample.ktx2` | image | ktx2/basisu | 0.53–0.64 | 9.1 MiB | 8×8 |
| `sample.webp` | image | webp | 0.55–0.67 | 9.1 MiB | 1×1 |

The whole ADR-0003 decoder closure (fastgltf, draco, meshoptimizer, libktx/basisu, libwebp) runs
statically inside `dllhost.exe`; no third-party DLL has to be found on the surrogate's search path.

Routing:
- Extension-level `ShellEx` resolved through `IThumbnailCache::GetThumbnail` and rendered in
  `dllhost.exe` even though `assoc .z3dprobe` reports no open association and the scratch extension's
  default was a foreign third-party ProgID (`ArchiveExtractor.Archive.1`) in one run and an
  unregistered scratch ProgID in another. So thumbnails do not require the extension to have an open
  association, and a third-party default does not shadow the handler.
- `cx` is honored exactly (32/48/64/96/128/192/256 → same bitmap size). On the Shell path the Shell
  snapped `cx=64` to its 96 px cache entry (`bitmap=96x96`), as documented Shell behavior.

## Scope not covered by this run (remaining work)

1. **Machine-level HKLM registration** was not exercised: the session is not elevated (`HKLM\Software\Classes` write is denied, UAC-filtered token). Re-run `register-probe.ps1 -Scope HKLM` from an elevated shell and confirm the same `dllhost` result.
2. **Third-party default selected through the Default Apps UI** could not be driven from a non-interactive session; a foreign ProgID was written as the scratch extension's classes default instead. The protected `UserChoice` value was never written.
3. **100/150/200% DPI** were not produced by changing session DPI (fixed at 96 here); the DPI-scaled `cx` values (48/144/384 and 64/192/512) were only exercised as direct `cx` requests. Change display scaling or run under a DPI-scaled session host to observe the Shell's own requests.
4. **Compressed-glTF decode** is measured (see above). The `.ktx2`/`.webp` fixtures are tiny, so the
   image-decoder timing is a dependency-load proof, not a stress profile; the production
   `TranscodeKtx2BasisImage` bounds tests remain the authority on limits.

## Gotchas

- `DllSurrogate` must be present (empty string) on the CLSID's `AppID` for `CLSCTX_LOCAL_SERVER` to route to `DllHost.exe`; without it COM loads the in-proc DLL into the caller.
- The probe reads its `Mode`/`LogPath`/`RunToken` from `HKCU\Software\Preview3DThumbnailSpike`; a fresh surrogate must be started after replacing the DLL (kill only the target `dllhost.exe` PID).
- The probe log is UTF-8; `GetProcessMemoryInfo` `PrivateUsage` settles after the scene is freed, so **peak** commit is the meaningful cap number, not the post-call delta.
- `unregister-probe.ps1` removes its own `.probe-state` backup directory; keep scratch files outside it.
- Any translation unit that includes `<fastgltf/*.hpp>` must define `FASTGLTF_ENABLE_DEPRECATED_EXT=1`
  to match vcpkg's `FASTGLTF_ENABLE_DEPRECATED_EXT=ON` library build. The vcpkg CMake config publishes
  this through `INTERFACE_COMPILE_DEFINITIONS`, but MSBuild consumes fastgltf through autolink only, so
  the define has to be set explicitly; without it, parsing a fixture with a material crashes inside
  `fastgltf::Parser::loadGltf` (`Preview3DImportWorker.vcxproj` already sets it).