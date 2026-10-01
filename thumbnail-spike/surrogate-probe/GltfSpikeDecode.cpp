// SPIKE-8b (T03) throwaway compressed-glTF decode. See GltfSpikeDecode.h.

#include "GltfSpikeDecode.h"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <draco/compression/decode.h>
#include <draco/mesh/mesh.h>

#include <meshoptimizer.h>

#include <webp/decode.h>
#include <ktx.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace gltf_spike
{

namespace
{

using thumbnail_rasterizer::Triangle;
using thumbnail_rasterizer::Vec3;

constexpr std::uint64_t kMaxSourceBytes = 128ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxDecodedBytes = 512ull * 1024ull * 1024ull;
constexpr std::size_t kMaxTriangles = 250000;
constexpr std::size_t kMaxVertices = 4000000;

bool ReadFileBytes(const std::wstring& path, std::vector<std::byte>& out, std::uint64_t& size)
{
    std::ifstream stream(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!stream) return false;
    const std::streamoff length = stream.tellg();
    if (length <= 0) return false;
    size = static_cast<std::uint64_t>(length);
    if (size > kMaxSourceBytes) return false;
    out.resize(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return stream.good() || stream.gcount() == static_cast<std::streamsize>(out.size());
}

// Resolves bufferViews, transparently decompressing EXT_meshopt_compression
// views and caching the result so a view read twice is decoded once.
class Adapter
{
public:
    explicit Adapter(const fastgltf::Asset& asset)
        : asset_(asset)
    {
    }

    std::span<const std::byte> operator()(const fastgltf::Asset&, std::size_t bufferViewIndex) const
    {
        return Resolve(bufferViewIndex);
    }

    std::span<const std::byte> Resolve(std::size_t bufferViewIndex) const
    {
        if (bufferViewIndex >= asset_.bufferViews.size()) return {};
        const fastgltf::BufferView& view = asset_.bufferViews[bufferViewIndex];
        if (view.meshoptCompression) {
            auto cached = meshopt_.find(bufferViewIndex);
            if (cached == meshopt_.end()) {
                std::vector<std::byte> decoded;
                if (!DecodeMeshopt(view, *view.meshoptCompression, decoded)) {
                    error_ = "meshopt decode failed";
                    return {};
                }
                cached = meshopt_.emplace(bufferViewIndex, std::move(decoded)).first;
            }
            return { cached->second.data(), cached->second.size() };
        }
        const std::span<const std::byte> buffer = ResolveBuffer(view.bufferIndex);
        if (view.byteOffset > buffer.size() || view.byteLength > buffer.size() - view.byteOffset) return {};
        return buffer.subspan(view.byteOffset, view.byteLength);
    }

    bool UsedMeshopt() const { return !meshopt_.empty(); }
    const std::string& Error() const { return error_; }

private:
    std::span<const std::byte> ResolveBuffer(std::size_t bufferIndex) const
    {
        if (bufferIndex >= asset_.buffers.size()) return {};
        const fastgltf::DataSource& data = asset_.buffers[bufferIndex].data;
        if (const auto* array = std::get_if<fastgltf::sources::Array>(&data))
            return { array->bytes.data(), array->bytes.size() };
        if (const auto* vector = std::get_if<fastgltf::sources::Vector>(&data))
            return { vector->bytes.data(), vector->bytes.size() };
        if (const auto* view = std::get_if<fastgltf::sources::ByteView>(&data))
            return { view->bytes.data(), view->bytes.size() };
        return {};
    }

    bool DecodeMeshopt(const fastgltf::BufferView& view, const fastgltf::CompressedBufferView& compressed,
                       std::vector<std::byte>& decoded) const
    {
        const std::span<const std::byte> source = ResolveBuffer(compressed.bufferIndex);
        if (compressed.byteOffset > source.size()
            || compressed.byteLength > source.size() - compressed.byteOffset)
            return false;
        const std::uint64_t bytes = static_cast<std::uint64_t>(compressed.count) * compressed.byteStride;
        if (bytes == 0 || bytes != view.byteLength || bytes > kMaxDecodedBytes) return false;
        decoded.resize(static_cast<std::size_t>(bytes));
        const auto* input = reinterpret_cast<const unsigned char*>(source.data() + compressed.byteOffset);
        int result = -1;
        switch (compressed.mode) {
        case fastgltf::MeshoptCompressionMode::Attributes:
            result = meshopt_decodeVertexBuffer(decoded.data(), compressed.count, compressed.byteStride, input,
                                                compressed.byteLength);
            break;
        case fastgltf::MeshoptCompressionMode::Triangles:
            result = meshopt_decodeIndexBuffer(decoded.data(), compressed.count, compressed.byteStride, input,
                                               compressed.byteLength);
            break;
        case fastgltf::MeshoptCompressionMode::Indices:
            result = meshopt_decodeIndexSequence(decoded.data(), compressed.count, compressed.byteStride, input,
                                                 compressed.byteLength);
            break;
        }
        if (result != 0) return false;
        switch (compressed.filter) {
        case fastgltf::MeshoptCompressionFilter::None:
            break;
        case fastgltf::MeshoptCompressionFilter::Octahedral:
            meshopt_decodeFilterOct(decoded.data(), compressed.count, compressed.byteStride);
            break;
        case fastgltf::MeshoptCompressionFilter::Quaternion:
            meshopt_decodeFilterQuat(decoded.data(), compressed.count, compressed.byteStride);
            break;
        case fastgltf::MeshoptCompressionFilter::Exponential:
            meshopt_decodeFilterExp(decoded.data(), compressed.count, compressed.byteStride);
            break;
        }
        return true;
    }

    const fastgltf::Asset& asset_;
    mutable std::unordered_map<std::size_t, std::vector<std::byte>> meshopt_;
    mutable std::string error_;
};

bool DecodeDraco(const Adapter& adapter, const fastgltf::DracoCompressedPrimitive& draco,
                 std::size_t expectedVertices, std::size_t expectedIndices, std::vector<Vec3>& positions,
                 std::vector<std::uint32_t>& indices, std::string& error)
{
    const std::span<const std::byte> bytes = adapter.Resolve(draco.bufferView);
    if (bytes.size() < 5 || std::memcmp(bytes.data(), "DRACO", 5) != 0) {
        error = "draco magic missing";
        return false;
    }
    draco::DecoderBuffer buffer;
    buffer.Init(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    auto status = draco::Decoder().DecodeMeshFromBuffer(&buffer);
    if (!status.ok()) {
        error = "draco decode failed";
        return false;
    }
    std::unique_ptr<draco::Mesh> mesh = std::move(status).value();
    if (mesh == nullptr) {
        error = "draco empty mesh";
        return false;
    }
    const std::size_t pointCount = static_cast<std::size_t>(mesh->num_points());
    const std::size_t faceCount = static_cast<std::size_t>(mesh->num_faces());
    if (pointCount != expectedVertices || faceCount * 3 != expectedIndices) {
        error = "draco count mismatch";
        return false;
    }
    if (pointCount * 3 > kMaxDecodedBytes || faceCount > kMaxTriangles) {
        error = "draco over cap";
        return false;
    }
    const auto positionIt = draco.findAttribute("POSITION");
    if (positionIt == draco.attributes.end()) {
        error = "draco has no POSITION";
        return false;
    }
    const draco::PointAttribute* position =
        mesh->GetAttributeByUniqueId(static_cast<std::uint32_t>(positionIt->accessorIndex));
    if (position == nullptr) {
        error = "draco POSITION attribute missing";
        return false;
    }
    positions.resize(pointCount);
    for (std::size_t p = 0; p < pointCount; ++p) {
        std::array<float, 3> value{};
        const draco::PointIndex pointIndex(static_cast<std::uint32_t>(p));
        if (!position->GetValue<float, 3>(position->mapped_index(pointIndex), &value)) {
            error = "draco position read failed";
            return false;
        }
        positions[p] = { value[0], value[1], value[2] };
    }
    indices.resize(faceCount * 3);
    for (std::size_t f = 0; f < faceCount; ++f) {
        const draco::Mesh::Face& face = mesh->face(draco::FaceIndex(static_cast<std::uint32_t>(f)));
        for (int corner = 0; corner < 3; ++corner) {
            const std::uint32_t value = face[corner].value();
            if (value >= pointCount) {
                error = "draco index out of range";
                return false;
            }
            indices[f * 3 + corner] = value;
        }
    }
    return true;
}

bool BufferViewUsesMeshopt(const fastgltf::Asset& asset, const fastgltf::Accessor& accessor)
{
    if (!accessor.bufferViewIndex.has_value()) return false;
    const std::size_t index = *accessor.bufferViewIndex;
    return index < asset.bufferViews.size() && asset.bufferViews[index].meshoptCompression != nullptr;
}

void AppendIndexedGeometry(const std::vector<Vec3>& positions, const std::vector<std::uint32_t>& indices,
                           std::vector<Triangle>& out)
{
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        if (out.size() >= kMaxTriangles) return;
        const std::uint32_t a = indices[i];
        const std::uint32_t b = indices[i + 1];
        const std::uint32_t c = indices[i + 2];
        if (a >= positions.size() || b >= positions.size() || c >= positions.size()) return;
        Triangle triangle;
        triangle.p[0] = positions[a];
        triangle.p[1] = positions[b];
        triangle.p[2] = positions[c];
        triangle.color[0] = triangle.color[1] = triangle.color[2] = { 0.72f, 0.73f, 0.75f };
        triangle.flags = thumbnail_rasterizer::kTriangleDoubleSided;
        out.push_back(triangle);
    }
}

bool LooksLikeKtx2(std::span<const std::byte> bytes)
{
    // KTX 2.0 identifier: AB 'K' 'T' 'X' ' ' '2' '0' BB 0D 0A 1A 0A.
    static constexpr std::uint8_t kMagic[12] = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                                 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
    return bytes.size() >= 12 && std::memcmp(bytes.data(), kMagic, 12) == 0;
}

// Minimal preflight before libktx allocates anything: a valid-looking magic on
// garbage bytes must be rejected here, not inside the decoder. Mirrors the
// header/level-index bounds the production TextureTranscodeAdapter checks first.
bool Ktx2IsPlausible(std::span<const std::byte> bytes)
{
    if (bytes.size() < 80) return false;
    const auto u32 = [&](std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, 4);
        return value;
    };
    const auto u64 = [&](std::size_t offset) {
        std::uint64_t value = 0;
        std::memcpy(&value, bytes.data() + offset, 8);
        return value;
    };
    const std::uint32_t width = u32(20);
    const std::uint32_t height = u32(24);
    const std::uint32_t levels = u32(40);
    if (width == 0 || height == 0 || width > 16384 || height > 16384) return false;
    if (u32(28) != 0 || u32(32) != 0 || u32(36) != 1) return false;
    if (levels == 0 || levels > 32 || bytes.size() < 80 + static_cast<std::size_t>(levels) * 24) return false;
    for (const auto range : { std::pair<std::uint64_t, std::uint64_t>{ u32(48), u32(52) },
                              std::pair<std::uint64_t, std::uint64_t>{ u32(56), u32(60) },
                              std::pair<std::uint64_t, std::uint64_t>{ u64(64), u64(72) } }) {
        if (range.second > 1024 * 1024 || range.first > bytes.size()
            || range.second > bytes.size() - range.first)
            return false;
    }
    const std::uint64_t indexEnd = 80 + static_cast<std::uint64_t>(levels) * 24;
    for (std::uint32_t level = 0; level < levels; ++level) {
        const std::size_t index = 80 + static_cast<std::size_t>(level) * 24;
        const std::uint64_t offset = u64(index);
        const std::uint64_t length = u64(index + 8);
        const std::uint64_t uncompressed = u64(index + 16);
        if (offset < indexEnd || offset > bytes.size() || length == 0 || length > bytes.size() - offset
            || uncompressed > kMaxDecodedBytes)
            return false;
    }
    return true;
}

bool LooksLikeWebp(std::span<const std::byte> bytes)
{
    return bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0
        && std::memcmp(bytes.data() + 8, "WEBP", 4) == 0;
}

std::span<const std::byte> ImageBytes(const fastgltf::Asset& asset, const Adapter& adapter,
                                      const fastgltf::Image& image)
{
    if (const auto* array = std::get_if<fastgltf::sources::Array>(&image.data))
        return { array->bytes.data(), array->bytes.size() };
    if (const auto* vector = std::get_if<fastgltf::sources::Vector>(&image.data))
        return { vector->bytes.data(), vector->bytes.size() };
    if (const auto* view = std::get_if<fastgltf::sources::ByteView>(&image.data))
        return { view->bytes.data(), view->bytes.size() };
    if (const auto* bufferView = std::get_if<fastgltf::sources::BufferView>(&image.data))
        return adapter.Resolve(bufferView->bufferViewIndex);
    (void)asset;
    return {};
}

struct ImageBlob
{
    std::vector<std::byte> bytes;
};

// Copies every image's bytes out of the parsed asset (while it is alive) so the
// decoders run only after fastgltf's own storage is gone.
void CollectImageBlobs(const fastgltf::Asset& asset, const Adapter& adapter, std::vector<ImageBlob>& blobs)
{
    for (const fastgltf::Image& image : asset.images) {
        const std::span<const std::byte> bytes = ImageBytes(asset, adapter, image);
        if (bytes.empty() || bytes.size() > kMaxSourceBytes) continue;
        ImageBlob blob;
        blob.bytes.assign(bytes.begin(), bytes.end());
        blobs.push_back(std::move(blob));
    }
}

void DecodeImageBlobs(const std::vector<ImageBlob>& blobs, DecodeStats& stats)
{
    for (const ImageBlob& blob : blobs) {
        const std::span<const std::byte> bytes(blob.bytes.data(), blob.bytes.size());
        if (LooksLikeKtx2(bytes) && Ktx2IsPlausible(bytes)) {
            ktxTexture2* texture = nullptr;
            if (ktxTexture2_CreateFromMemory(reinterpret_cast<const ktx_uint8_t*>(bytes.data()), bytes.size(),
                                             KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture) != KTX_SUCCESS
                || texture == nullptr)
                continue;
            if (ktxTexture2_NeedsTranscoding(texture))
                ktxTexture2_TranscodeBasis(texture, KTX_TTF_RGBA32, 0);
            stats.imagePixels += static_cast<std::uint64_t>(texture->baseWidth) * texture->baseHeight;
            stats.decodedImages.push_back("ktx2/basisu");
            ktxTexture2_Destroy(texture);
        } else if (LooksLikeWebp(bytes)) {
            int width = 0;
            int height = 0;
            if (!WebPGetInfo(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &width, &height))
                continue;
            if (width <= 0 || height <= 0) continue;
            std::uint8_t* rgba = WebPDecodeRGBA(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
                                                &width, &height);
            if (rgba != nullptr) {
                stats.imagePixels += static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
                stats.decodedImages.push_back("webp");
                WebPFree(rgba);
            }
        }
    }
}

// The spike deliberately parses with Options::None and loads external buffers
// and images itself instead of Options::LoadExternalBuffers: the latter is not
// the production path (the importer brokers sidecars) and triggered a
// fastgltf-side fault on this build's GLB + KHR_texture_basisu fixture.
bool LoadExternalData(fastgltf::Asset& asset, const std::filesystem::path& baseDir, std::string& error)
{
    auto load = [&](fastgltf::DataSource& source) -> bool {
        auto* uri = std::get_if<fastgltf::sources::URI>(&source);
        if (uri == nullptr || uri->uri.isDataUri()) return true;
        const std::filesystem::path filePath = baseDir / uri->uri.fspath();
        std::ifstream stream(filePath, std::ios::binary | std::ios::ate);
        if (!stream) {
            error = "external file unavailable: " + filePath.string();
            return false;
        }
        const std::streamoff length = stream.tellg();
        if (length < 0 || static_cast<std::uint64_t>(length) > kMaxSourceBytes) {
            error = "external file empty or over 128 MiB";
            return false;
        }
        fastgltf::sources::Vector data;
        data.bytes.resize(static_cast<std::size_t>(length));
        stream.seekg(0, std::ios::beg);
        stream.read(reinterpret_cast<char*>(data.bytes.data()), length);
        if (uri->fileByteOffset > data.bytes.size()) {
            error = "external file offset out of range";
            return false;
        }
        if (uri->fileByteOffset > 0)
            data.bytes.erase(data.bytes.begin(), data.bytes.begin() + static_cast<std::ptrdiff_t>(uri->fileByteOffset));
        data.mimeType = uri->mimeType;
        source = std::move(data);
        return true;
    };
    for (fastgltf::Buffer& buffer : asset.buffers)
        if (!load(buffer.data)) return false;
    for (fastgltf::Image& image : asset.images)
        if (!load(image.data)) return false;
    return true;
}

} // namespace

namespace
{

bool DecodeImpl(const std::wstring& path, std::vector<Triangle>& triangles, DecodeStats& stats,
                std::vector<ImageBlob>& imageBlobs, std::string& error)
{
    std::vector<std::byte> fileBytes;
    if (!ReadFileBytes(path, fileBytes, stats.sourceBytes)) {
        error = "read failed or file exceeds 128 MiB";
        return false;
    }

    auto dataBuffer = fastgltf::MappedGltfFile::FromPath(std::filesystem::path(path));
    if (!dataBuffer) {
        error = "MappedGltfFile::FromPath failed";
        return false;
    }

    fastgltf::Parser parser(fastgltf::Extensions::KHR_draco_mesh_compression
                            | fastgltf::Extensions::KHR_texture_basisu
                            | fastgltf::Extensions::KHR_texture_transform
                            | fastgltf::Extensions::KHR_mesh_quantization
                            | fastgltf::Extensions::EXT_meshopt_compression
                            | fastgltf::Extensions::EXT_texture_webp
                            | fastgltf::Extensions::KHR_materials_unlit);
    const std::filesystem::path filePath(path);
    auto assetResult = parser.loadGltf(dataBuffer.get(), filePath.parent_path(),
                                       fastgltf::Options::None);
    if (!assetResult) {
        error = fastgltf::getErrorName(assetResult.error());
        return false;
    }
    fastgltf::Asset& asset = assetResult.get();

    if (asset.nodes.size() > 100000 || asset.accessors.size() > 300000 || asset.bufferViews.size() > 300000) {
        error = "asset exceeds spike structural caps";
        return false;
    }
    if (!LoadExternalData(asset, filePath.parent_path(), error)) return false;

    Adapter adapter(asset);
    bool usedMeshopt = false;
    std::size_t totalVertices = 0;

    for (const fastgltf::Node& node : asset.nodes) {
        if (!node.meshIndex.has_value()) continue;
        const fastgltf::Mesh& mesh = asset.meshes[*node.meshIndex];
        for (const fastgltf::Primitive& primitive : mesh.primitives) {
            if (primitive.type != fastgltf::PrimitiveType::Triangles) continue;
            const auto positionIt = primitive.findAttribute("POSITION");
            if (positionIt == primitive.attributes.end()) continue;
            const fastgltf::Accessor& positionAccessor = asset.accessors[positionIt->accessorIndex];
            if (positionAccessor.count == 0 || positionAccessor.count > kMaxVertices
                || totalVertices > kMaxVertices - positionAccessor.count) {
                error = "vertex count over cap";
                return false;
            }

            std::vector<Vec3> positions;
            std::vector<std::uint32_t> indices;

            if (primitive.dracoCompression != nullptr) {
                const std::size_t indexCount = primitive.indicesAccessor ? asset.accessors[*primitive.indicesAccessor].count
                                                                         : positionAccessor.count;
                if (!DecodeDraco(adapter, *primitive.dracoCompression, positionAccessor.count, indexCount,
                                 positions, indices, error))
                    return false;
                stats.geometryDecoder = "draco";
            } else {
                positions.resize(positionAccessor.count);
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                    asset, positionAccessor,
                    [&](fastgltf::math::fvec3 position, std::size_t index) {
                        positions[index] = { position.x(), position.y(), position.z() };
                    },
                    adapter);

                if (primitive.indicesAccessor.has_value()) {
                    const fastgltf::Accessor& indexAccessor = asset.accessors[*primitive.indicesAccessor];
                    indices.resize(indexAccessor.count);
                    fastgltf::iterateAccessorWithIndex<std::uint32_t>(
                        asset, indexAccessor,
                        [&](std::uint32_t index, std::size_t i) { indices[i] = index; }, adapter);
                } else {
                    indices.resize(positionAccessor.count);
                    for (std::size_t i = 0; i < indices.size(); ++i) indices[i] = static_cast<std::uint32_t>(i);
                }
                usedMeshopt = usedMeshopt || BufferViewUsesMeshopt(asset, positionAccessor);
                if (stats.geometryDecoder == "none")
                    stats.geometryDecoder = BufferViewUsesMeshopt(asset, positionAccessor) ? "meshopt" : "plain";
            }

            if (adapter.Error().size() != 0) {
                error = adapter.Error();
                return false;
            }

            stats.vertices += positions.size();
            totalVertices += positions.size();
            AppendIndexedGeometry(positions, indices, triangles);
        }
    }

    CollectImageBlobs(asset, adapter, imageBlobs);

    if (triangles.empty()) {
        error = "no triangles decoded";
        return false;
    }
    if (usedMeshopt && stats.geometryDecoder == "plain") stats.geometryDecoder = "meshopt";
    stats.triangles = triangles.size();
    return true;
}

} // namespace

