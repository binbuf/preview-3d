#pragma once

#include "OpenUsdSpikeProtocol.h"

#include <cstddef>
#include <cstdint>

extern "C" __declspec(dllexport) int __cdecl Preview3DRunOpenUsdSpike(
    compatibility_host::OpenUsdSpikeSection* output, const std::byte* section,
    std::size_t sectionSize, const wchar_t* payloadDirectory);

// Production bootstrap hook used by USD-006 before any stage is opened. The
// implementation hashes the closed resource inventory selected by USD-002.
extern "C" __declspec(dllexport) int __cdecl Preview3DAuditOpenUsdPayload(
    const wchar_t* payloadDirectory);
