# Tests.ProviderHost.exe — provider COM host harness (T17)

A standalone Catch2 host that exercises the built
`x64\<Config>\Preview3DThumbnailProvider.dll` the way the Shell does, without
linking it (the two COM entry points are `PRIVATE`; ADR-0010). It is the harness
every family task (T21–T34) reuses for activation, golden images, parallel
apartments and load/unload leak checks.

It is **not** the real `DllHost.exe` surrogate: isolation is proven separately by
T03 and re-qualified on a clean machine by T44. This host runs in-process and is
deterministic where it can be, so it is CI-runnable.

## Build

Through the solution (recommended), so `$(SolutionDir)` and app-local
dependencies resolve:

```
msbuild Preview3D.slnx /p:Configuration=Release /p:Platform=x64
```

Building the project directly works too, but must set `SolutionDir`:

```
msbuild tests\provider-host\Tests.ProviderHost.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=<repo root>\
```

## Run

From the repository root (the same shape as every other provider verify
command):

```
x64\Release\Tests.ProviderHost.exe                    # all host cases
x64\Debug\Tests.ProviderHost.exe
x64\Release\Tests.ProviderHost.exe "[host][com]"      # one layer
x64\Release\Tests.ProviderHost.exe "[host][golden]"
x64\Release\Tests.ProviderHost.exe "[host][parallel]"
x64\Release\Tests.ProviderHost.exe "[host][leak]"
```

Layers (`docs/design/testing-strategy.md`):

| tag | what it proves |
| --- | --- |
| `[host][com]` | `DllGetClassObject` → `IClassFactory::CreateInstance` → `IInitializeWithStream` → `IThumbnailProvider::GetThumbnail` for every frozen CLSID, then `DllCanUnloadNow == S_OK`. A linked adapter returns `S_OK` + `WTSAT_ARGB`; an unlinked family returns the `ERROR_NOT_SUPPORTED` generic-icon fallback and a null bitmap, never a fabricated image. |
| `[host][golden]` | every registered fixture rendered through the real `RunThumbnailPipeline` (adapter → T14 sampler → T15 rasterizer) matches its committed PAM golden within tolerance. |
| `[host][parallel]` | several STA apartments drive provider objects and the in-process pipeline concurrently without leaking or corrupting. |
| `[host][leak]` | repeated load/use/unload keeps GDI objects, User handles, thread count and private bytes stable within fixed tolerances. |

## Golden fixture workflow (for family tasks T21–T34)

A family task registers one fixture and commits one golden.

1. **Link the adapter into the host.** Add the family's adapter source(s) to
   `tests/provider-host/Tests.ProviderHost.vcxproj` (the `ClCompile` list) so
   `CreateFamilyAdapter(Family::X)` resolves for the fixture. The host compiles
   the same `FamilyAdapterRegistry.cpp` as the DLL, so the two agree.

2. **Register the fixture** in `FixtureRegistry.cpp`:

   ```cpp
   GoldenFixture cube;
   cube.name = "stl-cube";
   cube.family = preview3d::provider::Family::Stl;
   cube.source = ReadSmallFixture("test-assets/stl/cube.stl"); // small, committed
   cube.goldenPath = ProviderHostGoldenDirectory() + "stl-cube-256.pam";
   cube.tolerance = GoldenTolerance{2.0, 48};                  // the policy default
   cube.cx = 256;
   fixtures.push_back(std::move(cube));
   ```

   Keep fixture inputs small — never commit a multi-gigabyte binary. The
   `[host][golden]` case renders every registered fixture.

3. **Generate the golden** once, from the repository root, then commit it:

   ```
   x64\Release\Tests.ProviderHost.exe "[write-host-goldens]"
   ```

   The hidden case reruns every fixture and writes each `goldenPath` as a PAM.
   Review the output before committing; a golden that encodes a fallback is
   wrong — the fixture must render real geometry (a failed parse must assert the
   null-bitmap error path instead, per `docs/design/testing-strategy.md`).

4. **Verify** both configurations and the dependency closure:

   ```
   x64\Release\Tests.ProviderHost.exe
   x64\Debug\Tests.ProviderHost.exe
   x64\Release\Tests.Unit.exe
   pwsh -File tests\unit\check-provider-dependency-closure.ps1 -Configuration Release
   ```

The comparator (`GoldenImage.h`) is mean-absolute-error per channel plus a
max-outlier guard (`meanAbs <= 2.0`, `maxAbs <= 48` by default), the same metric
the T15 rasterizer goldens use. It tolerates platform/CPU rounding and catches
real regressions; it is not byte equality.

## Notes

- `GoldenImage.h`, `ProviderHostSupport.h` and `FixtureRegistry.h` are the only
  host-only headers. The pipeline, sampler, rasterizer and DIB boundary are the
  shipped `thumbnail-provider` sources, compiled in — the host is not a copy.
- The `Family::Unknown` placeholder fixture (`placeholder-sphere`) proves the
  harness before the first real adapter; keep it or replace it as families land.