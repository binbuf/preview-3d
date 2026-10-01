# SPIKE-8a — mesh and point CPU rasterizer prototype (T02)

A product-owned CPU tile rasterizer prototype for the Explorer thumbnail provider. It is
dependency-free (no GPU, no D3D, no third-party renderer, no COM) and is the artifact T03 embeds in
its isolated Shell-surrogate probe.

## Layout

| Path | Purpose |
| --- | --- |
| `rasterizer/ThumbnailRasterizer.h` | Reusable entry point: `thumbnail_rasterizer::Render()`. T03 calls this inside its surrogate probe. |
| `rasterizer/ThumbnailRasterizer.cpp` | Framing, fixed isometric view, double-precision transforms, near-plane clip, depth test, ambient + two fixed lights, masked/weighted-opaque triangles, round depth-tested splats, linear-space downsample, premultiplied BGRA. |
| `rasterizer/SceneFixtures.{h,cpp}` | Deterministic golden mesh/point cloud and cap-sized generators with reservoir sampling. |
| `rasterizer/PamImage.h` | Lossless PAM (P7) golden read/write. |
| `driver/main.cpp` | Standalone driver (`RasterizerSpike.exe`): `goldens` and `perf` modes. |
| `goldens/*.pam` | Committed deterministic golden images (mesh and points at 32/64/256/512 px, 2x supersampled). |

## Build and run

Build through the solution so `$(SolutionDir)` is defined for the golden path macro:

```
msbuild Preview3D.slnx /p:Configuration=Release /p:Platform=x64
x64\Release\RasterizerSpike.exe goldens thumbnail-spike\goldens
x64\Release\RasterizerSpike.exe perf
x64\Release\Tests.Unit.exe "[thumbnail-rasterizer]"
```

Gotchas:
- MSBuild rejects `/t:Tests.Unit` ("invalid character ."). Build a single dotted-name project
  directly and pass the solution dir explicitly, or build the whole solution.
- Building a project directly leaves `$(SolutionDir)` unset; pass
  `/p:SolutionDir=<repo root>\` so `THUMBNAIL_SPIKE_GOLDEN_DIR` resolves. It is a `LR"(...)"` wide
  literal (Windows path separators); use `std::filesystem::path` on it, not `std::string`.

## Measured results (Release x64, 512 px, 2x supersampling)

Provider caps: 2M triangles inspected / 6M points inspected / 250k rasterized samples.

| Scene | Inspected | Kept | Render time | Private commit | Peak commit |
| --- | ---: | ---: | ---: | ---: | ---: |
| Mesh | 2,000,000 tri | 250,000 tri | ~59 ms | 59 MiB | 79 MiB |
| Points | 6,000,000 pts | 250,000 pts | ~186 ms | 60 MiB | 80 MiB |

Small goldens render in 0.2–32 ms. Both cap cases clear the 750 ms p95 target and the 384 MiB
measured-commit target with wide margin, so the 2x-supersampling fallback was not needed. Numbers
vary with host load; T03 repeats the measurement inside the surrogate.

## Compressed-glTF fixtures for T03

Prepared under `interactive-viewer/test-assets/corpus/`; the decoder closure is the ADR-0003 provider
set. This rasterizer-only spike (T02) exercises only the **Draco** decoder (see the `[draco]` Catch2
case, which encodes then decodes a bounded Draco bitstream before rasterizing it). The full closure
(fastgltf + draco + meshoptimizer + KTX2/Basis + libwebp) is linked and measured by the T03 surrogate
probe (`surrogate-probe/GltfSpikeDecode.{h,cpp}`); see `surrogate-probe/README.md`.

| Fixture | Exercises |
| --- | --- |
| `interactive-viewer/test-assets/corpus/draco_triangle.glb` | `KHR_draco_mesh_compression` geometry (draco) |
| `interactive-viewer/test-assets/corpus/draco_position_only.glb` | Draco geometry, position-only |
| `interactive-viewer/test-assets/corpus/basisu_textured_triangle.glb` | `KHR_texture_basisu` (ktx/basisu) |
| `interactive-viewer/test-assets/corpus/basisu_corrupt_ktx2.glb` | corrupt KTX2 rejection path |
| `interactive-viewer/test-assets/corpus/meshopt.glb` | `EXT_meshopt_compression` (meshoptimizer) |
| `interactive-viewer/test-assets/corpus/webp.gltf` + `sample.webp` | embedded/referenced WebP image (libwebp) |

Decoder closure to link in the provider: draco, ktx (basisu), meshoptimizer, libwebp (ADR-0003).

## Contract mapping

- Transparent canvas, soft neutral contact shadow; fixed isometric (orthographic) view, ~7% margin.
- Model/view transform and bounds math in `double`; near-plane clip in view space.
- `Triangle.color`/`Point.color` are **linear** RGB; masked triangles discard below cutoff; weighted-opaque
  transparency blends toward a neutral frost and writes opaque.
- Points are round, depth-tested splats with a bounds-derived default world radius (1–128 px clamp).
- Supersampled buffers are averaged in premultiplied linear space, then converted to premultiplied sRGB BGRA.