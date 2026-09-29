# USD-002 OpenUSD host and resolver spike

Date: 2026-09-17
Decision: **proceed with the isolated monolithic compatibility payload**
Product status: test-only spike; USD-003 still owns the production protocol and no USD extension is registered

## Result

OpenUSD 26.08 composes USDA layers and reads USDZ entirely through a
`preview3d://` byte resolver in a distinct zero-capability AppContainer. The
spike proves sublayers, references, initially-unloaded payloads, and texture
assets without giving the host a model path or directory handle. Crash, hang,
Job-memory exhaustion, malformed shared output, resolver denial, and payload
budget exhaustion are generation-local; a fresh host imports the next valid
request.

Proceed with this shape in USD-003/006, with these non-negotiable constraints:

- Keep the compatibility payload in its own `OpenUsdHost` directory and grant
  that AppContainer read/execute access only to that directory. Do not merge it
  with the general worker payload.
- The bootstrap executable must remain free of OpenUSD imports. It locks the
  DLL search path, clears discovery environment variables, maps only inherited
  handles, and only then loads the core DLL by absolute path.
- Every model/dependency identifier must remain under `preview3d://`. Relative
  arcs are anchored inside that namespace; absolute paths, other URI schemes,
  backslashes, drive syntax, and namespace escape are rejected. The default
  resolver is used only for the hash-audited installed schema resources.
- Production must preserve `LoadNone`, breadth-first payload loading, byte/open/
  payload/depth/time limits, and the Job commit backstop. OpenUSD exposes no
  useful cooperative cancellation point for the composed-stage operation, so
  cancellation remains bounded host termination and replacement.

## Immutable dependency and build

| Item | Value |
| --- | --- |
| Upstream | `PixarAnimationStudios/OpenUSD` |
| Release | `v26.08` / `26.8.0` |
| Commit | `ee47c679abde5b467a7b6a41f3b2285564a4222e` |
| Tag object | `cb561cc77b08a38546431e1346996afda41682ec` |
| Source tarball SHA-512 | `140c0d81b71f7b2a815dea78da37ebc85ebf7f1858f5f96ef5d497a8424a3f0722f0713b9908d659b0ad3d0e0a2d1093b2e90d38d36f60dd015f6cb662b6971e` |
| SBOM identity | `pkg:github/PixarAnimationStudios/OpenUSD@ee47c679abde5b467a7b6a41f3b2285564a4222e` (`versionInfo` `26.8.0`) |
| License | Apache-2.0; upstream `LICENSE.txt` and `NOTICE.txt` installed by the overlay |
| Runtime dependency | oneTBB 2023.1.0 `tbb12.dll`; no Python runtime |

The checked-in overlay uses a dynamic monolithic build with safety preferred
over speed. Python, imaging/Hydra, usdview, tools, examples, tutorials, tests,
validation, Exec, MaterialX, Alembic, Draco, RenderMan, OpenImageIO,
OpenColorIO, OpenVDB, PTex, OSL, HDF5, GL, and Vulkan are disabled. Plugin-path
lookup is compiled to the renamed `PREVIEW3D_DISABLED_PLUGIN_PATH` variable and
the bootstrap clears both it and the conventional discovery variables before
loading OpenUSD.

The overlay also normalizes upstream's Windows monolithic DLL into `bin`,
removes non-relocatable CMake metadata containing build-machine paths, and
retains only installed headers, libraries, licenses, and resources needed by
the consuming project.

### Monolithic versus shared

Both forms were built from the pinned source with the same Release feature
flags on this machine.

| Form | OpenUSD DLLs | DLL bytes | Installed USD resources | Assessment |
| --- | ---: | ---: | ---: | --- |
| Minimal shared | 33 | 21,945,856 | 70 files / 791,709 B | Larger aggregate DLL payload and a much broader load/servicing allowlist |
| Chosen monolithic | 1 `usd_ms.dll` | 18,053,120 | 13 files / 344,075 B | Smaller, one OpenUSD load target, exact resource allowlist |

The chosen Release runtime is 18,402,816 bytes including the 349,696-byte TBB
DLL. Shared linkage offered no footprint advantage and materially enlarged the
DLL and plugin search surface, so the spike selects monolithic linkage.
The baseline TBB port also builds hwloc 2.11.2, but neither `usd_ms.dll` nor
`tbb12.dll` imports or deploys a hwloc DLL in this payload; retain it in build
SBOM data without adding it to the runtime inventory.

## Host and authority boundary

`Preview3DImportHost.exe` is a 31,232-byte bootstrap with only Windows/CRT
imports. `Preview3DOpenUsdCore.dll` is loaded by absolute path after
`SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 |
LOAD_LIBRARY_SEARCH_USER_DIRS)` and `AddDllDirectory` restrict lookup to the
private payload. The core is deliberately separate because MSVC cannot
delay-load `usd_ms.dll`: OpenUSD exports imported data symbols, producing
`LNK1194` with `/DELAYLOAD`.

The existing launcher creates the process suspended in a distinct
zero-capability AppContainer, supplies an explicit inherited-handle list,
assigns a Job with active-process limit 1 and process-memory limit
`min(4 GiB, 35% physical RAM)`, and resumes only after Job assignment. The
spike inherits two control pipes and one bounded shared section. It receives no
source path, model-directory handle, token capability, socket, or writable
asset interface. The AppContainer denies network use despite OpenUSD's static
`WS2_32.dll` import, and the Job prevents a child process.

