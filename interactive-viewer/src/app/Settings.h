#pragma once

#include "GroundAxis.h"

#include <string>

// Tiny persisted-preference store: %LOCALAPPDATA%\Binbuf\Preview 3D\settings.json,
// a sibling of (not inside) the future DerivedCache\v1 folder described in the
// design docs' rendering/streaming spec. Every field is optional and defaulted:
// a missing, corrupt, or unreadable file silently restores every default,
// matching that doc's "a missing/corrupt setting restores the default" rule,
// and this is never surfaced to the user as an error.
struct ViewerSettings
{
    static constexpr int kCurrentVersion = 7;
    int version = kCurrentVersion;

    // Display the model in the orientation its source file authored (true)
    // instead of this app's normalized Z-up correction (false, the default).
    bool showNativeOrientation = false;

    // Automatic preserves the source-declared behavior until the user picks
    // an explicit model axis to ground from the title-bar cycle button.
    GroundAxis groundAxis = GroundAxis::Automatic;

    // Treat the negative rather than positive side of groundAxis as up.
    bool groundAxisInverted = false;

    // Hide the pointer during viewport mouse drags so wrapped/infinite
    // camera gestures do not show the cursor jumping across the screen.
    bool hideCursorWhileDragging = true;

    // Keep a newly opened model out of the viewport until its import has
    // finished and the visible refinement pass has drained (true, the
    // default), instead of showing coarse chunks as they stream in. Turning
    // this off restores the progressive build-up.
    bool hideModelUntilComplete = true;

    // UI language pack code (e.g. "fr", "pt-BR", "zh_Hans"). Empty means
    // "follow the Windows display language".
    std::wstring language;

    // T15 view presentation. Stored as small integers/float so this header
    // stays independent of the renderer's enums; unknown values fall back to
    // the defaults on load.
    int toneMapMode = 0;   // viewer_presentation::ToneMapMode
    int backgroundMode = 0; // viewer_presentation::BackgroundMode
    float exposure = 1.0f;
    // T16 contact shadow; presets still choose the per-document default until
    // the user toggles it (H), which pins and persists this value.
    bool groundShadowEnabled = true;
    // True once the user toggled the shadow, so presets must not replace the
    // saved choice on the next launch/document.
    bool groundShadowOverridden = false;

    // Future preferences are added here as additional flat fields with their
    // own defaults — see Settings.cpp for why this needs no migration step.
};

// Tolerant load: any failure (missing file, unreadable, malformed JSON,
// unrecognized fields) returns default-constructed ViewerSettings for
// whichever fields it couldn't recover. Never throws, never reports an error.
ViewerSettings LoadSettings();

// Best-effort save: failures are silently swallowed. Writes atomically (temp
// file + rename) so a crash mid-write never corrupts the settings a future
// launch would read.
void SaveSettings(const ViewerSettings& settings);