bool Decode(const std::wstring& path, std::vector<Triangle>& triangles, DecodeStats& stats, std::string& error)
{
    std::vector<ImageBlob> imageBlobs;
    try {
        if (!DecodeImpl(path, triangles, stats, imageBlobs, error)) return false;
    } catch (const std::exception& exception) {
        error = std::string("unhandled exception: ") + exception.what();
        return false;
    } catch (...) {
        error = "unhandled non-standard exception";
        return false;
    }
    // fastgltf's storage is gone by now; decode images from owned copies.
    DecodeImageBlobs(imageBlobs, stats);
    return true;
}

bool DecodeStandaloneImage(const std::wstring& path, DecodeStats& stats, std::string& error)
{
    std::vector<std::byte> bytes;
    if (!ReadFileBytes(path, bytes, stats.sourceBytes)) {
        error = "read failed or file exceeds 128 MiB";
        return false;
    }
    const std::span<const std::byte> span(bytes.data(), bytes.size());
    if (LooksLikeKtx2(span)) {
        if (!Ktx2IsPlausible(span)) {
            error = "ktx2 header failed preflight";
            return false;
        }
        ktxTexture2* texture = nullptr;
        if (ktxTexture2_CreateFromMemory(reinterpret_cast<const ktx_uint8_t*>(span.data()), span.size(),
                                         KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture) != KTX_SUCCESS
            || texture == nullptr) {
            error = "ktx2 create failed";
            return false;
        }
        if (ktxTexture2_NeedsTranscoding(texture)) ktxTexture2_TranscodeBasis(texture, KTX_TTF_RGBA32, 0);
        stats.imagePixels = static_cast<std::uint64_t>(texture->baseWidth) * texture->baseHeight;
        stats.decodedImages.push_back("ktx2/basisu");
        ktxTexture2_Destroy(texture);
        return true;
    }
    if (LooksLikeWebp(span)) {
        int width = 0;
        int height = 0;
        if (!WebPGetInfo(reinterpret_cast<const std::uint8_t*>(span.data()), span.size(), &width, &height)
            || width <= 0 || height <= 0) {
            error = "webp header failed";
            return false;
        }
        std::uint8_t* rgba = WebPDecodeRGBA(reinterpret_cast<const std::uint8_t*>(span.data()), span.size(), &width,
                                            &height);
        if (rgba == nullptr) {
            error = "webp decode failed";
            return false;
        }
        stats.imagePixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
        stats.decodedImages.push_back("webp");
        WebPFree(rgba);
        return true;
    }
    error = "unrecognized image container";
    return false;
}

} // namespace gltf_spike