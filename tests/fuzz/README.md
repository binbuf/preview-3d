# No-GPU format fuzz targets

## STEP

`StepFuzz` is an explicitly built sanitizer target for the bounded static
STEP/STP admission boundary. It creates no window, GPU device, file mapping,
resolver, or child process, and it never links the pinned OCCT kernel. Its
bounded envelope selects one of four production surfaces:

- the product-owned `StepPart21Preflight` lexical admission scanner, including
  the `FILE_POPULATION`/`DOCUMENT_FILE` external-declaration discovery, in both
  its pure byte form and the handle-streaming form;
- declaration discovery with the external-document ceiling raised, so the
  scanner keeps lexing instead of failing closed at the first declaration;
- the production framed control decoder with mutated
  `StartStepImportFromFile` request-flag masks and `StepProgress` records; and
- the trusted normalized-output copy-and-validate decoder.

The seed preparer also materializes the STEP-008 frozen adversarial families
(truncated terminator, embedded NUL, UTF-16 BOM, ZIP signature, wrong physical
envelope, duplicate entity, unterminated string/comment, deep nesting, and an
oversized record) from `tests/fixtures/step/manifest.json`. The pinned OCCT
transfer/tessellation boundary is not sanitizer-instrumented by this target; it
stays covered by the real AppContainer/Job ImportIsolation cases, including the
derived malformed families in `StepQualificationTests.cpp`.

```powershell
python tests/fuzz/prepare_step_seeds.py TestResults/step-008/fuzz-seeds
msbuild tests/fuzz/StepFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1
tests/fuzz/x64/Release/StepFuzz.exe TestResults/step-008/fuzz-seeds -max_total_time=45 -timeout=5 -rss_limit_mb=1024 -max_len=1048576 -print_final_stats=1 -verbosity=0
```

The 1 MiB standalone input cap is deliberately smaller than the product's
Tier-B primary-source cap, and the scanner limits are reduced so an in-process
run is bounded. The pinned OCCT transfer/tessellation containment stays in the
real AppContainer/Job ImportIsolation tests. The seed preparer refuses a
non-empty output directory because libFuzzer evolves the corpus it receives.

## 3MF

`ThreeMfFuzz` instruments the product-owned OPC/ZIP preflight, bounded Deflate
and CRC extraction, and the worker's XML display/lattice scan. It takes no
model path, sidecar, network, window, or GPU. The standalone envelope can also
force cancellation. The 1 MiB input, 4 MiB expansion, 128-entry, and 32:1
limits keep an in-process fuzz run bounded; the production AppContainer and Job
limits are separately exercised by ImportIsolation.

```powershell
python tests/fixtures/3mf/verify.py
python tests/fuzz/prepare_3mf_seeds.py TestResults/3mf-007/fuzz-seeds-fresh
msbuild tests/fuzz/ThreeMfFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1
tests/fuzz/x64/Release/ThreeMfFuzz.exe TestResults/3mf-007/fuzz-seeds-fresh -max_total_time=60 -timeout=5 -rss_limit_mb=1024 -max_len=1048576 -print_final_stats=1 -verbosity=0
```

The seed preparer refuses a non-empty directory because libFuzzer evolves the
corpus it receives. `lib3mf` itself is a separately built app-local DLL and is
not sanitizer-instrumented by this target; model construction, full adapter
normalization, process containment, and recovery remain real-worker tests.

## USD

`UsdFuzz` is an explicitly built multi-domain sanitizer target for USD-009. It
creates no window, GPU device, filesystem-derived resolver, or child process.
Its bounded envelope selects one of five production surfaces:

- TinyUSDZ USDA object-graph parsing and a bounded normalization walk, plus
  USDC byte classification (mutated USDC parsing stays in the Job-contained
  real-worker regression);
- product-owned USDZ central/local-directory, path, expansion, CRC, and
  cancellation preflight;
- trusted normalized-output copy-and-validate;
- the compatibility host's exact pure identifier/anchoring policy for
  constrained virtual dependency maps; and
- the production framed control decoder with mutated OpenUSD start and
  resolver-sidecar records.

```powershell
python tests/fixtures/usd/verify.py
python tests/fuzz/prepare_usd_seeds.py TestResults/usd-009/fuzz-seeds
msbuild tests/fuzz/UsdFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1
tests/fuzz/x64/Release/UsdFuzz.exe TestResults/usd-009/fuzz-seeds -max_total_time=60 -timeout=5 -rss_limit_mb=1024 -max_len=2097160 -print_final_stats=1 -verbosity=0
```

