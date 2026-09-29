# USD-001 TinyUSDZ feasibility and policy spike

Date: 2026-09-17
Decision: **proceed with a revised fast-path boundary**
Product status: test-only spike; no USD extension or production opcode is enabled

## Result

TinyUSDZ 0.9.1 can read USDA, USDC, and USDZ from broker-supplied memory in the
existing zero-capability AppContainer worker on MSVC x64. The three encodings
produce stable render-data hashes in Debug and Release, malformed requests do
not poison the pool, and the existing 500 ms cancellation grace plus worker
replacement contains a non-interruptible library call.

Two findings require the design to use a narrower contract than the library's
surface suggests:

1. TinyUSDZ exposes no allocation, progress, or cancellation callback. Its
   `USDLoadOptions::max_memory_limit_in_mb` is advisory and did not reject a
   roughly 4 MiB USDA input with the option set to 1 MiB. Product-owned input,
   object, archive, and normalized-output accounting must be backed by the
   worker Job commit limit. The option may be set as defense in depth but is
   not an enforcement boundary.
2. The fast path will accept only a self-contained root layer. Any USD
   composition arc (sublayer, reference, payload, inherit, specialize,
   variant, clip, or layer relocation) returns `UnsupportedComposition`
   before candidate output is published. The OpenUSD host owns bounded local
   composition. Asset-valued texture reads are dependencies, not composition,
   and remain eligible only through the broker.

This revision keeps the small common case useful while making atomic fallback
and authority review tractable. It also avoids depending on incomplete
TinyUSDZ composition behavior for a generic USD claim.

## Immutable dependency and build

| Item | Value |
| --- | --- |
| Upstream | `lighttransport/tinyusdz` |
| Release | `v0.9.1` |
| Commit | `a04ee0bcbd1a930e30cc40938fcee3526a6fa8eb` |
| Source tarball SHA-512 | `350a6a3bce13348c42ef1ee475ffa01c54401ccbfd413eaeea8b79521dbe7ec84a24be42fadb4353638b5efffc53135593a1da5ca7901782658b2380c10b4233` |
| SBOM identity | `pkg:github/lighttransport/tinyusdz@a04ee0bcbd1a930e30cc40938fcee3526a6fa8eb` (`versionInfo` `0.9.1`) |
| Linkage | Static `tinyusdz_static.lib`; no new runtime DLL |
| License | Apache-2.0 for TinyUSDZ; upstream license installed by the overlay and copied beside redistributed fixtures |

The checked-in overlay adds the missing upstream install rules and builds only
the static target. Its configured feature surface is:

```text
TINYUSDZ_PRODUCTION_BUILD=ON
TINYUSDZ_BUILD_SHARED_LIBS=OFF
TINYUSDZ_BUILD_TESTS=OFF
TINYUSDZ_BUILD_BENCHMARKS=OFF
TINYUSDZ_BUILD_EXAMPLES=OFF
TINYUSDZ_WITH_C_API=OFF
TINYUSDZ_WITH_PYTHON=OFF
TINYUSDZ_WITH_PXR_COMPAT_API=OFF
TINYUSDZ_WITH_TYDRA=ON
TINYUSDZ_WITH_BUILTIN_IMAGE_LOADER=OFF
TINYUSDZ_WITH_USDMTLX=OFF
TINYUSDZ_WITH_JSON=OFF
TINYUSDZ_WITH_USD_TO_GLTF=OFF
TINYUSDZ_WITH_USDOBJ=OFF
TINYUSDZ_WITH_USDFBX=OFF
TINYUSDZ_WITH_USDVOX=OFF
TINYUSDZ_WITH_OPENSUBDIV=OFF
TINYUSDZ_WITH_AUDIO=OFF
TINYUSDZ_WITH_ALAC_AUDIO=OFF
TINYUSDZ_WITH_TIFF=OFF
TINYUSDZ_WITH_EXR=OFF
TINYUSDZ_WITH_COLORIO=OFF
TINYUSDZ_WITH_MODULE_USDA_READER=ON
TINYUSDZ_WITH_MODULE_USDA_WRITER=OFF
TINYUSDZ_WITH_MODULE_USDC_READER=ON
TINYUSDZ_WITH_MODULE_USDC_WRITER=OFF
TINYUSDZ_WITH_TOOL_USDA_PARSER=OFF
TINYUSDZ_WITH_TOOL_USDC_PARSER=OFF
TINYUSDZ_ENABLE_THREAD=OFF
TINYUSDZ_USE_CCACHE=OFF
TINYUSDZ_CXX_MP_FLAG=OFF
```

