---
timeoutMin: 300
verify: python tests/render-quality/verify.py --task T21a --configuration Debug --output TestResults/render-quality/T21a
---
# T21a — Windows Photos-style background and ground-highlight settings

## Goal

Restore the original Windows Photos-style dark viewport by default. Add a
Background dropdown and an independent Ground highlight toggle in Settings,
so users can customize both without changing model lighting.

## Context (read first)

- Depends on **T20, T21**. Run immediately after the currently executing T21;
  this additional task does not replace or reset either task.
- `docs/0.7.0/RENDER-QUALITY-FOLLOWUP.md` — shared acceptance/safety contract.
- `interactive-viewer/src/render/Presentation.cpp::BackgroundLinear`,
  `BackgroundDisplay`, `SettingsForPreset`: Dark returns display-encoded
  #1C1C1E values as if linear; presets can replace the background.
- `interactive-viewer/src/app/D3D12ViewerPath.cpp`: HDR clear/resolve,
  exposure/tone mapping, background blending and ground shader.
- `interactive-viewer/src/render/GroundShadow.cpp::BuildConstants`: broad
  ground color currently uses linear (0.160, 0.170, 0.190), opacity 0.90.
- `interactive-viewer/src/app/Settings.{h,cpp}`, `Preview3D.cpp` Settings
  panel geometry/controls, preference persistence and preset reset behavior.
- `interactive-viewer/src/graphics/D3D11On12Overlay.cpp::DrawSettingsPanel`,
  `render/Renderer.h` overlay state and Settings accessibility.

## Required product behavior

- **Background dropdown:** "Windows Photos dark" is the default, restoring
  the original app's intended #1C1C1E display color. Compare the earlier app
  and Photos reference during visual review. Offer "Studio gray" and
  "Custom color…". Preserve existing saved enum meanings and checkerboard/
  transparent functionality, with accurate labels for supported uses.
- **Ground highlight toggle:** independently control the illuminated area
  around the model's base. Keep a subtle highlight enabled by default.
  Highlight off restores a flat selected backdrop while retaining contact
  shadows, grid state and model lighting.
- Apply immediately and persist across model opens, restarts and all three
  lighting modes, including automatic/manual presentation presets. These
  global user preferences take precedence over preset appearance defaults.

## Scope

- [ ] Correct the background display/color-space contract: #1C1C1E is
  currently treated as linear HDR, then tone-mapped and sRGB-encoded, making
  it gray. Simply selecting lower exposure or relabeling Dark is not a fix.
  Untouched background pixels must reproduce the selected display color
  with/without a model and remain stable across exposure/tone-map changes.
  Keep model blending/coverage/glass correct, document the HDR/display
  compositing choice and make the CPU reference agree with GPU output.
- [ ] Default every model/preset to the original dark backdrop; gray and
  checkerboard become explicit choices. Do not let model counts, source
  format or Scan/Transparent presets override the saved background.
- [ ] Add the Background dropdown/custom selector and Ground highlight
  switch using existing Settings/native popup conventions. Reflow the
  panel for supported window sizes and DPI. Preserve keyboard/focus,
  accessible names/values, high contrast and localized fallback labels.
- [ ] Add bounded versioned preferences for custom color and highlight.
  Preserve existing saved background values and unrelated settings; handle
  malformed/out-of-range values. Make the background shortcut and Settings
  update the same authoritative persisted state. Manual preset selection
  must no longer reset these appearance preferences.
- [ ] Decouple the illuminated ground treatment from contact-shadow and
  grid visibility. Highlight off must not disable shadows; shadows off
  must not unexpectedly disable the highlight. Define alpha behavior for
  checkerboard/transparent captures. Do not use the existing entire-ground-
  pass toggle as the new highlight control.
- [ ] Keep material shading, environment/ambient intensity, exposure and
  tone-map selection unchanged by these settings. Keep the default base
  highlight subtle and local rather than a bright whole-viewport sweep.
- [ ] Register a real T21a verification profile in the T20 runner. Check
  actual GPU background patches against the selected #1C1C1E, gray and
  custom color across exposure/tone-map changes, allowing only justified
  quantization tolerance. Check persistence across reopen/restart/modes and
  all independent highlight/shadow/grid combinations.
- [ ] Review Settings/viewport images at 100%, 150% and 200% DPI, a narrow
  window, keyboard use and high contrast. Review Car windows and Camera
  lens on dark/gray/custom backgrounds without halos or changed materials.
  Promote intentional backdrop references through T20's reviewed process;
  no automatic golden acceptance.
- [ ] Update settings/presentation design and shortcut documentation.
  T28/T31/T35 must preserve the user appearance contract. Do not change
  the running harness provider, notifications or in-progress task work.

## Verification

Extend the T20 runner with the front-matter profile; unknown/empty profiles
and missing images must fail. Build changed binaries first. Use focused
settings/presentation checks and actual GPU pixel/UI evidence; shader-string
checks and self-created goldens do not establish the requested appearance.

## Done when

- [ ] Initial and model-surrounding background pixels reproduce the
  restored dark display color instead of the current gray.
- [ ] The dropdown/custom selector and independent highlight switch work,
  are accessible, apply immediately and persist without preset overrides.
- [ ] T21a verification and reviewed image evidence pass; Camera/Car glass
  and contact-shadow behavior remain correct. Update the hand-off/design.

