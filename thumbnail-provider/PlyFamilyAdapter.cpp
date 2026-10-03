// T23 PLY family adapter implementation (see PlyFamilyAdapter.h).

#include "PlyFamilyAdapter.h"

#include "AllocationLedger.h"
#include "ContainmentStage.h"
#include "Deadline.h"
#include "ProviderLimits.h"

#include "parser_core/AsciiTokenizer.h"
#include "parser_core/PlyParserCore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace preview3d::provider {
namespace {

// Header scan window: mirrors parser_core's private kMaxHeaderBytes so the
// non-contiguous path reads only what the bounded header parser can consume.
constexpr std::size_t kHeaderProbeBytes = 64 * 1024;

// Adapter-only record/list bounds supplementing the Tier A caps. Neither is
// derived from a declared count, so a hostile count cannot size a read or an
// allocation before these checks.
constexpr std::uint64_t kMaxSkippedListLength = 65'536;
constexpr std::uint32_t kMaxPolygonVerticesPerFace = 255;
constexpr std::uint64_t kMaxSkippedElementRecords = ProviderLimits::kPointsInspectedMax;

constexpr std::size_t kCheckpointInterval = 1024;
// The header parser caps properties at 64 and a scalar at 8 bytes, so a fixed
// vertex record cannot exceed 512 bytes.
constexpr std::size_t kMaxVertexRecordBytes = 64 * 8;
constexpr std::size_t kVertexCacheSize = 256;
constexpr std::size_t kPointBlockBytes = 64 * 1024;

bool IsFinite3(const double value[3]) noexcept
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

// Every property is fixed-width, so the record byte stride is known; nullopt
// when a list-typed property makes the stride data-dependent.
std::optional<std::uint64_t> FixedStride(const parser_core::PlyElement& element) noexcept
{
    std::uint64_t stride = 0;
    for (const parser_core::PlyProperty& property : element.properties) {
        if (property.isList) {
            return std::nullopt;
        }
        stride += parser_core::ScalarByteSize(property.valueType);
    }
    return stride;
}

} // namespace

void PlyAdapter::NormalizeNormal(DecodedVertex& vertex) noexcept
{
    const float lengthSq = vertex.normal[0] * vertex.normal[0] + vertex.normal[1] * vertex.normal[1] +
                           vertex.normal[2] * vertex.normal[2];
    if (!std::isfinite(lengthSq) || lengthSq <= 1e-12f) {
        vertex.normal[0] = 0.0f;
        vertex.normal[1] = 0.0f;
        vertex.normal[2] = 0.0f;
        return;
    }
    const float inverse = 1.0f / std::sqrt(lengthSq);
    vertex.normal[0] *= inverse;
    vertex.normal[1] *= inverse;
    vertex.normal[2] *= inverse;
}

void PlyAdapter::Reset() noexcept
{
    input_ = AdapterInput{};
    header_ = parser_core::PlyHeader{};
    parsed_ = false;
    contiguous_ = {};
    sourceSize_ = 0;
    bodyOffset_ = 0;
    vertexElement_ = nullptr;
    faceElement_ = nullptr;
    vertexElementIndex_ = 0;
    faceElementIndex_ = 0;
    faceListProperty_ = 0;
    hasFace_ = false;
    ascii_ = false;
    bigEndian_ = false;
    vertexCount_ = 0;
    vertexStart_ = 0;
    faceStart_ = 0;
    schema_ = VertexSchema{};
    haveOrigin_ = false;
    origin_[0] = origin_[1] = origin_[2] = 0.0;
}

ErrorCode PlyAdapter::Initialize(const AdapterInput& input) noexcept
{
    return RunContainedStageMember([this, &input]() { return InitializeImpl(input); },
                                   DiagnosticStage::AdapterInitialize);
}

ErrorCode PlyAdapter::InitializeImpl(const AdapterInput& input)
{
    Reset();
    if (input.source == nullptr || input.limits == nullptr || input.deadline == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }
    input_ = input;
    return ErrorCode::None;
}

ErrorCode PlyAdapter::SourceReadFailure() const noexcept
{
    if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }
    return ErrorCode::MalformedData;
}