Upstream's static target is monolithic: disabled modules may still have an
object in the archive, guarded by compile definitions, and the MSVC archive is
not a useful shipped-size measure. Link-time dead-code removal reduced the
Release worker impact substantially:

| Artifact | Before spike | With spike | Delta |
| --- | ---: | ---: | ---: |
| Debug worker | 4,120,064 B | 25,664,512 B | 21,544,448 B |
| Release worker | 779,264 B | 5,304,320 B | 4,525,056 B |
| Debug static archive | — | 619,885,022 B | build-only |
| Release static archive | — | 559,786,208 B | build-only |

`dumpbin /dependents` confirms that TinyUSDZ adds no DLL. The current binary
increase is acceptable for continuing the spike, but USD-005 must measure the
final package and may split/prune the upstream target if the Release delta
grows materially.

### Transitive-code and notice notes

TinyUSDZ has no vcpkg runtime dependency, but “dependency-free” means that it
vendors code. The enabled reader/Tydra build compiles or includes, at minimum,
the upstream LZ4 and integer-coding sources plus header code for nonstd
expected/optional, fast_float, linalg, mapbox eternal/earcut, string_id,
half-edge, numeric formatting, and color/resize utilities. Disabled Wuffs,
stb image decode, TinyEXR/TinyDNG, pugixml/MaterialX, JSON, OpenFBX, OBJ, VOX,
OpenSubdiv, ALAC, Python, C API, tools, tests, and examples do not become
product runtime features.

The upstream source contains separate license files for several embedded
components. The release SBOM/third-party-notice generator must enumerate the
actually compiled archive members and retain their corresponding upstream
notices; installing only TinyUSDZ's top-level Apache-2.0 file is not the final
release-notice solution. This is an explicit USD-005 packaging gate, not an
unreviewed permission to ship all files in `src/external`.

## Installed API audit

The API decisions below were taken from the installed headers under
`vcpkg_installed/x64-windows/x64-windows/include`, not from online examples.

| Need | Installed API / result |
| --- | --- |
| Byte sniff | `IsUSDA`, `IsUSDC`, and `IsUSDZ` work for the three fixture encodings. |
| Memory load | `LoadUSDFromMemory` accepts all three encodings and fills a `Stage`. It requires a contiguous buffer and does not offer a zero-copy mapped-range contract. |
| Explicit loaders | USDA, USDC/crate, and USDZ memory loaders are present. The product still sniffs independently and rejects suffix/encoding mismatch. |
| Custom resolver | `AssetResolutionResolver` plus `AssetResolutionHandler` (`resolve_fun`, `size_fun`, `read_fun`) works for all three root layers through `LoadLayerFromAsset`. The read callback is whole-asset, not a ranged `ArAsset` equivalent. |
| Render data | `tydra::RenderSceneConverter` converts the parsed stage; the spike disables library texture loading and enables triangulation/index building/normal generation. |
| Allocation limit | No allocator callbacks. `max_memory_limit_in_mb` is advisory and demonstrably not a hard cap. |
| Cancellation/progress | No parser/converter progress or cancellation hook. Cancellation is checked between product-owned phases; an in-call cancel uses the 500 ms grace and replacement path. |
| Error boundary | Boolean result plus warning/error strings; the worker also catches `std::bad_alloc` and all other exceptions so pressure remains a controlled failed reply when possible. |

