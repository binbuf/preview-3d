# TSK-203 verification (2026-09-15)

The existing worker-only WIC PNG/JPEG and KTX2/Basis paths now emit validated
mip chains, with semantic color spaces, bounded expansion and progressive
immutable replacements. BMP/TIFF remain explicit WIC adapter paths; the direct
glTF allowlist remains PNG/JPEG/KTX2. No dependency version or license changed.

Protocol v3 preserves the v2 section/descriptor layouts and the 32-byte
`ImagePayloadHeader`. Its formerly reserved `reserved0` identifies an earlier
logical image for refinement; zero means an initial image. Older/unknown versions
are rejected. A separate `TextureWarning` chunk carries only a validated u32
count (1–64), never a source path or arbitrary worker text. The broker retains
bounded header catalogs rather than previous image payloads, and requires
refinements to preserve format/color space, strictly increase resolution and
contain the prior image's dimensions/mip count as their mip tail. Missing roots,
forward roots, chained refinement IDs and repeated/stale refinements fail closed.

## Decode and publication limits

| Limit | Implementation |
| --- | --- |
| Aggregate encoded texture input | 256 MiB per glTF import, including semantic reuses and failed attempts; external sidecars checked before private copying |
| One decoded/transcoded chain | 32 MiB, further reduced by remaining generation budget |
| Aggregate decoded image payload | 128 MiB, including duplicated low-resolution tails; independently enforced across broker batches |
| Aggregate pixels | One billion, counting every mip and low-resolution duplicate; independently enforced by the broker |
| Source/protocol dimensions | 16,384 maximum per axis; checked before codec/KTX expansion |
| Published resolution | At most 2,048 per axis, reduced further to fit remaining bytes |
| KTX2 metadata | Each DFD/KVD/SGD range at most 1 MiB and inside the source; level ranges nonoverlapping and bounded before loading |
| Raster work | WIC source tiles at most 32 rows / 2 MiB; mip output tiles at most 64×64, with cancellation checkpoints |

