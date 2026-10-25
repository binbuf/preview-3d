// SEC-17 provider pipeline fuzz target.
//
// Instruments the provider-owned boundary that the worker-format targets
// (SEC-15/16) do not reach: the bounded stream source, every family adapter's
// normalization into the frozen sample types, the deterministic sampler, and
// the CPU tile rasterizer. No COM object, no window, no GPU device and no
// child process is created; the input is an in-process IStream double and the
// output is discarded.
//
// One 24-byte envelope selects a domain:
//
//   - Pipeline  RunThumbnailPipeline over an in-memory BoundedStreamSource for
//               the routed family (the production registry -> adapter ->
//               sampler -> rasterizer path). Hostile STATSTG/seek/short-read
//               behaviour is selected by the stream flags, so the same domain
//               covers the SEC-07 stream hardening.
//   - Stream    Drive BoundedStreamSource directly and read fuzzed ranges.
//   - Sampler   Feed fuzzed TriangleSample/PointSample records.
//   - Raster    Render fuzzed SampledGeometry (NaN/Inf, degenerate transforms,
//               extreme aspect ratios) through the real tile rasterizer.
//
// The pinned third-party parsers (fastgltf/ufbx/lib3mf/TinyUSDZ/OCCT/draco/ktx)
// are linked but not sanitizer-instrumented; ASan instruments the product-owned
// preflights, adapters, sampler and rasterizer, and its interceptor catches
// some third-party overwrites. The real AppContainer/Job worker and the OCCT
// transfer boundary stay the process-containment evidence (SEC-08/09).

#include "AllocationLedger.h"
#include "Containment.h"
#include "CpuRasterizer.h"
#include "CpuRasterizerImpl.h"
#include "Deadline.h"
#include "DeterministicGeometrySampler.h"
#include "FamilyAdapterRegistry.h"
#include "GeometrySampler.h"
#include "ProviderLimits.h"
#include "ProviderTypes.h"
#include "StreamSource.h"
#include "ThumbnailPipeline.h"

#include <windows.h>

#include <objidl.h>
#include <unknwn.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

using namespace preview3d::provider;

namespace {

constexpr uint32_t kEnvelopeMagic = 0x5a565250; // "PRVZ"
constexpr std::size_t kInputLimit = 2u * 1024u * 1024u;
constexpr std::size_t kMaxEnvelopeRecords = 8192;

#pragma pack(push, 1)
struct Envelope {
    uint32_t magic;
    uint8_t domain;
    uint8_t family;
    uint8_t flags;
    uint8_t reserved;
    uint32_t cx;
    uint64_t reportedSize;
    uint32_t payloadBytes;
};
#pragma pack(pop)
static_assert(sizeof(Envelope) == 24);

enum : uint8_t {
    kPipeline = 0,
    kStream = 1,
    kSampler = 2,
    kRaster = 3,
};

// Stream-behaviour flags (shared by the Pipeline and Stream domains).
enum : uint8_t {
    kStreamNonSeekable = 1u << 0, // Seek always fails -> non-seekable path
    kStreamStatFails = 1u << 1,   // Stat fails -> force the SEEK_END probe
    kStreamStatSize = 1u << 2,    // Stat reports `reportedSize`
    kStreamShortRead = 1u << 3,   // Read returns fewer bytes than requested
    kStreamStatWrongType = 1u << 4,
};

struct Input {
    uint8_t domain = kPipeline;
    uint8_t family = 0;
    uint8_t flags = 0;
    uint32_t cx = 0;
    uint64_t reportedSize = 0;
    std::span<const std::byte> payload;
};

Input Decode(std::span<const std::byte> bytes)
{
    Input result{};
    if (bytes.size() < sizeof(Envelope)) {
        // A bare blob is treated as a Pipeline-domain source with the family
        // chosen by the first byte so the mutator can reach every adapter.
        result.family =
            bytes.empty() ? 0
                          : static_cast<uint8_t>(std::to_integer<unsigned>(bytes[0]) % 9);
        result.payload = bytes;
        return result;
    }
    Envelope envelope{};
    std::memcpy(&envelope, bytes.data(), sizeof(envelope));
    if (envelope.magic != kEnvelopeMagic) {
        result.family = static_cast<uint8_t>(std::to_integer<unsigned>(bytes[0]) % 9);
        result.payload = bytes;
        return result;
    }
    result.domain = static_cast<uint8_t>(envelope.domain % 4);
    result.family = static_cast<uint8_t>(envelope.family % 9);
    result.flags = envelope.flags;
    result.cx = envelope.cx;
    result.reportedSize = envelope.reportedSize;
    const std::size_t offset = sizeof(envelope);
    const std::size_t available = bytes.size() - offset;
    const std::size_t count =
        (std::min)({static_cast<std::size_t>(envelope.payloadBytes), available,
                    kInputLimit});
    result.payload = bytes.subspan(offset, count);
    return result;
}

// Stack-allocated in-memory IStream double with selectable hostile behaviour.
// `Release` never deletes: the fuzz function owns the stream for its whole
// scope and the provider only releases the reference it added.
class FuzzStream final : public IStream {
public:
    FuzzStream(std::span<const std::byte> data, uint8_t flags, uint64_t reportedSize)
        : data_(data), flags_(flags), reportedSize_(reportedSize)
    {
    }

