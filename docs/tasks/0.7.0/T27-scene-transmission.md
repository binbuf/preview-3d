---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T27 --configuration Debug --output TestResults/render-quality/T27
---
# T27 — Scene-based transmission and tinted or rough glass

## Goal

Render glass as surface reflection plus transmitted scene color, with authored coverage and tint preserved independently.

## Context (read first)

- Depends on **T22, T24, T25, T26**; execute in roadmap order.
- Khronos KHR_materials_transmission, alpha coverage and optional volume semantics.
- `D3D12ViewerPath.cpp` transmission approximation and T26 pass/resource contract.
- Car black/orange/red BLEND glass; Camera's OPAQUE transmission lens; Suit IOR≈1 cases.
- Shared acceptance and safety contract: `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md`.

## Scope

- [ ] Introduce a bounded scene-color transmission path in linear HDR, with roughness-aware filtered transmitted radiance, interface Fresnel/IOR and base-color tint. Keep coverage alpha separate from optical transmission; do not replace authored coverage with Fresnel.
- [ ] Route OPAQUE transmissive materials through the appropriate render pass without rewriting their authored coverage meaning. MASK/cutout transmission, double-sided thin surfaces, environment fallback and emissive layers need explicit policies.
- [ ] Define ordering/sampling for at least two overlapping transmissive layers and their opaque background in coordination with T26. Never sample a render target simultaneously bound for writing or claim screen-space fallback sees offscreen/occluded content it cannot contain.
- [ ] Use zero-thickness/thin-surface behavior when no volume thickness is authored. Do not fabricate thickness from object bounds or material names. Volume absorption/refraction, if implemented, requires separately validated authored inputs; otherwise retain specific degradation warnings.
- [ ] Test neutral, tinted, dark, rough, IOR=1, OPAQUE and BLEND-coverage glass over a checker and actual interior geometry; include coverage×transmission sweeps, multiple layers and edge/offscreen cases. Compare controlled fixtures with Khronos, not only F3D's lens.
- [ ] Review Car windshield/side windows/tinted lights and Camera lens together on neutral, dark and light backgrounds. Keep the lens's reflection/depth advantage over the observed flat F3D disk; preserve Suit based on authored semantics and a recorded reference comparison.
- [ ] Budget the scene-color hierarchy/intermediates and exercise resize, residency changes, cancellation, device loss and fallback. Report approximation limits honestly.

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

- [ ] T27 verification checks independent coverage/tint/transmission numeric invariants and GPU close-ups/orbit clips, plus pass/resource correctness.
- [ ] Both Car and Camera pass the reviewed glass rubric; no asset-name exception or global opacity/exposure workaround is present.
- [ ] Update material/transparency support claims, renderer design and hand-off.

## Out of scope

Path tracing, caustics, full dispersion and unsupported volume features silently presented as correct.

## Design notes

Preserve worker-only parsing/decoding, brokered sidecars, validated bounded
wire data, generation/fence retirement, GPU budget and feature-level 11_0.
Do not add Release test-control authority. Update relevant design records
and cache identities when contracts change. No per-asset-name rendering hacks.

## Hand-off

Pending. Record implemented behavior, deviations, exact build/verification
commands and results, image/review manifests, performance evidence and what
the next task must know. Do not mark this task done with missing visual evidence.