The 2 MiB standalone input cap is deliberately smaller than the product's
Tier-B cap. OpenUSD itself is a separately built pinned DLL and is not
sanitizer-instrumented by this target; its resolver, composition, process, Job,
and recovery boundary remains covered by real-host ImportIsolation tests and
the hostile-process lane. TinyUSDZ has no enforceable allocator hook, so
arbitrary mutated USDC parsing likewise stays in the real worker behind its Job
limit. The seed preparer refuses non-empty output directories because
libFuzzer mutates its corpus in place. Any sanitizer finding must be minimized
into the immutable USD corpus before release signoff.

## FBX

`FbxFuzz` is an explicitly built sanitizer target; it is not part of the
shipping solution. It fuzzes pinned ufbx load options, progress cancellation,
a one-entry bounded virtual sidecar stream, deterministic start-pose
evaluation, a bounded normalized scene walk, triangulation, and the trusted
host's protocol/shared-section copy-and-validate decoder. It creates no window
or GPU device and never opens a model-derived path.

```powershell
python tests/fuzz/prepare_fbx_seeds.py TestResults/fbx-007/fuzz-seeds
msbuild tests/fuzz/FbxFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1
x64/Release/FbxFuzz.exe TestResults/fbx-007/fuzz-seeds -max_total_time=60 -timeout=5 -rss_limit_mb=512 -max_len=1048576
```

Run the same command after adding any minimized fault to the immutable corpus.
The target's 16 MiB primary / 1 MiB virtual-sidecar caps and 64 MiB ufbx
allocator cap are intentionally lower than product limits so a smoke run has a
tight, deterministic envelope. Release qualification still relies on the real
AppContainer/Job tests for process containment and product-size caps.

## STL

`StlFuzz` links the production STL fast path: ASCII/binary detection, the
binary facet scan, the ASCII `AsciiTokenizer` walk, per-facet supplied/flat
normal normalization, and the bounded chunk/checkpoint writer behind
`import_worker::ImportStl`. A second envelope domain drives the shared
`parser_core::StlParserCore` primitives directly. It creates no window, GPU
device, mapped file, resolver, or child process: the source is a bounded
in-memory span and `mappedSource` is always null. The seeds are generated, not
committed.

```powershell
python tests/fuzz/prepare_stl_seeds.py TestResults/security-t15/stl-seeds
msbuild tests/fuzz/StlFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=<root>\"
tests/fuzz/x64/Release/StlFuzz.exe TestResults/security-t15/stl-seeds -max_total_time=60 -timeout=5 -rss_limit_mb=1024 -max_len=2097160 -print_final_stats=1 -verbosity=0
```

The 2 MiB input cap and 4 MiB output window keep one libFuzzer unit bounded
without the AppContainer/Job. Real mapping, Tier A/B product-size limits,
random-detail/coarse-proxy streaming, and batch acknowledgement stay covered by
the import-isolation suite. The seed preparer refuses a non-empty directory.

## PLY

`PlyFuzz` links the production PLY fast path behind
`import_worker::ImportPly` for both the binary little/big-endian and the ASCII
materializing paths: bounded header parsing, element/property walks, list skips,
endian-aware scalar reads, vertex normalization, polygon fan triangulation, and
the chunk writer. A second envelope domain drives `parser_core::PlyParserCore`
primitives directly. It creates no window, GPU device, mapped file, resolver, or
child process; `mappedSource` is always null.

```powershell
python tests/fuzz/prepare_ply_seeds.py TestResults/security-t15/ply-seeds
msbuild tests/fuzz/PlyFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=<root>\"
tests/fuzz/x64/Release/PlyFuzz.exe TestResults/security-t15/ply-seeds -max_total_time=60 -timeout=5 -rss_limit_mb=1024 -max_len=2097160 -print_final_stats=1 -verbosity=0
```

The 2 MiB input cap and 4 MiB output window bound a unit. Mapped-window
streaming, checkpoint replay, cancellation, and the production Tier A/B caps
stay covered by the import-isolation suite. The seed preparer refuses a
non-empty directory.

## OBJ/MTL

