#include "PlyAdapter.h"
#include "BoundedChunkWriter.h"
#include <unordered_map>

#include "parser_core/AsciiTokenizer.h"
#include "parser_core/PlyParserCore.h"
#include "model_core/Checksum.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "model_core/GeometryBounds.h"
#include "platform/CheckedMath.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <ppl.h>

namespace import_worker {

using namespace model_core;
using namespace parser_core;
using platform::CheckedAdd;
using platform::CheckedMultiply;

using Vec3 = Vec3f;

// Adapter-only bounds (Tier A/B count and scratch caps). The shared PLY
// scalar-type, scalar-read and header-parsing primitives live in
// shared/parser-core/PlyParserCore.h (T07) so the provider compiles the same
// source with no worker/broker header.
constexpr uint64_t kMaxVertices = kTierAVertices;
constexpr uint64_t kMaxFaces = kTierATriangles;
constexpr uint64_t kMaxPolygonVerticesPerFace = 255; // largest value a conventional uchar count_type encodes
constexpr uint64_t kMaxTrianglesAfterTriangulation =
    kTierATriangles; // running total across all faces --
                     // kMaxFaces * (kMaxPolygonVerticesPerFace-2) alone amplifies past any single-file limit
constexpr uint64_t kMaxSkippedElementRecordCount = kTierAVertices;
constexpr uint64_t kMaxSkippedListLength = 65'536; // generic bound for any list-typed property
    // this adapter doesn't specifically recognize (vertex-element lists, non-index face lists,
    // and any property on an unrecognized element)

bool IsAsciiPly(std::span<const std::byte> sourceHeader)
{
    auto parsed = ParseHeader(sourceHeader);
    if (const auto* header = std::get_if<PlyHeader>(&parsed))
        return header->format == PlyFormat::Ascii;
    return false;
}

std::variant<PlyImportResult, ImportErrorCode> ImportPly(std::span<const std::byte> sourcePlyBytes,
                                                         std::span<std::byte> destination,
                                                         uint64_t generationId, uint32_t maxChunkCount,
                                                         bool allowAscii, ChunkBatchSink* batchSink,
                                                         model_core::MappedFile* mappedSource)
{
    const uint64_t sourceSize = mappedSource ? mappedSource->SizeBytes() : sourcePlyBytes.size();
    BoundedMappedReader mappedReader(mappedSource);
    auto readSource = [&](uint64_t& at, uint64_t bytes) -> std::optional<std::span<const std::byte>> {
        if (!mappedSource)
            return ReadBytes(sourcePlyBytes, at, bytes);
        auto result = mappedReader.Read(at, bytes);
        if (result)
            at += bytes;
        return result;
    };
    if (sourceSize > kTierAPrimaryBytes || TierAScratchLimit() < 32ull * 1024 * 1024)
        return ImportErrorCode::ResourceLimit;
    if (maxChunkCount < 1) {
        return ImportErrorCode::ResourceLimit;
    }

    auto headerResult = ParseHeader(sourcePlyBytes);
    if (auto* err = std::get_if<ImportErrorCode>(&headerResult)) {
        return *err;
    }
    PlyHeader header = std::move(std::get<PlyHeader>(headerResult));
    const bool ascii = header.format == PlyFormat::Ascii;
    if (ascii && !allowAscii)
        return ImportErrorCode::UnsupportedEncoding;
    if (ascii && (sourceSize > kTierBPrimarySourceBytes || TierBScratchLimit() < 32ull * 1024 * 1024))
        return ImportErrorCode::ResourceLimit;
    const uint64_t vertexLimit = ascii ? kTierBVertexLimit : kMaxVertices;
    const uint64_t faceLimit = ascii ? kTierBTriangleLimit : kMaxFaces;
    const uint64_t skippedRecordLimit = ascii ? kTierBVertexLimit : kMaxSkippedElementRecordCount;

    PlyElement* vertexElement = nullptr;
    PlyElement* faceElement = nullptr;
    for (auto& element : header.elements) {
        if (element.name == "vertex" && vertexElement == nullptr) {
            vertexElement = &element;
        } else if (element.name == "face" && faceElement == nullptr) {
            faceElement = &element;
        }
    }
    if (vertexElement == nullptr) {
        return ImportErrorCode::MalformedData;
    }
    if (vertexElement->count == 0) return ImportErrorCode::EmptyGeometry;
    if (vertexElement->count > vertexLimit) {
        return ImportErrorCode::ResourceLimit;
    }

    int xIdx = -1, yIdx = -1, zIdx = -1;
    int nxIdx = -1, nyIdx = -1, nzIdx = -1;
    int uIdx = -1, vIdx = -1, sIdx = -1, tIdx = -1;
    int redIdx=-1,greenIdx=-1,blueIdx=-1,alphaIdx=-1;
    for (size_t i = 0; i < vertexElement->properties.size(); ++i) {
        const PlyProperty& p = vertexElement->properties[i];
        if (p.isList) {
            continue; // a list-typed property is never recognized as position/normal/uv
        }
        int idx = static_cast<int>(i);
        if (p.name == "x")
            xIdx = idx;
        else if (p.name == "y")
            yIdx = idx;
        else if (p.name == "z")
            zIdx = idx;
        else if (p.name == "nx")
            nxIdx = idx;
        else if (p.name == "ny")
            nyIdx = idx;
        else if (p.name == "nz")
            nzIdx = idx;
        else if (p.name == "u")
            uIdx = idx;
        else if (p.name == "v")
            vIdx = idx;
        else if (p.name == "s")
            sIdx = idx;
        else if (p.name == "t")
            tIdx = idx;
        else if (p.name == "red" || p.name == "r") redIdx=idx;
        else if (p.name == "green" || p.name == "g") greenIdx=idx;
        else if (p.name == "blue" || p.name == "b") blueIdx=idx;
        else if (p.name == "alpha" || p.name == "a") alphaIdx=idx;
    }
    if (xIdx < 0 || yIdx < 0 || zIdx < 0) {
        return ImportErrorCode::MalformedData;
    }
    const auto hasProperty = [&](const char* name) {
        for (const auto& property : vertexElement->properties) if (!property.isList && property.name == name) return true;
        return false;
    };
    const bool hasColors = redIdx>=0 && greenIdx>=0 && blueIdx>=0;
    bool hasNormal = (nxIdx >= 0 && nyIdx >= 0 && nzIdx >= 0);
    bool hasUvUv = (uIdx >= 0 && vIdx >= 0);
    bool hasUvSt = (!hasUvUv) && (sIdx >= 0 && tIdx >= 0);
    bool hasUv = hasUvUv || hasUvSt;
    int actualUIdx = hasUvUv ? uIdx : (hasUvSt ? sIdx : -1);
    int actualVIdx = hasUvUv ? vIdx : (hasUvSt ? tIdx : -1);

    bool hasFace = (faceElement != nullptr && faceElement->count > 0);
    if (ascii && !hasFace && vertexElement->count > kTierBPointLimit)
        return ImportErrorCode::ResourceLimit;
    int listPropIdx = -1;
    if (faceElement != nullptr) {
        if (faceElement->count > faceLimit) {
            return ImportErrorCode::ResourceLimit;
        }
        if (hasFace) {
            for (size_t i = 0; i < faceElement->properties.size(); ++i) {
                const auto& p = faceElement->properties[i];
                if (p.isList && (p.name == "vertex_indices" || p.name == "vertex_index")) {
                    if (listPropIdx >= 0) {
                        return ImportErrorCode::MalformedData; // more than one -- ambiguous
                    }
                    listPropIdx = static_cast<int>(i);
                }
            }
            if (listPropIdx < 0) {
                return ImportErrorCode::MalformedData;
            }
        }
    }

    for (const auto& element : header.elements) {
        bool isVertex = (&element == vertexElement);
        bool isFace = (faceElement != nullptr && &element == faceElement);
        if (isVertex || isFace) {
            continue;
        }
        if (element.count > skippedRecordLimit) {
            return ImportErrorCode::ResourceLimit;
        }
    }

    bool bigEndian = (header.format == PlyFormat::BinaryBigEndian);

    uint64_t cursor = header.bodyOffset; // binary path only
    if (!ascii && cursor > sourceSize)
    {
        return ImportErrorCode::MalformedData;
    }
    AsciiTokenizer tokenizer(sourcePlyBytes, static_cast<size_t>(header.bodyOffset)); // ASCII path only

    // readScalar/skipRawValue are the only format-dependent primitives --
    // every element/property-walking loop below calls through them and is
    // otherwise identical for binary and ASCII PLY. This is the "reader
    // abstraction" the ASCII slice added: sharing validation/triangulation
    // logic between dialects, rather than duplicating this whole function
    // the way STL/PLY/glTF each get their own request struct (that
    // precedent is about avoiding *cross-format* coupling; duplicating
    // *within* one format's two dialects is exactly what this avoids).
    std::function<std::optional<double>(PlyScalarType)> readScalar;
    std::function<bool(PlyScalarType)> skipRawValue;
    if (ascii) {
        // The declared PlyScalarType is deliberately ignored here -- ASCII
        // PLY values are plain decimal text with no fixed width, so there's
        // no per-type byte size to honor the way the binary path has. The
        // same downstream checks that already apply to binary values
        // (list-length caps, the vertex-count bound, isfinite checks on
        // positions/normals, index range checks) still validate every
        // value read this way.
        readScalar = [&](PlyScalarType) -> std::optional<double> { return tokenizer.NextNumber(); };
        skipRawValue = [&](PlyScalarType) -> bool { return tokenizer.NextToken().has_value(); };
    } else {
        readScalar = [&, bigEndian](PlyScalarType type) -> std::optional<double> {
            auto bytes = readSource(cursor, ScalarByteSize(type));
            if (!bytes)
            {
                return std::nullopt;
            }
            return ReadScalarAsDouble(type, bigEndian, *bytes);
        };
        skipRawValue = [&](PlyScalarType type) -> bool {
            return readSource(cursor, ScalarByteSize(type)).has_value();
        };
    }

    auto skipList = [&](const PlyProperty& prop) -> std::optional<ImportErrorCode> {
        auto countOpt = readScalar(prop.countType);
        if (!countOpt || *countOpt < 0.0) {
            return ImportErrorCode::MalformedData;
        }
        if (*countOpt > static_cast<double>(kMaxSkippedListLength)) {
            return ImportErrorCode::ResourceLimit;
        }
        uint64_t count = static_cast<uint64_t>(*countOpt);
        if (!ascii) {
            const uint64_t bytes=count*ScalarByteSize(prop.valueType);
            if (!readSource(cursor,bytes)) return ImportErrorCode::MalformedData;
            return std::nullopt;
        }
        for (uint64_t i = 0; i < count; ++i) {
            if (i % 4096 == 0 && batchSink && batchSink->Cancelled())
                return ImportErrorCode::Cancelled;
            if (!skipRawValue(prop.valueType)) {
                return ImportErrorCode::MalformedData;
            }
        }
        return std::nullopt;
    };

    if (!ascii)
    {
        if (!hasFace && vertexElement->count > kTierAPoints)
            return ImportErrorCode::ResourceLimit;
        SceneMetadata scene{};
        scene.format = SourceFormatId::Ply;
        scene.meshCount = hasFace ? 1 : 0;
        BoundedChunkWriter writer(destination, generationId, maxChunkCount, scene, batchSink);
        const uint64_t room = destination.size() > kSectionHeaderSize + kChunkDescriptorSize
                                  ? destination.size() - kSectionHeaderSize - kChunkDescriptorSize
                                  : 0;
        const bool preview=batchSink && batchSink->Preview();
        const bool compactPointLayout = !hasFace && !hasNormal && !hasUv && !hasColors;
        const uint64_t triangleBytes=3*sizeof(VertexPositionNormalUv0TangentColorF32)+3*sizeof(uint32_t);
        uint32_t chunkTriangles = preview ? 1 : uint32_t(std::min<uint64_t>(kChunkTriangles, room / triangleBytes));
        // Keep an odd fan capacity when possible so a large polygon can be
        // resumed at a non-zero fan offset; this also continuously exercises
        // the exact source-range replay path instead of only face boundaries.
        if (!preview && chunkTriangles>1 && !(chunkTriangles&1)) --chunkTriangles;
        const uint64_t pointBytes = compactPointLayout ? sizeof(VertexPositionOnlyF32)
                                                       : sizeof(VertexPositionNormalUv0TangentColorF32);
        // Keep colored/full-layout point regions independently admissible
        // under the 96 MiB pressure-smoke target after the immutable renderer
        // and coarse reserves are charged. The validator's 16 MiB ceiling is
        // an absolute limit, not a desirable streaming granularity.
        const uint64_t pointRoom = (std::min)(room, 8ull * 1024 * 1024);
        const uint32_t chunkPoints = preview ? 3 : uint32_t(std::min<uint64_t>(kChunkPoints, pointRoom / pointBytes));
        const auto previewVertices=PreviewOffsets(vertexElement->count,3);
        const auto previewFaces=PreviewOffsets(faceElement ? faceElement->count : 0);
        if ((hasFace && !chunkTriangles) || (!hasFace && !chunkPoints))
            return ImportErrorCode::ResourceLimit;
        std::vector<VertexPositionNormalUv0TangentColorF32> mesh;
        std::vector<VertexPositionNormalUv0TangentColorF32> points;
        std::vector<VertexPositionOnlyF32> positionOnlyPoints;
        std::vector<uint32_t> indices;
        std::unordered_map<uint64_t, uint32_t> remap;
        std::vector<uint64_t> checkpoints;
        if (vertexElement->count / 256 * 8 > TierAScratchLimit() / 4)
            return ImportErrorCode::ResourceLimit;
        uint64_t vertexStart = 0, vertexStride = 0, sourceFirst = 0, sourceEnd = 0, totalTriangles = 0;
        uint32_t sourceElementOffset=0;
        bool vertexSeen = false, haveOrigin = false;
        double origin[3]{};
        bool havePrecomputedPointBounds=false;
        float precomputedPointMin[3]{},precomputedPointMax[3]{};
        std::array<uint64_t, 64> vertexPropertyOffsets{};
        for (const auto& property : vertexElement->properties)
        {
            if (property.isList)
            {
                vertexStride = 0;
                break;
            }
            vertexPropertyOffsets[&property - vertexElement->properties.data()] = vertexStride;
            vertexStride += ScalarByteSize(property.valueType);
        }
        std::array<int,12> fixedUsedProperties{};
        size_t fixedUsedPropertyCount=0;
        for (const int index:{xIdx,yIdx,zIdx,nxIdx,nyIdx,nzIdx,actualUIdx,actualVIdx,
                              redIdx,greenIdx,blueIdx,alphaIdx}) {
            if (index<0 || std::find(fixedUsedProperties.begin(),
                fixedUsedProperties.begin()+fixedUsedPropertyCount,index)!=
                fixedUsedProperties.begin()+fixedUsedPropertyCount) continue;
            fixedUsedProperties[fixedUsedPropertyCount++]=index;
        }
        auto skipRecord = [&](const PlyElement& element) -> std::optional<ImportErrorCode> {
            for (const auto& property : element.properties)
            {
                if (property.isList)
                {
                    if (auto error = skipList(property))
                        return error;
                }
                else if (!skipRawValue(property.valueType))
                    return ImportErrorCode::MalformedData;
            }
            return std::nullopt;
        };
        auto finishVertex = [&](const double* values, double* position,
                                VertexPositionNormalUv0TangentColorF32& vertex) -> std::optional<ImportErrorCode> {
            position[0] = values[xIdx];
            position[1] = values[yIdx];
            position[2] = values[zIdx];
            for (unsigned axis = 0; axis < 3; ++axis)
                if (!std::isfinite(position[axis]))
                    return ImportErrorCode::MalformedData;
            vertex.nx = 0;
            vertex.ny = 0;
            vertex.nz = 1;
            vertex.tx = 1; vertex.tw = 1;
            vertex.r = vertex.g = vertex.b = vertex.a = 1;
            if (hasNormal)
            {
                const double length =
                    std::sqrt(values[nxIdx] * values[nxIdx] + values[nyIdx] * values[nyIdx] +
                              values[nzIdx] * values[nzIdx]);
                if (std::isfinite(length) && length > 1e-12)
                {
                    vertex.nx = float(values[nxIdx] / length);
                    vertex.ny = float(values[nyIdx] / length);
                    vertex.nz = float(values[nzIdx] / length);
                }
            }
            if (hasUv)
            {
                vertex.u = float(values[actualUIdx]);
                vertex.v = float(values[actualVIdx]);
            }
            if (hasColors) {
                vertex.r=NormalizeColor(values[redIdx],vertexElement->properties[redIdx].valueType);
                vertex.g=NormalizeColor(values[greenIdx],vertexElement->properties[greenIdx].valueType);
                vertex.b=NormalizeColor(values[blueIdx],vertexElement->properties[blueIdx].valueType);
                if (alphaIdx>=0) vertex.a=NormalizeColor(values[alphaIdx],vertexElement->properties[alphaIdx].valueType);
            }
            return std::nullopt;
        };
        auto decodeFixedVertex = [&](std::span<const std::byte> record, double* position,
                                     VertexPositionNormalUv0TangentColorF32& vertex) -> std::optional<ImportErrorCode> {
            double values[64]{};
            // Unknown fixed-width properties are skipped by advancing the
            // record, as the PLY contract requires. Decoding every padding or
            // application-specific scalar made wide point records needlessly
            // expensive on the complete scan.
            for (size_t used=0;used<fixedUsedPropertyCount;++used) {
                const int index=fixedUsedProperties[used];
                const auto& property=vertexElement->properties[size_t(index)];
                const size_t size=ScalarByteSize(property.valueType);
                values[index]=ReadScalarAsDouble(property.valueType,bigEndian,
                    record.subspan(size_t(vertexPropertyOffsets[size_t(index)]),size));
            }
            return finishVertex(values,position,vertex);
        };
        auto readVertex = [&](double* position,
                              VertexPositionNormalUv0TangentColorF32& vertex) -> std::optional<ImportErrorCode> {
            if (vertexStride) {
                auto record = readSource(cursor, vertexStride);
                if (!record)
                    return ImportErrorCode::MalformedData;
                return decodeFixedVertex(*record,position,vertex);
            } else {
                double values[64]{};
                for (size_t i = 0; i < vertexElement->properties.size(); ++i) {
                    const auto& property = vertexElement->properties[i];
                    if (property.isList) {
                        if (auto error = skipList(property))
                            return error;
                    } else {
                        auto value = readScalar(property.valueType);
                        if (!value)
                            return ImportErrorCode::MalformedData;
                        values[i] = *value;
                    }
                }
                return finishVertex(values,position,vertex);
            }
        };
        auto flush = [&]() -> std::optional<ImportErrorCode> {
            if (mesh.empty() && points.empty() && positionOnlyPoints.empty())
                return std::nullopt;
            ChunkDescriptor d{};
            d.chunkId = writer.NextId();
            d.topology = hasFace ? ChunkTopology::TriangleList : ChunkTopology::PointList;
            d.meshId = hasFace ? 1 : 0;
            d.vertexLayoutId = uint32_t(compactPointLayout ? VertexLayoutId::PositionOnly_F32
                                                           : VertexLayoutId::PositionNormalUv0TangentColor_F32);
            d.vertexCount = uint32_t(hasFace ? mesh.size()
                                             : compactPointLayout ? positionOnlyPoints.size() : points.size());
            d.indexCount = uint32_t(indices.size());
            d.sourceRangeOffset = sourceFirst;
            d.sourceRangeLength = sourceEnd - sourceFirst;
            d.sourceElementOffset=sourceElementOffset;
            std::memcpy(d.origin, origin, sizeof(origin));
            d.geometryFlags = hasFace && hasUv ? kGeometryHasUv0 : 0;
            if (hasColors)
                d.geometryFlags |= kGeometryHasColors;
            if (hasFace && !hasNormal)
            {
                std::vector<Vec3> accum(mesh.size());
                for (size_t i = 0; i < indices.size(); i += 3)
                {
                    const auto& a = mesh[indices[i]];
                    const auto& b = mesh[indices[i + 1]];
                    const auto& c = mesh[indices[i + 2]];
                    const auto n = Cross(Vec3{b.px - a.px, b.py - a.py, b.pz - a.pz},
                                         Vec3{c.px - a.px, c.py - a.py, c.pz - a.pz});
                    for (unsigned j = 0; j < 3; ++j)
                        accum[indices[i + j]] = accum[indices[i + j]] + n;
                }
                for (size_t i = 0; i < mesh.size(); ++i)
                {
                    const float length = std::sqrt(Dot(accum[i], accum[i]));
                    if (length > 1e-12f && std::isfinite(length))
                    {
                        mesh[i].nx = accum[i].x / length;
                        mesh[i].ny = accum[i].y / length;
                        mesh[i].nz = accum[i].z / length;
                    }
                }
            }
            std::span<const std::byte> bytes;
            if (hasFace) {
                bytes = ChunkBytes(mesh);
            } else if (compactPointLayout) {
                bytes = ChunkBytes(positionOnlyPoints);
            } else {
                bytes = ChunkBytes(points);
            }
            if (havePrecomputedPointBounds) {
                std::memcpy(d.localMin,precomputedPointMin,sizeof(d.localMin));
                std::memcpy(d.localMax,precomputedPointMax,sizeof(d.localMax));
                d.boundsState=BoundsState::Verified;
            } else if (!SetLocalBounds(d, bytes)) {
                return ImportErrorCode::MalformedData;
            }
            if (!writer.Add(d, bytes, ChunkBytes(indices)))
                return writer.Error();
            mesh.clear();
            points.clear();
            positionOnlyPoints.clear();
            indices.clear();
            remap.clear();
            haveOrigin = false;
            havePrecomputedPointBounds=false;
            return std::nullopt;
        };
        const auto* requested = batchSink ? batchSink->RequestedSource() : nullptr;
        if (requested && (requested->sourceRangeOffset > sourceSize || requested->sourceRangeLength > sourceSize - requested->sourceRangeOffset))
            return ImportErrorCode::MalformedData;
        for (const auto& element : header.elements)
        {
            if (&element == vertexElement)
            {
                vertexStart = cursor;
                vertexSeen = true;
            }
            if (&element == faceElement && hasFace && !vertexSeen)
                return ImportErrorCode::UnsupportedEncoding;
            if ((preview || requested) && &element==vertexElement && hasFace && vertexStride) {
                const uint64_t bytes=element.count*vertexStride;
                if (cursor>sourceSize || bytes>sourceSize-cursor) return ImportErrorCode::MalformedData;
                cursor+=bytes; continue;
            }
            if (requested && ((&element == faceElement && hasFace) || (&element == vertexElement && !hasFace))) cursor = requested->sourceRangeOffset;
            if (&element == vertexElement && !hasFace && vertexStride && !preview)
            {
                uint64_t records=element.count;
                if (requested) {
                    const uint64_t vertexBytes=element.count*vertexStride;
                    if (requested->sourceRangeOffset < vertexStart ||
                        requested->sourceRangeOffset-vertexStart > vertexBytes ||
                        (requested->sourceRangeOffset-vertexStart)%vertexStride ||
                        requested->sourceRangeLength%vertexStride ||
                        requested->sourceRangeLength > vertexBytes-(requested->sourceRangeOffset-vertexStart))
                        return ImportErrorCode::MalformedData;
                    records=requested->sourceRangeLength/vertexStride;
                }
                for (uint64_t first=0;first<records;)
                {
                    if (mappedSource && !mappedSource->IsUnchanged())
                        return ImportErrorCode::FileChanged;
                    if (batchSink && batchSink->Cancelled())
                        return ImportErrorCode::Cancelled;
                    const uint64_t count=(std::min)(uint64_t(chunkPoints),records-first);
                    const uint64_t start=cursor;
                    auto raw=readSource(cursor,count*vertexStride);
                    if (!raw) return ImportErrorCode::MalformedData;
                    double firstPosition[3];
                    VertexPositionNormalUv0TangentColorF32 firstVertex{};
                    if (auto error=decodeFixedVertex(raw->first(size_t(vertexStride)),firstPosition,firstVertex))
                        return *error;
                    std::memcpy(origin,firstPosition,sizeof(origin));
                    haveOrigin=true; sourceFirst=start; sourceEnd=cursor;
                    if (compactPointLayout && count>=16384) {
                        constexpr uint64_t kPointBlock=8192;
                        const size_t blocks=size_t((count+kPointBlock-1)/kPointBlock);
                        std::vector<std::array<float,6>> bounds(blocks);
                        std::atomic_bool valid=true;
                        positionOnlyPoints.resize(size_t(count));
                        concurrency::parallel_for(size_t(0),blocks,[&](size_t block) {
                            const uint64_t begin=uint64_t(block)*kPointBlock;
                            const uint64_t end=(std::min)(count,begin+kPointBlock);
                            std::array<float,6> local{};
                            for (uint64_t i=begin;i<end;++i) {
                                const auto record=raw->subspan(size_t(i*vertexStride),size_t(vertexStride));
                                const auto readCoordinate=[&](int index) {
                                    const auto& property=vertexElement->properties[size_t(index)];
                                    return ReadScalarAsDouble(property.valueType,bigEndian,
                                        record.subspan(size_t(vertexPropertyOffsets[size_t(index)]),
                                                       ScalarByteSize(property.valueType)));
                                };
                                const double position[]{readCoordinate(xIdx),readCoordinate(yIdx),readCoordinate(zIdx)};
                                if (!std::isfinite(position[0])||!std::isfinite(position[1])||!std::isfinite(position[2])) {
                                    valid.store(false,std::memory_order_relaxed);continue;
                                }
                                const float value[]{float(position[0]-origin[0]),float(position[1]-origin[1]),
                                                    float(position[2]-origin[2])};
                                positionOnlyPoints[size_t(i)]={value[0],value[1],value[2]};
                                for (unsigned axis=0;axis<3;++axis) {
                                    if (i==begin||value[axis]<local[axis]) local[axis]=value[axis];
                                    if (i==begin||value[axis]>local[axis+3]) local[axis+3]=value[axis];
                                }
                            }
                            bounds[block]=local;
                        });
                        if (!valid.load(std::memory_order_relaxed)) return ImportErrorCode::MalformedData;
                        for (size_t block=0;block<blocks;++block) {
                            for (unsigned axis=0;axis<3;++axis) {
                                if (!block||bounds[block][axis]<precomputedPointMin[axis])
                                    precomputedPointMin[axis]=bounds[block][axis];
                                if (!block||bounds[block][axis+3]>precomputedPointMax[axis])
                                    precomputedPointMax[axis]=bounds[block][axis+3];
                            }
                        }
                        havePrecomputedPointBounds=true;
                    } else {
                        for (uint64_t i=0;i<count;++i)
                        {
                            double position[3];
                            VertexPositionNormalUv0TangentColorF32 vertex{};
                            if (i==0) {
                                std::memcpy(position,firstPosition,sizeof(position));
                                vertex=firstVertex;
                            } else if (auto error=decodeFixedVertex(
                                          raw->subspan(size_t(i*vertexStride),size_t(vertexStride)),position,vertex)) {
                                return *error;
                            }
                            const float x=float(position[0]-origin[0]);
                            const float y=float(position[1]-origin[1]);
                            const float z=float(position[2]-origin[2]);
                            const float local[]{x,y,z};
                            for (unsigned axis=0;axis<3;++axis) {
                                if (!i||local[axis]<precomputedPointMin[axis]) precomputedPointMin[axis]=local[axis];
                                if (!i||local[axis]>precomputedPointMax[axis]) precomputedPointMax[axis]=local[axis];
                            }
                            havePrecomputedPointBounds=true;
                            if (compactPointLayout)
                                positionOnlyPoints.push_back({x,y,z});
                            else {
                                vertex.px=x; vertex.py=y; vertex.pz=z;
                                points.push_back(vertex);
                            }
                        }
                    }
                    if (auto error=flush()) return *error;
                    first+=count;
                }
                if (requested) break;
                continue;
            }
            for (uint64_t record = 0; record < element.count; ++record)
            {
                if (requested && ((&element == faceElement && hasFace) || (&element == vertexElement && !hasFace))
                    && cursor >= requested->sourceRangeOffset + requested->sourceRangeLength) break;
                if (preview && &element==vertexElement && !hasFace && vertexStride) {
                    const auto next=std::lower_bound(previewVertices.begin(),previewVertices.end(),record);
                    if (next==previewVertices.end()) break;
                    record=*next; cursor=vertexStart+record*vertexStride;
                }
                if (preview && &element==faceElement && hasFace) {
                    if (!previewFaces.empty() && record>previewFaces.back()) break;
                    if (!std::binary_search(previewFaces.begin(),previewFaces.end(),record)) {
                        if (auto error=skipRecord(element)) return *error;
                        continue;
                    }
                }
                if (record % 4096 == 0 && mappedSource && !mappedSource->IsUnchanged())
                    return ImportErrorCode::FileChanged;
                if (record % 1024 == 0 && batchSink && batchSink->Cancelled())
                    return ImportErrorCode::Cancelled;
                const uint64_t start = cursor;
                if (&element == vertexElement)
                {
                    if (!vertexStride && record % 256 == 0)
                        checkpoints.push_back(cursor);
                    if ((preview || requested) && hasFace) {
                        if (auto error=skipRecord(element)) return *error;
                        continue;
                    }
                    double position[3];
                    VertexPositionNormalUv0TangentColorF32 vertex{};
                    if (auto error = readVertex(position, vertex))
                        return *error;
                    if (!hasFace)
                    {
                        const size_t pointCount = compactPointLayout ? positionOnlyPoints.size() : points.size();
                        if (pointCount == chunkPoints)
                            if (auto error = flush())
                                return *error;
                        if (!haveOrigin)
                        {
                            std::memcpy(origin, position, sizeof(origin));
                            sourceFirst = start;
                            haveOrigin = true;
                        }
                        const float x=float(position[0] - origin[0]);
                        const float y=float(position[1] - origin[1]);
                        const float z=float(position[2] - origin[2]);
                        if (compactPointLayout) {
                            positionOnlyPoints.push_back({x, y, z});
                        } else {
                            VertexPositionNormalUv0TangentColorF32 point=vertex;
                            point.px=x; point.py=y; point.pz=z;
                            points.push_back(point);
                        }
                        sourceEnd = cursor;
                    }
                }
                else if (&element == faceElement && hasFace)
                {
                    uint64_t polygon[255]{};
                    size_t count = 0;
                    bool valid = true;
                    for (size_t pi = 0; pi < element.properties.size(); ++pi)
                    {
                        const auto& property = element.properties[pi];
                        if (int(pi) != listPropIdx)
                        {
                            if (property.isList)
                            {
                                if (auto error = skipList(property))
                                    return *error;
                            }
                            else if (!skipRawValue(property.valueType))
                                return ImportErrorCode::MalformedData;
                            continue;
                        }
                        auto size = readScalar(property.countType);
                        if (!size || *size < 0 || std::floor(*size) != *size)
                            return ImportErrorCode::MalformedData;
                        if (*size > 255)
                            return ImportErrorCode::ResourceLimit;
                        count = size_t(*size);
                        for (size_t i = 0; i < count; ++i)
                        {
                            auto index = readScalar(property.valueType);
                            if (!index || !std::isfinite(*index) || *index < 0 ||
                                std::floor(*index) != *index)
                                return ImportErrorCode::MalformedData;
                            if (*index >= double(vertexElement->count))
                                valid = false;
                            else
                                polygon[i] = uint64_t(*index);
                        }
                    }
                    const uint64_t end = cursor;
                    if (!valid || count < 3)
                        continue;
                    if (count - 2 > kTierATriangles - totalTriangles)
                        return ImportErrorCode::ResourceLimit;
                    totalTriangles += count - 2;
                    const size_t firstTriangle=requested && start==requested->sourceRangeOffset ? 1+requested->sourceElementOffset : 1;
                    for (size_t triangle = firstTriangle; triangle + 1 < count; ++triangle)
                    {
                        if (requested && indices.size()>=requested->indexCount) break;
                        if (indices.size() / 3 == chunkTriangles)
                            if (auto error = flush())
                                return *error;
                        const uint64_t sourceIndices[]{polygon[0], polygon[triangle], polygon[triangle + 1]};
                        for (const auto sourceIndex : sourceIndices)
                        {
                            auto found = remap.find(sourceIndex);
                            if (found != remap.end())
                            {
                                indices.push_back(found->second);
                                continue;
                            }
                            if (vertexStride)
                                cursor = vertexStart + sourceIndex * vertexStride;
                            else
                            {
                                cursor = checkpoints[size_t(sourceIndex / 256)];
                                for (uint64_t skipped = sourceIndex / 256 * 256; skipped < sourceIndex;
                                     ++skipped)
                                    if (auto error = skipRecord(*vertexElement))
                                        return *error;
                            }
                            double position[3];
                            VertexPositionNormalUv0TangentColorF32 vertex{};
                            if (auto error = readVertex(position, vertex))
                                return *error;
                            cursor = end;
                            if (!haveOrigin)
                            {
                                std::memcpy(origin, position, sizeof(origin));
                                sourceFirst = start; sourceElementOffset=uint32_t(triangle-1);
                                haveOrigin = true;
                            }
                            vertex.px = float(position[0] - origin[0]);
                            vertex.py = float(position[1] - origin[1]);
                            vertex.pz = float(position[2] - origin[2]);
                            const uint32_t local = uint32_t(mesh.size());
                            remap.emplace(sourceIndex, local);
                            mesh.push_back(vertex);
                            indices.push_back(local);
                        }
                        sourceEnd = end;
                    }
                }
                else if (auto error = skipRecord(element))
                    return *error;
            }
            if ((preview || requested) && ((&element==faceElement && hasFace) || (&element==vertexElement && !hasFace))) break;
        }
        if (auto error = flush())
            return *error;
        if (!writer.Count())
            return ImportErrorCode::EmptyGeometry;
        if (!writer.Complete()) return writer.Error();
        return PlyImportResult{writer.Count(), writer.Length()};
    }
    const uint64_t tierBScratch = TierBScratchLimit();
    const auto baseVertexBytes = CheckedMultiply(
        vertexElement->count, uint64_t(sizeof(VertexPositionNormalUv0TangentColorF32)));
    // A point cloud retains only its vertex array. Meshes also retain a
    // triangulated index array (and may temporarily accumulate normals), so
    // reserve half of the Tier B scratch budget for that bounded expansion.
    const uint64_t vertexBudget = hasFace ? tierBScratch / 2 : tierBScratch;
    if (!baseVertexBytes || *baseVertexBytes > vertexBudget)
        return ImportErrorCode::ResourceLimit;
    std::vector<VertexPositionNormalUv0TangentColorF32> meshVertices;
    std::vector<VertexPositionNormalUv0TangentColorF32> pointVertices;
    std::vector<uint32_t> meshIndices;
    uint64_t triangleTotal = 0;
    double clusterOrigin[3]{}; bool haveOrigin = false;

    if (hasFace) {
        meshVertices.reserve(static_cast<size_t>(vertexElement->count));
        meshIndices.reserve(static_cast<size_t>(vertexElement->count) * 3);
    } else {
        pointVertices.reserve(static_cast<size_t>(vertexElement->count));
    }

    // Walk declared elements in header order -- never assume vertex precedes
    // face on disk, even though every real file does.
    for (const auto& element : header.elements) {
        bool isVertex = (&element == vertexElement);
        bool isFace = (faceElement != nullptr && &element == faceElement);

        for (uint64_t recordIndex = 0; recordIndex < element.count; ++recordIndex) {
            if ((recordIndex & 4095) == 0) {
                if (batchSink && batchSink->Cancelled())
                    return ImportErrorCode::Cancelled;
                if (mappedSource && !mappedSource->IsUnchanged())
                    return ImportErrorCode::FileChanged;
            }
            if (isVertex) {
                double x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0, u = 0, v = 0;
                double red=1,green=1,blue=1,alpha=1;
                for (size_t pi = 0; pi < element.properties.size(); ++pi) {
                    const PlyProperty& prop = element.properties[pi];
                    if (prop.isList) {
                        auto err = skipList(prop);
                        if (err) {
                            return *err;
                        }
                        continue;
                    }
                    auto valOpt = readScalar(prop.valueType);
                    if (!valOpt) {
                        return ImportErrorCode::MalformedData;
                    }
                    int idx = static_cast<int>(pi);
                    if (idx == xIdx)
                        x = *valOpt;
                    else if (idx == yIdx)
                        y = *valOpt;
                    else if (idx == zIdx)
                        z = *valOpt;
                    else if (hasNormal && idx == nxIdx)
                        nx = *valOpt;
                    else if (hasNormal && idx == nyIdx)
                        ny = *valOpt;
                    else if (hasNormal && idx == nzIdx)
                        nz = *valOpt;
                    else if (hasUv && idx == actualUIdx)
                        u = *valOpt;
                    else if (hasUv && idx == actualVIdx)
                        v = *valOpt;
                    else if (hasColors && idx==redIdx) red=*valOpt;
                    else if (hasColors && idx==greenIdx) green=*valOpt;
                    else if (hasColors && idx==blueIdx) blue=*valOpt;
                    else if (hasColors && idx==alphaIdx) alpha=*valOpt;
                    // else: a recognized-but-unused (color, etc.) or wholly
                    // unrecognized scalar -- read for cursor correctness,
                    // discarded from wire output.
                }

                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return ImportErrorCode::MalformedData;
                if (!haveOrigin) { clusterOrigin[0]=x; clusterOrigin[1]=y; clusterOrigin[2]=z; haveOrigin=true; }
                float px = static_cast<float>(x-clusterOrigin[0]), py = static_cast<float>(y-clusterOrigin[1]), pz = static_cast<float>(z-clusterOrigin[2]);
                if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) {
                    // Fails the whole file, unlike StlAdapter.cpp's per-facet
                    // drop: PLY vertices are shared/indexed across faces, so
                    // dropping just this one would require renumbering every
                    // face that referenced it.
                    return ImportErrorCode::MalformedData;
                }

                float pnx = 0.0f, pny = 0.0f, pnz = 1.0f;
                if (hasNormal) {
                    float rnx = static_cast<float>(nx), rny = static_cast<float>(ny),
                          rnz = static_cast<float>(nz);
                    bool finite = std::isfinite(rnx) && std::isfinite(rny) && std::isfinite(rnz);
                    float lenSq = rnx * rnx + rny * rny + rnz * rnz;
                    if (finite && lenSq > 1e-12f) {
                        float invLen = 1.0f / std::sqrt(lenSq);
                        pnx = rnx * invLen;
                        pny = rny * invLen;
                        pnz = rnz * invLen;
                    }
                    // else: non-finite or zero supplied normal -- per-vertex
                    // fallback to (0,0,1) already set above, file is not
                    // failed (topology-safe, same tolerance spirit as STL).
                }

                if (hasFace) {
                    VertexPositionNormalUv0TangentColorF32 vertex{};
                    vertex.px = px;
                    vertex.py = py;
                    vertex.pz = pz;
                    vertex.nx = pnx;
                    vertex.ny = pny;
                    vertex.nz = pnz;
                    vertex.u = hasUv ? static_cast<float>(u) : 0.0f;
                    vertex.v = hasUv ? static_cast<float>(v) : 0.0f;
                    vertex.tx=1;vertex.tw=1;vertex.r=vertex.g=vertex.b=vertex.a=1;
                    if (hasColors) {
                        vertex.r=NormalizeColor(red,vertexElement->properties[redIdx].valueType);
                        vertex.g=NormalizeColor(green,vertexElement->properties[greenIdx].valueType);
                        vertex.b=NormalizeColor(blue,vertexElement->properties[blueIdx].valueType);
                        if (alphaIdx>=0) vertex.a=NormalizeColor(alpha,vertexElement->properties[alphaIdx].valueType);
                    }
                    meshVertices.push_back(vertex);
                } else {
                    VertexPositionNormalUv0TangentColorF32 vertex{};
                    vertex.px=px;vertex.py=py;vertex.pz=pz;
                    vertex.nx=pnx;vertex.ny=pny;vertex.nz=pnz;
                    vertex.u=hasUv?float(u):0;vertex.v=hasUv?float(v):0;
                    vertex.tx=1;vertex.tw=1;vertex.r=vertex.g=vertex.b=vertex.a=1;
                    if (hasColors) {
                        vertex.r=NormalizeColor(red,vertexElement->properties[redIdx].valueType);
                        vertex.g=NormalizeColor(green,vertexElement->properties[greenIdx].valueType);
                        vertex.b=NormalizeColor(blue,vertexElement->properties[blueIdx].valueType);
                        if (alphaIdx>=0) vertex.a=NormalizeColor(alpha,vertexElement->properties[alphaIdx].valueType);
                    }
                    pointVertices.push_back(vertex);
                }
            } else if (isFace) {
                std::vector<uint64_t> faceIndices;
                std::optional<ImportErrorCode> hardError;

                for (size_t pi = 0; pi < element.properties.size() && !hardError; ++pi) {
                    const PlyProperty& prop = element.properties[pi];
                    if (!prop.isList) {
                        auto valOpt = readScalar(prop.valueType);
                        if (!valOpt) {
                            hardError = ImportErrorCode::MalformedData;
                        }
                        continue;
                    }

                    auto countOpt = readScalar(prop.countType);
                    if (!countOpt || *countOpt < 0.0) {
                        hardError = ImportErrorCode::MalformedData;
                        continue;
                    }

                    if (static_cast<int>(pi) == listPropIdx) {
                        if (*countOpt > static_cast<double>(kMaxPolygonVerticesPerFace)) {
                            hardError = ImportErrorCode::ResourceLimit;
                            continue;
                        }
                        uint64_t count = static_cast<uint64_t>(*countOpt);
                        faceIndices.clear();
                        faceIndices.reserve(static_cast<size_t>(count));
                        for (uint64_t k = 0; k < count; ++k) {
                            auto idxOpt = readScalar(prop.valueType);
                            if (!idxOpt || *idxOpt < 0.0) {
                                hardError = ImportErrorCode::MalformedData;
                                break;
                            }
                            faceIndices.push_back(static_cast<uint64_t>(*idxOpt));
                        }
                    } else {
                        if (*countOpt > static_cast<double>(kMaxSkippedListLength)) {
                            hardError = ImportErrorCode::ResourceLimit;
                            continue;
                        }
                        uint64_t count = static_cast<uint64_t>(*countOpt);
                        for (uint64_t k = 0; k < count; ++k) {
                            if (!skipRawValue(prop.valueType)) {
                                hardError = ImportErrorCode::MalformedData;
                                break;
                            }
                        }
                    }
                }
                if (hardError) {
                    return *hardError;
                }

                if (faceIndices.size() < 3) {
                    continue; // degenerate face dropped
                }
                bool outOfRange = false;
                for (uint64_t idx : faceIndices) {
                    if (idx >= vertexElement->count) {
                        outOfRange = true;
                        break;
                    }
                }
                if (outOfRange) {
                    continue; // dropped, rest of the file keeps going
                }

                uint64_t triCount = static_cast<uint64_t>(faceIndices.size()) - 2;
                auto newTotalOpt = CheckedAdd(triangleTotal, triCount);
                if (!newTotalOpt || *newTotalOpt > kTierBTriangleLimit) {
                    return ImportErrorCode::ResourceLimit;
                }
                const auto indexBytes = CheckedMultiply(*newTotalOpt, uint64_t(3 * sizeof(uint32_t)));
                if (!indexBytes || *indexBytes > tierBScratch
                    || *baseVertexBytes > tierBScratch - *indexBytes)
                    return ImportErrorCode::ResourceLimit;
                triangleTotal = *newTotalOpt;

                for (size_t i = 1; i + 1 < faceIndices.size(); ++i) {
                    meshIndices.push_back(static_cast<uint32_t>(faceIndices[0]));
                    meshIndices.push_back(static_cast<uint32_t>(faceIndices[i]));
                    meshIndices.push_back(static_cast<uint32_t>(faceIndices[i + 1]));
                }
            } else {
                // An element this adapter doesn't recognize: walk its
                // declared properties purely to advance the cursor, storing
                // nothing -- "unknown elements/properties skipped within
                // bounds," per the design doc.
                for (const auto& prop : element.properties) {
                    if (prop.isList) {
                        auto err = skipList(prop);
                        if (err) {
                            return *err;
                        }
                    } else {
                        auto valOpt = readScalar(prop.valueType);
                        if (!valOpt) {
                            return ImportErrorCode::MalformedData;
                        }
                    }
                }
            }
        }
    }