ErrorCode PlyAdapter::LoadHeader()
{
    // The bounded contiguous view is the whole source when it fits the 128 MiB
    // backing cap (T12); otherwise each read is a checked ranged read.
    contiguous_ = input_.source->ContiguousView();
    sourceSize_ = input_.source->Size();
    if (sourceSize_ == 0 && !contiguous_.empty()) {
        sourceSize_ = contiguous_.size();
    }

    std::vector<std::byte> probe;
    std::span<const std::byte> headerSpan;
    if (!contiguous_.empty()) {
        headerSpan = contiguous_;
    } else {
        if (sourceSize_ == 0) {
            return ErrorCode::MalformedData;
        }
        const std::uint64_t toRead = (std::min)(sourceSize_, std::uint64_t{kHeaderProbeBytes});
        probe.resize(static_cast<std::size_t>(toRead));
        if (!input_.source->ReadAt(0, probe)) {
            return SourceReadFailure();
        }
        headerSpan = probe;
    }

    auto parsed = parser_core::ParseHeader(headerSpan);
    if (auto* error = std::get_if<model_core::ImportErrorCode>(&parsed)) {
        return *error;
    }
    header_ = std::move(std::get<parser_core::PlyHeader>(parsed));
    bodyOffset_ = header_.bodyOffset;
    if (sourceSize_ != 0 && bodyOffset_ > sourceSize_) {
        return ErrorCode::MalformedData;
    }
    ascii_ = header_.format == parser_core::PlyFormat::Ascii;
    bigEndian_ = header_.format == parser_core::PlyFormat::BinaryBigEndian;
    return ErrorCode::None;
}