The parent copies approved assets into a section capped at 64 MiB and 64
entries. `ByteStore` copies each bounded range before OpenUSD access. The URI
resolver returns only `ArAsset` byte buffers; `GetFileUnsafe` and writable
assets are unavailable. Missing assets increment a denial counter instead of
falling back to the filesystem. The tested malicious plugin-path environment
is ignored, and the registry audit rejects a plugin or resource outside the
private payload.

The host opens with `UsdStage::LoadNone`. It discovers unloaded payload prims
with `TraverseAll`, sorts each breadth, and stops at the request's payload,
resolver-open, total-byte, graph-depth, or wall-clock bound. Composition errors
are fatal even if some root geometry would otherwise be displayable.

## Release payload inventory

Executable/DLL SHA-256 values for the verified Release build:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `Preview3DImportHost.exe` | 31,232 | `29a57cdfbc58f1ae9624a5e1db693fff09524f62f3b91141f4d61401c6e52be3` |
| `Preview3DOpenUsdCore.dll` | 133,632 | `6ae99494d3c82b3d9d8d31a6298ed40b6bc8fc107e90c0297c056c7b56896a1c` |
| `usd_ms.dll` | 18,053,120 | `18e6ac3001caf5852284290e8d8231d3544a1562ac6730d885460c4e7aa9acc0` |
| `tbb12.dll` | 349,696 | `4cc8251c6f71ebcf2aa59ab9b673b36f26605d8e7f353cce0fdc2c7191d6b972` |

The 13 allowed `usd/` files and hashes are embedded in `OpenUsdHost.cpp` and
checked before registry initialization. They are the root manifest, `ar` and
`sdf` manifests, the product URI-resolver manifest, and the `usd`, `usdGeom`,
and `usdShade` manifests/generated schemas/schema layers. Recursive inventory
must match the list exactly, so missing, modified, or unlisted files fail with
`PayloadIntegrityFailure`. The product resolver manifest is
`06c9138c6376571196fa50f5e5d9c7a4fc800a6c94981e8a4d5eda917f10de53`;
the other 12 hashes are recorded directly beside their paths in the source.
Resource digests are computed over line-ending-normalized (LF) text so the
audit does not depend on the checkout's git EOL policy.

This layout is relocatable only as one directory tree: bootstrap, core,
`usd_ms.dll`, `tbb12.dll`, and `usd/`. Moving individual files or adding a
resource invalidates the audit.

## Composition and equivalence evidence

The brokered corpus composes a root USDA with a sublayer, reference, initially
unloaded payload, Preview Surface asset input, Z-up/centimeter metadata, and a
translated hierarchy. It produces four meshes, 12 points, four faces, one
payload load, one texture open, bounds `(2,3,4)`–`(3,4,4)`, and stable semantic
digest `44dfa16764663b74`. A missing reference is a resolver denial, not a
filesystem lookup. The TinyUSDZ `cube.usdz` fixture also opens directly from
one broker byte entry through OpenUSD's package resolver.

For the common `mesh.usda` fixture, TinyUSDZ and OpenUSD agree on the overlap
used by this spike: one hierarchy root, one mesh, four points, one quad (two
triangles after TinyUSDZ normalization), Z-up, `0.01` meters/unit, the authored
translation, and world bounds `(1,2,4)`–`(3,4,4)`. TinyUSDZ's stable full
render-data fingerprint remains `3fbe0a4e45dc13e3`; OpenUSD intentionally has
a different spike digest because it hashes raw face topology rather than the
fast path's triangulated output. USD-003 must define the single production
canonical digest before either adapter publishes results; this spike proves
the source semantics needed to do that, not digest-byte identity between two
temporary representations.

## Measurements

Environment: Windows 11 Pro 10.0.26200, Ryzen 9 7900X3D, 64.7 GiB visible RAM,
MSBuild 18.10.1/MSVC 14.51, x64. Each row is a fresh host; the Debug/Release
values below are representative runs after the OS cache was warm and are not
release gates.

| Build | Bootstrap/resource setup | `LoadNone` | First normalized geometry | Total | Private bytes | Peak working set |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Debug | 14,306 us | 25,869 us | 153,385 us | 153,693 us | 28,041,216 | 48,914,432 |
| Release | 4,767 us | 3,492 us | 13,477 us | 13,513 us | 20,500,480 | 25,661,440 |

The hang/cancel probe is terminated through the Job after 150 ms. Crash, hang,
and a 64 MiB Job-memory exhaustion each exit without a valid reply; Job
termination completes within the 5-second test bound, and the subsequent fresh
host succeeds. OpenUSD initialization is process-global, so a warm persistent
host was deliberately not selected for this spike: production pooling in
USD-006 must retain generation isolation or prove an equally strong reset.

## Verification and decision

Focused tests pass in both configurations:

```text
Debug:   5 cases, 201 assertions
Release: 5 cases, 201 assertions
```

They cover environment/plugin hardening, exact resource audit, composed USDA,
USDZ bytes, common TinyUSDZ/OpenUSD semantics, `LoadNone` plus bounded payload
loading, invalid shared-section output, denied resolver access, payload budget,
crash, hang, Job memory termination, and restart.

Decision: **proceed**. USD-003 may define the format-neutral protocol and final
canonical digest. USD-006 must convert this test contract into the production
broker/host lifecycle without widening the URI resolver, payload directory, or
plugin/resource allowlist.