WIC decoders are instantiated by the enabled inbox PNG/JPEG/BMP/TIFF CLSIDs,
after byte sniffing; unrestricted decoder discovery is removed. Declared glTF
MIME types must match detected bytes. JPEG uses `IWICBitmapSourceTransform` only
after verifying its supported output size and pixel format, with scaled ROI
coordinates as required by Microsoft's
[CopyPixels contract](https://learn.microsoft.com/en-us/windows/win32/api/wincodec/nf-wincodec-iwicbitmapsourcetransform-copypixels).
When native scaling is unavailable, possible whole-frame RGBA expansion must fit
the decode limit before converter initialization. Raster filtering includes odd
trailing rows/columns, averages sRGB in linear light and renormalizes normal mips.

Basis color/emissive targets BC7 sRGB, with a fresh-source RGBA fallback after
transcode failure. Data/normal targets linear RGBA: Basis BC5 green/alpha packing
does not promise the normal texture's RG channel semantics. Existing supported
KTX2 compressed/RGBA levels are size/offset checked and retained. Large single-level
Basis uses RGBA to generate a complete progressive chain. Direct BC5 cannot be
used as an sRGB color texture.

Real-file imports publish a chain with maximum extent 64 first, then a separate
full-resolution chain after an enforced batch boundary. Materials keep the initial
logical image ID. The upload coordinator validates exact source sizes and D3D12
subresource footprints, records smallest mips first, and publishes only after the
entire resource's copy fence completes. Frame-boundary replacement rebinds
immutable descriptors and retires displaced resources behind the direct fence.
Cancellation is checked before allocation and between copies; failed partial
uploads retain resources for fence-safe retirement.

Missing/corrupt/unsupported optional images preserve valid geometry and use a
deterministic color checker, white data, neutral normal or black emissive fallback.
When even fallback bytes are exhausted, material factors remain usable and the
warning remains bounded. The existing warnings menu shows fixed host-owned text
and clears it on a new generation. Full status/error UX remains TSK-204.

## Repeatable commands

Run on an interactive Windows desktop with the pinned vcpkg dependencies.
Solution builds deploy the sandbox worker and test dependencies.

```powershell
$taskMsbuild = 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe'
foreach ($taskConfig in @('Debug', 'Release')) {
    & $taskMsbuild Preview3D.slnx /t:Preview3D,Tests_Unit,Tests_ImportIsolation /p:Configuration=$taskConfig /p:Platform=x64 /m /v:minimal
    & "./x64/$taskConfig/Tests.Unit.exe"
    & "./x64/$taskConfig/Tests.ImportIsolation.exe"
    $taskReportConfig = $taskConfig.ToLowerInvariant()
    python tests/app-smoke/textures.py --configuration $taskConfig --output "TestResults/tsk-203-$taskReportConfig-textures.json"
    python tests/app-smoke/progressive.py --configuration $taskConfig --output "TestResults/tsk-203-$taskReportConfig-progressive.json"
    python tests/app-smoke/run.py --configuration $taskConfig --output "TestResults/tsk-203-$taskReportConfig-lifecycle.json" --runs 1 --require-points
}
```

Both full solution builds pass. Final Catch2 results:

| Suite | Debug | Release |
| --- | --- | --- |
| Unit | 79 cases / 7,210 assertions | 79 cases / 7,122 assertions |
| ImportIsolation | 170 cases / 48,222 assertions | 170 cases / 48,222 assertions |

Coverage includes explicit inbox codecs, hostile/truncated headers, MIME mismatch,
invalid optional image indices, source/pixel/byte caps, normal and sRGB filtering,
native JPEG scaling, KTX2/Basis levels, malformed row pitches and mip sizes,
seeded payload mutation, cumulative multi-batch expansion and hostile refinement
catalogs. Worker decode and accepted-batch cancellation are exercised. Full logs
remain in ignored `TestResults/tsk-203-*.log`.

Three unit cases read back real GPU subresources: padded odd-size RGBA and BC7
mips; the product uploader's complete-chain publication, color-space resource
formats and cancellation; and frozen PNG/JPEG/Basis decode-to-product-upload pixel
goldens at every mip. Worker adapters are linked only into the test executable
for this composition; the shipping viewer contains no decoder/transcoder.
Frozen PNG/JPEG recipes and SHA-256 are in
[textures.json](../tests/fixtures/manifests/textures.json). Their Pillow 10.2.0
generation settings are pinned; runtime tests need no Pillow. The existing pinned
Basis fixture is also hash checked. App fixtures use standard-library PNG
generation and matched SHA-256 across two independent regenerations.

| Real app check | Debug report | Release report |
| --- | --- | --- |
| Low/full mips, resolution cap, fallback, cancel/reopen | [textures](../tests/fixtures/baselines/tsk-203/debug-textures.json) | [textures](../tests/fixtures/baselines/tsk-203/release-textures.json) |
| Four progressive/backpressure/cancel/texture modes | [progressive](../tests/fixtures/baselines/tsk-203/debug-progressive.json) | [progressive](../tests/fixtures/baselines/tsk-203/release-progressive.json) |
| Lifecycle with point presentation required | [lifecycle](../tests/fixtures/baselines/tsk-203/debug-lifecycle.json) | [lifecycle](../tests/fixtures/baselines/tsk-203/release-lifecycle.json) |

All twelve viewer processes pass and close with no surviving child workers.
Texture scenes show 64-wide / 7-level initial chains while Loading, then 256-wide /
9-level or capped 2,048-wide / 12-level chains with one logical texture retained.
Corrupt texture fallback remains textured and reports a 63-character warning;
valid untextured reopen clears it. Input continues presenting during delayed
imports. Peak accepted texture queue bytes are 2,796,758 Debug / 2,796,702 Release;
loading UI ping maxima are 1.85 / 3.37 ms. Debug reports an available D3D12 debug
layer and zero errors; Release has no debug info queue. Existing count pressure
still reaches four batches and stays inside its 16 KiB smoke byte cap.
Reports pin executable/fixture hashes; lifecycle build IDs name the parent commit
and record the working diff at smoke time, before documentation and commit.

## Qualification boundaries

These checks establish local correctness and lifecycle behavior. Startup still
uses the existing synchronous `WM_CREATE` path: texture-smoke startup UI ping
maxima are 385.75 / 439.37 ms and lifecycle first-background timings are 546.71 /
482.21 ms, so they do not establish the startup responsiveness gate. That lifecycle
work remains TSK-301. Decoder checkpoints detect transport disconnect, while
cooperative control messages, grace periods and worker pooling also remain there.
Individual bounded KTX library operations are checked before/after, not interrupted
inside the library.

No ETW/input-to-present p95, large-corpus CPU/UMA or dynamic DXGI pressure
qualification is claimed. Mapped geometry scanning/splitting is TSK-205, complete
representative coarse catalogs TSK-206 and live GPU pressure TSK-207. Material
shader consumption is TSK-208; WebP is TSK-209; TGA/DDS/HDR remain deferred.
No worker path/network authority or model-derived persistent output is added.
**TSK-204 is next.**