On Windows, `NOMINMAX` must be defined before including the installed headers;
otherwise Windows `min`/`max` macros break TinyUSDZ's standard-library calls.

## Sandbox and authority proof

The spike is reachable only through `--usd-spike-pool`. Tests launch it through
the real `WorkerPool`, which creates the existing zero-capability AppContainer,
assigns its kill-on-close Job before work, and duplicates only shared-section
and event handles. Model bytes are decoded/read by the trusted test process and
copied into the broker-created section. The worker is never given a source
path or directory handle.

The resolver proof serves the root bytes through callbacks and observes at
least the resolve/size/read sequence. Library texture loading is disabled, and
the fixtures contain no external assets, so the tested worker has neither a
filesystem-originated dependency read nor a network-originated read. Production
dependency requests still need the broker protocol work in USD-003/USD-005.

The spike deliberately piggybacks a reserved test scene variant on protocol
v10; it adds no production opcode or wire-format claim.

## Fixtures

| Fixture | Provenance | Decoded bytes SHA-256 | Coverage |
| --- | --- | --- | --- |
| `mesh.usda` | Original Preview3D fixture | `A56B200F47B0112078A3C01A37C66B94C24F7E5C82E853B873E41D40A646BDB3` | Text layer, hierarchy, transform, Z-up, centimeters, mesh, face-varying normals/UVs, display color |
| `cube.usdc.base64` | TinyUSDZ v0.9.1 `models/cube.usdc` | `D4C3C527DE10837036C602982E9D6733B14AD26AB17C2BCA6D43F2FCFE7C97C3` | Crate reader and render-data conversion |
| `cube.usdz.base64` | TinyUSDZ v0.9.1 `models/cube.usdz` | `DBBFCC999D2ABE55F0F1DD49E158D65739B87F002287C8B669A57DBA5F33E52D` | Stored USDZ, independent archive preflight, contained crate |

The base64 form preserves redistributable bytes without creating a file the
sandbox could open. `TinyUSDZ-LICENSE.txt` records the fixture license. The
same byte-sniff behavior applies when either USDA or USDC has a `.usd` name;
extension routing itself belongs to USD-003 and is intentionally absent here.

Policy fixtures for the rows below must be added beside adapter code in
USD-004/005 so they assert the actual typed result, rather than merely proving
that the third-party parser happens to accept a construct.

## Deterministic fast-subset policy

Classification happens before publication. A valid but compatibility-only
stage returns `UnsupportedComposition`; unsafe/malformed/resource-limit input
never receives OpenUSD retry. “Warn” below means retain coherent visible
geometry and attach a bounded diagnostic.