ErrorCode PlyAdapter::ResolveSchema() noexcept
{
    for (std::size_t i = 0; i < header_.elements.size(); ++i) {
        parser_core::PlyElement& element = header_.elements[i];
        if (vertexElement_ == nullptr && element.name == "vertex") {
            vertexElement_ = &element;
            vertexElementIndex_ = i;
        } else if (faceElement_ == nullptr && element.name == "face") {
            faceElement_ = &element;
            faceElementIndex_ = i;
        }
    }
    if (vertexElement_ == nullptr) {
        return ErrorCode::MalformedData;
    }
    vertexCount_ = vertexElement_->count;
    if (vertexCount_ == 0) {
        return ErrorCode::MalformedData;
    }
    if (vertexCount_ > ProviderLimits::kPointsInspectedMax) {
        return ErrorCode::ResourceLimit;
    }

    const auto propertyIndex = [this](const char* name) -> int {
        for (std::size_t i = 0; i < vertexElement_->properties.size(); ++i) {
            const parser_core::PlyProperty& property = vertexElement_->properties[i];
            if (!property.isList && property.name == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    schema_ = VertexSchema{};
    schema_.x = propertyIndex("x");
    schema_.y = propertyIndex("y");
    schema_.z = propertyIndex("z");
    if (schema_.x < 0 || schema_.y < 0 || schema_.z < 0) {
        return ErrorCode::MalformedData;
    }
    schema_.nx = propertyIndex("nx");
    schema_.ny = propertyIndex("ny");
    schema_.nz = propertyIndex("nz");
    schema_.hasNormals = schema_.nx >= 0 && schema_.ny >= 0 && schema_.nz >= 0;
    schema_.red = propertyIndex("red");
    if (schema_.red < 0) schema_.red = propertyIndex("r");
    schema_.green = propertyIndex("green");
    if (schema_.green < 0) schema_.green = propertyIndex("g");
    schema_.blue = propertyIndex("blue");
    if (schema_.blue < 0) schema_.blue = propertyIndex("b");
    schema_.alpha = propertyIndex("alpha");
    if (schema_.alpha < 0) schema_.alpha = propertyIndex("a");
    schema_.hasColors = schema_.red >= 0 && schema_.green >= 0 && schema_.blue >= 0;

    const std::optional<std::uint64_t> stride = FixedStride(*vertexElement_);
    schema_.stride = stride.value_or(0);
    if (schema_.stride > kMaxVertexRecordBytes) {
        return ErrorCode::ResourceLimit;
    }

    hasFace_ = faceElement_ != nullptr && faceElement_->count > 0;
    if (hasFace_) {
        if (faceElementIndex_ < vertexElementIndex_) {
            // Faces reference the vertex table by index; without the vertices
            // first there is nothing to resolve against.
            return ErrorCode::UnsupportedRequiredFeature;
        }
        const auto notFound = faceElement_->properties.size();
        faceListProperty_ = notFound;
        for (std::size_t i = 0; i < faceElement_->properties.size(); ++i) {
            const parser_core::PlyProperty& property = faceElement_->properties[i];
            if (property.isList &&
                (property.name == "vertex_indices" || property.name == "vertex_index")) {
                if (faceListProperty_ != notFound) {
                    return ErrorCode::MalformedData; // more than one index list: ambiguous
                }
                faceListProperty_ = i;
            }
        }
        if (faceListProperty_ == notFound) {
            return ErrorCode::MalformedData;
        }
    }

    for (const parser_core::PlyElement& element : header_.elements) {
        if (&element == vertexElement_ ||
            (faceElement_ != nullptr && &element == faceElement_)) {
            continue;
        }
        if (element.count > kMaxSkippedElementRecords) {
            return ErrorCode::ResourceLimit;
        }
    }

    if (hasFace_) {
        if (ascii_) {
            // The ASCII mesh retains a bounded vertex table; reject a table over
            // the accounted-scratch cap before any allocation exists.
            const auto tableBytes = CheckedMultiply(vertexCount_, sizeof(VertexSample));
            if (!tableBytes || *tableBytes > ProviderLimits::kAccountedScratchMaxBytes) {
                return ErrorCode::ResourceLimit;
            }
        } else {
            // A binary mesh addresses each referenced record by stride; a list
            // in the vertex element has no fixed stride to address.
            if (schema_.stride == 0) {
                return ErrorCode::UnsupportedRequiredFeature;
            }
            return ComputeBinaryOffsets();
        }
    }
    return ErrorCode::None;
}

ErrorCode PlyAdapter::ComputeBinaryOffsets() noexcept
{
    std::uint64_t cursor = bodyOffset_;
    vertexStart_ = 0;
    faceStart_ = 0;
    bool foundVertex = false;
    bool foundFace = false;
    for (const parser_core::PlyElement& element : header_.elements) {
        if (&element == faceElement_) {
            faceStart_ = cursor;
            foundFace = true;
            break;
        }
        const std::optional<std::uint64_t> stride = FixedStride(element);
        if (!stride) {
            // An element before the face body whose size is data-dependent means
            // the face body offset cannot be proven without a scan.
            return ErrorCode::UnsupportedRequiredFeature;
        }
        const auto bytes = CheckedMultiply(element.count, *stride);
        if (!bytes) {
            return ErrorCode::ResourceLimit;
        }
        if (!FitsInRange(cursor, *bytes, sourceSize_)) {
            return ErrorCode::MalformedData;
        }
        if (&element == vertexElement_) {
            vertexStart_ = cursor;
            foundVertex = true;
        }
        cursor += *bytes;
    }
    if (!foundFace || !foundVertex) {
        return ErrorCode::MalformedData;
    }
    return ErrorCode::None;
}

ErrorCode PlyAdapter::Parse() noexcept
{
    return RunContainedStageMember([this]() { return ParseImpl(); }, DiagnosticStage::Parse);
}

ErrorCode PlyAdapter::ParseImpl()
{
    if (!input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }
    parsed_ = false;
    haveOrigin_ = false;

    ErrorCode header = LoadHeader();
    if (header != ErrorCode::None) {
        return header;
    }
    ErrorCode schema = ResolveSchema();
    if (schema != ErrorCode::None) {
        return schema;
    }
    parsed_ = true;
    return ErrorCode::None;
}

ErrorCode PlyAdapter::EnumerateMaterials(IMaterialSink& sink) noexcept
{
    return RunContainedStageMember([this, &sink]() { return EnumerateMaterialsImpl(sink); },
                                   DiagnosticStage::Materials);
}

ErrorCode PlyAdapter::EnumerateMaterialsImpl(IMaterialSink& sink)
{
    // PLY carries vertex colors, not material records. The neutral palette is
    // emitted as material 1; when the vertex element carries colors, the base
    // color factor is white so `vertexColor * baseColor` preserves the source
    // color (CpuRasterizer.h, "Material resolution").
    model_core::MaterialPayload material = NeutralMaterial();
    if (schema_.hasColors) {
        material.baseColorFactor[0] = 1.0f;
        material.baseColorFactor[1] = 1.0f;
        material.baseColorFactor[2] = 1.0f;
        material.baseColorFactor[3] = 1.0f;
    }
    sink.OnMaterial(1, material);
    return ErrorCode::None;
}

bool PlyAdapter::ReadBytesAt(std::uint64_t offset, std::span<std::byte> dest) noexcept
{
    if (dest.empty()) {
        return true;
    }
    if (!contiguous_.empty() && offset <= contiguous_.size() &&
        dest.size() <= contiguous_.size() - offset) {
        std::memcpy(dest.data(), contiguous_.data() + offset, dest.size());
        return true;
    }
    if (sourceSize_ != 0 && !FitsInRange(offset, dest.size(), sourceSize_)) {
        return false;
    }
    return input_.source->ReadAt(offset, dest);
}

bool PlyAdapter::SkipBytes(std::uint64_t& cursor, std::uint64_t bytes) const noexcept
{
    if (!FitsInRange(cursor, bytes, sourceSize_)) {
        return false;
    }
    cursor += bytes;
    return true;
}

bool PlyAdapter::ReadScalar(std::uint64_t& cursor, parser_core::PlyScalarType type,
                            double& value) noexcept
{
    const std::size_t size = parser_core::ScalarByteSize(type);
    if (size == 0) {
        return false;
    }
    std::byte buffer[8];
    const std::span<std::byte> dest(buffer, size);
    if (!ReadBytesAt(cursor, dest)) {
        return false;
    }
    cursor += size;
    value = parser_core::ReadScalarAsDouble(
        type, bigEndian_, std::span<const std::byte>(buffer, size));
    return true;
}

ErrorCode PlyAdapter::SkipList(std::uint64_t& cursor,
                               const parser_core::PlyProperty& property) noexcept
{
    double count = 0.0;
    if (!ReadScalar(cursor, property.countType, count)) {
        return ErrorCode::MalformedData;
    }
    if (!(count >= 0.0) || std::floor(count) != count) {
        return ErrorCode::MalformedData;
    }
    if (count > static_cast<double>(kMaxSkippedListLength)) {
        return ErrorCode::ResourceLimit;
    }
    const auto bytes = CheckedMultiply(static_cast<std::uint64_t>(count),
                                       parser_core::ScalarByteSize(property.valueType));
    if (!bytes) {
        return ErrorCode::ResourceLimit;
    }
    if (!SkipBytes(cursor, *bytes)) {
        return ErrorCode::MalformedData;
    }
    return ErrorCode::None;
}

ErrorCode PlyAdapter::SkipBinaryRecord(std::uint64_t& cursor,
                                       const parser_core::PlyElement& element) noexcept
{
    for (const parser_core::PlyProperty& property : element.properties) {
        if (property.isList) {
            ErrorCode error = SkipList(cursor, property);
            if (error != ErrorCode::None) {
                return error;
            }
        } else if (!SkipBytes(cursor, parser_core::ScalarByteSize(property.valueType))) {
            return ErrorCode::MalformedData;
        }
    }
    return ErrorCode::None;
}

void PlyAdapter::ApplyVertexProperty(int index, const parser_core::PlyProperty& property,
                                     double value, DecodedVertex& out) const noexcept
{
    if (index == schema_.x) {
        out.position[0] = value;
    } else if (index == schema_.y) {
        out.position[1] = value;
    } else if (index == schema_.z) {
        out.position[2] = value;
    } else if (schema_.hasNormals && index == schema_.nx) {
        out.normal[0] = static_cast<float>(value);
    } else if (schema_.hasNormals && index == schema_.ny) {
        out.normal[1] = static_cast<float>(value);
    } else if (schema_.hasNormals && index == schema_.nz) {
        out.normal[2] = static_cast<float>(value);
    } else if (schema_.hasColors && index == schema_.red) {
        out.color[0] = parser_core::NormalizeColor(value, property.valueType);
    } else if (schema_.hasColors && index == schema_.green) {
        out.color[1] = parser_core::NormalizeColor(value, property.valueType);
    } else if (schema_.hasColors && index == schema_.blue) {
        out.color[2] = parser_core::NormalizeColor(value, property.valueType);
    } else if (schema_.hasColors && schema_.alpha >= 0 && index == schema_.alpha) {
        out.color[3] = parser_core::NormalizeColor(value, property.valueType);
    }
}

void PlyAdapter::DecodeFixedVertex(const std::byte* record, DecodedVertex& out) const noexcept
{
    std::size_t offset = 0;
    for (std::size_t i = 0; i < vertexElement_->properties.size(); ++i) {
        const parser_core::PlyProperty& property = vertexElement_->properties[i];
        const std::size_t size = parser_core::ScalarByteSize(property.valueType);
        const double value = parser_core::ReadScalarAsDouble(
            property.valueType, bigEndian_,
            std::span<const std::byte>(record + offset, size));
        ApplyVertexProperty(static_cast<int>(i), property, value, out);
        offset += size;
    }
    NormalizeNormal(out);
}

ErrorCode PlyAdapter::DecodeBinaryVertex(std::uint64_t& cursor, DecodedVertex& out) noexcept
{
    for (std::size_t i = 0; i < vertexElement_->properties.size(); ++i) {
        const parser_core::PlyProperty& property = vertexElement_->properties[i];
        if (property.isList) {
            ErrorCode error = SkipList(cursor, property);
            if (error != ErrorCode::None) {
                return error;
            }
            continue;
        }
        double value = 0.0;
        if (!ReadScalar(cursor, property.valueType, value)) {
            return SourceReadFailure();
        }
        ApplyVertexProperty(static_cast<int>(i), property, value, out);
    }
    NormalizeNormal(out);
    return ErrorCode::None;
}

bool PlyAdapter::EmitPoint(const DecodedVertex& vertex, IGeometrySink& sink) noexcept
{
    if (!IsFinite3(vertex.position)) {
        return true; // dropped, not a failure
    }
    if (!haveOrigin_) {
        origin_[0] = vertex.position[0];
        origin_[1] = vertex.position[1];
        origin_[2] = vertex.position[2];
        haveOrigin_ = true;
    }
    PointSample point{};
    for (int axis = 0; axis < 3; ++axis) {
        point.vertex.position[axis] = static_cast<float>(vertex.position[axis] - origin_[axis]);
        point.vertex.normal[axis] = vertex.normal[axis];
        point.origin[axis] = origin_[axis];
    }
    for (int channel = 0; channel < 4; ++channel) {
        point.vertex.color[channel] = vertex.color[channel];
    }
    point.materialIndex = 1;
    return sink.OnPoint(point);
}

bool PlyAdapter::EmitTriangle(const DecodedVertex& a, const DecodedVertex& b,
                              const DecodedVertex& c, IGeometrySink& sink) noexcept
{
    if (!IsFinite3(a.position) || !IsFinite3(b.position) || !IsFinite3(c.position)) {
        return true; // dropped, not a failure
    }
    if (!haveOrigin_) {
        origin_[0] = a.position[0];
        origin_[1] = a.position[1];
        origin_[2] = a.position[2];
        haveOrigin_ = true;
    }
    const DecodedVertex* corners[3] = {&a, &b, &c};
    TriangleSample triangle{};
    for (int corner = 0; corner < 3; ++corner) {
        const DecodedVertex& source = *corners[corner];
        for (int axis = 0; axis < 3; ++axis) {
            triangle.vertices[corner].position[axis] =
                static_cast<float>(source.position[axis] - origin_[axis]);
            triangle.vertices[corner].normal[axis] = source.normal[axis];
            triangle.origin[axis] = origin_[axis];
        }
        for (int channel = 0; channel < 4; ++channel) {
            triangle.vertices[corner].color[channel] = source.color[channel];
        }
    }
    triangle.materialIndex = 1;
    return sink.OnTriangle(triangle);
}

ErrorCode PlyAdapter::EmitBinaryPoints(IGeometrySink& sink)
{
    std::uint64_t cursor = bodyOffset_;
    if (sourceSize_ != 0 && bodyOffset_ > sourceSize_) {
        return ErrorCode::MalformedData;
    }

    std::optional<AllocationReservation> reservation;
    std::vector<std::byte> block;

    for (std::size_t elementIndex = 0; elementIndex < header_.elements.size(); ++elementIndex) {
        const parser_core::PlyElement& element = header_.elements[elementIndex];
        if (&element != vertexElement_) {
            for (std::uint64_t record = 0; record < element.count; ++record) {
                if ((record % kCheckpointInterval) == 0 && !input_.deadline->Checkpoint()) {
                    return ErrorCode::Cancelled;
                }
                ErrorCode error = SkipBinaryRecord(cursor, element);
                if (error != ErrorCode::None) {
                    return error;
                }
            }
            continue;
        }

        vertexStart_ = cursor;
        if (schema_.stride != 0) {
            const std::uint64_t recordsPerBlock =
                (std::max)(std::uint64_t{1}, std::uint64_t{kPointBlockBytes} / schema_.stride);
            const auto blockBytes = CheckedMultiply(recordsPerBlock, schema_.stride);
            if (!blockBytes) {
                return ErrorCode::ResourceLimit;
            }
            if (input_.ledger != nullptr) {
                reservation = input_.ledger->ReserveScoped(*blockBytes);
                if (!reservation.has_value()) {
                    return ErrorCode::ResourceLimit;
                }
            }
            block.resize(static_cast<std::size_t>(*blockBytes));

            for (std::uint64_t first = 0; first < vertexCount_;) {
                if (!input_.deadline->Checkpoint()) {
                    return ErrorCode::Cancelled;
                }
                const std::uint64_t count = (std::min)(recordsPerBlock, vertexCount_ - first);
                const auto relative = CheckedMultiply(first, schema_.stride);
                const auto offset = relative ? CheckedAdd(vertexStart_, *relative) : std::nullopt;
                if (!offset) {
                    return ErrorCode::ResourceLimit;
                }
                const std::span<std::byte> dest(block.data(),
                                                static_cast<std::size_t>(count * schema_.stride));
                if (!ReadBytesAt(*offset, dest)) {
                    return SourceReadFailure();
                }
                for (std::uint64_t i = 0; i < count; ++i) {
                    DecodedVertex vertex;
                    DecodeFixedVertex(block.data() + static_cast<std::size_t>(i * schema_.stride),
                                      vertex);
                    if (!EmitPoint(vertex, sink)) {
                        return ErrorCode::None;
                    }
                }
                first += count;
            }
            const auto consumed = CheckedMultiply(vertexCount_, schema_.stride);
            const auto end = consumed ? CheckedAdd(vertexStart_, *consumed) : std::nullopt;
            if (!end) {
                return ErrorCode::ResourceLimit;
            }
            cursor = *end;
        } else {
            for (std::uint64_t record = 0; record < vertexCount_; ++record) {
                if ((record % kCheckpointInterval) == 0 && !input_.deadline->Checkpoint()) {
                    return ErrorCode::Cancelled;
                }
                DecodedVertex vertex;
                ErrorCode error = DecodeBinaryVertex(cursor, vertex);
                if (error != ErrorCode::None) {
                    return error;
                }
                if (!EmitPoint(vertex, sink)) {
                    return ErrorCode::None;
                }
            }
        }
    }
    return ErrorCode::None;
}

ErrorCode PlyAdapter::EmitBinaryMesh(IGeometrySink& sink)
{
    if (schema_.stride == 0 || schema_.stride > kMaxVertexRecordBytes) {
        return ErrorCode::UnsupportedRequiredFeature;
    }

    struct CacheEntry {
        std::uint64_t index = (std::numeric_limits<std::uint64_t>::max)();
        DecodedVertex vertex{};
    };
    std::vector<CacheEntry> cache(kVertexCacheSize);
    std::vector<std::byte> record(static_cast<std::size_t>(schema_.stride));

    std::optional<AllocationReservation> reservation;
    if (input_.ledger != nullptr) {
        const auto cacheBytes = CheckedMultiply(schema_.stride, kVertexCacheSize + 1);
        if (!cacheBytes) {
            return ErrorCode::ResourceLimit;
        }
        reservation = input_.ledger->ReserveScoped(*cacheBytes);
        if (!reservation.has_value()) {
            return ErrorCode::ResourceLimit;
        }
    }

    const auto readVertex = [this, &cache, &record](std::uint64_t index,
                                                    DecodedVertex& out) -> bool {
        CacheEntry& entry = cache[static_cast<std::size_t>(index % kVertexCacheSize)];
        if (entry.index == index) {
            out = entry.vertex;
            return true;
        }
        const auto relative = CheckedMultiply(index, schema_.stride);
        const auto offset = relative ? CheckedAdd(vertexStart_, *relative) : std::nullopt;
        if (!offset || !ReadBytesAt(*offset, record)) {
            return false;
        }
        DecodedVertex decoded;
        DecodeFixedVertex(record.data(), decoded);
        if (!IsFinite3(decoded.position)) {
            return false;
        }
        entry.index = index;
        entry.vertex = decoded;
        out = decoded;
        return true;
    };

    std::uint64_t cursor = faceStart_;
    for (std::uint64_t face = 0; face < faceElement_->count; ++face) {
        if ((face % kCheckpointInterval) == 0 && !input_.deadline->Checkpoint()) {
            return ErrorCode::Cancelled;
        }
        std::uint32_t indices[kMaxPolygonVerticesPerFace]{};
        std::size_t count = 0;
        bool valid = true;
        for (std::size_t propertyIndex = 0; propertyIndex < faceElement_->properties.size();
             ++propertyIndex) {
            const parser_core::PlyProperty& property = faceElement_->properties[propertyIndex];
            if (propertyIndex == faceListProperty_) {
                double countValue = 0.0;
                if (!ReadScalar(cursor, property.countType, countValue)) {
                    return ErrorCode::MalformedData;
                }
                if (!(countValue >= 0.0) || std::floor(countValue) != countValue) {
                    return ErrorCode::MalformedData;
                }
                if (countValue > static_cast<double>(kMaxPolygonVerticesPerFace)) {
                    return ErrorCode::ResourceLimit;
                }
                count = static_cast<std::size_t>(countValue);
                for (std::size_t k = 0; k < count; ++k) {
                    double indexValue = 0.0;
                    if (!ReadScalar(cursor, property.valueType, indexValue)) {
                        return ErrorCode::MalformedData;
                    }
                    if (!(indexValue >= 0.0) || !std::isfinite(indexValue) ||
                        std::floor(indexValue) != indexValue) {
                        return ErrorCode::MalformedData;
                    }
                    if (indexValue >= static_cast<double>(vertexCount_)) {
                        valid = false; // out-of-range index drops the face
                    } else {
                        indices[k] = static_cast<std::uint32_t>(indexValue);
                    }
                }
            } else if (property.isList) {
                ErrorCode error = SkipList(cursor, property);
                if (error != ErrorCode::None) {
                    return error;
                }
            } else if (!SkipBytes(cursor, parser_core::ScalarByteSize(property.valueType))) {
                return ErrorCode::MalformedData;
            }
        }
        if (!valid || count < 3) {
            continue;
        }
        for (std::size_t triangle = 1; triangle + 1 < count; ++triangle) {
            DecodedVertex a;
            DecodedVertex b;
            DecodedVertex c;
            if (!readVertex(indices[0], a) || !readVertex(indices[triangle], b) ||
                !readVertex(indices[triangle + 1], c)) {
                continue;
            }
            if (!EmitTriangle(a, b, c, sink)) {
                return ErrorCode::None;
            }
        }
    }
    return ErrorCode::None;
}

ErrorCode PlyAdapter::SkipAsciiList(parser_core::AsciiTokenizer& tokenizer,
                                    const parser_core::PlyProperty& property) noexcept
{
    const std::optional<double> count = tokenizer.NextNumber();
    if (!count || !(*count >= 0.0) || std::floor(*count) != *count) {
        return ErrorCode::MalformedData;
    }
    if (*count > static_cast<double>(kMaxSkippedListLength)) {
        return ErrorCode::ResourceLimit;
    }
    const std::uint64_t values = static_cast<std::uint64_t>(*count);
    for (std::uint64_t i = 0; i < values; ++i) {
        if (!tokenizer.NextToken().has_value()) {
            return ErrorCode::MalformedData;
        }
    }
    (void)property;
    return ErrorCode::None;
}

ErrorCode PlyAdapter::SkipAsciiRecord(parser_core::AsciiTokenizer& tokenizer,
                                      const parser_core::PlyElement& element) noexcept
{
    for (const parser_core::PlyProperty& property : element.properties) {
        if (property.isList) {
            ErrorCode error = SkipAsciiList(tokenizer, property);
            if (error != ErrorCode::None) {
                return error;
            }
        } else if (!tokenizer.NextNumber().has_value()) {
            return ErrorCode::MalformedData;
        }
    }
    return ErrorCode::None;
}

ErrorCode PlyAdapter::ReadAsciiVertex(parser_core::AsciiTokenizer& tokenizer,
                                      DecodedVertex& out) noexcept
{
    for (std::size_t i = 0; i < vertexElement_->properties.size(); ++i) {
        const parser_core::PlyProperty& property = vertexElement_->properties[i];
        if (property.isList) {
            ErrorCode error = SkipAsciiList(tokenizer, property);
            if (error != ErrorCode::None) {
                return error;
            }
            continue;
        }
        const std::optional<double> value = tokenizer.NextNumber();
        if (!value.has_value()) {
            return ErrorCode::MalformedData;
        }
        ApplyVertexProperty(static_cast<int>(i), property, *value, out);
    }
    NormalizeNormal(out);
    return ErrorCode::None;
}

ErrorCode PlyAdapter::EmitAsciiPoints(IGeometrySink& sink) noexcept
{
    if (contiguous_.empty()) {
        return ErrorCode::ResourceLimit;
    }
    parser_core::AsciiTokenizer tokenizer(contiguous_,
                                          static_cast<std::size_t>(bodyOffset_));
    for (std::size_t elementIndex = 0; elementIndex < header_.elements.size(); ++elementIndex) {
        const parser_core::PlyElement& element = header_.elements[elementIndex];
        if (&element != vertexElement_) {
            for (std::uint64_t record = 0; record < element.count; ++record) {
                if ((record % kCheckpointInterval) == 0 && !input_.deadline->Checkpoint()) {
                    return ErrorCode::Cancelled;
                }
                ErrorCode error = SkipAsciiRecord(tokenizer, element);
                if (error != ErrorCode::None) {
                    return error;
                }
            }
            continue;
        }
        for (std::uint64_t record = 0; record < vertexCount_; ++record) {
            if ((record % kCheckpointInterval) == 0 && !input_.deadline->Checkpoint()) {
                return ErrorCode::Cancelled;
            }
            DecodedVertex vertex;
            ErrorCode error = ReadAsciiVertex(tokenizer, vertex);
            if (error != ErrorCode::None) {
                return error;
            }
            if (!EmitPoint(vertex, sink)) {
                return ErrorCode::None;
            }
        }
    }
    return ErrorCode::None;
}

ErrorCode PlyAdapter::EmitAsciiMesh(IGeometrySink& sink)
{
    if (contiguous_.empty()) {
        return ErrorCode::ResourceLimit;
    }
    parser_core::AsciiTokenizer tokenizer(contiguous_,
                                          static_cast<std::size_t>(bodyOffset_));

    const auto tableBytes = CheckedMultiply(vertexCount_, sizeof(VertexSample));
    if (!tableBytes) {
        return ErrorCode::ResourceLimit;
    }
    std::optional<AllocationReservation> reservation;
    if (input_.ledger != nullptr) {
        reservation = input_.ledger->ReserveScoped(*tableBytes);
        if (!reservation.has_value()) {
            return ErrorCode::ResourceLimit;
        }
    }
    std::vector<VertexSample> vertices;
    vertices.reserve(static_cast<std::size_t>(vertexCount_));

    // 1. Retain the bounded vertex table in header order.
    for (std::size_t elementIndex = 0; elementIndex < header_.elements.size(); ++elementIndex) {
        const parser_core::PlyElement& element = header_.elements[elementIndex];
        if (&element == faceElement_) {
            break;
        }
        if (&element != vertexElement_) {
            for (std::uint64_t record = 0; record < element.count; ++record) {
                if ((record % kCheckpointInterval) == 0 && !input_.deadline->Checkpoint()) {
                    return ErrorCode::Cancelled;
                }
                ErrorCode error = SkipAsciiRecord(tokenizer, element);
                if (error != ErrorCode::None) {
                    return error;
                }
            }
            continue;
        }
        for (std::uint64_t record = 0; record < vertexCount_; ++record) {
            if ((record % kCheckpointInterval) == 0 && !input_.deadline->Checkpoint()) {
                return ErrorCode::Cancelled;
            }
            DecodedVertex decoded;
            ErrorCode error = ReadAsciiVertex(tokenizer, decoded);
            if (error != ErrorCode::None) {
                return error;
            }
            VertexSample sample{};
            if (IsFinite3(decoded.position)) {
                if (!haveOrigin_) {
                    origin_[0] = decoded.position[0];
                    origin_[1] = decoded.position[1];
                    origin_[2] = decoded.position[2];
                    haveOrigin_ = true;
                }
                for (int axis = 0; axis < 3; ++axis) {
                    sample.position[axis] =
                        static_cast<float>(decoded.position[axis] - origin_[axis]);
                    sample.normal[axis] = decoded.normal[axis];
                }
            } else {
                for (int axis = 0; axis < 3; ++axis) {
                    sample.position[axis] = std::numeric_limits<float>::quiet_NaN();
                    sample.normal[axis] = 0.0f;
                }
            }
            for (int channel = 0; channel < 4; ++channel) {
                sample.color[channel] = decoded.color[channel];
            }
            vertices.push_back(sample);
        }
    }
    if (vertices.size() != vertexCount_) {
        return ErrorCode::MalformedData;
    }

    // 2. Walk the face element from the tokenizer's current position.
    for (std::size_t elementIndex = vertexElementIndex_ + 1;
         elementIndex < header_.elements.size(); ++elementIndex) {
        const parser_core::PlyElement& element = header_.elements[elementIndex];
        if (&element != faceElement_) {
            for (std::uint64_t record = 0; record < element.count; ++record) {
                if ((record % kCheckpointInterval) == 0 && !input_.deadline->Checkpoint()) {
                    return ErrorCode::Cancelled;
                }
                ErrorCode error = SkipAsciiRecord(tokenizer, element);
                if (error != ErrorCode::None) {
                    return error;
                }
            }
            continue;
        }
        for (std::uint64_t face = 0; face < faceElement_->count; ++face) {
            if ((face % kCheckpointInterval) == 0 && !input_.deadline->Checkpoint()) {
                return ErrorCode::Cancelled;
            }
            std::uint32_t indices[kMaxPolygonVerticesPerFace]{};
            std::size_t count = 0;
            bool valid = true;
            for (std::size_t propertyIndex = 0;
                 propertyIndex < faceElement_->properties.size(); ++propertyIndex) {
                const parser_core::PlyProperty& property =
                    faceElement_->properties[propertyIndex];
                if (propertyIndex == faceListProperty_) {
                    const std::optional<double> countValue = tokenizer.NextNumber();
                    if (!countValue || !(*countValue >= 0.0) ||
                        std::floor(*countValue) != *countValue) {
                        return ErrorCode::MalformedData;
                    }
                    if (*countValue > static_cast<double>(kMaxPolygonVerticesPerFace)) {
                        return ErrorCode::ResourceLimit;
                    }
                    count = static_cast<std::size_t>(*countValue);
                    for (std::size_t k = 0; k < count; ++k) {
                        const std::optional<double> indexValue = tokenizer.NextNumber();
                        if (!indexValue || !(*indexValue >= 0.0) ||
                            !std::isfinite(*indexValue) ||
                            std::floor(*indexValue) != *indexValue) {
                            return ErrorCode::MalformedData;
                        }
                        if (*indexValue >= static_cast<double>(vertexCount_)) {
                            valid = false;
                        } else {
                            indices[k] = static_cast<std::uint32_t>(*indexValue);
                        }
                    }
                } else if (property.isList) {
                    ErrorCode error = SkipAsciiList(tokenizer, property);
                    if (error != ErrorCode::None) {
                        return error;
                    }
                } else if (!tokenizer.NextNumber().has_value()) {
                    return ErrorCode::MalformedData;
                }
            }
            if (!valid || count < 3) {
                continue;
            }
            for (std::size_t triangle = 1; triangle + 1 < count; ++triangle) {
                const VertexSample& a = vertices[indices[0]];
                const VertexSample& b = vertices[indices[triangle]];
                const VertexSample& c = vertices[indices[triangle + 1]];
                if (!std::isfinite(a.position[0]) || !std::isfinite(b.position[0]) ||
                    !std::isfinite(c.position[0])) {
                    continue;
                }
                TriangleSample sample{};
                sample.vertices[0] = a;
                sample.vertices[1] = b;
                sample.vertices[2] = c;
                for (int axis = 0; axis < 3; ++axis) {
                    sample.origin[axis] = origin_[axis];
                }
                sample.materialIndex = 1;
                if (!sink.OnTriangle(sample)) {
                    return ErrorCode::None;
                }
            }
        }
    }
    return ErrorCode::None;
}

ErrorCode PlyAdapter::EnumerateGeometry(IGeometrySink& sink) noexcept
{
    return RunContainedStageMember([this, &sink]() { return EnumerateGeometryImpl(sink); },
                                   DiagnosticStage::Geometry);
}

ErrorCode PlyAdapter::EnumerateGeometryImpl(IGeometrySink& sink)
{
    if (!parsed_) {
        return ErrorCode::InternalImporterFailure;
    }
    if (hasFace_) {
        return ascii_ ? EmitAsciiMesh(sink) : EmitBinaryMesh(sink);
    }
    return ascii_ ? EmitAsciiPoints(sink) : EmitBinaryPoints(sink);
}

} // namespace preview3d::provider
