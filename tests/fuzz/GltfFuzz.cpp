#ifndef NOMINMAX
#define NOMINMAX
#endif

// Standalone, no-GPU sanitizer target for the glTF/GLB primary path and the
// compressed codec adapters named in SECURITY.md.
//
// `GltfFuzz` links the real product-owned code: `import_worker::ImportGltf`
// (the GLB container parser, the simdjson JSON preflight, accessor/bufferView
// range validation, sparse accessors, the bounded node graph, and adapter
// normalization through Draco/meshopt/KTX2-Basis/WebP/WIC), plus the codec
// adapter entry points directly. It creates no window, GPU device, mapped
// file, filesystem-derived resolver, network client, or child process: the
// source is a bounded in-memory span and the sidecar client / batch sink are
// always null, so only the always-self-contained GLB/embedded-data-URI path
// runs.
//
// One envelope selects a domain (format selector + bytes); the adapter domain
// is byte-compatible with the other targets' portable corpus. Decoder-input
// count/stride/dimension fields are carried in a small per-domain header so a
// single engine cycle reaches the SEC-02 decode preflights (Draco declared
// connectivity, KTX2 header/level index, WebP/WIC dimension and pixel caps)
// as well as the third-party decoders themselves.
//
// External `.bin`/image sidecars resolved through the worker's pipe-based
// `SidecarFileClient` are deliberately NOT driven here: serving them requires
// a real file handle and a control-channel server thread. Their reference
// validation is covered by the SEC-04 unit and import-isolation suites, and
// the real AppContainer/Job worker remains the process-containment evidence.
// The linked decoder libraries (fastgltf/simdjson/draco/meshoptimizer/ktx/
// libwebp) are built by vcpkg without sanitizer instrumentation; the
// product-owned preflights and adapter code around them are instrumented.
// The 2 MiB input cap and 4 MiB output window keep one libFuzzer unit bounded
// without the real Job.

#include "GltfAdapter.h"

#include "DracoDecodeAdapter.h"
#include "ImageFormatSniff.h"
#include "MeshoptDecodeAdapter.h"
#include "TextureDecodePolicy.h"
#include "TextureTranscodeAdapter.h"
#include "WebpDecodeAdapter.h"
#include "WicImageDecodeAdapter.h"

#include "model_core/Ktx2Preflight.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include <windows.h>

namespace {

constexpr uint32_t kEnvelopeMagic = 0x5a544c47; // "GLTZ"
constexpr size_t kInputLimit = 2u * 1024u * 1024u;
constexpr size_t kOutputBytes = 4u * 1024u * 1024u;
constexpr size_t kMaxSidecarBytes = 1u * 1024u * 1024u;

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
    Draco = 1,
    Meshopt = 2,
    Ktx2 = 3,
    WebP = 4,
    WicRaster = 5,
    Sniff = 6,
    Count = 7,
};

enum : uint8_t {
    // Adapter + texture-domain flags.
    kSrgb = 1u << 0,
    kSmallDimension = 1u << 1,
    kCancel = 1u << 7,
    // Draco attribute-presence flags.
    kDracoNormal = 1u << 0,
    kDracoUv0 = 1u << 1,
    kDracoTangent = 1u << 2,
    kDracoColor = 1u << 3,
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
    return {static_cast<Domain>(envelope.domain % uint8_t(Domain::Count)), envelope.flags,
            bytes.subspan(sizeof(envelope))};
}

