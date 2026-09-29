#pragma once

// SPIKE-8b (T03) throwaway compressed-glTF decode for the surrogate probe.
//
// This is NOT product code and is deliberately narrower than the production
// importer (T25 GltfAdapter): it decodes one geometry path per file (Draco,
// meshopt or plain accessors), ignores node hierarchy transforms (the corpus
// fixtures are single identity nodes), and de-indexes into the T02 rasterizer's
// Triangle list. Its only purpose is to force the ADR-0003 decoder closure
// (fastgltf + draco + meshoptimizer + KTX2/Basis + libwebp) to load and run
// inside the real Shell surrogate so T03 can measure the dependency cost.

#include "ThumbnailRasterizer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gltf_spike
{

struct DecodeStats
{
    std::string geometryDecoder = "none"; // "plain" | "draco" | "meshopt"
    std::vector<std::string> decodedImages;
    std::uint64_t sourceBytes = 0;
    std::uint64_t vertices = 0;
    std::uint64_t triangles = 0;
    std::uint64_t imagePixels = 0;
};

// path: an absolute .gltf/.glb. Bounded: the file must be <= 128 MiB and the
// decoded geometry <= 250k triangles (the provider cap the rasterizer is
// qualified against). Returns false and sets `error` on any failure; on success
// `triangles` is a non-empty de-indexed triangle list and `stats` describes what
// the decoders actually produced.
bool Decode(const std::wstring& path, std::vector<thumbnail_rasterizer::Triangle>& triangles,
            DecodeStats& stats, std::string& error);

// path: an absolute .ktx2/.webp. Decodes it directly (no glTF container) so the
// KTX2/Basis and libwebp transcoders can be measured inside the surrogate even
// though the rasterizer itself consumes no textures. Returns false with a
// reason if the file is not a recognized image or the decode fails.
bool DecodeStandaloneImage(const std::wstring& path, DecodeStats& stats, std::string& error);

} // namespace gltf_spike