| Feature | TinyUSDZ fast-path decision |
| --- | --- |
| Encoding/name | Accept byte-sniffed USDA or USDC for `.usd`; accept `.usda`, `.usdc`, and `.usdz` only when suffix and bytes agree. Otherwise malformed/format mismatch, never fallback. |
| Layering/composition | Self-contained root layer only. Sublayers, references, payloads, inherits, specializes, variant sets/selections, value clips, relocates, and layer-offset composition are `UnsupportedComposition`. |
| Asset dependencies | Direct image assets may be contained in USDZ or supplied by the broker as approved local bytes. URI schemes, absolute paths, path escape, recursive packages, and unbrokered reads are terminal unsafe-input failures. |
| Evaluation time | Use `startTimeCode` when finite and authored; otherwise time 0. USD default values remain fallback values at that time. No animation is retained. Non-finite timing metadata is malformed. |
| Purpose | Include `default` and `render`; omit `proxy` and `guide` with a warning. If omission removes all required visible geometry, return `UnsupportedRequiredFeature`. |
| Visibility | Evaluate inherited visibility at the selected time. Omit invisible prims. Unsupported animated visibility beyond the selected static sample is diagnosed once. |
| Default prim | Preview the whole stage; `defaultPrim` is metadata/focus, not an authority boundary or a filter that hides other render-purpose roots. Invalid authored defaultPrim warns. |
| Mesh/topology | Accept finite polygon meshes with bounded fan/ear triangulation, validated indices/counts, holes, and degenerate removal. Invalid required topology is malformed; exceeding product counts is `ResourceLimit`. |
| Primvars | Accept constant, uniform, vertex, varying, and face-varying supported scalar/vector/color/UV primvars with optional validated indices. Unsupported optional primvars warn; a required geometry interpretation that cannot be represented is `UnsupportedRequiredFeature`. |
| Orientation | Accept `rightHanded` and `leftHanded`; normalize winding and tangent handedness after the complete world transform. Singular/non-finite transforms are malformed. |
| Subdivision | `none` is exact. Catmull-Clark, Loop, and bilinear render the bounded control cage with one warning only when it remains coherent; authored holes/creases/corners or a required smooth result return `UnsupportedRequiredFeature`. OpenSubdiv remains disabled. |
| Native prototypes/instances | Instanceable/reference-based composition is `UnsupportedComposition` in the fast path. OpenUSD may normalize it to shared geometry later. |
| Point instancers | Accept only self-contained prototypes and bounded static `protoIndices`, positions, orientations, scales, and invisible IDs. A prototype reached through composition is `UnsupportedComposition`; unsupported required per-instance data is `UnsupportedRequiredFeature`. |
| Material subsets | Accept non-overlapping `UsdGeomSubset` face sets with `materialBind` semantics and validated complete face indices. Overlap/invalid indices are malformed; unsupported family semantics warn if optional or return `UnsupportedRequiredFeature` if needed to present coherent required geometry. |
| Display material | Accept displayColor/displayOpacity and the USD Preview Surface fields that map to the normalized base-color, metallic, roughness, emissive, normal, opacity, and UV-transform contract. Unsupported optional inputs warn and use the neutral fallback. |
| Unsupported schemas | Omit cameras, lights, guides, and other optional non-geometry schemas with a bounded warning. Skeletal, procedural, MaterialX, renderer-plugin, volume, or other schemas that provide required visible content return `UnsupportedRequiredFeature`. Composition remains the only valid OpenUSD fallback trigger. |

The spike conversion uses `TimeCode::Default()` only to compare library
encoding behavior. USD-004 must set the policy time above explicitly and add
time-sampled fixtures before production normalization is accepted.

## USDZ preflight

`UsdZipPreflight` is product code and runs before TinyUSDZ sees a USDZ. It
parses EOCD, central-directory, and local headers with checked arithmetic and
rejects:

- multi-disk archives and ZIP64 sentinels;
- more than 4,096 entries, path depth over 32, empty/dot/dot-dot segments,
  backslashes, absolute/drive/colon paths, and ASCII case-folded duplicates;
- encryption, data descriptors, and every compression method except stored;
- stored entries whose compressed and expanded sizes differ;
- local/central name, flag, method, size, or range disagreement;
- local data not aligned to the USDZ-required 64-byte boundary;
- entries over 2 GiB, aggregate expansion over 4 GiB, or expansion over 200:1.

Tests mutate a valid upstream archive to cover traversal, encryption,
compression, misalignment, ZIP64, case collision, per-entry and aggregate
limits. The parser accepts the valid fixture independently of TinyUSDZ. Later
product work must use the same routine rather than reimplementing the checks in
the adapter.

## Measurements

Environment: Windows 11 Pro 10.0.26200, AMD Ryzen 9 7900X3D, 64.7 GiB visible
RAM, MSBuild 18.10.1, MSVC 14.51.36257, x64. Values are the first of three
same-process runs and are evidence for order of magnitude, not release gates.
`private-*` is current process private usage sampled at phase boundaries;
`peak-working-set` is the Windows process peak. TinyUSDZ has no allocator hook,
so a phase-exclusive allocation peak cannot be measured from inside the
library. The reported hash is a spike fingerprint over scene metadata,
node identity, mesh topology/points/normals, handedness, and display factors;
it is not the future protocol's complete canonical-scene checksum.