    if (hasFace) {
        if (meshIndices.empty()) {
            return ImportErrorCode::EmptyGeometry; // every face dropped
        }
        if (!hasNormal) {
            const auto normalBytes = CheckedMultiply(uint64_t(meshVertices.size()), uint64_t(sizeof(Vec3)));
            const uint64_t retainedBytes = uint64_t(meshVertices.size()) * sizeof(meshVertices[0])
                + uint64_t(meshIndices.size()) * sizeof(meshIndices[0]);
            if (!normalBytes || retainedBytes > tierBScratch
                || *normalBytes > tierBScratch - retainedBytes)
                return ImportErrorCode::ResourceLimit;
            std::vector<Vec3> accum(meshVertices.size(), Vec3{});
            for (size_t i = 0; i + 2 < meshIndices.size(); i += 3) {
                uint32_t ia = meshIndices[i], ib = meshIndices[i + 1], ic = meshIndices[i + 2];
                Vec3 pa{ meshVertices[ia].px, meshVertices[ia].py, meshVertices[ia].pz };
                Vec3 pb{ meshVertices[ib].px, meshVertices[ib].py, meshVertices[ib].pz };
                Vec3 pc{ meshVertices[ic].px, meshVertices[ic].py, meshVertices[ic].pz };
                Vec3 faceNormal = Cross(pb - pa, pc - pa);
                if (Dot(faceNormal, faceNormal) <= 1e-12f) {
                    continue; // degenerate triangle contributes nothing
                }
                accum[ia] = accum[ia] + faceNormal;
                accum[ib] = accum[ib] + faceNormal;
                accum[ic] = accum[ic] + faceNormal;
            }
            for (size_t i = 0; i < meshVertices.size(); ++i) {
                float lenSq = Dot(accum[i], accum[i]);
                if (lenSq <= 1e-12f) {
                    meshVertices[i].nx = 0.0f;
                    meshVertices[i].ny = 0.0f;
                    meshVertices[i].nz = 1.0f;
                } else {
                    float invLen = 1.0f / std::sqrt(lenSq);
                    meshVertices[i].nx = accum[i].x * invLen;
                    meshVertices[i].ny = accum[i].y * invLen;
                    meshVertices[i].nz = accum[i].z * invLen;
                }
            }
        }
    } else {
        if (pointVertices.empty()) {
            return ImportErrorCode::EmptyGeometry;
        }
    }

