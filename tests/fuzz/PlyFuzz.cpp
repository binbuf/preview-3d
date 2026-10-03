#ifndef NOMINMAX
#define NOMINMAX
#endif

// Standalone, no-GPU sanitizer target for the product-owned PLY fast path.
//
// `PlyFuzz` drives `import_worker::ImportPly` -- the same entry point the
// import worker uses -- for both the binary little/big-endian and the ASCII
// materializing paths: bounded header parsing, element/property walks, list
// skips, endian-aware scalar reads, vertex normalization, polygon fan
// triangulation and the chunk writer. A second envelope domain drives
// `parser_core::PlyParserCore` primitives directly (header lines, scalar
// reads, color normalization) so every engine cycle reaches the shared code
// even when the adapter rejects the source early. It creates no window, GPU
// device, file mapping, resolver, or child process: `mappedSource` is always
// null and the source is a bounded in-memory span. The 2 MiB input cap keeps
// one libFuzzer unit bounded without the real AppContainer/Job.

#include "PlyAdapter.h"
#include "parser_core/AsciiTokenizer.h"
#include "parser_core/PlyParserCore.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace {

constexpr uint32_t kEnvelopeMagic = 0x5a594c50; // "PLYZ"
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
    AdapterBinary = 0,
    AdapterAscii = 1,
    Header = 2,
    ScalarPrimitives = 3,
};

struct Input {
    Domain domain = Domain::AdapterBinary;
    uint8_t flags = 0;
    std::span<const std::byte> payload;
};

Input Decode(std::span<const std::byte> bytes)
{
    if (bytes.size() < sizeof(Envelope)) return {Domain::AdapterBinary, 0, bytes};
    Envelope envelope{};
    std::memcpy(&envelope, bytes.data(), sizeof(envelope));
    if (envelope.magic != kEnvelopeMagic) return {Domain::AdapterBinary, 0, bytes};
    return {static_cast<Domain>(envelope.domain % 4), envelope.flags,
            bytes.subspan(sizeof(envelope))};
}

void RunAdapter(const Input& input, bool allowAscii, std::span<std::byte> destination)
{
    const uint32_t maxChunkCount = 1u + (input.flags & 0x1f);
    (void)import_worker::ImportPly(input.payload, destination, 1, maxChunkCount, allowAscii,
                                   nullptr, nullptr);
}

void RunHeader(const Input& input)
{
    auto parsed = parser_core::ParseHeader(input.payload);
    (void)std::get_if<parser_core::PlyHeader>(&parsed);
    (void)parser_core::IsAsciiPly(input.payload);
    std::uint64_t cursor = input.payload.size() ? 1 : 0;
    (void)parser_core::ReadBytes(input.payload, cursor, input.payload.size() / 2);
}

void RunScalarPrimitives(const Input& input)
{
    const parser_core::PlyScalarType types[] = {
        parser_core::PlyScalarType::Int8,    parser_core::PlyScalarType::UInt8,
        parser_core::PlyScalarType::Int16,   parser_core::PlyScalarType::UInt16,
        parser_core::PlyScalarType::Int32,   parser_core::PlyScalarType::UInt32,
        parser_core::PlyScalarType::Float32, parser_core::PlyScalarType::Float64,
    };
    for (parser_core::PlyScalarType type : types) {
        const size_t size = parser_core::ScalarByteSize(type);
        if (input.payload.size() < size) continue;
        (void)parser_core::ReadScalarAsDouble(type, true,
                                              input.payload.first(size));
        (void)parser_core::ReadScalarAsDouble(type, false,
                                              input.payload.first(size));
        (void)parser_core::NormalizeColor(0.5, type);
    }
    // Token/name parsing over arbitrary bytes: invalid names must fail closed.
    const char* text = reinterpret_cast<const char*>(input.payload.data());
    const size_t textSize = input.payload.size();
    for (size_t start = 0; start < textSize && start < 256; ++start) {
        const size_t end = (std::min)(textSize, start + 16);
        (void)parser_core::ParseScalarTypeName(std::string_view(text + start, end - start));
    }
    parser_core::AsciiTokenizer tokenizer(input.payload);
    for (size_t step = 0; step < 4096 && !tokenizer.AtEnd(); ++step) {
        const auto token = tokenizer.NextToken();
        if (!token) break;
        (void)parser_core::ParseScalarTypeName(*token);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!data || size > kInputLimit + sizeof(Envelope)) return 0;
    const Input input = Decode({reinterpret_cast<const std::byte*>(data), size});
    if (input.payload.empty()) return 0;

    switch (input.domain) {
    case Domain::AdapterBinary:
    case Domain::AdapterAscii: {
        std::vector<std::byte> destination(kOutputBytes);
        RunAdapter(input, input.domain == Domain::AdapterAscii, destination);
        break;
    }
    case Domain::Header:
        RunHeader(input);
        break;
    case Domain::ScalarPrimitives:
        RunScalarPrimitives(input);
        break;
    }
    return 0;
}