`ObjFuzz` drives the pinned `ufbx` Wavefront parser the same way
`import_worker::ImportObj` does: an OBJ primary with a bounded, single-entry
in-memory MTL sidecar served through the open-file callback, and a second
envelope domain that parses the primary as MTL directly. It covers OBJ/MTL
tokenization, index/normal normalization, triangulation, missing sidecars, and
hostile external references; every external open that is not the one sidecar is
denied without touching the filesystem. It creates no window, GPU device,
mapped file, filesystem-derived resolver, network client, or child process.
Seeds are generated, not committed.

```powershell
python tests/fuzz/prepare_obj_seeds.py TestResults/security-t15/obj-seeds
msbuild tests/fuzz/ObjFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=<root>\"
tests/fuzz/x64/Release/ObjFuzz.exe TestResults/security-t15/obj-seeds -max_total_time=60 -timeout=5 -rss_limit_mb=1024 -max_len=2097160 -print_final_stats=1 -verbosity=0
```

`ObjFuzz` uses the root vcpkg manifest for the header-only `ufbx`; build it with
`VcpkgRoot` pointing at the restored vcpkg installation. The 2 MiB primary /
1 MiB sidecar caps and 64 MiB ufbx allocator cap are intentionally smaller than
product limits. The adapter's texture/image decode stages and the normalized
chunk writer are not sanitizer-instrumented here: compressed codecs are SEC-16,
and the provider/worker lanes cover adapter emission. The seed preparer refuses
a non-empty directory.

## glTF + compressed codecs

`GltfFuzz` links the real product code: `import_worker::ImportGltf` (GLB
container parser, the simdjson JSON preflight and its array/storage bounds,
accessor/bufferView range validation, sparse accessors, the bounded node graph,
and adapter normalization) plus the Draco, meshopt, KTX2/Basis, WebP, and WIC
adapter entry points. One 8-byte envelope (format selector + flags + bytes)
selects a domain:

- `Adapter` — `ImportGltf` with `sidecarClient`/`batchSink` null and a 4 MiB
  output window, so only the always-self-contained GLB/embedded-data-URI path
  runs (external `.bin`/image sidecars go through the pipe-based
  `SidecarFileClient` and are covered by SEC-04 and the real worker, not here);
- `Draco` — `DecodeDracoMesh` with expected vertex/index counts carried in an
  8-byte header, exercising the SEC-02 declared-vs-expected connectivity check
  and the decoder bounds;
- `Meshopt` — `DecodeMeshoptBuffer` with count/stride/decoded-length in a
  16-byte header and every mode/filter reachable;
- `Ktx2` — `model_core::PreflightKtx2` header/level validation for BasisLZ
  (ETC1S) containers and the real `TranscodeKtx2BasisImage` for
  UASTC/uncompressed containers;
- `WebP`, `WicRaster` — `DecodeWebpImage` and `DecodeRasterImageWic` with
  mutated dimension/encoded/decoded caps and semantics;
- `Sniff` — `SniffImageFormat` plus the reference-extension policy.

```powershell
python tests/fuzz/prepare_gltf_seeds.py TestResults/security-t16/gltf-seeds
msbuild tests/fuzz/GltfFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=<root>\" /p:VcpkgRoot=<vcpkg> /p:VcpkgManifestInstall=false
tests/fuzz/x64/Release/GltfFuzz.exe TestResults/security-t16/gltf-seeds -max_total_time=60 -timeout=5 -rss_limit_mb=1024 -max_len=2097160 -print_final_stats=1 -verbosity=0
```

The target uses the root vcpkg manifest (fastgltf/simdjson/draco/meshoptimizer/
ktx/basisu/libwebp). Those pinned static libraries are **not**
sanitizer-instrumented; ASan still instruments the product-owned preflights and
adapter code and its interceptor catches some third-party over-writes.
`_DISABLE_STL_ANNOTATION` is set for this target only because an
ASan-instrumented TU cannot link `ktx.lib`/`simdjson.lib` (which emit
`detect_mismatch(annotate_*=0)`) with the default asan container annotations.
The 2 MiB input and 4 MiB output caps keep one unit bounded; the real
AppContainer/Job worker stays the process-containment evidence.

**Known findings — both fixed.** Two deterministic crashes were found in the
pinned third-party decoders and are minimized under
`tests/fuzz/corpus/gltf/` (see its README):

- fastgltf 0.9.0 `base64::fallback_decode_inplace` heap overflow on a `.gltf`
  data URI whose base64 length is not a multiple of four — **fixed in SEC-16**
  by `model_core::ValidateGltfDataUri`, called from the `GltfAdapter.cpp`
  simdjson preflight before `loadGltf`; the seed is a safe regression;
