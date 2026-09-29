---
verify: x64\Release\Tests.Unit.exe
---
# T15 — Implement the CPU tile rasterizer and bitmap output

## Goal
Render the sampled geometry to the final Explorer bitmap: a deterministic, framed, lit, depth-tested
tile raster at the correct resolution with a premultiplied BGRA result, no GPU device.

## Context (read first)
- `docs/design/05-thumbnail-provider.md` — "CPU renderer".
- `docs/tasks/02-spike-rasterizer-prototype.md` — the validated prototype and its measured budget.
- `docs/tasks/06-budgets-deadlines-hresults.md` — deadline and limit helpers.
- `docs/tasks/14-geometry-sampler.md` — the deterministic sample the rasterizer consumes.

## Scope
- [ ] Render at `min(cx, 512)` for nonzero cx, using 2× internal supersampling only when the deadline budget permits; the returned bitmap must fit within `cx` in each dimension.
- [ ] Frame verified bounds in a fixed isometric view with ~7% margin; apply model transforms in double precision and clip against the near plane.
- [ ] Depth-test tiles; shade with ambient plus two fixed lights using the same neutral palette as the viewer; approximate transparent materials as weighted opaque color.
- [ ] Render opaque/masked triangles and depth-tested round splats for point clouds with deterministic size and source/neutral color.
- [ ] Downsample in linear space and convert to premultiplied BGRA; produce the `HBITMAP`/DIB section and `WTS_ALPHATYPE`.
- [ ] Charge raster targets/scratch against the T06 process-wide product-owned allocation ledger; a charge crossing 384 MiB returns the null-bitmap HRESULT before allocation.
- [ ] Add golden-image tests at 32/64/256/512 px plus a transparent/alpha and a point-cloud case.

## Out of scope
- Family-specific material semantics (→ T21–T34 supply product-owned material values).
- GPU/D3D rendering — explicitly not used.

## Design notes
- No text, file path, watermark, network content or nondeterministic animation in the bitmap.
- A failed render returns null with a precise HRESULT; never a partial or fabricated image.
- Release every GDI object except the returned bitmap.

## Done when
- [ ] Golden images at all four sizes pass with tolerant perceptual comparison; alpha and point cases pass.
- [ ] Measured render time meets the prototype budget on the reference machine.
- [ ] Hand-off below filled in.

## Hand-off
_(filled in by the implementing session: what landed, what deviated and why, what the next task must know)_
