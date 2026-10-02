// T16 provider diagnostics implementation (see Diagnostics.h).

#include "Diagnostics.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

namespace preview3d::provider {
namespace {

// -1 = defer to the troubleshooting environment probe, 0 = forced off,
// 1 = forced on. SetEnabled() pins it; a fresh process starts at -1.
std::atomic<int> g_override{-1};
std::atomic<int> g_envProbe{-1}; // -1 = not probed yet
std::atomic<Diagnostics::Sink> g_sink{nullptr};
std::atomic<void*> g_sinkContext{nullptr};

bool EnvironmentRequestsDiagnostics() noexcept
{
    const wchar_t* name = L"PREVIEW3D_THUMBNAIL_DIAGNOSTICS";
    wchar_t value[8] = {};
    const DWORD length = ::GetEnvironmentVariableW(name, value, 8);
    if (length == 0 || length >= 8) {
        return false;
    }
    return value[0] == L'1' && value[1] == L'\0';
}

bool EnabledImpl() noexcept
{
    const int forced = g_override.load(std::memory_order_acquire);
    if (forced >= 0) {
        return forced != 0;
    }
    int probed = g_envProbe.load(std::memory_order_acquire);
    if (probed < 0) {
        probed = EnvironmentRequestsDiagnostics() ? 1 : 0;
        g_envProbe.store(probed, std::memory_order_release);
    }
    return probed != 0;
}

} // namespace

const char* DiagnosticStageName(DiagnosticStage stage) noexcept
{
    switch (stage) {
        case DiagnosticStage::Stream: return "stream";
        case DiagnosticStage::AdapterInitialize: return "adapter_initialize";
        case DiagnosticStage::Parse: return "parse";
        case DiagnosticStage::Materials: return "materials";
        case DiagnosticStage::Geometry: return "geometry";
        case DiagnosticStage::Render: return "render";
        case DiagnosticStage::Bitmap: return "bitmap";
        case DiagnosticStage::Unload: return "unload";
        case DiagnosticStage::Containment: return "containment";
    }
    return "unknown";
}

const char* DiagnosticOutcomeName(ProviderOutcome outcome) noexcept
{
    switch (outcome) {
        case ProviderOutcome::Success: return "success";
        case ProviderOutcome::BadPointer: return "bad_pointer";
        case ProviderOutcome::InvalidCallSequence: return "invalid_call_sequence";
        case ProviderOutcome::BadArgument: return "bad_argument";
        case ProviderOutcome::Unsupported: return "unsupported";
        case ProviderOutcome::BadFormat: return "bad_format";
        case ProviderOutcome::LimitExceeded: return "limit_exceeded";
        case ProviderOutcome::Deadline: return "deadline";
        case ProviderOutcome::OutOfMemory: return "out_of_memory";
        case ProviderOutcome::DecoderFailure: return "decoder_failure";
    }
    return "unknown";
}

namespace Diagnostics {

bool Enabled() noexcept { return EnabledImpl(); }

void SetEnabled(bool enabled) noexcept
{
    g_override.store(enabled ? 1 : 0, std::memory_order_release);
}

void SetSink(Sink sink, void* context) noexcept
{
    g_sinkContext.store(context, std::memory_order_release);
    g_sink.store(sink, std::memory_order_release);
}

void Emit(const DiagnosticEvent& event) noexcept
{
    if (!EnabledImpl()) {
        return;
    }
    const Sink sink = g_sink.load(std::memory_order_acquire);
    if (sink == nullptr) {
        return;
    }
    sink(g_sinkContext.load(std::memory_order_acquire), event);
}

std::size_t FormatDiagnostic(const DiagnosticEvent& event, char* buffer,
                             std::size_t size) noexcept
{
    if (buffer == nullptr || size == 0) {
        return 0;
    }
    const int written = std::snprintf(
        buffer, size,
        "stage=%s outcome=%s elapsed_ms=%u seh=%u cpp=%u overrun=%u quarantined=%u",
        DiagnosticStageName(event.stage), DiagnosticOutcomeName(event.outcome),
        static_cast<unsigned>(event.elapsedMs),
        static_cast<unsigned>(event.structuredException),
        static_cast<unsigned>(event.cppException),
        static_cast<unsigned>(event.overranStop),
        static_cast<unsigned>(event.quarantined));
    if (written < 0) {
        buffer[0] = '\0';
        return 0;
    }
    return static_cast<std::size_t>(written);
}

} // namespace Diagnostics

} // namespace preview3d::provider