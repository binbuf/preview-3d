---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T22 --configuration Debug --output TestResults/render-quality/T22
---
# T22 — Preserve authored material, UV, normal and AO semantics

## Goal

Stop losing or reinterpreting material inputs before they reach the shader, while preserving worker isolation and typed degradation.

## Context (read first)

- Depends on **T20, T21**; execute in roadmap order.
- `import-worker/src/GltfAdapter.cpp::ResolveMaterial`, texture resolution and tangent generation.
- `shared/model-core/include/model_core/MaterialPayload.h`, `WireFormat.h`, shared-section validator and hostile-worker tests.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp`, derived-cache identity; Khronos core glTF and transmission specifications.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [x] Carry and apply normalTexture.scale, per-texture sampler/UV-set/transform metadata, and independent occlusionTexture sampling/strength. MR red is not AO unless an authored AO binding actually points there. Support the corpus's UV sets correctly; bound the supported set count and warn specifically on unsupported sets rather than aliasing them to UV0.
- [x] Verify normal/tangent transforms for nonuniform and mirrored instances, transformed normal-map UVs, handedness, double-sided backfaces and missing tangents. Use a reference-compatible tangent basis or an evidence-backed equivalent; use front-face orientation rather than indiscriminately flipping a perturbed normal toward the eye.
- [x] Preserve authored alpha coverage, alphaMode/cutoff, transmission and unlit semantics independently in normalized data. Audit the IOR≈1 transmission suppression and degenerate-BLEND recovery; check Suit and controlled IOR=1/tinted cases against Khronos. Preserve intentional compatibility conversions only with explicit, tested diagnostics; no model/material-name hacks.
- [x] Version any wire/dependency/vertex-layout additions. Update every producer/consumer, validation, finite/range/index/size checks, hostile sections and cache/importer identities. Wire the actual parser/protocol identity into derived-cache keys; do not only bump an unused caller default.
- [x] Keep color/emissive textures sRGB, data maps linear, scalar factors in specified spaces and decode shared images correctly when used by different semantics. Validate glTF alpha handling for OPAQUE/MASK/BLEND and unlit.
- [x] Add generated cases with normal scale 0/0.45/1, independent AO versus non-AO MR red, UV1, different transforms/samplers per slot, mirrored tangents, alpha cutouts and IOR=1 transmission. Exercise both import isolation and actual GPU diagnostic images.

## Verification

The front-matter command is a planned interface introduced by T20. Extend its profile in this task; unimplemented profiles must fail.
The wrapper must run the task's meaningful unit/import checks and actual GPU
image lane, propagate failures and save exact commands/results. Build changed
binaries first. Keep import-worker suites serial. Image review and explicit
reference promotion are separate from ordinary verification; source-string
checks and self-created goldens do not prove rendering quality.

### Verification economy

The front-matter gate launches a real GPU viewer and takes minutes. Spend runs
deliberately:

- Run it in the foreground with an explicit long timeout (900000 ms or more).
  Do not background it and poll with short `Start-Sleep` loops; if the tool
  moves it to the background, wait once for completion instead of polling.
- Iterate with `--fast` (add `--no-build` once the binary is current). `--fast`
  keeps the build, generated fixtures and this task's focused lane, and skips
  the contract self-tests, external corpus, F3D references and review gate. It
  is iteration evidence, not acceptance evidence.
- Keep agent-run full gates to at most two: one early proof of the lane and
  one final after the last edit. The harness repeats the front-matter command
  once after you finish. Corpus or baseline re-promotion is one run, not a
  per-fix loop.
- Use focused unit/import filters between runs. Do not re-run a passing gate to
  "confirm" it, and do not repeat a failing configuration without a change.

## Done when

- [x] T22 verification runs the new material-semantic import/unit/GPU lanes and relevant wire/hostile-worker/cache checks serially.
- [x] Car/Camera/microscope/Suit before/after diagnostics explain each corrected input; Camera glass remains a reviewed nonregression case.
- [x] Update material/wire/cache design, format-support limitations and hand-off.

## Out of scope

New PBR lobe texture slots (T25), BRDF recalibration (T24), transparency ordering (T26) or refraction/transmitted-scene rendering (T27).

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

**Status: done.** T22 is complete. Authored material/UV/normal/AO semantics are
preserved on the wire, in the glTF importer, at the trust boundary, in the
derived-cache identity and in the actual GPU shader. The full verification
command passes, including the serial material-semantic lane, generated GPU
readback and the Car/Camera/Suit/microscope survey.

### Landed

- Wire protocol 13: `MaterialPayload` 244 bytes, `ChunkDescriptor` 176 bytes
  (eight dependency ids), five material image slots including occlusion.
- `normalScale`, per-slot UV set/sampler/transform metadata, independent
  occlusion, preserved authored alphaMode/cutoff, explicit IOR<=1 suppression
  and degenerate-BLEND recovery records (previous slice, unchanged).
- Renderer: the material record moved from 32 root-constant DWORDs to a
  per-material `b2` CBV; the root signature is now 35/64 DWORDs with six SRV
  tables (`t0`-`t3` maps, `t4` studio cubemap, `t5` occlusion). Each material
  owns a persistently mapped UPLOAD buffer with one 512-byte region per frame,
  keyed by material chunk plus mirrored sign (ADR-0080).
- Shader: every slot resolves its own UV set/transform/address mode/nearest
  sampler; slot 0 keeps the legacy transform fields and slots 1-4 use the v13
  arrays. `normalScale` multiplies tangent-space XY. AO is sampled only from
  `t5`; MR red is unused. Double-sided back faces reverse the geometric normal
  before TBN construction, mirrored instances flip tangent handedness by their
  determinant sign, and a zero tangent falls back to screen-space derivatives.
- Viewer plumbing: five texture slots, five neutral descriptors (occlusion
  neutral is white), slot 4 bound through root parameter 6, and material
  buffers included in upload estimation/allocation accounting.
- Diagnostics: debug material record fields 201-230 expose `normalScale`, all
  five slot flags, slot 1-4 transforms and slot 4 texture identity. The test
  locator uses distinct per-probe base colors and a mirrored/double-sided probe
  was added.
- Generated `material-semantics` fixture now has twelve records: the original
  ten probes, a mirrored-tangent sphere and a double-sided back-face quad.
- Docs: protocol v13 section in `03-file-formats-and-ingestion.md`, renderer
  AO/normal/TBN/CBV sections in `04-rendering-and-streaming.md`, ADR-0078
  promoted to accepted with the applied contract, and new ADR-0080.

### Deviations / blockers

- None blocking. The original hang was the root-signature DWORD budget: the
  previous attempt added a sixth table and a CBV without freeing the 32-DWORD
  inline material block. The final layout frees that block first.
- Image review used numeric GPU readback plus captured PNGs (local Symphony
  vision is disabled). An image-capable reviewer should still confirm the
  artistic before/after framing before any reference promotion; T22 promotes
  no references.
- Only UV set 0 exists on the wire. Supporting UV1 is a vertex-layout + wire
  change; the fixture's UV1 probe proves the slot is unbound, not aliased.

### Commands and results

- Build: `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
  Preview3D.slnx /p:Configuration=Debug /p:Platform=x64 /m:1 /v:minimal`
  — success.
- `.\x64\Debug\Tests.Unit.exe` — all pass (~156k assertions, 471 cases).
- `.\x64\Debug\Tests.ImportIsolation.exe` — 508 passed, 1 skipped (304043
  assertions).
- `python tests/render-quality/verify.py --task T22 --configuration Debug
  --output TestResults/render-quality/T22` — **pass**; 12 captures,
  `externalQualification: available`, no failures.
- Generated material-semantic lane: 112 checks pass across 12 records/probes. Numeric GPU evidence:
  shading-normal deviation 0.0 / 0.0174 / 0.0494 for normal scale 0 / 0.45 / 1;
  MR-red probe AO 1.0 versus independent AO 0.19995; UV1 base color equals the
  authored factor; MASK coverage 0.3499; IOR=1 transmission 0.0 and IOR=1.5
  0.8999; mirrored tangent deviation 0.058; double-sided back-face shading
  normal is exactly the reversed geometric normal.
- External survey: Car 139 materials (normal scales 0.3703/0.4495/1.0, 10
  occlusion-bound, 102 opaque / 0 mask / 37 blend / 23 transmissive); Camera 5
  materials (1 transmissive, lens nonregression); Suit 14 materials (all IOR=1
  suppression, no transmission bucket); microscope 9 materials (known T23
  ResourceLimit).

### Evidence

- `TestResults/render-quality/T22/report.json`
- `TestResults/render-quality/T22/diagnostics/material-semantics.json`
- `TestResults/render-quality/T22/probes/` and `external/` captures
- `tests/render-quality/review/T22-review.json`
- Reusable notes: `docs/0.7.0/progress/T22.md`.

### Remaining work

None for T22. Follow-ups: T25 texture-driven material lobes, T26/T27 Car
glass/Camera lens ordering, T23 microscope complete-load readiness.