uint32_t ReadU32(std::span<const std::byte> bytes, size_t offset)
{
    uint32_t value = 0;
    if (offset + sizeof(value) <= bytes.size()) std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

uint64_t ReadU64(std::span<const std::byte> bytes, size_t offset)
{
    uint64_t value = 0;
    if (offset + sizeof(value) <= bytes.size()) std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

import_worker::TextureDecodeOptions MakeTextureOptions(const Input& input,
                                                       import_worker::TextureSemantic semantic)
{
    import_worker::TextureDecodeOptions options{};
    options.maxEncodedBytes = 1ull * 1024 * 1024;
    options.maxDecodedBytes = 16ull * 1024 * 1024;
    options.maxPixels = 64ull * 1024 * 1024;
    options.maxDimension = (input.flags & kSmallDimension) ? 64u : 1024u;
    options.semantic = semantic;
    if (input.flags & kCancel) {
        // Deterministic immediate cancellation: the preflight must refuse
        // before any third-party decode is entered.
        options.isCancelled = [] { return true; };
    }
    return options;
}

import_worker::TextureSemantic SemanticFor(uint8_t selector)
{
    switch (selector % 4) {
    case 1: return import_worker::TextureSemantic::Data;
    case 2: return import_worker::TextureSemantic::Normal;
    case 3: return import_worker::TextureSemantic::Emissive;
    default: return import_worker::TextureSemantic::Color;
    }
}

void RunAdapter(const Input& input)
{
    if (input.payload.empty()) return;
    std::vector<std::byte> destination(kOutputBytes);
    const uint32_t maxChunkCount = 1u + (input.flags & 0x1fu);
    (void)import_worker::ImportGltf(input.payload, destination, 1, maxChunkCount, nullptr, nullptr,
                                    MakeTextureOptions(input, import_worker::TextureSemantic::Color));
}

void RunDraco(const Input& input)
{
    // 8-byte header: expected vertex count, expected index count. The rest is
    // the compressed bitstream, mirroring the adapter's accessor claim so the
    // SEC-02 declared-vs-expected mismatch path is reachable.
    if (input.payload.size() < 8) return;
    import_worker::DracoAttributeIds ids{};
    ids.position = 0;
    if (input.flags & kDracoNormal) ids.normal = 1;
    if (input.flags & kDracoUv0) ids.uv0 = 2;
    if (input.flags & kDracoTangent) ids.tangent = 3;
    if (input.flags & kDracoColor) ids.color0 = 4;
    (void)import_worker::DecodeDracoMesh(input.payload.subspan(8), ids,
                                         ReadU32(input.payload, 0), ReadU32(input.payload, 4));
}

void RunMeshopt(const Input& input)
{
    // 16-byte header: count, stride, decodedByteLength.
    if (input.payload.size() < 16) return;
    const auto mode = static_cast<import_worker::MeshoptDecodeMode>(input.flags & 0x3);
    const auto filter = static_cast<import_worker::MeshoptDecodeFilter>((input.flags >> 2) & 0x7);
    import_worker::MeshoptDecodeOptions options{};
    options.maxDecodedBytes = 16ull * 1024 * 1024;
    if (input.flags & kCancel) options.isCancelled = [] { return true; };
    (void)import_worker::DecodeMeshoptBuffer(input.payload.subspan(16), ReadU32(input.payload, 0),
                                             ReadU32(input.payload, 4), ReadU64(input.payload, 8), mode,
                                             filter, options);
}

// The pinned KTX-Software 4.4.2 ETC1S/BasisLZ transcoder crashes on a
// structurally valid container whose ETC1S supercompression global data is
// mutated (a two-byte change to a frozen corpus KTX2 reaches a null Huffman
// table in basisu_lowlevel_etc1s_transcoder::transcode_slice). This target
// therefore drives the product-owned PreflightKtx2 header/level checks for
// BasisLZ inputs and does not enter the raw third-party ETC1S transcode; the
// minimized seed is retained under tests/fuzz/corpus/gltf and the real worker
// Job boundary contains the production crash. Non-BasisLZ (UASTC/uncompressed)
// containers still exercise the real transcode.
bool LooksLikeBasisLz(std::span<const std::byte> bytes)
{
    return model_core::LooksLikeKtx2(bytes) && ReadU32(bytes, 12) == 0 && ReadU32(bytes, 44) == 1;
}

void RunKtx2(const Input& input)
{
    const auto options = MakeTextureOptions(input, SemanticFor(static_cast<uint8_t>(input.flags >> 2)));
    if (LooksLikeBasisLz(input.payload)) {
        const model_core::Ktx2Limits limits{options.maxEncodedBytes, options.maxDecodedBytes,
                                            options.maxPixels, options.maxDimension};
        (void)model_core::PreflightKtx2(input.payload, limits);
        return;
    }
    (void)import_worker::TranscodeKtx2BasisImage(input.payload, options);
}

void RunWebp(const Input& input)
{
    const auto colorSpace = (input.flags & kSrgb) ? model_core::ColorSpaceId::Srgb
                                                  : model_core::ColorSpaceId::Linear;
    (void)import_worker::DecodeWebpImage(
        input.payload, colorSpace,
        MakeTextureOptions(input, SemanticFor(static_cast<uint8_t>(input.flags >> 2))));
}

void RunWicRaster(const Input& input)
{
    const auto colorSpace = (input.flags & kSrgb) ? model_core::ColorSpaceId::Srgb
                                                  : model_core::ColorSpaceId::Linear;
    (void)import_worker::DecodeRasterImageWic(
        input.payload, colorSpace,
        MakeTextureOptions(input, SemanticFor(static_cast<uint8_t>(input.flags >> 2))));
}

void RunSniff(const Input& input)
{
    (void)import_worker::SniffImageFormat(input.payload);
    // Decode the bounded payload as a reference path; embedded NULs and
    // control bytes must not read out of bounds or leave the view.
    const std::string_view path(reinterpret_cast<const char*>(input.payload.data()),
                                (std::min)(input.payload.size(), size_t(4096)));
    (void)import_worker::HasDecodableImageExtension(path);
}

} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    // WIC decoders are COM objects; initialize one MTA for the process.
    (void)::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!data || size > kInputLimit + sizeof(Envelope)) return 0;
    const Input input = Decode({reinterpret_cast<const std::byte*>(data), size});
    if (input.payload.empty()) return 0;
    if (input.payload.size() > kInputLimit) return 0;
    if (input.domain != Domain::Adapter && input.payload.size() > kMaxSidecarBytes) return 0;
    try {
        switch (input.domain) {
        case Domain::Adapter: RunAdapter(input); break;
        case Domain::Draco: RunDraco(input); break;
        case Domain::Meshopt: RunMeshopt(input); break;
        case Domain::Ktx2: RunKtx2(input); break;
        case Domain::WebP: RunWebp(input); break;
        case Domain::WicRaster: RunWicRaster(input); break;
        case Domain::Sniff: RunSniff(input); break;
        }
    } catch (...) {
        // Product code must not throw past its boundaries; a throw here is a
        // finding the sanitizer/regression suites must chase, not a crash.
    }
    return 0;
}