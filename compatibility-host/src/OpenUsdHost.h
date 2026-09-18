#pragma once

#include "OpenUsdSpikeProtocol.h"

#include <cstddef>
#include <cstdint>

extern "C" __declspec(dllexport) int __cdecl Preview3DRunOpenUsdSpike(
    compatibility_host::OpenUsdSpikeSection* output, const std::byte* section,
    std::size_t sectionSize, const wchar_t* payloadDirectory);
