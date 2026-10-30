#include "AssimpAdapter.h"

#include "AssimpTextureDecode.h"
#include "BoundedChunkWriter.h"
#include "ChunkBatchSink.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/TierALimits.h"
#include "model_core/VertexLayouts.h"

#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>
#include <assimp/ProgressHandler.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <pugixml.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>

namespace import_worker {
namespace {

using namespace model_core;

constexpr uint32_t kMaxWarningCount = 64;

struct FormatInfo {
    const char* hint;
    SourceFormatId wire;
    UpAxisId upAxis;
};

FormatInfo InfoFor(AssimpFormat format)
{
    switch (format) {
    case AssimpFormat::ThreeDs: return {"3ds", SourceFormatId::ThreeDs, UpAxisId::Z};
    case AssimpFormat::Dxf: return {"dxf", SourceFormatId::Dxf, UpAxisId::Z};
    case AssimpFormat::DirectX: return {"x", SourceFormatId::DirectX, UpAxisId::Y};
    case AssimpFormat::Off: return {"off", SourceFormatId::Off, UpAxisId::Y};
    case AssimpFormat::Amf: return {"amf", SourceFormatId::Amf, UpAxisId::Z};
    case AssimpFormat::X3d: return {"x3d", SourceFormatId::X3d, UpAxisId::Y};
    case AssimpFormat::Dae:
    default: return {"dae", SourceFormatId::Dae, UpAxisId::Y};
    }
}

bool Finite(double value) { return std::isfinite(value) && std::abs(value) <= 1.0e30; }
bool Finite(const aiVector3D& value)
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}
float SafeFloat(double value, float fallback = 0.0f)
{
    return Finite(value) && value >= -double((std::numeric_limits<float>::max)())
        && value <= double((std::numeric_limits<float>::max)())
        ? static_cast<float>(value) : fallback;
}
float Saturate(double value, float fallback = 1.0f)
{
    return Finite(value) ? static_cast<float>(std::clamp(value, 0.0, 1.0)) : fallback;
}
void Warn(uint32_t& warnings) { warnings = (std::min)(kMaxWarningCount, warnings + 1); }

bool IsImageExtension(std::string_view lowerExtension)
{
    return lowerExtension == ".png" || lowerExtension == ".jpg" || lowerExtension == ".jpeg"
        || lowerExtension == ".bmp" || lowerExtension == ".tif" || lowerExtension == ".tiff"
        || lowerExtension == ".webp" || lowerExtension == ".ktx2";
}

std::string LowerAscii(std::string_view text)
{
    std::string result(text);
    for (char& value : result) {
        if (value >= 'A' && value <= 'Z') value = static_cast<char>(value + ('a' - 'A'));
    }
    return result;
}

// The adapter's own path boundary. This is checked before any sidecar request:
// no absolute/rooted/UNC/device path, no drive-relative or URI colon, no '..',
// no alternate data stream, no control byte, no unknown extension. The broker
// repeats the same checks on the authored reference; neither layer trusts the
// other.
bool HasControlByte(std::string_view text)
{
    for (const unsigned char value : text) {
        if (value < 0x20 || value == 0x7F) return true;
    }
    return false;
}

bool ContainsCaseInsensitive(std::span<const std::byte> bytes, std::string_view needle)
{
    if (needle.empty() || bytes.size() < needle.size()) return false;
    const size_t limit = (std::min)(bytes.size(), size_t(4096) + needle.size());
    for (size_t offset = 0; offset + needle.size() <= limit; ++offset) {
        bool match = true;
        for (size_t index = 0; index < needle.size(); ++index) {
            char value = char(std::to_integer<unsigned char>(bytes[offset + index]));
            if (value >= 'A' && value <= 'Z') value = char(value + ('a' - 'A'));
            char expected = needle[index];
            if (expected >= 'A' && expected <= 'Z') expected = char(expected + ('a' - 'A'));
            if (value != expected) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// Product-owned admission before any Assimp importer runs. Assimp's historical
// parsers can dereference incomplete state on inputs their own sniffers accept
// (for example an X3D document whose root token was corrupted), so the adapter
// rejects control bytes and missing container markers itself. This is a crash
// boundary, not a syntax validator: the importer still owns real parsing.
bool PassesTextAdmission(std::span<const std::byte> bytes, AssimpFormat format)
{
    if (bytes.size() < 4) return false;
    for (const std::byte value : bytes) {
        const unsigned char byte = std::to_integer<unsigned char>(value);
        if (byte < 0x20 && byte != 0x09 && byte != 0x0A && byte != 0x0D) return false;
        if (byte == 0x7F) return false;
    }
    switch (format) {
    case AssimpFormat::Dae:
        return ContainsCaseInsensitive(bytes, "<COLLADA");
    case AssimpFormat::X3d:
        return ContainsCaseInsensitive(bytes, "<X3D");
    case AssimpFormat::Amf:
        return ContainsCaseInsensitive(bytes, "<amf");
    case AssimpFormat::Off: {
        size_t offset = 0;
        while (offset < bytes.size()
               && (bytes[offset] == std::byte{0x20} || bytes[offset] == std::byte{0x09}
                   || bytes[offset] == std::byte{0x0A} || bytes[offset] == std::byte{0x0D}))
            ++offset;
        return offset + 3 <= bytes.size() && bytes[offset] == std::byte{'O'}
            && bytes[offset + 1] == std::byte{'F'} && bytes[offset + 2] == std::byte{'F'};
    }
    case AssimpFormat::Dxf:
        return ContainsCaseInsensitive(bytes, "SECTION")
            && ContainsCaseInsensitive(bytes, "ENTITIES");
    case AssimpFormat::DirectX:
        return ContainsCaseInsensitive(bytes, "xof ");
    case AssimpFormat::ThreeDs:
        return false; // binary container, handled separately
    }
    return false;
}

bool PassesBinaryAdmission(std::span<const std::byte> bytes, AssimpFormat format)
{
    if (format != AssimpFormat::ThreeDs) return true;
    // MAIN3DS chunk id (0x4D4D little-endian) plus a declared length that fits
    // the source. Deeper structural validation remains Assimp's job.
    if (bytes.size() < 6) return false;
    const unsigned char a = std::to_integer<unsigned char>(bytes[0]);
    const unsigned char b = std::to_integer<unsigned char>(bytes[1]);
    if (a != 0x4D || b != 0x4D) return false;
    const uint32_t length = uint32_t(std::to_integer<unsigned char>(bytes[2]))
        | (uint32_t(std::to_integer<unsigned char>(bytes[3])) << 8)
        | (uint32_t(std::to_integer<unsigned char>(bytes[4])) << 16)
        | (uint32_t(std::to_integer<unsigned char>(bytes[5])) << 24);
    return length >= 6 && uint64_t(length) <= bytes.size();
}

bool IsXmlAssimpFormat(AssimpFormat format)
{
    return format == AssimpFormat::Dae || format == AssimpFormat::X3d
        || format == AssimpFormat::Amf;
}

bool ParseUnsigned(std::string_view value, uint64_t& out)
{
    if (value.empty()) return false;
    const char* first = value.data();
    const char* last = value.data() + value.size();
    auto parsed = std::from_chars(first, last, out);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

bool ParseReal(std::string_view value, double& out)
{
    if (value.empty()) return false;
    const char* first = value.data();
    const char* last = value.data() + value.size();
    auto parsed = std::from_chars(first, last, out, std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

bool CountNumericTokens(std::string_view text, uint64_t limit, bool integersOnly, uint64_t& count)
{
    count = 0;
    size_t position = 0;
    while (position < text.size()) {
        while (position < text.size()
               && (text[position] == ' ' || text[position] == '\t'
                   || text[position] == '\r' || text[position] == '\n'))
            ++position;
        if (position >= text.size()) break;
        const size_t begin = position;
        while (position < text.size() && text[position] != ' ' && text[position] != '\t'
               && text[position] != '\r' && text[position] != '\n')
            ++position;
        const std::string_view token = text.substr(begin, position - begin);
        if (integersOnly) {
            uint64_t value = 0;
            if (!ParseUnsigned(token, value)) return false;
        } else {
            double value = 0;
            if (!ParseReal(token, value) || !std::isfinite(value)) return false;
        }
        if (++count > limit) return false;
    }
    return true;
}

bool ValidateDaeNode(const pugi::xml_node& node, const AssimpImportOptions& options,
                     uint32_t& imageCount, uint32_t& animations)
{
    for (const pugi::xml_attribute attribute : node.attributes()) {
        const std::string_view name = attribute.name();
        if (name == "count" || name == "stride" || name == "offset") {
            uint64_t value = 0;
            if (!ParseUnsigned(attribute.value(), value)) return false;
            const uint64_t limit = name == "offset" ? 16
                : name == "stride" ? 32 : options.maxVertices * 8;
            if (value > limit) return false;
        }
    }
    const std::string_view name = node.name();
    if (name == "float_array" || name == "int_array") {
        uint64_t tokens = 0;
        if (!CountNumericTokens(node.text().get(), options.maxVertices * 8,
                                name == "int_array", tokens))
            return false;
        uint64_t declared = 0;
        if (ParseUnsigned(node.attribute("count").value(), declared) && declared > tokens)
            return false;
    } else if (name == "vcount" || name == "p") {
        uint64_t tokens = 0;
        if (!CountNumericTokens(node.text().get(), options.maxVertices * 8, true, tokens))
            return false;
        if (name == "vcount" && tokens > options.maxTriangles) return false;
    } else if (name == "polylist" || name == "triangles" || name == "polygons") {
        uint64_t count = 0;
        if (ParseUnsigned(node.attribute("count").value(), count)
            && count > options.maxTriangles)
            return false;
    } else if (name == "image") {
        if (++imageCount > 256) return false;
    } else if (name == "animation" || name == "animation_clip") {
        ++animations;
    }
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
        if (child.type() == pugi::node_element && !ValidateDaeNode(child, options, imageCount, animations))
            return false;
    }
    return true;
}

bool CountIndexTokens(std::string_view text, uint64_t limit, uint64_t maxValue, uint64_t& count)
{
    count = 0;
    size_t position = 0;
    while (position < text.size()) {
        while (position < text.size()
               && (text[position] == ' ' || text[position] == '\t'
                   || text[position] == '\r' || text[position] == '\n'))
            ++position;
        if (position >= text.size()) break;
        const size_t begin = position;
        while (position < text.size() && text[position] != ' ' && text[position] != '\t'
               && text[position] != '\r' && text[position] != '\n')
            ++position;
        const std::string_view token = text.substr(begin, position - begin);
        int64_t value = 0;
        const char* first = token.data();
        const char* last = token.data() + token.size();
        auto parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc{} || parsed.ptr != last) return false;
        if (value < -1 || (value >= 0 && uint64_t(value) > maxValue)) return false;
        if (++count > limit) return false;
    }
    return count != 0;
}

bool ValidateX3dNode(const pugi::xml_node& node, const AssimpImportOptions& options)
{
    for (const pugi::xml_attribute attribute : node.attributes()) {
        const std::string_view name = attribute.name();
        if (name == "coordIndex" || name == "index" || name == "colorIndex"
            || name == "normalIndex" || name == "texCoordIndex") {
            uint64_t tokens = 0;
            if (!CountIndexTokens(attribute.value(), options.maxTriangles * 4,
                                  options.maxVertices * 4, tokens))
                return false;
        } else if (name == "point" || name == "color" || name == "normal") {
            uint64_t tokens = 0;
            if (!CountNumericTokens(attribute.value(), options.maxVertices * 3, false, tokens))
                return false;
        }
    }
    const std::string_view name = node.name();
    if (name == "point" || name == "Coordinate" || name == "CoordinateDouble") {
        uint64_t tokens = 0;
        if (!CountNumericTokens(node.text().get(), options.maxVertices * 3, false, tokens)
            || tokens % 3 != 0)
            return false;
    } else if (name == "color" || name == "normal") {
        uint64_t tokens = 0;
        if (!CountNumericTokens(node.text().get(), options.maxVertices * 3, false, tokens)
            || tokens % 3 != 0)
            return false;
    } else if (name == "TextureCoordinate") {
        uint64_t tokens = 0;
        if (!CountNumericTokens(node.text().get(), options.maxVertices * 2, false, tokens)
            || tokens % 2 != 0)
            return false;
    }
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
        if (child.type() == pugi::node_element && !ValidateX3dNode(child, options))
            return false;
    }
    return true;
}

ImportErrorCode XmlAdmission(std::span<const std::byte> bytes, AssimpFormat format,
                             const AssimpImportOptions& options)
{
    // A DOM parse bounds the whole document, so cap XML inputs well below the
    // generic Tier-B source ceiling. Real DAE/AMF/X3D files are far smaller.
    constexpr uint64_t kMaxXmlBytes = 128ull * 1024 * 1024;
    if (bytes.size() > kMaxXmlBytes) return ImportErrorCode::ResourceLimit;
    for (const std::byte value : bytes) {
        const unsigned char byte = std::to_integer<unsigned char>(value);
        if ((byte < 0x20 && byte != 0x09 && byte != 0x0A && byte != 0x0D) || byte == 0x7F)
            return ImportErrorCode::MalformedData;
    }
    pugi::xml_document document;
    const pugi::xml_parse_result parsed = document.load_buffer(
        bytes.data(), bytes.size(), pugi::parse_default);
    if (!parsed) return ImportErrorCode::MalformedData;
    const pugi::xml_node root = document.document_element();
    if (!root) return ImportErrorCode::MalformedData;
    const char* expected = format == AssimpFormat::Dae ? "COLLADA"
        : format == AssimpFormat::X3d ? "X3D" : "amf";
    if (std::strcmp(root.name(), expected) != 0) return ImportErrorCode::MalformedData;
    if (format == AssimpFormat::Dae) {
        uint32_t images = 0, animations = 0;
        if (!ValidateDaeNode(root, options, images, animations))
            return ImportErrorCode::MalformedData;
    } else if (format == AssimpFormat::X3d) {
        if (!ValidateX3dNode(root, options)) return ImportErrorCode::MalformedData;
    }
    return ImportErrorCode::None;
}

ImportErrorCode OffAdmission(std::span<const std::byte> bytes, const AssimpImportOptions& options)
{
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    size_t position = 0;
    auto skip = [&] {
        for (;;) {
            while (position < text.size()
                   && (text[position] == ' ' || text[position] == '\t'
                       || text[position] == '\r' || text[position] == '\n'))
                ++position;
            if (position < text.size() && text[position] == '#') {
                while (position < text.size() && text[position] != '\n') ++position;
                continue;
            }
            break;
        }
    };
    auto token = [&]() -> std::string_view {
        skip();
        const size_t begin = position;
        while (position < text.size() && text[position] != ' ' && text[position] != '\t'
               && text[position] != '\r' && text[position] != '\n')
            ++position;
        return text.substr(begin, position - begin);
    };
    auto number = [](std::string_view value, double& out) {
        if (value.empty()) return false;
        const char* first = value.data();
        const char* last = value.data() + value.size();
        auto parsed = std::from_chars(first, last, out, std::chars_format::general);
        return parsed.ec == std::errc{} && parsed.ptr == last;
    };
    auto unsignedNumber = [](std::string_view value, uint64_t& out) {
        if (value.empty()) return false;
        const char* first = value.data();
        const char* last = value.data() + value.size();
        auto parsed = std::from_chars(first, last, out);
        return parsed.ec == std::errc{} && parsed.ptr == last;
    };

    const std::string_view keyword = token();
    if (keyword != "OFF" && keyword != "off") return ImportErrorCode::MalformedData;
    uint64_t vertexCount = 0, faceCount = 0, edgeCount = 0;
    if (!unsignedNumber(token(), vertexCount) || !unsignedNumber(token(), faceCount)
        || !unsignedNumber(token(), edgeCount))
        return ImportErrorCode::MalformedData;
    if (vertexCount > options.maxVertices || faceCount > options.maxTriangles)
        return ImportErrorCode::ResourceLimit;
    for (uint64_t index = 0; index < vertexCount; ++index) {
        for (unsigned axis = 0; axis < 3; ++axis) {
            double value = 0;
            if (!number(token(), value) || !std::isfinite(value))
                return ImportErrorCode::MalformedData;
        }
    }
    for (uint64_t index = 0; index < faceCount; ++index) {
        uint64_t corners = 0;
        if (!unsignedNumber(token(), corners) || corners < 3 || corners > 1'000'000
            || corners > vertexCount)
            return ImportErrorCode::MalformedData;
        for (uint64_t corner = 0; corner < corners; ++corner) {
            uint64_t vertex = 0;
            if (!unsignedNumber(token(), vertex) || vertex >= vertexCount)
                return ImportErrorCode::MalformedData;
        }
    }
    return token().empty() ? ImportErrorCode::None : ImportErrorCode::MalformedData;
}

bool IsSafeReference(std::string_view reference, bool requireImage)
{
    if (reference.empty() || reference.size() > 512 || HasControlByte(reference)) return false;
    if (reference.find(':') != std::string_view::npos) return false;
    if (reference.front() == '/' || reference.front() == '\\') return false;
    std::string normalized(reference);
    for (char& value : normalized) {
        if (value == '\\') value = '/';
    }
    if (normalized.find("//") != std::string::npos) return false;
    size_t start = 0;
    while (start <= normalized.size()) {
        const size_t end = normalized.find('/', start);
        const std::string_view component(normalized.data() + start,
            (end == std::string::npos ? normalized.size() : end) - start);
        if (component.empty() || component == "." || component == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (requireImage) {
        const size_t dot = normalized.find_last_of('.');
        if (dot == std::string::npos) return false;
        if (!IsImageExtension(LowerAscii(std::string_view(normalized).substr(dot)))) return false;
    }
    return true;
}

class MemoryStream final : public Assimp::IOStream {
public:
    explicit MemoryStream(std::shared_ptr<const std::vector<std::byte>> bytes)
        : bytes_(std::move(bytes)) {}

    size_t Read(void* buffer, size_t size, size_t count) override
    {
        if (!buffer || !size || !count || !bytes_) return 0;
        const size_t available = bytes_->size() - offset_;
        const size_t elements = (std::min)(count, available / size);
        const size_t bytes = elements * size;
        if (bytes) std::memcpy(buffer, bytes_->data() + offset_, bytes);
        offset_ += bytes;
        return elements;
    }
    size_t Write(const void*, size_t, size_t) override { return 0; }
    aiReturn Seek(size_t offset, aiOrigin origin) override
    {
        uint64_t target = offset;
        if (origin == aiOrigin_CUR) target = uint64_t(offset_) + offset;
        else if (origin == aiOrigin_END) target = uint64_t(bytes_->size()) - (std::min)(offset, bytes_->size());
        if (target > bytes_->size()) return aiReturn_FAILURE;
        offset_ = size_t(target);
        return aiReturn_SUCCESS;
    }
    size_t Tell() const override { return offset_; }
    size_t FileSize() const override { return bytes_ ? bytes_->size() : 0; }
    void Flush() override {}

private:
    std::shared_ptr<const std::vector<std::byte>> bytes_;
    size_t offset_ = 0;
};

struct CachedSidecar {
    std::shared_ptr<const std::vector<std::byte>> bytes;
    ImportErrorCode error = ImportErrorCode::None;
};

struct ImportContext {
    const AssimpImportOptions* options = nullptr;
    ImportErrorCode error = ImportErrorCode::None;
    uint32_t optionalWarnings = 0;
    uint32_t textureWarnings = 0;
    uint32_t sidecarRequests = 0;
    uint64_t encodedBytes = 0;
    uint64_t decodedBytes = 0;
    uint64_t decodedPixels = 0;
    std::map<std::string, CachedSidecar> sidecarCache;

    bool Cancelled() const { return options && options->Cancelled(); }

    const CachedSidecar& FetchSidecar(const std::string& reference, uint64_t maxBytes)
    {
        if (auto found = sidecarCache.find(reference); found != sidecarCache.end())
            return found->second;
        CachedSidecar cached;
        if (++sidecarRequests > options->maxSidecarRequests) {
            cached.error = ImportErrorCode::ResourceLimit;
        } else if (!options->requestSidecar || !maxBytes) {
            cached.error = ImportErrorCode::FileUnavailable;
        } else {
            AssimpSidecarResult result = options->requestSidecar(reference, maxBytes);
            cached.error = result.error;
            if (result.bytes && !result.bytes->empty()) {
                cached.bytes = std::make_shared<std::vector<std::byte>>(
                    std::move(*result.bytes));
            }
        }
        return sidecarCache.emplace(reference, std::move(cached)).first->second;
    }
};

class BrokerIOSystem final : public Assimp::IOSystem {
public:
    explicit BrokerIOSystem(ImportContext& context) : context_(context) {}

    bool Exists(const char* file) const override
    {
        // Existence is answered statically: the broker is only consulted by
        // Open, so a hostile document cannot make Exists touch a path.
        return file && IsSafeReference(file, true);
    }

    Assimp::IOStream* Open(const char* file, const char* mode) override
    {
        if (!file || !mode || (std::strcmp(mode, "rb") != 0 && std::strcmp(mode, "r") != 0))
            return nullptr;
        if (!IsSafeReference(file, true)) {
            // A reference outside the image allowlist is a containment fault,
            // not an optional missing texture.
            context_.error = ImportErrorCode::UnsafeReference;
            return nullptr;
        }
        if (context_.Cancelled()) {
            context_.error = ImportErrorCode::Cancelled;
            return nullptr;
        }
        const uint64_t remaining = context_.encodedBytes < context_.options->textureOptions.maxEncodedBytes
            ? context_.options->textureOptions.maxEncodedBytes - context_.encodedBytes : 0;
        const CachedSidecar& cached = context_.FetchSidecar(file, remaining);
        if (cached.error == ImportErrorCode::UnsafeReference
            || cached.error == ImportErrorCode::FileChanged
            || cached.error == ImportErrorCode::ImportProtocolViolation
            || cached.error == ImportErrorCode::ResourceLimit
            || cached.error == ImportErrorCode::AggregateSourceLimit) {
            context_.error = cached.error;
            return nullptr;
        }
        if (!cached.bytes) {
            Warn(context_.textureWarnings);
            return nullptr;
        }
        return new MemoryStream(cached.bytes);
    }

    void Close(Assimp::IOStream* stream) override { delete stream; }
    char getOsSeparator() const override { return '/'; }

private:
    ImportContext& context_;
};

class CancelProgressHandler final : public Assimp::ProgressHandler {
public:
    explicit CancelProgressHandler(ImportContext& context) : context_(context) {}
    bool Update(float) override { return !context_.Cancelled(); }
private:
    ImportContext& context_;
};

uint32_t EmitImage(BoundedChunkWriter& writer, AssimpDecodedImage&& image)
{
    ImagePayloadHeader header{};
    header.pixelFormat = uint32_t(image.format);
    header.width = image.width;
    header.height = image.height;
    header.mipLevels = image.levels;
    header.colorSpace = uint32_t(image.space);
    header.pixelDataByteSize = image.pixels.size();
    ChunkDescriptor descriptor{};
    descriptor.topology = ChunkTopology::Image;
    descriptor.chunkId = writer.NextId();
    if (!writer.Add(descriptor, ChunkBytes(header), image.pixels)) return 0;
    return descriptor.chunkId;
}

AssimpDecodedImage FallbackImage(ColorSpaceId space, TextureSemantic semantic)
{
    AssimpDecodedImage fallback;
    fallback.format = PixelFormatId::RGBA8_UNORM;
    fallback.space = space;
    fallback.width = fallback.height = semantic == TextureSemantic::Color ? 2u : 1u;
    fallback.levels = 1;
    fallback.pixels.resize(size_t(fallback.width) * fallback.height * 4, std::byte{255});
    if (semantic == TextureSemantic::Color) {
        for (unsigned i = 0; i < 4; ++i) {
            for (unsigned c = 0; c < 3; ++c) {
                fallback.pixels[i * 4 + c] = std::byte((i == 0 || i == 3) ? 64 : 192);
            }
        }
    } else if (semantic == TextureSemantic::Normal) {
        fallback.pixels[0] = fallback.pixels[1] = std::byte{128};
    } else if (semantic == TextureSemantic::Emissive) {
        fallback.pixels[0] = fallback.pixels[1] = fallback.pixels[2] = std::byte{0};
    }
    return fallback;
}

std::optional<AssimpDecodedImage> DecodeEncodedTexture(
    ImportContext& context, std::span<const std::byte> encoded, ColorSpaceId space,
    TextureSemantic semantic)
{
    if (encoded.empty() || encoded.size()
        > context.options->textureOptions.maxEncodedBytes - context.encodedBytes) {
        Warn(context.textureWarnings);
        return std::nullopt;
    }
    context.encodedBytes += encoded.size();
    TextureDecodeOptions options = context.options->textureOptions;
    options.maxDecodedBytes = (std::min)(options.maxDecodedBytes,
        kMaxAggregateTextureBytes - context.decodedBytes);
    options.maxPixels = (std::min)(options.maxPixels,
        kMaxAggregateTexturePixels - context.decodedPixels);
    auto decoded = DecodeAssimpTexture(encoded, space, semantic, options);
    if (context.Cancelled()) {
        context.error = ImportErrorCode::Cancelled;
        return std::nullopt;
    }
    if (!decoded) {
        Warn(context.textureWarnings);
        return std::nullopt;
    }
    const uint64_t pixels = uint64_t(decoded->width) * decoded->height;
    if (decoded->pixels.size() > kMaxAggregateTextureBytes - context.decodedBytes
        || pixels > kMaxAggregateTexturePixels - context.decodedPixels) {
        context.error = ImportErrorCode::ResourceLimit;
        return std::nullopt;
    }
    context.decodedBytes += decoded->pixels.size();
    context.decodedPixels += pixels;
    return decoded;
}

std::optional<AssimpDecodedImage> DecodeEmbeddedTexture(
    ImportContext& context, const aiTexture& texture, ColorSpaceId space,
    TextureSemantic semantic)
{
    if (!texture.pcData) return std::nullopt;
    if (texture.mHeight != 0) {
        // Uncompressed RGBA8888 embedded texel array. Assimp stores the texel
        // count in mWidth and the row count in mHeight.
        const uint64_t width = texture.mWidth;
        const uint64_t height = texture.mHeight;
        const uint64_t bytes = width * height * 4;
        const uint32_t maxDimension = context.options->textureOptions.maxDimension;
        if (!width || !height || width > maxDimension || height > maxDimension
            || bytes > context.options->textureOptions.maxDecodedBytes
            || bytes > kMaxAggregateTextureBytes - context.decodedBytes
            || width * height > kMaxAggregateTexturePixels - context.decodedPixels) {
            Warn(context.textureWarnings);
            return std::nullopt;
        }
        AssimpDecodedImage image;
        image.format = PixelFormatId::RGBA8_UNORM;
        image.space = space;
        image.width = uint32_t(width);
        image.height = uint32_t(height);
        image.levels = 1;
        image.pixels.assign(reinterpret_cast<const std::byte*>(texture.pcData),
                            reinterpret_cast<const std::byte*>(texture.pcData) + bytes);
        context.decodedBytes += bytes;
        context.decodedPixels += width * height;
        return image;
    }
    // Compressed embedded images store the byte length in mWidth.
    if (!texture.mWidth) return std::nullopt;
    return DecodeEncodedTexture(context,
        {reinterpret_cast<const std::byte*>(texture.pcData), texture.mWidth}, space, semantic);
}

uint32_t ResolveTexture(BoundedChunkWriter& writer, ImportContext& context,
                        const aiScene& scene, const char* path, ColorSpaceId space,
                        TextureSemantic semantic,
                        std::unordered_map<std::string, uint32_t>& cache)
{
    if (!path || !*path) return 0;
    std::string key(path);
    key.push_back(char('0' + unsigned(space)));
    key.push_back(char('0' + unsigned(semantic)));
    if (auto found = cache.find(key); found != cache.end()) return found->second;

    std::optional<AssimpDecodedImage> image;
    if (path[0] == '*') {
        const char* digits = path + 1;
        uint64_t index = 0;
        bool valid = *digits != '\0';
        for (const char* cursor = digits; *cursor; ++cursor) {
            if (*cursor < '0' || *cursor > '9') { valid = false; break; }
            index = index * 10 + uint64_t(*cursor - '0');
            if (index > scene.mNumTextures) { valid = false; break; }
        }
        if (valid && index < scene.mNumTextures) {
            image = DecodeEmbeddedTexture(context, *scene.mTextures[index], space, semantic);
        } else {
            Warn(context.textureWarnings);
        }
    } else if (IsSafeReference(path, true)) {
        // External references are resolved through the custom IOSystem, which
        // is the only path boundary. Opening the same reference here keeps the
        // material/image decode in one place.
        const aiTexture* embedded = scene.GetEmbeddedTexture(path);
        if (embedded) {
            image = DecodeEmbeddedTexture(context, *embedded, space, semantic);
        } else if (context.options->requestSidecar) {
            const uint64_t remaining = context.encodedBytes < context.options->textureOptions.maxEncodedBytes
                ? context.options->textureOptions.maxEncodedBytes - context.encodedBytes : 0;
            const CachedSidecar& cached = context.FetchSidecar(path, remaining);
            if (cached.error == ImportErrorCode::UnsafeReference
                || cached.error == ImportErrorCode::FileChanged
                || cached.error == ImportErrorCode::ImportProtocolViolation
                || cached.error == ImportErrorCode::ResourceLimit
                || cached.error == ImportErrorCode::AggregateSourceLimit) {
                context.error = cached.error;
                return 0;
            }
            if (!cached.bytes) {
                Warn(context.textureWarnings);
            } else {
                image = DecodeEncodedTexture(context, std::span<const std::byte>(*cached.bytes),
                                             space, semantic);
            }
        }
    } else {
        context.error = ImportErrorCode::UnsafeReference;
        return 0;
    }

    if (context.error != ImportErrorCode::None) return 0;
    if (!image) image = FallbackImage(space, semantic);
    const uint32_t id = EmitImage(writer, std::move(*image));
    if (id) cache.emplace(std::move(key), id);
    return id;
}

MaterialPayload ConvertMaterial(const aiMaterial& material, uint32_t& warnings)
{
    MaterialPayload payload{};
    payload.baseColorFactor[0] = payload.baseColorFactor[1] = payload.baseColorFactor[2] = 1.0f;
    payload.baseColorFactor[3] = 1.0f;
    payload.metallicFactor = 0.0f;
    payload.roughnessFactor = 1.0f;
    payload.uvScale[0] = payload.uvScale[1] = 1.0f;
    payload.alphaCutoff = 0.5f;
    payload.flags = kMaterialFlagDoubleSided | kMaterialFlagFlipV;

    aiColor4D diffuse(1.0f, 1.0f, 1.0f, 1.0f);
    if (material.Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == aiReturn_SUCCESS) {
        payload.baseColorFactor[0] = Saturate(diffuse.r);
        payload.baseColorFactor[1] = Saturate(diffuse.g);
        payload.baseColorFactor[2] = Saturate(diffuse.b);
        payload.baseColorFactor[3] = Saturate(diffuse.a);
    }
    float opacity = 1.0f;
    if (material.Get(AI_MATKEY_OPACITY, opacity) == aiReturn_SUCCESS) {
        payload.baseColorFactor[3] = Saturate(opacity, payload.baseColorFactor[3]);
    }
    float metallic = 0.0f;
    if (material.Get(AI_MATKEY_METALLIC_FACTOR, metallic) == aiReturn_SUCCESS) {
        payload.metallicFactor = Saturate(metallic, 0.0f);
    }
    float roughness = 1.0f;
    if (material.Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == aiReturn_SUCCESS) {
        payload.roughnessFactor = Saturate(roughness);
    } else {
        float glossiness = 1.0f;
        if (material.Get(AI_MATKEY_GLOSSINESS_FACTOR, glossiness) == aiReturn_SUCCESS) {
            payload.roughnessFactor = Saturate(1.0 - glossiness);
        }
    }
    aiColor3D emissive(0.0f, 0.0f, 0.0f);
    if (material.Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == aiReturn_SUCCESS) {
        payload.emissiveFactor[0] = SafeFloat(emissive.r);
        payload.emissiveFactor[1] = SafeFloat(emissive.g);
        payload.emissiveFactor[2] = SafeFloat(emissive.b);
    }
    int twoSided = 0;
    if (material.Get(AI_MATKEY_TWOSIDED, twoSided) == aiReturn_SUCCESS && !twoSided) {
        payload.flags &= ~kMaterialFlagDoubleSided;
    }
    payload.alphaMode = uint32_t(payload.baseColorFactor[3] < 0.999f
        ? AlphaModeId::Blend : AlphaModeId::Opaque);
    (void)warnings;
    return payload;
}

bool GetTexturePath(const aiMaterial& material, aiTextureType type, aiString& path)
{
    return material.GetTexture(type, 0, &path) == aiReturn_SUCCESS && path.length != 0;
}

bool EmitMaterials(const aiScene& scene, BoundedChunkWriter& writer,
                   ImportContext& context, std::map<uint32_t, uint32_t>& materialIds)
{
    std::unordered_map<std::string, uint32_t> imageIds;
    std::map<std::string, uint32_t> deduplicated;
    for (uint32_t index = 0; index < scene.mNumMaterials; ++index) {
        if (context.Cancelled()) {
            context.error = ImportErrorCode::Cancelled;
            return false;
        }
        const aiMaterial* material = scene.mMaterials[index];
        if (!material) continue;
        MaterialPayload payload = ConvertMaterial(*material, context.optionalWarnings);

        aiString basePath, normalPath, emissivePath;
        const bool hasBase = GetTexturePath(*material, aiTextureType_BASE_COLOR, basePath)
            || GetTexturePath(*material, aiTextureType_DIFFUSE, basePath);
        const bool hasNormal = GetTexturePath(*material, aiTextureType_NORMALS, normalPath)
            || GetTexturePath(*material, aiTextureType_NORMAL_CAMERA, normalPath);
        const bool hasEmissive = GetTexturePath(*material, aiTextureType_EMISSIVE, emissivePath);

        // A packed metallic/roughness texture has no separate slot in the
        // normalized contract; keep the scalar factors and report the map as a
        // bounded optional-feature degradation.
        aiString metalRoughPath, metalPath, roughPath;
        if (GetTexturePath(*material, aiTextureType_METALNESS, metalRoughPath)
            || GetTexturePath(*material, aiTextureType_DIFFUSE_ROUGHNESS, metalRoughPath)
            || GetTexturePath(*material, aiTextureType_METALNESS, metalPath)
            || GetTexturePath(*material, aiTextureType_DIFFUSE_ROUGHNESS, roughPath)) {
            Warn(context.optionalWarnings);
        }

        uint32_t images[4]{};
        if (hasBase) {
            images[0] = ResolveTexture(writer, context, scene, basePath.C_Str(),
                                       ColorSpaceId::Srgb, TextureSemantic::Color, imageIds);
            if (context.error != ImportErrorCode::None || !images[0]) return false;
        }
        if (hasNormal) {
            images[2] = ResolveTexture(writer, context, scene, normalPath.C_Str(),
                                       ColorSpaceId::Linear, TextureSemantic::Normal, imageIds);
            if (context.error != ImportErrorCode::None || !images[2]) return false;
        }
        if (hasEmissive) {
            images[3] = ResolveTexture(writer, context, scene, emissivePath.C_Str(),
                                       ColorSpaceId::Srgb, TextureSemantic::Emissive, imageIds);
            if (context.error != ImportErrorCode::None || !images[3]) return false;
        }

        // Stable, content-derived de-duplication key. Identical converted
        // materials share one chunk regardless of source material order.
        std::string key(reinterpret_cast<const char*>(&payload), sizeof(payload));
        for (uint32_t image : images) {
            key.push_back(char(image & 0xff));
            key.push_back(char((image >> 8) & 0xff));
            key.push_back(char((image >> 16) & 0xff));
            key.push_back(char((image >> 24) & 0xff));
        }
        if (auto found = deduplicated.find(key); found != deduplicated.end()) {
            materialIds.emplace(index, found->second);
            continue;
        }

        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::Material;
        descriptor.chunkId = writer.NextId();
        for (unsigned slot = 0; slot < 4; ++slot) descriptor.dependencyIds[slot] = images[slot];
        descriptor.dependencyCount = uint32_t(std::count_if(std::begin(images), std::end(images),
            [](uint32_t id) { return id != 0; }));
        if (!writer.Add(descriptor, ChunkBytes(payload))) return false;
        deduplicated.emplace(std::move(key), descriptor.chunkId);
        materialIds.emplace(index, descriptor.chunkId);
    }
    return true;
}

bool Normalize(const aiVector3D& value, float out[3])
{
    const double length = std::sqrt(double(value.x) * value.x + double(value.y) * value.y
        + double(value.z) * value.z);
    if (!Finite(length) || length <= 1e-20) {
        out[0] = 0; out[1] = 0; out[2] = 1;
        return false;
    }
    out[0] = float(value.x / length);
    out[1] = float(value.y / length);
    out[2] = float(value.z / length);
    return true;
}

void GenerateTriangleTangent(VertexPositionNormalUv0TangentColorF32* vertices)
{
    const float e1[3]{vertices[1].px - vertices[0].px, vertices[1].py - vertices[0].py,
                      vertices[1].pz - vertices[0].pz};
    const float e2[3]{vertices[2].px - vertices[0].px, vertices[2].py - vertices[0].py,
                      vertices[2].pz - vertices[0].pz};
    const float du1 = vertices[1].u - vertices[0].u, dv1 = vertices[1].v - vertices[0].v;
    const float du2 = vertices[2].u - vertices[0].u, dv2 = vertices[2].v - vertices[0].v;
    const float determinant = du1 * dv2 - du2 * dv1;
    float tangent[3]{1, 0, 0};
    float handedness = 1.0f;
    if (std::abs(determinant) > 1e-12f) {
        const float inverse = 1.0f / determinant;
        for (unsigned axis = 0; axis < 3; ++axis)
            tangent[axis] = (e1[axis] * dv2 - e2[axis] * dv1) * inverse;
        const float length = std::sqrt(tangent[0] * tangent[0] + tangent[1] * tangent[1]
            + tangent[2] * tangent[2]);
        if (length > 1e-12f) for (float& value : tangent) value /= length;
        const float bitangent[3]{(e2[0] * du1 - e1[0] * du2) * inverse,
                                 (e2[1] * du1 - e1[1] * du2) * inverse,
                                 (e2[2] * du1 - e1[2] * du2) * inverse};
        const float cross[3]{vertices[0].ny * tangent[2] - vertices[0].nz * tangent[1],
                             vertices[0].nz * tangent[0] - vertices[0].nx * tangent[2],
                             vertices[0].nx * tangent[1] - vertices[0].ny * tangent[0]};
        handedness = cross[0] * bitangent[0] + cross[1] * bitangent[1]
            + cross[2] * bitangent[2] < 0 ? -1.0f : 1.0f;
    }
    for (unsigned i = 0; i < 3; ++i) {
        vertices[i].tx = tangent[0]; vertices[i].ty = tangent[1];
        vertices[i].tz = tangent[2]; vertices[i].tw = handedness;
    }
}

struct GeometryChunk {
    ChunkDescriptor descriptor{};
    std::vector<VertexPositionNormalUv0TangentColorF32> vertices;
    std::vector<uint32_t> indices;
    bool hasOrigin = false;
    bool hasUv = false;
    bool hasColor = false;
};

bool AddVertex(GeometryChunk& chunk, const aiMesh& mesh, unsigned vertexIndex,
               const aiVector3D& worldPosition, const aiVector3D& worldNormal,
               const aiVector3D& faceNormal)
{
    if (vertexIndex >= mesh.mNumVertices) return false;
    if (!Finite(worldPosition) || !Finite(worldNormal) || !Finite(faceNormal)) return false;
    if (!chunk.hasOrigin) {
        chunk.descriptor.origin[0] = worldPosition.x;
        chunk.descriptor.origin[1] = worldPosition.y;
        chunk.descriptor.origin[2] = worldPosition.z;
        chunk.hasOrigin = true;
    }
    VertexPositionNormalUv0TangentColorF32 vertex{};
    vertex.px = SafeFloat(double(worldPosition.x) - chunk.descriptor.origin[0]);
    vertex.py = SafeFloat(double(worldPosition.y) - chunk.descriptor.origin[1]);
    vertex.pz = SafeFloat(double(worldPosition.z) - chunk.descriptor.origin[2]);
    float normalized[3];
    if (!Normalize(worldNormal, normalized)) Normalize(faceNormal, normalized);
    vertex.nx = normalized[0]; vertex.ny = normalized[1]; vertex.nz = normalized[2];
    if (mesh.HasTextureCoords(0)) {
        const aiVector3D& uv = mesh.mTextureCoords[0][vertexIndex];
        if (Finite(uv.x) && Finite(uv.y)) {
            vertex.u = SafeFloat(uv.x);
            vertex.v = SafeFloat(uv.y);
            chunk.hasUv = true;
        }
    }
    if (mesh.HasVertexColors(0)) {
        const aiColor4D& color = mesh.mColors[0][vertexIndex];
        if (Finite(color.r) && Finite(color.g) && Finite(color.b) && Finite(color.a)) {
            vertex.r = Saturate(color.r); vertex.g = Saturate(color.g);
            vertex.b = Saturate(color.b); vertex.a = Saturate(color.a);
            chunk.hasColor = true;
        }
    }
    vertex.tx = 1; vertex.tw = 1;
    for (unsigned axis = 0; axis < 3; ++axis) {
        const float value = (&vertex.px)[axis];
        if (chunk.vertices.empty()) {
            chunk.descriptor.localMin[axis] = chunk.descriptor.localMax[axis] = value;
        } else {
            chunk.descriptor.localMin[axis] = (std::min)(chunk.descriptor.localMin[axis], value);
            chunk.descriptor.localMax[axis] = (std::max)(chunk.descriptor.localMax[axis], value);
        }
    }
    chunk.indices.push_back(uint32_t(chunk.vertices.size()));
    chunk.vertices.push_back(vertex);
    return true;
}

bool FlushGeometry(BoundedChunkWriter& writer, GeometryChunk& chunk, uint32_t materialId,
                   uint32_t meshId, uint32_t nodeId, uint32_t sourceFirst, uint64_t& totalTriangles,
                   uint64_t& totalVertices, const AssimpImportOptions& options,
                   ImportErrorCode& error)
{
    if (chunk.indices.empty()) return true;
    if (totalTriangles + chunk.indices.size() / 3 > options.maxTriangles
        || totalVertices + chunk.vertices.size() > options.maxVertices) {
        error = ImportErrorCode::ResourceLimit;
        return false;
    }
    chunk.descriptor.topology = ChunkTopology::TriangleList;
    chunk.descriptor.vertexLayoutId = uint32_t(VertexLayoutId::PositionNormalUv0TangentColor_F32);
    chunk.descriptor.vertexCount = uint32_t(chunk.vertices.size());
    chunk.descriptor.indexCount = uint32_t(chunk.indices.size());
    chunk.descriptor.chunkId = writer.NextId();
    chunk.descriptor.meshId = meshId;
    chunk.descriptor.nodeId = nodeId;
    chunk.descriptor.boundsState = BoundsState::Verified;
    chunk.descriptor.geometryFlags = kGeometryDeindexed
        | (chunk.hasUv ? kGeometryHasUv0 : 0u) | (chunk.hasColor ? kGeometryHasColors : 0u);
    chunk.descriptor.sourceRangeOffset = (uint64_t(meshId) << 32) | sourceFirst;
    chunk.descriptor.sourceRangeLength = chunk.indices.size();
    if (materialId) {
        chunk.descriptor.dependencyIds[0] = materialId;
        chunk.descriptor.dependencyCount = 1;
    }
    totalTriangles += chunk.indices.size() / 3;
    totalVertices += chunk.vertices.size();
    if (!writer.Add(chunk.descriptor, ChunkBytes(chunk.vertices), ChunkBytes(chunk.indices))) {
        error = writer.Error();
        return false;
    }
    chunk = {};
    return true;
}

struct GeometryState {
    BoundedChunkWriter* writer = nullptr;
    const aiScene* scene = nullptr;
    ImportContext* context = nullptr;
    const AssimpImportOptions* options = nullptr;
    std::map<uint32_t, uint32_t> materialIds;
    uint32_t chunkTriangleLimit = 0;
    uint64_t totalTriangles = 0;
    uint64_t totalVertices = 0;
};

bool EmitMesh(const aiMesh& mesh, uint32_t meshId, uint32_t nodeId,
              const aiMatrix4x4& world, GeometryState& state)
{
    const aiMatrix3x3 upper(world);
    aiMatrix3x3 normalMatrix = upper;
    normalMatrix.Inverse();
    normalMatrix.Transpose();

    uint32_t materialIndex = mesh.mMaterialIndex;
    if (materialIndex >= state.scene->mNumMaterials) materialIndex = 0;
    auto material = state.materialIds.find(materialIndex);
    const uint32_t materialId = material == state.materialIds.end() ? 0 : material->second;

    GeometryChunk chunk;
    uint32_t sourceFirst = 0;
    for (uint32_t faceIndex = 0; faceIndex < mesh.mNumFaces; ++faceIndex) {
        if ((faceIndex & 1023u) == 0 && state.context->Cancelled()) {
            state.context->error = ImportErrorCode::Cancelled;
            return false;
        }
        const aiFace& face = mesh.mFaces[faceIndex];
        if (face.mNumIndices != 3) {
            // Lines/points/leftover polygons are not representable as the
            // normalized triangle contract; fail closed rather than fabricate
            // triangles. A mesh with no triangles at all is a bounded warning.
            Warn(state.context->optionalWarnings);
            continue;
        }
        aiVector3D positions[3];
        aiVector3D normals[3];
        for (unsigned corner = 0; corner < 3; ++corner) {
            const unsigned index = face.mIndices[corner];
            if (index >= mesh.mNumVertices) {
                state.context->error = ImportErrorCode::MalformedData;
                return false;
            }
            positions[corner] = world * mesh.mVertices[index];
            if (mesh.HasNormals()) normals[corner] = normalMatrix * mesh.mNormals[index];
            else normals[corner] = aiVector3D(0, 0, 1);
            if (!Finite(positions[corner]) || !Finite(normals[corner])) {
                state.context->error = ImportErrorCode::MalformedData;
                return false;
            }
        }
        aiVector3D faceNormal(0, 0, 1);
        if (!mesh.HasNormals()) {
            faceNormal = (positions[1] - positions[0]) ^ (positions[2] - positions[0]);
            normals[0] = normals[1] = normals[2] = faceNormal;
        }
        if (chunk.indices.size() / 3 >= state.chunkTriangleLimit) {
            if (!FlushGeometry(*state.writer, chunk, materialId, meshId, nodeId, sourceFirst,
                               state.totalTriangles, state.totalVertices, *state.options,
                               state.context->error)) return false;
            sourceFirst = faceIndex * 3;
        }
        const size_t firstVertex = chunk.vertices.size();
        for (unsigned corner = 0; corner < 3; ++corner) {
            if (!AddVertex(chunk, mesh, face.mIndices[corner], positions[corner],
                           normals[corner], faceNormal)) {
                state.context->error = ImportErrorCode::MalformedData;
                return false;
            }
        }
        GenerateTriangleTangent(chunk.vertices.data() + firstVertex);
    }

    return FlushGeometry(*state.writer, chunk, materialId, meshId, nodeId, sourceFirst,
                         state.totalTriangles, state.totalVertices, *state.options,
                         state.context->error);
}

bool WalkNode(const aiNode& node, const aiMatrix4x4& parent, uint32_t depth,
              uint32_t& nextOrdinal, GeometryState& state)
{
    if (state.context->Cancelled()) {
        state.context->error = ImportErrorCode::Cancelled;
        return false;
    }
    if (depth > kMaxSceneHierarchyDepth || nextOrdinal >= state.options->maxObjects) {
        state.context->error = ImportErrorCode::ResourceLimit;
        return false;
    }
    const uint32_t nodeOrdinal = nextOrdinal++;
    const aiMatrix4x4 world = parent * node.mTransformation;
    for (uint32_t index = 0; index < node.mNumMeshes; ++index) {
        const uint32_t meshId = node.mMeshes[index];
        if (meshId >= state.scene->mNumMeshes) {
            state.context->error = ImportErrorCode::MalformedData;
            return false;
        }
        const aiMesh* mesh = state.scene->mMeshes[meshId];
        if (!mesh) continue;
        if ((mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0) {
            Warn(state.context->optionalWarnings);
            continue;
        }
        if (!EmitMesh(*mesh, meshId, nodeOrdinal, world, state)) return false;
    }
    for (uint32_t index = 0; index < node.mNumChildren; ++index) {
        const aiNode* child = node.mChildren[index];
        if (!child) continue;
        if (!WalkNode(*child, world, depth + 1, nextOrdinal, state)) return false;
    }
    return true;
}

uint64_t CountNodes(const aiNode* node, uint32_t depth)
{
    if (!node || depth > kMaxSceneHierarchyDepth) return uint64_t(kMaxSceneHierarchyDepth) + 1;
    uint64_t count = 1;
    for (uint32_t index = 0; index < node->mNumChildren; ++index) {
        count += CountNodes(node->mChildren[index], depth + 1);
        if (count > 1'000'000) break;
    }
    return count;
}

ImportErrorCode MapLoadError(const Assimp::Importer& importer)
{
    const char* error = importer.GetErrorString();
    if (!error) return ImportErrorCode::MalformedData;
    if (std::strstr(error, "memory") || std::strstr(error, "Memory")
        || std::strstr(error, "allocation")) return ImportErrorCode::OutOfMemory;
    if (std::strstr(error, "No suitable reader") || std::strstr(error, "unknown file format")
        || std::strstr(error, "Unknown file format") || std::strstr(error, "not supported"))
        return ImportErrorCode::UnsupportedFormat;
    return ImportErrorCode::MalformedData;
}

} // namespace

bool IsSafeAssimpSidecarReference(std::string_view reference) noexcept
{
    return IsSafeReference(reference, true);
}

AssimpImportOutcome ImportAssimp(std::span<const std::byte> sourceBytes,
                                 std::span<std::byte> destination,
                                 uint64_t generationId, uint32_t maxChunkCount,
                                 ChunkBatchSink* batchSink,
                                 const AssimpImportOptions& options)
{
    if (sourceBytes.empty()) return ImportErrorCode::EmptyGeometry;
    if (sourceBytes.size() > kTierBPrimarySourceBytes) return ImportErrorCode::PrimarySourceLimit;
    if (!IsKnownAssimpFormat(options.format)) return ImportErrorCode::UnsupportedFormat;
    if (!TierBScratchLimit()) return ImportErrorCode::ScratchLimit;

    AssimpImportOptions effective = options;
    if (options.maxObjects == 0 || options.maxMaterials == 0 || options.maxTriangles == 0
        || options.maxVertices == 0) return ImportErrorCode::ResourceLimit;
    if (effective.format == AssimpFormat::ThreeDs) {
        if (!PassesBinaryAdmission(sourceBytes, effective.format))
            return ImportErrorCode::MalformedData;
    } else if (IsXmlAssimpFormat(effective.format)) {
        const ImportErrorCode admission = XmlAdmission(sourceBytes, effective.format, effective);
        if (admission != ImportErrorCode::None) return admission;
    } else if (effective.format == AssimpFormat::Off) {
        const ImportErrorCode admission = OffAdmission(sourceBytes, effective);
        if (admission != ImportErrorCode::None) return admission;
    } else if (!PassesTextAdmission(sourceBytes, effective.format)) {
        return ImportErrorCode::MalformedData;
    }

    ImportContext context;
    context.options = &effective;
    const FormatInfo format = InfoFor(effective.format);

    try {
        Assimp::Importer importer;
        importer.SetIOHandler(new BrokerIOSystem(context));
        importer.SetProgressHandler(new CancelProgressHandler(context));
        importer.SetPropertyBool(AI_CONFIG_GLOB_MEASURE_TIME, false);
        importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, 0);

        constexpr unsigned int kFlags = aiProcess_Triangulate
            | aiProcess_JoinIdenticalVertices | aiProcess_GenNormals
            | aiProcess_SortByPType | aiProcess_ValidateDataStructure;
        const aiScene* scene = importer.ReadFileFromMemory(
            sourceBytes.data(), sourceBytes.size(), kFlags, format.hint);
        if (context.error != ImportErrorCode::None) return context.error;
        if (!scene) {
            if (context.Cancelled()) return ImportErrorCode::Cancelled;
            return MapLoadError(importer);
        }
        if (CountNodes(scene->mRootNode, 0) > effective.maxObjects
            || scene->mNumMeshes > effective.maxObjects
            || scene->mNumMaterials > effective.maxMaterials) {
            return ImportErrorCode::ResourceLimit;
        }

        uint64_t declaredTriangles = 0, declaredVertices = 0;
        for (uint32_t index = 0; index < scene->mNumMeshes; ++index) {
            const aiMesh* mesh = scene->mMeshes[index];
            if (!mesh) continue;
            declaredVertices += mesh->mNumVertices;
            if (mesh->mNumFaces > effective.maxTriangles) return ImportErrorCode::ResourceLimit;
            declaredTriangles += mesh->mNumFaces;
            if (declaredTriangles > effective.maxTriangles
                || declaredVertices > effective.maxVertices) {
                return ImportErrorCode::ResourceLimit;
            }
        }

        SceneMetadata metadata{};
        metadata.generationId = generationId;
        metadata.format = format.wire;
        metadata.meshCount = scene->mNumMeshes;
        metadata.nodeCount = uint32_t((std::min<uint64_t>)(CountNodes(scene->mRootNode, 0),
                                                           effective.maxObjects));
        // Assimp compatibility formats author no length unit this adapter can
        // verify. Zero is the explicit "unspecified" value; the viewer never
        // treats it as metres.
        metadata.metersPerUnit = 0.0;
        metadata.upAxis = format.upAxis;
        metadata.animationCount = 0;

        BoundedChunkWriter writer(destination, generationId, maxChunkCount, metadata, batchSink);
        constexpr uint64_t kNormalizedTriangleBytes =
            3 * sizeof(VertexPositionNormalUv0TangentColorF32) + 3 * sizeof(uint32_t);
        if (destination.size() <= kSectionHeaderSize + kChunkDescriptorSize)
            return ImportErrorCode::ResourceLimit;
        const uint32_t chunkTriangleLimit = uint32_t((std::min<uint64_t>)(kChunkTriangles,
            (destination.size() - kSectionHeaderSize - kChunkDescriptorSize) / kNormalizedTriangleBytes));
        if (!chunkTriangleLimit) return ImportErrorCode::ResourceLimit;

        std::map<uint32_t, uint32_t> materialIds;
        if (!EmitMaterials(*scene, writer, context, materialIds))
            return context.error != ImportErrorCode::None ? context.error : writer.Error();

        GeometryState geometry;
        geometry.writer = &writer;
        geometry.scene = scene;
        geometry.context = &context;
        geometry.options = &effective;
        geometry.materialIds = std::move(materialIds);
        geometry.chunkTriangleLimit = chunkTriangleLimit;
        uint32_t nextNodeOrdinal = 0;
        if (!WalkNode(*scene->mRootNode, aiMatrix4x4(), 0, nextNodeOrdinal, geometry))
            return context.error != ImportErrorCode::None ? context.error : writer.Error();
        if (!geometry.totalTriangles) return ImportErrorCode::EmptyGeometry;

        // Ignored optional content is a bounded degradation record, never a
        // silent claim that it was rendered.
        if (scene->mNumAnimations) Warn(context.optionalWarnings);
        if (scene->mNumCameras) Warn(context.optionalWarnings);
        if (scene->mNumLights) Warn(context.optionalWarnings);

        if (context.textureWarnings) {
            ChunkDescriptor descriptor{};
            descriptor.topology = ChunkTopology::TextureWarning;
            descriptor.chunkId = writer.NextId();
            if (!writer.Add(descriptor, ChunkBytes(context.textureWarnings))) return writer.Error();
        }
        if (context.textureWarnings || context.optionalWarnings) {
            ImportStatusPayload status{};
            status.textureWarnings = context.textureWarnings;
            status.optionalFeatureWarnings = context.optionalWarnings;
            ChunkDescriptor descriptor{};
            descriptor.topology = ChunkTopology::ImportStatus;
            descriptor.chunkId = writer.NextId();
            if (!writer.Add(descriptor, ChunkBytes(status))) return writer.Error();
        }
        if (!writer.Complete()) return writer.Error();
        return AssimpImportResult{writer.Count(), writer.Length()};
    } catch (const std::bad_alloc&) {
        return ImportErrorCode::OutOfMemory;
    } catch (...) {
        return ImportErrorCode::InternalImporterFailure;
    }
}

} // namespace import_worker
