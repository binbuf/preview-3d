# TSK-209 verification

TSK-209 finishes the limited-MVP static glTF subset. The sandboxed import
worker now accepts `KHR_mesh_quantization`, bounded
`EXT_meshopt_compression`, and static `EXT_texture_webp`, while preserving the
existing Draco, KTX2/Basis, PNG/JPEG, material, precision and progressive paths.

## Implemented behavior and exact limits

- Quantized POSITION and TEXCOORD accessors accept signed/unsigned 8/16-bit,
  normalized or unnormalized storage. NORMAL and TANGENT accept normalized
  signed 8/16-bit storage. Existing FLOAT and core color/index forms remain.
- Each meshopt bufferView is decoded once and retained in the worker cache.
  Declared `count * byteStride` must exactly equal the logical bufferView
  length. Attribute stride is a multiple of four and at most 256 bytes;
  triangle/index stride is 2 or 4 bytes, triangle count is divisible by three,
  and filter/stride combinations are closed. The pinned fastgltf API exposes
  NONE/OCTAHEDRAL/QUATERNION/EXPONENTIAL filters; its version does not expose
  the newer COLOR filter, so COLOR is not claimed. One decoded view is capped
  at 512 MiB and also at the remaining dynamic worker scratch budget (at most
  1 GiB or one quarter physical RAM, including parser/images/other views).
- Required meshopt decode/range/sidecar failure is fatal. If meshopt is optional,
  its valid core bufferView is the fallback; missing core geometry is still
  fatal. This is compressed-data decode only—no meshoptimizer LOD builder or
  intermediate hierarchy was added.
- WebP must sniff as RIFF/WebP and agree with `image/webp` when MIME is present.
  Animated WebP is rejected to semantic fallback. Static decode uses an
  external RGBA output allocation, optional decoder scaling, and the existing
  cancellable semantic mip generator. Limits are 256 MiB aggregate encoded
  images, 32 MiB per decoded mip chain, 128 MiB aggregate decoded texture
  bytes, 2,048 pixels per output axis and one billion aggregate decoded pixels.
  Corrupt/unsupported/over-budget optional images emit the existing bounded
  warning and deterministic color/data/normal/emissive fallback.
- The parser enables only Draco, Basis/KTX2, texture transform, mesh
  quantization, meshopt, WebP and unlit from the documented static subset.
  Unknown required extensions fail with `UnsupportedRequiredFeature`; unknown
  optional extensions produce at most 64 warnings. Data URIs keep the existing
  32 MiB allocation cap. Valid zero-base sparse/non-indexed accessors now work
  in both streaming and single-section paths; sparse indices remain strictly
  increasing and all base/indices/value extents are checked before fastgltf.
- External BIN/PNG/JPEG/WebP/KTX2 stays sibling-only and handle-brokered. The
  worker has no path or network authority. Existing absolute/UNC/network,
  traversal, encoded traversal, ADS, wrong-extension and containment tests are
  unchanged. Required geometry sidecars fail rather than yield partial scenes.

## Dependency, license, SBOM and threat review

The pinned vcpkg baseline is unchanged. The import worker now links
`meshoptimizer` 1.2 (MIT) and `libwebp` 1.6.0#3 plus packaged `libsharpyuv`
(upstream BSD-style notice). Installed copyright/SPDX records are the source
for release notices and SBOM. Neither decoder links into `Preview3D.exe` or the
thumbnail provider; both run only in the zero-capability AppContainer worker
behind Job Object memory/process limits and copy-then-validate publication.
`directxtex` was removed from `vcpkg.json`: no included limited-MVP feature uses
it while TGA/DDS/HDR are deferred.

Meshoptimizer documents its decoder as safe on untrusted input but able to
produce garbage; the wrapper therefore validates shapes before entry and the
normal geometry path validates every resulting accessor/index/bound afterward.
Libwebp writes into caller-owned bounded output; its internal allocations are
still contained by the worker Job. Both wrappers check cancellation before and
after library calls. Cooperative interruption during a single library call and
decoder fuzz automation remain TSK-301/stabilization work; frozen malformed
seeds and the full hostile-worker protocol suite are checked here. Compressed
glTF remains Tier A for ordinary/meshopt data with independently bounded decode
units; reference-system startup/frame/input classification remains TSK-302.

## Verification evidence

Debug and Release solution builds complete successfully. Catch2 results:

- `Tests.Unit`: 85 cases; 7,318 Debug / 7,230 Release assertions.
- `Tests.ImportIsolation`: 198 cases / 51,933 assertions in each configuration.
- New focused coverage uses the pinned meshoptimizer encoder/decoder ABI,
  corrupt stream and expansion/stride limits; valid/corrupt/encoded/decoded
  WebP boundaries; normalized integer quantization; required-extension
  rejection; frozen meshopt GLB; brokered WebP and sparse sidecars.
- The full isolation run also covers Draco/KTX2, texture validation, all sidecar
  containment cases, recovery, sandbox/job restrictions and hostile-worker
  shared-section/protocol attacks.

The real-app lifecycle smoke now includes `sparse-valid.gltf`, `meshopt.glb`
and `webp.gltf`. One Debug and one Release run opened/replaced all seven mesh,
point, sidecar and compressed fixtures, preserved cancel semantics, closed with
zero surviving workers, and had no failures. Texture, progressive and recovery
smokes pass in both configurations; Debug texture validation reports zero D3D12
errors. Frozen reports are under `tests/fixtures/baselines/tsk-209/`.

Commands used from the repository root:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' Preview3D.slnx /m /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
& .\x64\Debug\Tests.Unit.exe --reporter console
& .\x64\Debug\Tests.ImportIsolation.exe --reporter console
python tests/app-smoke/run.py --configuration Debug --output tests/fixtures/baselines/tsk-209/Debug-lifecycle.json --runs 1 --timeout 30 --require-points
python tests/app-smoke/textures.py --configuration Debug --output tests/fixtures/baselines/tsk-209/Debug-textures.json
python tests/app-smoke/recovery.py --configuration Debug --output tests/fixtures/baselines/tsk-209/Debug-recovery.json
python tests/app-smoke/progressive.py --configuration Debug --output tests/fixtures/baselines/tsk-209/Debug-progressive.json

& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' Preview3D.slnx /m /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
& .\x64\Release\Tests.Unit.exe --reporter console
& .\x64\Release\Tests.ImportIsolation.exe --reporter console
python tests/app-smoke/run.py --configuration Release --output tests/fixtures/baselines/tsk-209/Release-lifecycle.json --runs 1 --timeout 30 --require-points
python tests/app-smoke/textures.py --configuration Release --output tests/fixtures/baselines/tsk-209/Release-textures.json
python tests/app-smoke/recovery.py --configuration Release --output tests/fixtures/baselines/tsk-209/Release-recovery.json
python tests/app-smoke/progressive.py --configuration Release --output tests/fixtures/baselines/tsk-209/Release-progressive.json
```

Qualification boundary: these are local functional/boundedness smokes, not the
TSK-302 reference-system performance run, multi-hour soak, ETW input/present
evidence, ASan fuzz campaign, installer/SBOM/signature scan or thumbnail-host
compressed decode qualification. No persistent cache or new path authority was
added. Phase 2 is complete; **TSK-301 is next**.