    FuzzStream(const FuzzStream&) = delete;
    FuzzStream& operator=(const FuzzStream&) = delete;

    HRESULT WINAPI QueryInterface(REFIID riid, void** ppvObject) noexcept override
    {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IStream))) {
            *ppvObject = static_cast<IStream*>(this);
            ++ref_;
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG WINAPI AddRef() noexcept override { return static_cast<ULONG>(++ref_); }
    ULONG WINAPI Release() noexcept override { return static_cast<ULONG>(--ref_); }

    HRESULT WINAPI Read(void* pv, ULONG cb, ULONG* pcbRead) noexcept override
    {
        if (pv == nullptr || pcbRead == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        ULONG count = 0;
        if (position_ < data_.size()) {
            const std::uint64_t available =
                static_cast<std::uint64_t>(data_.size() - position_);
            count = static_cast<ULONG>((std::min)(available, static_cast<std::uint64_t>(cb)));
            if (count != 0) {
                std::memcpy(pv, data_.data() + position_, count);
                position_ += count;
            }
        }
        if ((flags_ & kStreamShortRead) != 0 && count > 1) {
            count -= 1;
            // Rewind the byte we did not report so a later read can see it.
            --position_;
        }
        *pcbRead = count;
        return S_OK;
    }

    HRESULT WINAPI Write(const void*, ULONG, ULONG*) noexcept override
    {
        return STG_E_ACCESSDENIED;
    }

    HRESULT WINAPI Seek(LARGE_INTEGER move, DWORD origin,
                        ULARGE_INTEGER* newPosition) noexcept override
    {
        if ((flags_ & kStreamNonSeekable) != 0) {
            return STG_E_INVALIDFUNCTION;
        }
        std::int64_t base = 0;
        switch (origin) {
            case STREAM_SEEK_SET: base = 0; break;
            case STREAM_SEEK_CUR: base = static_cast<std::int64_t>(position_); break;
            case STREAM_SEEK_END: base = static_cast<std::int64_t>(data_.size()); break;
            default: return STG_E_INVALIDFUNCTION;
        }
        const std::int64_t next = base + move.QuadPart;
        if (next < 0) {
            return STG_E_INVALIDFUNCTION;
        }
        position_ = static_cast<std::uint64_t>(next);
        if (newPosition != nullptr) {
            newPosition->QuadPart = position_;
        }
        return S_OK;
    }

    HRESULT WINAPI SetSize(ULARGE_INTEGER) noexcept override { return E_NOTIMPL; }
    HRESULT WINAPI CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
                          ULARGE_INTEGER*) noexcept override
    {
        return E_NOTIMPL;
    }
    HRESULT WINAPI Commit(DWORD) noexcept override { return S_OK; }
    HRESULT WINAPI Revert() noexcept override { return E_NOTIMPL; }
    HRESULT WINAPI LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) noexcept override
    {
        return E_NOTIMPL;
    }
    HRESULT WINAPI UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) noexcept override
    {
        return E_NOTIMPL;
    }
    HRESULT WINAPI Stat(STATSTG* pstatstg, DWORD) noexcept override
    {
        if (pstatstg == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        std::memset(pstatstg, 0, sizeof(*pstatstg));
        if ((flags_ & kStreamStatFails) != 0) {
            return STG_E_ACCESSDENIED;
        }
        pstatstg->type =
            (flags_ & kStreamStatWrongType) != 0 ? STGTY_STORAGE : STGTY_STREAM;
        const std::uint64_t size =
            (flags_ & kStreamStatSize) != 0 ? reportedSize_ : data_.size();
        pstatstg->cbSize.QuadPart = static_cast<ULONGLONG>(size);
        return S_OK;
    }
    HRESULT WINAPI Clone(IStream**) noexcept override { return E_NOTIMPL; }

private:
    std::span<const std::byte> data_;
    uint8_t flags_ = 0;
    uint64_t reportedSize_ = 0;
    std::uint64_t position_ = 0;
    LONG ref_ = 1;
};

