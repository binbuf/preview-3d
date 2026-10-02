#pragma once

// T16 provider diagnostics.
//
// design/05-thumbnail-provider.md ("Security and robustness") requires
// diagnostic events to be path-redacted and disabled unless troubleshooting is
// enabled; design/09 ("Privacy") forbids full paths, model-derived names, file
// content and rendered images in default release logs.
//
// This header therefore carries no string payload at all. A `DiagnosticEvent`
// is a fixed set of numeric/enum fields only -- there is no field a path, a
// material name or model-derived text could travel in, so a path cannot leak by
// construction. `FormatDiagnostic` renders the fixed `key=value` text used by a
// troubleshooting log and deliberately emits no `\`, `/` or `:` characters.
//
// Events are emitted only while `Diagnostics::Enabled()` is true. Production
// leaves it false; troubleshooting can enable it explicitly or through the
// `PREVIEW3D_THUMBNAIL_DIAGNOSTICS=1` environment toggle. Tests install a sink
// to observe events.
//
// This header/free source pair is PCH/COM-free so Tests.Unit.exe compiles the
// same code.

#include "DiagnosticStage.h"
#include "ProviderErrors.h"

#include <cstddef>
#include <cstdint>

namespace preview3d::provider {

// A path-redacted diagnostic event: numeric/enum fields only.
struct DiagnosticEvent {
    DiagnosticStage stage = DiagnosticStage::Stream;
    ProviderOutcome outcome = ProviderOutcome::Success;
    std::uint32_t elapsedMs = 0;     // wall time of the contained call
    bool overranStop = false;        // cooperative stop point passed
    bool cppException = false;       // a C++ exception was translated
    bool structuredException = false;// a structured exception was translated
    std::uint32_t structuredCode = 0;// SEH code when structuredException
};

namespace Diagnostics {

// True when events are currently emitted. False by default (and for a release
// build) unless troubleshooting enabled it.
bool Enabled() noexcept;

// Explicit troubleshooting/test toggle. Overrides the environment probe.
void SetEnabled(bool enabled) noexcept;

// Test/troubleshooting capture. A null sink detaches. The sink and its context
// must outlive any emission on the calling thread.
using Sink = void (*)(void* context, const DiagnosticEvent& event) noexcept;
void SetSink(Sink sink, void* context) noexcept;

// Emits `event` to the installed sink when enabled; a no-op otherwise. Never
// throws and never allocates.
void Emit(const DiagnosticEvent& event) noexcept;

// Renders the fixed `key=value` diagnostic text. Returns the number of
// characters that would be written (like snprintf) and always NUL-terminates
// when `size` is nonzero. The text contains no `\`, `/` or `:`.
std::size_t FormatDiagnostic(const DiagnosticEvent& event, char* buffer,
                             std::size_t size) noexcept;

} // namespace Diagnostics

// Stable short names for the fixed diagnostic text (also used by tests).
const char* DiagnosticStageName(DiagnosticStage stage) noexcept;
const char* DiagnosticOutcomeName(ProviderOutcome outcome) noexcept;

} // namespace preview3d::provider