    SceneMetadata scene{};
    scene.format = SourceFormatId::AsciiPly;
    scene.meshCount = hasFace ? 1 : 0;
    BoundedChunkWriter writer(destination, generationId, maxChunkCount, scene, batchSink);
    const uint64_t room = destination.size() > kSectionHeaderSize + kChunkDescriptorSize
        ? destination.size() - kSectionHeaderSize - kChunkDescriptorSize : 0;
    if (hasFace) {
        const uint32_t trianglesPerChunk = uint32_t((std::min<uint64_t>)(
            kChunkTriangles, room / (3 * sizeof(VertexPositionNormalUv0TangentColorF32)
                                      + 3 * sizeof(uint32_t))));
        if (!trianglesPerChunk)
            return ImportErrorCode::ResourceLimit;
        std::vector<VertexPositionNormalUv0TangentColorF32> vertices;
        std::vector<uint32_t> indices;
        vertices.reserve(size_t(trianglesPerChunk) * 3);
        indices.reserve(size_t(trianglesPerChunk) * 3);
        const uint64_t triangleCount = meshIndices.size() / 3;
        for (uint64_t first = 0; first < triangleCount; first += trianglesPerChunk) {
            if (batchSink && batchSink->Cancelled())
                return ImportErrorCode::Cancelled;
            const uint32_t count = uint32_t((std::min<uint64_t>)(trianglesPerChunk,
                                                                 triangleCount - first));
            vertices.clear();
            indices.clear();
            for (uint32_t triangle = 0; triangle < count; ++triangle) {
                for (uint32_t corner = 0; corner < 3; ++corner) {
                    vertices.push_back(meshVertices[meshIndices[size_t(first + triangle) * 3 + corner]]);
                    indices.push_back(uint32_t(indices.size()));
                }
            }
            ChunkDescriptor descriptor{};
            descriptor.chunkId = writer.NextId();
            descriptor.meshId = 1;
            descriptor.sourceRangeLength = sourceSize;
            descriptor.topology = ChunkTopology::TriangleList;
            descriptor.vertexLayoutId = uint32_t(VertexLayoutId::PositionNormalUv0TangentColor_F32);
            descriptor.vertexCount = uint32_t(vertices.size());
            descriptor.indexCount = uint32_t(indices.size());
            std::memcpy(descriptor.origin, clusterOrigin, sizeof(clusterOrigin));
            descriptor.geometryFlags = kGeometryDeindexed | (hasUv ? kGeometryHasUv0 : 0)
                | (hasColors ? kGeometryHasColors : 0);
            if (!SetLocalBounds(descriptor, std::as_bytes(std::span(vertices))))
                return ImportErrorCode::MalformedData;
            if (!writer.Add(descriptor, ChunkBytes(vertices), ChunkBytes(indices)))
                return writer.Error();
        }
    } else {
        const uint32_t pointsPerChunk = uint32_t((std::min<uint64_t>)(
            kChunkPoints, room / sizeof(VertexPositionNormalUv0TangentColorF32)));
        if (!pointsPerChunk)
            return ImportErrorCode::ResourceLimit;
        for (uint64_t first = 0; first < pointVertices.size(); first += pointsPerChunk) {
            if (batchSink && batchSink->Cancelled())
                return ImportErrorCode::Cancelled;
            const uint32_t count = uint32_t((std::min<uint64_t>)(pointsPerChunk,
                                                                 pointVertices.size() - first));
            auto points = std::span(pointVertices).subspan(size_t(first), count);
            ChunkDescriptor descriptor{};
            descriptor.chunkId = writer.NextId();
            descriptor.sourceRangeLength = sourceSize;
            descriptor.topology = ChunkTopology::PointList;
            descriptor.vertexLayoutId = uint32_t(VertexLayoutId::PositionNormalUv0TangentColor_F32);
            descriptor.vertexCount = count;
            std::memcpy(descriptor.origin, clusterOrigin, sizeof(clusterOrigin));
            descriptor.geometryFlags = hasColors ? kGeometryHasColors : 0;
            const auto bytes = std::as_bytes(points);
            if (!SetLocalBounds(descriptor, bytes))
                return ImportErrorCode::MalformedData;
            if (!writer.Add(descriptor, bytes))
                return writer.Error();
        }
    }
    if (!writer.Complete())
        return writer.Error();
    return PlyImportResult{writer.Count(), writer.Length()};
}

} // namespace import_worker