- KTX-Software 4.4.2 ETC1S/BasisLZ `transcode_slice` null-deref on a two-byte
  mutation of the frozen Basis sample (via an embedded `KHR_texture_basisu`
  image) — **fixed in SEC-16b** by `model_core::ValidateEtc1sGlobalData`
  (`Etc1sTablePreflight.h`), called from `PreflightKtx2` before
  `ktxTexture2_TranscodeBasis`; see
  [ADR-0046](../../docs/design/adr/0046-etc1s-global-data-preflight.md).

Both minimized `.env` seeds, the extracted `basislz-etc1s-crash.ktx2`, and the
valid `basisu_textured_triangle.glb` are promoted into the generated smoke
seeds, and the `Ktx2` domain drives the real ETC1S transcode for every accepted
container. `GltfFuzz` is in the `fuzz-smoke` matrix in
`.github/workflows/ci.yml` (one target per runner); SEC-17/T25 promoted that
lane into the required pull-request/push and release gate. The seed preparer
refuses a non-empty directory.

## Provider pipeline (SEC-17)

`ProviderFuzz` instruments the provider-owned boundary the worker-format
targets do not reach: `BoundedStreamSource` (SEC-07 stream hardening), every
family adapter's normalization through the frozen `IFamilyAdapter` contract,
`DeterministicGeometrySampler`, and the real `RenderCpuTileRaster` tile
rasterizer. It creates no COM object, window, GPU device, file mapping or child
process: the input is an in-process `IStream` double and the output image is
discarded. The eight family adapters and the pinned third-party parsers are
linked (fastgltf/ufbx/lib3mf/TinyUSDZ/OCCT/draco/ktx) but are **not**
sanitizer-instrumented; ASan instruments the product-owned preflights, adapters,
sampler and rasterizer and its allocator interceptor catches some third-party
overwrites, exactly as `GltfFuzz` does. The real AppContainer/Job worker and the
OCCT transfer boundary remain the process-containment evidence (SEC-08/09).

A 24-byte envelope (magic, domain, family, flags, reserved, cx, reported size,
payload length) selects one of four domains:

- `Pipeline` — `RunThumbnailPipeline` over a memory-backed
  `BoundedStreamSource` for the routed family, so registry → adapter → sampler →
  rasterizer runs exactly as in `GetThumbnail`; the stream flags select hostile
  `STATSTG`/seek/short-read behaviour;
- `Stream` — `BoundedStreamSource` driven directly with fuzzed read ranges;
- `Sampler` — fuzzed `TriangleSample`/`PointSample` records (NaN/Inf from raw bit
  patterns) into the deterministic sampler;
- `Raster` — fuzzed `SampledGeometry` (clipping math, degenerate transforms,
  NaN/Inf coordinates, extreme aspect ratios, `cx == 0`/`0xFFFFFFFF`) into the
  real tile rasterizer.

```powershell
python tests/fuzz/prepare_provider_seeds.py TestResults/security-t17/provider-seeds
msbuild tests\fuzz\ProviderFuzz.vcxproj /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=<root>\" /m:1
tests/fuzz/x64/Release/ProviderFuzz.exe TestResults/security-t17/provider-seeds -max_total_time=60 -timeout=10 -rss_limit_mb=2048 -max_len=2100000 -print_final_stats=1 -verbosity=0
```

The provider STEP manifest is isolated from the root manifest
(`thumbnail-provider\step-occt\vcpkg.json`, ADR-0002), so a cold build needs
both `vcpkg_installed` trees restored. The `fuzz-smoke` job in
`.github/workflows/ci.yml` restores the isolated `step-occt` manifest on the
`Provider` runner (the root manifest is restored for every target), so
`ProviderFuzz` is now one of the seven `fuzz-smoke` matrix entries; SEC-17/T25
promoted that lane from the nightly schedule into the required PR/push and
release gate (SEC-13/T13). The seed preparer refuses a non-empty directory
because libFuzzer mutates its corpus in place. Measured Release smoke on the
reference machine: 30 s / 590 executions / 330 MiB peak RSS, and 60 s / 788
executions / 447 MiB peak RSS on 36 seeds (SEC-17); re-run by T25, 30 s / 580
executions / 338 MiB peak RSS on 37 seeds, exit 0, no crash/ASan report.

The surrogate soak that proves Explorer stability across concurrent DllHost
apartments, repeated load/unload and thumbnail-cache churn is documented in
`packaging/smoke/README.md` (SEC-17 soak).