// Bit-casts a word into a float without invoking FP conversions, so fuzzed
// payloads reach NaN/Inf naturally.
float AsFloat(uint32_t bits) noexcept
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double AsDouble(uint64_t bits) noexcept
{
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::vector<uint32_t> Words(std::span<const std::byte> payload)
{
    std::vector<uint32_t> words;
    const std::size_t count = (std::min)(payload.size() / 4, kMaxEnvelopeRecords * 24);
    words.resize(count);
    if (count != 0) {
        std::memcpy(words.data(), payload.data(), count * sizeof(uint32_t));
    }
    return words;
}

void FuzzStreamDomain(const Input& input, Deadline& deadline, AllocationLedger& ledger)
{
    FuzzStream stream(input.payload, input.flags, input.reportedSize);
    ProviderOutcome outcome = ProviderOutcome::Success;
    std::unique_ptr<BoundedStreamSource> source =
        BoundedStreamSource::Create(&stream, deadline, ledger, outcome);
    if (!source) {
        return;
    }
    const std::vector<uint32_t> words = Words(input.payload);
    const std::size_t probes = (std::min)(words.size() / 2, std::size_t{16});
    std::vector<std::byte> scratch(64 * 1024);
    for (std::size_t i = 0; i < probes; ++i) {
        const std::uint64_t offset =
            (static_cast<std::uint64_t>(words[2 * i]) << 32) | words[2 * i + 1];
        const std::size_t length =
            scratch.empty() ? 0 : (words[i % words.size()] % (scratch.size() + 1));
        (void)source->ReadAt(offset, std::span<std::byte>(scratch.data(), length));
    }
    (void)source->ContiguousView();
    (void)source->Size();
    (void)source->Seekable();
}

void FuzzSamplerDomain(const Input& input)
{
    DeterministicGeometrySampler sampler;
    sampler.Begin(input.reportedSize ^ (static_cast<uint64_t>(input.family) << 8));
    const std::vector<uint32_t> words = Words(input.payload);
    if (words.empty()) {
        return;
    }
    const std::size_t records =
        (std::min)(words.size() / 24, kMaxEnvelopeRecords);
    for (std::size_t r = 0; r < records; ++r) {
        const uint32_t* w = words.data() + r * 24;
        TriangleSample triangle{};
        for (int v = 0; v < 3; ++v) {
            for (int c = 0; c < 3; ++c) {
                triangle.vertices[v].position[c] = AsFloat(w[3 * v + c]);
                triangle.vertices[v].normal[c] = AsFloat(w[9 + 3 * v + c]);
            }
            for (int c = 0; c < 4; ++c) {
                triangle.vertices[v].color[c] = AsFloat(w[18 + c]);
            }
        }
        triangle.origin[0] = AsDouble(static_cast<uint64_t>(w[23]) << 32 | w[22]);
        triangle.materialIndex = w[21] % 4096;
        if (!sampler.AddTriangle(triangle)) {
            break;
        }
        PointSample point{};
        point.vertex.position[0] = AsFloat(w[3]);
        point.vertex.position[1] = AsFloat(w[4]);
        point.vertex.position[2] = AsFloat(w[5]);
        point.radius = AsFloat(w[6]);
        point.materialIndex = w[7] % 4096;
        if (!sampler.AddPoint(point)) {
            break;
        }
    }
    (void)sampler.Result();
}

void FuzzRasterDomain(const Input& input, Deadline& deadline, AllocationLedger& ledger)
{
    const std::vector<uint32_t> words = Words(input.payload);
    std::vector<TriangleSample> triangles;
    std::vector<PointSample> points;
    Bounds bounds{};
    const std::size_t records = (std::min)(words.size() / 24, kMaxEnvelopeRecords);
    triangles.reserve(records);
    points.reserve(records);
    const auto accumulate = [&bounds](const VertexSample& v) {
        for (int c = 0; c < 3; ++c) {
            const double value = static_cast<double>(v.position[c]);
            if (!std::isfinite(value)) {
                continue;
            }
            if (!bounds.valid) {
                for (int k = 0; k < 3; ++k) {
                    bounds.min[k] = value;
                    bounds.max[k] = value;
                }
                bounds.valid = true;
            } else {
                bounds.min[c] = (std::min)(bounds.min[c], value);
                bounds.max[c] = (std::max)(bounds.max[c], value);
            }
        }
    };
    for (std::size_t r = 0; r < records; ++r) {
        const uint32_t* w = words.data() + r * 24;
        TriangleSample triangle{};
        for (int v = 0; v < 3; ++v) {
            for (int c = 0; c < 3; ++c) {
                triangle.vertices[v].position[c] = AsFloat(w[3 * v + c]);
            }
            accumulate(triangle.vertices[v]);
        }
        triangle.origin[0] = AsDouble(static_cast<uint64_t>(w[19]) << 32 | w[9]);
        triangle.materialIndex = w[20] % 4096;
        triangles.push_back(triangle);
    }
    for (std::size_t r = 0; r + 4 <= words.size() && r / 4 < kMaxEnvelopeRecords; r += 4) {
        PointSample point{};
        point.vertex.position[0] = AsFloat(words[r]);
        point.vertex.position[1] = AsFloat(words[r + 1]);
        point.vertex.position[2] = AsFloat(words[r + 2]);
        point.radius = AsFloat(words[r + 3]);
        accumulate(point.vertex);
        points.push_back(point);
    }

    SampledGeometry geometry{};
    geometry.triangles = std::span<const TriangleSample>(triangles);
    geometry.points = std::span<const PointSample>(points);
    geometry.bounds = bounds;

    std::vector<model_core::MaterialPayload> materials(1, NeutralMaterial());
    if ((input.flags & 1u) != 0) {
        materials.push_back(NeutralMaterial());
        materials.back().baseColorFactor[0] = AsFloat(input.cx);
        materials.back().alphaMode = 1; // mask
    }

    RasterRequest request{};
    request.geometry = &geometry;
    request.materials = std::span<const model_core::MaterialPayload>(materials);
    request.requestedSize = input.cx;
    request.allowSupersample = (input.flags & 2u) == 0;
    request.deadline = &deadline;
    request.ledger = &ledger;

    RasterImage image;
    (void)RenderCpuTileRaster(request, image);
}

void FuzzPipelineDomain(const Input& input, Deadline& deadline, AllocationLedger& ledger)
{
    const uint8_t family =
        static_cast<uint8_t>(input.family == 0 ? 1 : input.family);
    FuzzStream stream(input.payload, input.flags, input.reportedSize);
    ProviderOutcome outcome = ProviderOutcome::Success;
    std::unique_ptr<BoundedStreamSource> source =
        BoundedStreamSource::Create(&stream, deadline, ledger, outcome);
    if (!source) {
        return;
    }

    ThumbnailRequest request{};
    request.family = static_cast<Family>(family % 9);
    request.source = source.get();
    request.limits = &ProviderLimits::Default();
    request.deadline = &deadline;
    request.ledger = &ledger;
    request.cx = input.cx;

    RasterImage image;
    IThumbnailDependencies& dependencies = DefaultThumbnailDependencies();
    (void)RunThumbnailPipeline(request, dependencies, image);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (data == nullptr || size == 0 || size > kInputLimit + sizeof(Envelope)) {
        return 0;
    }

    const Input input =
        Decode({reinterpret_cast<const std::byte*>(data), size});

    // A per-input ledger keeps fuzz iterations independent; the production
    // ledger is process-wide and a leaked charge would poison later runs.
    AllocationLedger ledger;
    Deadline deadline;

    switch (input.domain) {
        case kStream:
            FuzzStreamDomain(input, deadline, ledger);
            break;
        case kSampler:
            FuzzSamplerDomain(input);
            break;
        case kRaster:
            FuzzRasterDomain(input, deadline, ledger);
            break;
        case kPipeline:
        default:
            FuzzPipelineDomain(input, deadline, ledger);
            break;
    }

    // SEC-08: a contained structured fault quarantined the process. Surface the
    // triggering input as a libFuzzer crash artifact instead of silently
    // returning DecoderFailure for every later input (which would hide it).
    if (ContainmentQuarantined()) {
        std::fprintf(stderr, "contained structured fault code=0x%08X\n",
                     ContainmentQuarantineCode());
        std::abort();
    }
    return 0;
}