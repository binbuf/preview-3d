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

Pending. Record compositing/migration/precedence decisions, implementation,
exact build/check results, reviewed pixel/UI captures and downstream notes.
