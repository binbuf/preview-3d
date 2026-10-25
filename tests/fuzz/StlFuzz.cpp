#ifndef NOMINMAX
#define NOMINMAX
#endif

// Standalone, no-GPU sanitizer target for the product-owned STL fast path.
//
// `StlFuzz` drives the same `import_worker::ImportStl` entry point the import
// worker uses: ASCII/binary detection, the binary facet scan, the ASCII
// tokenizer walk, per-facet supplied/flat-normal normalization, and the
// bounded chunk/checkpoint writer. It also exercises the shared
// `parser_core::StlParserCore` primitives directly so a single engine cycle
// reaches the header-shape and facet-math code even when the adapter rejects
// the source early. It creates no window, GPU device, file mapping, resolver,
// or child process: `mappedSource` is always null and the source is a bounded
// in-memory span. The 2 MiB input cap keeps a single libFuzzer unit bounded
// without the real AppContainer/Job; Tier A/B mapping and product size limits
// stay covered by the real import-isolation tests.

#include "StlAdapter.h"
#include "parser_core/AsciiTokenizer.h"
#include "parser_core/StlParserCore.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace {

constexpr uint32_t kEnvelopeMagic = 0x5a4c5453; // "STLZ"
constexpr size_t kInputLimit = 2u * 1024u * 1024u;
constexpr size_t kOutputBytes = 4u * 1024u * 1024u;

#pragma pack(push, 1)
struct Envelope {
    uint32_t magic;
    uint8_t domain;
    uint8_t flags;
    uint16_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(Envelope) == 8);

enum class Domain : uint8_t {
    Adapter = 0,
    AdapterBinaryOnly = 1,
    Detection = 2,
    FacetPrimitives = 3,
};

struct Input {
    Domain domain = Domain::Adapter;
    uint8_t flags = 0;
    std::span<const std::byte> payload;
};

Input Decode(std::span<const std::byte> bytes)
{
    if (bytes.size() < sizeof(Envelope)) return {Domain::Adapter, 0, bytes};
    Envelope envelope{};
    std::memcpy(&envelope, bytes.data(), sizeof(envelope));
    if (envelope.magic != kEnvelopeMagic) return {Domain::Adapter, 0, bytes};
    return {static_cast<Domain>(envelope.domain % 4), envelope.flags,
            bytes.subspan(sizeof(envelope))};
}

void RunAdapter(const Input& input, bool allowAscii, std::span<std::byte> destination)
{
    const uint32_t maxChunkCount = 1u + (input.flags & 0x1f);
    (void)import_worker::ImportStl(input.payload, destination, 1, maxChunkCount, allowAscii,
                                   nullptr, nullptr);
}

void RunDetection(const Input& input)
{
    const bool ascii = parser_core::IsAsciiStl(input.payload, input.payload.size());
    parser_core::StlBinaryHeader header;
    if (input.payload.size() >= parser_core::kStlPrefixBytes) {
        (void)parser_core::DecodeStlBinaryHeader(input.payload, header);
    }
    (void)ascii;
}

void RunFacetPrimitives(const Input& input)
{
    // Parse every full 50-byte binary facet record as raw LE floats and run the
    // production normalization. The last partial record is ignored.
    const size_t records = input.payload.size() / parser_core::kStlFacetBytes;
    parser_core::NormalizedStlFacet out{};
    for (size_t i = 0; i < records; ++i) {
        const std::byte* facet = input.payload.data() + i * parser_core::kStlFacetBytes;
        parser_core::StlFacet raw{
            {parser_core::ReadFloatLE(facet), parser_core::ReadFloatLE(facet + 4),
             parser_core::ReadFloatLE(facet + 8)},
            {parser_core::ReadFloatLE(facet + 12), parser_core::ReadFloatLE(facet + 16),
             parser_core::ReadFloatLE(facet + 20)},
            {parser_core::ReadFloatLE(facet + 24), parser_core::ReadFloatLE(facet + 28),
             parser_core::ReadFloatLE(facet + 32)},
            {parser_core::ReadFloatLE(facet + 36), parser_core::ReadFloatLE(facet + 40),
             parser_core::ReadFloatLE(facet + 44)}};
        (void)parser_core::NormalizeStlFacet(raw, out);
    }
    // Also drive the ASCII tokenizer over arbitrary bytes. Invalid number
    // tokens are expected and must not read out of bounds.
    parser_core::AsciiTokenizer tokenizer(input.payload);
    for (size_t step = 0; step < 4096 && !tokenizer.AtEnd(); ++step) {
        (void)tokenizer.NextNumber();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!data || size > kInputLimit + sizeof(Envelope)) return 0;
    const Input input = Decode({reinterpret_cast<const std::byte*>(data), size});
    if (input.payload.empty()) return 0;

    switch (input.domain) {
    case Domain::Adapter:
    case Domain::AdapterBinaryOnly: {
        std::vector<std::byte> destination(kOutputBytes);
        RunAdapter(input, input.domain == Domain::Adapter, destination);
        break;
    }
    case Domain::Detection:
        RunDetection(input);
        break;
    case Domain::FacetPrimitives:
        RunFacetPrimitives(input);
        break;
    }
    return 0;
}