### Debug

| Input | Parse | Convert | Private before / after parse / after convert | Peak working set | Hash |
| --- | ---: | ---: | ---: | ---: | --- |
| USDA | 4,747 us | 1,096 us | 12,115,968 / 12,574,720 / 12,582,912 B | 16,257,024 B | `3fbe0a4e45dc13e3` |
| USDC | 8,880 us | 1,037 us | 12,726,272 / 13,746,176 / 13,754,368 B | 19,193,856 B | `7fe568bd71e18b59` |
| USDZ | 6,596 us | 685 us | 13,885,440 / 13,885,440 / 13,885,440 B | 19,591,168 B | `7660ed785f8f7c44` |

### Release

| Input | Parse | Convert | Private before / after parse / after convert | Peak working set | Hash |
| --- | ---: | ---: | ---: | ---: | --- |
| USDA | 808 us | 176 us | 1,798,144 / 2,007,040 / 2,007,040 B | 9,990,144 B | `3fbe0a4e45dc13e3` |
| USDC | 837 us | 108 us | 2,048,000 / 2,322,432 / 2,322,432 B | 10,977,280 B | `7fe568bd71e18b59` |
| USDZ | 520 us | 67 us | 2,347,008 / 2,490,368 / 2,490,368 B | 11,227,136 B | `7660ed785f8f7c44` |

Archive preflight is below the harness's 1 ms reporting resolution for the
2,931-byte USDZ. Root resolver load is measured as part of the dedicated
resolver case. TinyUSDZ does not expose a separate composition phase, and the
revised fast path does no layer composition; the root load timing is therefore
the parse measurement. Render-scene conversion and normalized hashing are
separate phases.

A roughly 4 MiB / 500,000-point USDA pressure fixture succeeds despite the
1 MiB library option. Under a 16 MiB Job process-memory cap it produces a
controlled allocation failure before the post-parse sample, remains in the
same process, and the next valid USDA succeeds. This proves the Job backstop
without claiming that current sampling exposes TinyUSDZ's internal peak.

The cancellation probe enters a real completed parse and then simulates an
uninterruptible library phase. Debug replacement took 515 ms and Release took
514 ms; the old process exited, a different PID was installed in the same pool
slot, and the next valid request succeeded.

## Verification

The focused suite passes in both configurations:

```text
Debug:   7 cases, 180 assertions
Release: 7 cases, 180 assertions
```

It covers deterministic three-run loads, the memory resolver, malformed
recovery, advisory memory behavior, low-Job pressure/recovery, cancellation
replacement/recovery, and independent USDZ policy. Release initially exposed
an MSVC 14.51 LTCG internal compiler/linker error in the already-large
ImportIsolation executable. Whole-program optimization is disabled only for
that test target; the product worker retains Release LTCG and links cleanly.

A full Debug ImportIsolation run reached 257 cases / 100,850 assertions. Four
unrelated existing FBX tests fail in `FbxImportTests.cpp:230` because their
fixture rewrite cannot find the expected substring; the standalone failures
reproduce without USD execution. They are not caused or changed by USD-001.

## Follow-on gates

- USD-002 may proceed with the OpenUSD payload/resolver spike using the exact
  policy above as its overlap boundary.
- USD-003 must add byte-derived source-format identity, X-up, typed
  `UnsupportedComposition`/required-feature/resource errors, and atomic
  discard-before-fallback. No USD extension becomes discoverable there.
- USD-004 must implement pre-publication feature classification and explicit
  policy time rather than treating successful TinyUSDZ parsing as acceptance.
- USD-005 must reuse the independent USDZ preflight, broker all image bytes,
  add the policy fixtures, audit compiled vendored notices, and remeasure final
  package size.
