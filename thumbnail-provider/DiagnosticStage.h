#pragma once

// The bounded-stage tag carried by a diagnostic event (T16).
//
// Split out of Diagnostics.h so a translation unit that only needs the stage
// enum (an adapter routing a call through `RunContainedStage`) does not pull in
// ProviderErrors.h/windows.h and its macros. Values are stable for log parsing.

#include <cstdint>

namespace preview3d::provider {

enum class DiagnosticStage : std::uint32_t {
    Stream = 0,
    AdapterInitialize = 1,
    Parse = 2,
    Materials = 3,
    Geometry = 4,
    Render = 5,
    Bitmap = 6,
    Unload = 7,
};

} // namespace preview3d::provider