## Out of scope

Changing Windows/Photos settings, adding background-image parsers, changing
model lighting, implementing T31's new shadow algorithm or weakening worker
isolation. Do not edit/reset the active T21 to bypass its evidence work.

## Hand-off

### Changes

- **Display-space backdrop contract.** `Presentation.{h,cpp}` now returns
  display-encoded backdrop colors (`BackgroundDisplayColor`), including
  `BackgroundMode::Custom = 4` and packed `0xRRGGBB` helpers. The HDR scene no
  longer carries the backdrop. `D3D12ViewerPath` owns a separate
  `R8G8B8A8_UNORM` backdrop target, clears it with the selected display color,
  draws the ground treatment into it, and the resolve recovers straight-alpha
  scene color, tone-maps the model, then composites over the backdrop. The CPU
  reference is `CompositeOverBackground`.
- **Ground highlight.** `GroundShadow::BuildConstants` now takes an independent
  highlight flag plus a display-space tint; the ground shader emits
  premultiplied source-over, keeps the contact shadow when the highlight is off,
  and keeps the highlight when the shadow is off. The default tint is backdrop
  + 0.055 with a radial fade and peak alpha 0.55.
- **Settings.** `ViewerSettings` version 8 adds `backgroundColor` and
  `groundHighlightEnabled`; load masks out-of-range colors and defaults
  malformed values. The Settings panel adds a native Background popup
  (Windows Photos dark, Studio gray, Custom color…, Transparent, Checkerboard)
  and an independent Ground highlight switch, with accessible names/values and
  pointer/keyboard hit-testing. The panel reflows at 100/150/200% DPI, narrow
  windows and high contrast. The `\` shortcut and the popup write the same
  persisted state.
- **Precedence.** `ApplyPresentationSettings` no longer applies a preset
  background; `ApplyManualPresentationPreset` no longer clears the background
  override. Global background/custom color/highlight survive model opens,
  restarts, all lighting modes and manual presets.
- **Verification.** New `tests/render-quality/profiles/T21a.json`,
  `tests/render-quality/appearance.py`, T21a Debug fields 195-199, and
  `tests/render-quality/review/T21a-review.json`. `verify.py` registers the
  T21a appearance lane. T20's 15 generated-fixture accepted references were
  re-promoted after the intentional dark-backdrop change, with the reviewed
  delta recorded in `review/T20-review.json`.

### Checks

- `python tests/render-quality/verify.py --task T21a --configuration Debug --output TestResults/render-quality/T21a` — **pass**, 0 failures.
  - 24 GPU corner/combination checks; dark = 28/28/30, gray = 127/127/127,
    custom = 51/102/153 (max delta 1) before/after model load across exposure
    0.3/1.0/2.5 and Neutral/ACES/Filmic.
  - 8/8 highlight x shadow x grid combinations query the requested state;
    highlight on/off changes the ground but not a sampled model interior; the
    shadow toggle remains effective with the highlight on, and shadows remain
    visible with the highlight off (2.94).
  - Persistence across restart/model open, all three lighting modes and a
    manual Transparent preset; malformed settings fall back to dark/highlight
    on while preserving an unrelated field.
  - Settings images at 100/150/200% Debug DPI, narrow window and high contrast;
    Tab traversal exercised. Car window and Camera lens captured on
    dark/gray/custom.
- `Tests.Unit.exe "[tonemap],[ground-shadow],[appearance],[studio-presets],[texture-ao]"` — **pass**, 6473 assertions / 17 cases.
- `python tests/render-quality/verify.py --task T20 --configuration Debug --output TestResults/render-quality/T20 --no-build --capture-only` — 53 captures, no failures. 15 generated-fixture references re-promoted.
- `python tests/render-quality/verify.py --task T20 --configuration Debug --output TestResults/render-quality/T20-regression --no-build` — **pass**, 53 captures, 15/15 regression comparisons, 0 failures.
- `MSBuild Preview3D.slnx /t:Preview3D /p:Configuration=Release /p:Platform=x64` — **pass**.
- Full `Tests.Unit.exe` is not a clean signal in this tree: provider/golden tests fail on absent external fixture assets and an undeployed provider DLL, unrelated to this change. The focused appearance suite passes.

### Decisions / deviations

- The backdrop bypasses exposure/tone mapping by construction; model pixels are
  tone-mapped first and then composited, so glass/coverage remain correct.
- Transparent captures emit premultiplied display color with preserved alpha;
  checkerboard is display-space; neither draws the local ground treatment.
- Existing background enum values 0-3 are unchanged; `Custom = 4` is appended.
- Preset `SettingsForPreset` still reports historical background defaults for
  pure-function callers, but the viewer never applies them.
- The 150/200% UI images use the existing Debug DPI relayout control
  (`WM_APP+104` field 67) in one physical client. Physical per-monitor 150/200%
  display review remains a T34/T35 item.
- Two stale unit-test shader-string expectations from T21 (`EvaluateLight` vs
  the split `EvaluateLightSplit` helper) were corrected so the appearance
  contract suite is green; no rendering behavior changed.

### Remaining

- T28/T31/T35 must preserve the global appearance precedence and independent
  highlight/shadow behavior.
- Physical per-monitor 150/200% DPI display review (the current images use the
  Debug relayout control in one physical client) belongs to T34/T35.

