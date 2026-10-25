#include "framework.h"
#include "Model.h"

#include <charconv>
#include <cstring>
#include <sstream>

using namespace DirectX;

namespace
{
constexpr std::uint32_t kGlbMagic = 0x46546C67;
constexpr std::uint32_t kJsonChunk = 0x4E4F534A;
constexpr std::uint32_t kBinChunk = 0x004E4942;
constexpr std::uint64_t kMaxFileBytes = 1024ull * 1024ull * 1024ull;
constexpr std::size_t kMaxJsonBytes = 64ull * 1024ull * 1024ull;
constexpr std::size_t kMaxAccessors = 100000;
constexpr std::size_t kMaxNodes = 100000;
constexpr std::size_t kMaxOutputVertices = 12000000;
constexpr std::size_t kMaxOutputIndices = 36000000;

std::uint32_t ReadU32(const std::uint8_t* value)
{
    std::uint32_t result = 0;
    std::memcpy(&result, value, sizeof(result));
    return result;
}

class MappedFile
{
public:
    ~MappedFile()
    {
        if (data_)
        {
            UnmapViewOfFile(data_);
        }
        if (mapping_)
        {
            CloseHandle(mapping_);
        }
        if (file_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file_);
        }
    }

    bool Open(const std::wstring& path, std::wstring& error)
    {
        file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (file_ == INVALID_HANDLE_VALUE)
        {
            error = L"The file could not be opened (Windows error " + std::to_wstring(GetLastError()) + L").";
            return false;
        }

        if (GetFileType(file_) != FILE_TYPE_DISK)
        {
            error = L"The selected item is not a regular local file.";
            return false;
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_, &size) || size.QuadPart <= 0)
        {
            error = L"The file is empty or its size could not be read.";
            return false;
        }
        if (static_cast<std::uint64_t>(size.QuadPart) > kMaxFileBytes)
        {
            error = L"This vertical slice currently limits GLB files to 1 GiB.";
            return false;
        }

        size_ = static_cast<std::size_t>(size.QuadPart);
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_)
        {
            error = L"A read-only view of the file could not be created.";
            return false;
        }
        data_ = static_cast<const std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (!data_)
        {
            error = L"The file could not be mapped into memory.";
            return false;
        }
        return true;
    }

    const std::uint8_t* Data() const { return data_; }
    std::size_t Size() const { return size_; }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

struct JsonValue
{
    enum class Type { Null, Boolean, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::unordered_map<std::string, JsonValue> object;

    const JsonValue* Find(std::string_view name) const
    {
        if (type != Type::Object)
        {
            return nullptr;
        }
        const auto found = object.find(std::string(name));
        return found == object.end() ? nullptr : &found->second;
    }

    int Integer(int fallback = -1) const
    {
        if (type != Type::Number || !std::isfinite(number) || std::floor(number) != number ||
            number < static_cast<double>(std::numeric_limits<int>::min()) ||
            number > static_cast<double>(std::numeric_limits<int>::max()))
        {
            return fallback;
        }
        return static_cast<int>(number);
    }
};

class JsonParser
{
public:
    JsonParser(const char* begin, const char* end, const std::shared_ptr<std::atomic_bool>& cancel)
        : begin_(begin), cursor_(begin), end_(end), cancel_(cancel)
    {
    }

    bool Parse(JsonValue& output, std::wstring& error)
    {
        SkipSpace();
        if (!ParseValue(output, 0))
        {
            std::wostringstream stream;
            stream << L"Invalid GLB JSON near byte " << static_cast<std::size_t>(cursor_ - begin_)
                << L": " << error_;
            error = stream.str();
            return false;
        }
        SkipSpace();
        if (cursor_ != end_)
        {
            error = L"The GLB JSON contains unexpected trailing data.";
            return false;
        }
        return true;
    }

private:
    void SkipSpace()
    {
        while (cursor_ != end_ && (*cursor_ == ' ' || *cursor_ == '\t' || *cursor_ == '\r' || *cursor_ == '\n'))
        {
            ++cursor_;
        }
    }

    bool ParseValue(JsonValue& output, int depth)
    {
        if (depth > 128)
        {
            return Fail(L"JSON nesting is too deep");
        }
        if (cancel_ && cancel_->load(std::memory_order_relaxed))
        {
            return Fail(L"opening was cancelled");
        }
        if (cursor_ == end_)
        {
            return Fail(L"unexpected end of JSON");
        }

        switch (*cursor_)
        {
        case '{': return ParseObject(output, depth + 1);
        case '[': return ParseArray(output, depth + 1);
        case '"':
            output.type = JsonValue::Type::String;
            return ParseString(output.string);
        case 't': return ParseLiteral("true", JsonValue::Type::Boolean, output, true);
        case 'f': return ParseLiteral("false", JsonValue::Type::Boolean, output, false);
        case 'n': return ParseLiteral("null", JsonValue::Type::Null, output, false);
        default: return ParseNumber(output);
        }
    }

    bool ParseObject(JsonValue& output, int depth)
    {
        output.type = JsonValue::Type::Object;
        ++cursor_;
        SkipSpace();
        if (cursor_ != end_ && *cursor_ == '}')
        {
            ++cursor_;
            return true;
        }
        while (cursor_ != end_)
        {
            if (*cursor_ != '"')
            {
                return Fail(L"expected an object property name");
            }
            std::string key;
            if (!ParseString(key))
            {
                return false;
            }
            SkipSpace();
            if (cursor_ == end_ || *cursor_++ != ':')
            {
                return Fail(L"expected ':' after a property name");
            }
            SkipSpace();
            JsonValue value;
            if (!ParseValue(value, depth))
            {
                return false;
            }
            output.object.insert_or_assign(std::move(key), std::move(value));
            SkipSpace();
            if (cursor_ != end_ && *cursor_ == '}')
            {
                ++cursor_;
                return true;
            }
            if (cursor_ == end_ || *cursor_++ != ',')
            {
                return Fail(L"expected ',' between object properties");
            }
            SkipSpace();
        }
        return Fail(L"unterminated object");
    }

    bool ParseArray(JsonValue& output, int depth)
    {
        output.type = JsonValue::Type::Array;
        ++cursor_;
        SkipSpace();
        if (cursor_ != end_ && *cursor_ == ']')
        {
            ++cursor_;
            return true;
        }
        while (cursor_ != end_)
        {
            JsonValue value;
            if (!ParseValue(value, depth))
            {
                return false;
            }
            output.array.push_back(std::move(value));
            SkipSpace();
            if (cursor_ != end_ && *cursor_ == ']')
            {
                ++cursor_;
                return true;
            }
            if (cursor_ == end_ || *cursor_++ != ',')
            {
                return Fail(L"expected ',' between array values");
            }
            SkipSpace();
        }
        return Fail(L"unterminated array");
    }

    bool ParseString(std::string& output)
    {
        ++cursor_;
        while (cursor_ != end_)
        {
            const unsigned char c = static_cast<unsigned char>(*cursor_++);
            if (c == '"')
            {
                return true;
            }
            if (c < 0x20)
            {
                return Fail(L"control character in a string");
            }
            if (c != '\\')
            {
                output.push_back(static_cast<char>(c));
                continue;
            }
            if (cursor_ == end_)
            {
                return Fail(L"unterminated string escape");
            }
            const char escaped = *cursor_++;
            switch (escaped)
            {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u':
                if (end_ - cursor_ < 4)
                {
                    return Fail(L"short Unicode escape");
                }
                for (int index = 0; index < 4; ++index)
                {
                    const char hex = cursor_[index];
                    if (!((hex >= '0' && hex <= '9') || (hex >= 'a' && hex <= 'f') || (hex >= 'A' && hex <= 'F')))
                    {
                        return Fail(L"invalid Unicode escape");
                    }
                }
                cursor_ += 4;
                output.push_back('?');
                break;
            default: return Fail(L"invalid string escape");
            }
        }
        return Fail(L"unterminated string");
    }

    bool ParseLiteral(const char* literal, JsonValue::Type type, JsonValue& output, bool boolean)
    {
        const std::size_t length = std::strlen(literal);
        if (static_cast<std::size_t>(end_ - cursor_) < length || std::memcmp(cursor_, literal, length) != 0)
        {
            return Fail(L"invalid literal");
        }
        cursor_ += length;
        output.type = type;
        output.boolean = boolean;
        return true;
    }

    bool ParseNumber(JsonValue& output)
    {
        const char* start = cursor_;
        if (cursor_ != end_ && *cursor_ == '-') ++cursor_;
        if (cursor_ == end_) return Fail(L"invalid number");
        if (*cursor_ == '0')
        {
            ++cursor_;
        }
        else
        {
            if (*cursor_ < '1' || *cursor_ > '9') return Fail(L"invalid number");
            while (cursor_ != end_ && *cursor_ >= '0' && *cursor_ <= '9') ++cursor_;
        }
        if (cursor_ != end_ && *cursor_ == '.')
        {
            ++cursor_;
            if (cursor_ == end_ || *cursor_ < '0' || *cursor_ > '9') return Fail(L"invalid fraction");
            while (cursor_ != end_ && *cursor_ >= '0' && *cursor_ <= '9') ++cursor_;
        }
        if (cursor_ != end_ && (*cursor_ == 'e' || *cursor_ == 'E'))
        {
            ++cursor_;
            if (cursor_ != end_ && (*cursor_ == '+' || *cursor_ == '-')) ++cursor_;
            if (cursor_ == end_ || *cursor_ < '0' || *cursor_ > '9') return Fail(L"invalid exponent");
            while (cursor_ != end_ && *cursor_ >= '0' && *cursor_ <= '9') ++cursor_;
        }

        std::string text(start, cursor_);
        char* parsedEnd = nullptr;
        output.number = std::strtod(text.c_str(), &parsedEnd);
        if (parsedEnd != text.c_str() + text.size() || !std::isfinite(output.number))
        {
            return Fail(L"invalid or non-finite number");
        }
        output.type = JsonValue::Type::Number;
        return true;
    }

    bool Fail(const wchar_t* message)
    {
        error_ = message;
        return false;
    }

    const char* begin_ = nullptr;
    const char* cursor_ = nullptr;
    const char* end_ = nullptr;
    std::shared_ptr<std::atomic_bool> cancel_;
    std::wstring error_;
};

struct BufferView
{
    std::size_t offset = 0;
    std::size_t length = 0;
    std::size_t stride = 0;
    int buffer = 0;
};

struct Accessor
{
    int view = -1;
    std::size_t offset = 0;
    std::size_t count = 0;
    int componentType = 0;
    int components = 0;
    bool normalized = false;
};

bool GetSize(const JsonValue* value, std::size_t& output)
{
    if (!value || value->type != JsonValue::Type::Number || !std::isfinite(value->number) ||
        value->number < 0.0 || std::floor(value->number) != value->number ||
        value->number > static_cast<double>(std::numeric_limits<std::size_t>::max()))
    {
        return false;
    }
    output = static_cast<std::size_t>(value->number);
    return true;
}

int ComponentCount(const JsonValue* type)
{
    if (!type || type->type != JsonValue::Type::String) return 0;
    if (type->string == "SCALAR") return 1;
    if (type->string == "VEC2") return 2;
    if (type->string == "VEC3") return 3;
    if (type->string == "VEC4") return 4;
    if (type->string == "MAT2") return 4;
    if (type->string == "MAT3") return 9;
    if (type->string == "MAT4") return 16;
    return 0;
}

std::size_t ComponentSize(int componentType)
{
    switch (componentType)
    {
    case 5120:
    case 5121: return 1;
    case 5122:
    case 5123: return 2;
    case 5125:
    case 5126: return 4;
    default: return 0;
    }
}

bool CheckedAdd(std::size_t left, std::size_t right, std::size_t& output)
{
    if (left > std::numeric_limits<std::size_t>::max() - right) return false;
    output = left + right;
    return true;
}

bool CheckedMultiply(std::size_t left, std::size_t right, std::size_t& output)
{
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) return false;
    output = left * right;
    return true;
}

class GlbImporter
{
public:
    GlbImporter(const JsonValue& root, const std::uint8_t* binary, std::size_t binarySize,
        std::shared_ptr<std::atomic_bool> cancel)
        : root_(root), binary_(binary), binarySize_(binarySize), cancel_(std::move(cancel))
    {
    }

    bool Import(ModelData& model, std::wstring& error)
    {
        model_ = &model;
        if (!ValidateRequiredExtensions(error))
        {
            return false;
        }
        if (!ParseTables(error) || !ParseMaterials(error))
        {
            return false;
        }
        if (const JsonValue* animations = root_.Find("animations");
            animations && animations->type == JsonValue::Type::Array)
        {
            model_->stats.animationCount = static_cast<int>(animations->array.size());
        }
        if (const JsonValue* skins = root_.Find("skins"); skins && skins->type == JsonValue::Type::Array)
        {
            model_->stats.skinCount = static_cast<int>(skins->array.size());
            for (const JsonValue& skin : skins->array)
            {
                if (const JsonValue* joints = skin.Find("joints"); joints && joints->type == JsonValue::Type::Array)
                {
                    model_->stats.boneCount += static_cast<int>(joints->array.size());
                }
            }
        }
        const JsonValue* meshes = root_.Find("meshes");
        if (!meshes || meshes->type != JsonValue::Type::Array || meshes->array.empty())
        {
            error = L"The GLB does not contain any meshes.";
            return false;
        }

        meshes_ = meshes;
        const JsonValue* nodes = root_.Find("nodes");
        if (nodes && nodes->type == JsonValue::Type::Array)
        {
            if (nodes->array.size() > kMaxNodes)
            {
                error = L"The GLB contains more nodes than this preview can safely open.";
                return false;
            }
            nodes_ = nodes;
            model.stats.nodeCount = static_cast<int>(nodes->array.size());
            visitState_.resize(nodes->array.size());

            std::vector<int> roots;
            const JsonValue* scenes = root_.Find("scenes");
            if (scenes && scenes->type == JsonValue::Type::Array && !scenes->array.empty())
            {
                int sceneIndex = 0;
                if (const JsonValue* selected = root_.Find("scene")) sceneIndex = selected->Integer(0);
                if (sceneIndex < 0 || static_cast<std::size_t>(sceneIndex) >= scenes->array.size())
                {
                    error = L"The GLB selects a scene that does not exist.";
                    return false;
                }
                const JsonValue* sceneNodes = scenes->array[sceneIndex].Find("nodes");
                if (sceneNodes && sceneNodes->type == JsonValue::Type::Array)
                {
                    for (const JsonValue& value : sceneNodes->array) roots.push_back(value.Integer());
                }
            }

            if (roots.empty())
            {
                std::vector<bool> child(nodes->array.size());
                for (const JsonValue& node : nodes->array)
                {
                    const JsonValue* children = node.Find("children");
                    if (!children || children->type != JsonValue::Type::Array) continue;
                    for (const JsonValue& value : children->array)
                    {
                        const int index = value.Integer();
                        if (index >= 0 && static_cast<std::size_t>(index) < child.size()) child[index] = true;
                    }
                }
                for (std::size_t index = 0; index < child.size(); ++index)
                {
                    if (!child[index]) roots.push_back(static_cast<int>(index));
                }
            }

            for (const int root : roots)
            {
                if (!VisitNode(root, XMMatrixIdentity(), 0, error)) return false;
            }
        }
        else
        {
            for (std::size_t index = 0; index < meshes->array.size(); ++index)
            {
                if (!AppendMesh(static_cast<int>(index), XMMatrixIdentity(), error)) return false;
            }
        }

        if (model.vertices.empty() || model.indices.empty())
        {
            error = L"The GLB contains no supported triangle geometry.";
            return false;
        }
        model.triangleCount = model.indices.size() / 3;
        if (ignoredTextures_)
        {
            model.warning = L"Embedded textures are not shown in this GLB-only preview slice.";
        }
        if (skippedPrimitives_)
        {
            if (!model.warning.empty()) model.warning += L" ";
            model.warning += L"Non-triangle or unsupported primitives were skipped.";
        }
        return true;
    }

private:
    bool Cancelled() const
    {
        return cancel_ && cancel_->load(std::memory_order_relaxed);
    }

    bool ValidateRequiredExtensions(std::wstring& error) const
    {
        const JsonValue* required = root_.Find("extensionsRequired");
        if (!required) return true;
        if (required->type != JsonValue::Type::Array)
        {
            error = L"The required-extension list is invalid.";
            return false;
        }
        for (const JsonValue& extension : required->array)
        {
            if (extension.type != JsonValue::Type::String)
            {
                error = L"The required-extension list is invalid.";
                return false;
            }
        }
        if (!required->array.empty())
        {
            const std::string& extension = required->array.front().string;
            std::wstring name(extension.begin(), extension.end());
            error = L"This GLB requires " + name + L", which is deferred in this vertical slice.";
            return false;
        }
        return true;
    }

    bool ParseTables(std::wstring& error)
    {
        const JsonValue* views = root_.Find("bufferViews");
        if (views && views->type == JsonValue::Type::Array)
        {
            if (views->array.size() > kMaxAccessors)
            {
                error = L"The GLB contains too many buffer views.";
                return false;
            }
            for (const JsonValue& value : views->array)
            {
                BufferView view;
                if (const JsonValue* buffer = value.Find("buffer")) view.buffer = buffer->Integer();
                if (view.buffer != 0 || !GetSize(value.Find("byteLength"), view.length))
                {
                    error = L"A buffer view is invalid or does not refer to the embedded GLB buffer.";
                    return false;
                }
                if (const JsonValue* offset = value.Find("byteOffset"))
                {
                    if (!GetSize(offset, view.offset)) { error = L"A buffer view has an invalid byte offset."; return false; }
                }
                if (const JsonValue* stride = value.Find("byteStride"))
                {
                    if (!GetSize(stride, view.stride)) { error = L"A buffer view has an invalid byte stride."; return false; }
                }
                std::size_t end = 0;
                if (!CheckedAdd(view.offset, view.length, end) || end > binarySize_)
                {
                    error = L"A buffer view points outside the embedded GLB buffer.";
                    return false;
                }
                views_.push_back(view);
            }
        }

        const JsonValue* accessors = root_.Find("accessors");
        if (!accessors || accessors->type != JsonValue::Type::Array || accessors->array.size() > kMaxAccessors)
        {
            error = L"The GLB accessor table is missing or too large.";
            return false;
        }
        for (const JsonValue& value : accessors->array)
        {
            Accessor accessor;
            if (const JsonValue* view = value.Find("bufferView")) accessor.view = view->Integer();
            if (accessor.view < 0 || static_cast<std::size_t>(accessor.view) >= views_.size())
            {
                error = value.Find("sparse")
                    ? L"Sparse GLB accessors are deferred in this vertical slice."
                    : L"An accessor does not refer to a valid embedded buffer view.";
                return false;
            }
            if (const JsonValue* offset = value.Find("byteOffset"))
            {
                if (!GetSize(offset, accessor.offset)) { error = L"An accessor has an invalid byte offset."; return false; }
            }
            if (!GetSize(value.Find("count"), accessor.count))
            {
                error = L"An accessor has an invalid element count.";
                return false;
            }
            if (accessor.count > kMaxOutputIndices)
            {
                error = L"An accessor exceeds the element-count limit for this preview slice.";
                return false;
            }
            const JsonValue* component = value.Find("componentType");
            accessor.componentType = component ? component->Integer() : 0;
            accessor.components = ComponentCount(value.Find("type"));
            const JsonValue* normalized = value.Find("normalized");
            accessor.normalized = normalized && normalized->type == JsonValue::Type::Boolean && normalized->boolean;
            if (!ComponentSize(accessor.componentType) || !accessor.components)
            {
                error = L"An accessor uses an unsupported component or shape.";
                return false;
            }
            accessors_.push_back(accessor);
        }
        return true;
    }

    // True if any component of a factor array (RGB or RGBA) is non-zero, so the
    // Information panel can say "constant" only where a factor actually
    // contributes something, rather than for an unset (all-zero) default.
    static bool FactorIsNonZero(const JsonValue* factor)
    {
        if (!factor || factor->type != JsonValue::Type::Array) return false;
        for (const JsonValue& component : factor->array)
        {
            if (component.type == JsonValue::Type::Number && component.number != 0.0) return true;
        }
        return false;
    }

    bool ParseMaterials(std::wstring& error)
    {
        UNREFERENCED_PARAMETER(error);
        colors_.push_back(XMFLOAT4(0.72f, 0.76f, 0.82f, 1.0f));
        const JsonValue* materials = root_.Find("materials");
        if (!materials || materials->type != JsonValue::Type::Array) return true;
        model_->stats.materialCount = static_cast<int>(materials->array.size());
        for (const JsonValue& material : materials->array)
        {
            XMFLOAT4 color(0.72f, 0.76f, 0.82f, 1.0f);
            const JsonValue* pbr = material.Find("pbrMetallicRoughness");
            if (pbr)
            {
                const JsonValue* factor = pbr->Find("baseColorFactor");
                if (factor && factor->type == JsonValue::Type::Array && factor->array.size() >= 4)
                {
                    color = XMFLOAT4(
                        static_cast<float>(factor->array[0].number), static_cast<float>(factor->array[1].number),
                        static_cast<float>(factor->array[2].number), static_cast<float>(factor->array[3].number));
                }
                if (pbr->Find("baseColorTexture"))
                {
                    ignoredTextures_ = true;
                    ++model_->stats.albedoTextureCount;
                }
                else
                {
                    model_->stats.hasConstantBaseColor = true;
                }
                if (pbr->Find("metallicRoughnessTexture"))
                {
                    ignoredTextures_ = true;
                    ++model_->stats.specularMetallicTextureCount;
                }
                if (factor && factor->type == JsonValue::Type::Array && factor->array.size() >= 4 &&
                    factor->array[3].number < 1.0)
                {
                    model_->stats.hasTransparency = true;
                }
            }
            else
            {
                model_->stats.hasConstantBaseColor = true;
            }
            if (const JsonValue* alphaMode = material.Find("alphaMode");
                alphaMode && alphaMode->type == JsonValue::Type::String && alphaMode->string != "OPAQUE")
            {
                model_->stats.hasTransparency = true;
            }
            if (material.Find("normalTexture"))
            {
                ignoredTextures_ = true;
                ++model_->stats.normalTextureCount;
            }
            if (material.Find("occlusionTexture"))
            {
                ignoredTextures_ = true;
                ++model_->stats.occlusionTextureCount;
            }
            if (material.Find("emissiveTexture"))
            {
                ignoredTextures_ = true;
                ++model_->stats.emissiveTextureCount;
            }
            else if (FactorIsNonZero(material.Find("emissiveFactor")))
            {
                model_->stats.hasConstantEmissiveColor = true;
            }
            if (const JsonValue* extensions = material.Find("extensions"))
            {
                if (const JsonValue* specular = extensions->Find("KHR_materials_specular"))
                {
                    if (specular->Find("specularColorTexture")) ignoredTextures_ = true;
                    else if (FactorIsNonZero(specular->Find("specularColorFactor")))
                        model_->stats.hasConstantSpecularColor = true;
                }
            }
            colors_.push_back(color);
        }
        return true;
    }

    bool GetAccessorSpan(int index, int expectedComponents, int expectedComponent, const std::uint8_t*& data,
        std::size_t& stride, const Accessor*& accessor, std::wstring& error) const
    {
        if (index < 0 || static_cast<std::size_t>(index) >= accessors_.size())
        {
            error = L"A mesh refers to an accessor that does not exist.";
            return false;
        }
        accessor = &accessors_[index];
        if ((expectedComponents && accessor->components != expectedComponents) ||
            (expectedComponent && accessor->componentType != expectedComponent))
        {
            error = L"A geometry accessor uses a component layout deferred in this GLB slice.";
            return false;
        }
        const BufferView& view = views_[accessor->view];
        std::size_t elementSize = 0;
        if (!CheckedMultiply(ComponentSize(accessor->componentType), static_cast<std::size_t>(accessor->components), elementSize))
        {
            error = L"Accessor element size overflow.";
            return false;
        }
        stride = view.stride ? view.stride : elementSize;
        if (stride < elementSize)
        {
            error = L"An accessor byte stride is smaller than its element.";
            return false;
        }
        std::size_t relativeEnd = accessor->offset;
        if (accessor->count)
        {
            std::size_t last = 0;
            if (!CheckedMultiply(accessor->count - 1, stride, last) ||
                !CheckedAdd(relativeEnd, last, relativeEnd) || !CheckedAdd(relativeEnd, elementSize, relativeEnd))
            {
                error = L"Accessor byte range overflow.";
                return false;
            }
        }
        if (relativeEnd > view.length)
        {
            error = L"An accessor points outside its buffer view.";
            return false;
        }
        data = binary_ + view.offset + accessor->offset;
        return true;
    }

    bool ReadVec3(int index, std::vector<XMFLOAT3>& output, std::wstring& error) const
    {
        const std::uint8_t* data = nullptr;
        std::size_t stride = 0;
        const Accessor* accessor = nullptr;
        if (!GetAccessorSpan(index, 3, 5126, data, stride, accessor, error)) return false;
        if (accessor->count > kMaxOutputVertices)
        {
            error = L"A vertex attribute exceeds the element-count limit for this preview slice.";
            return false;
        }
        output.resize(accessor->count);
        if (stride == sizeof(XMFLOAT3))
        {
            // Fast path for tightly packed float3 accessors: one bulk copy
            // instead of an element-by-element loop.
            if (Cancelled()) return false;
            std::memcpy(output.data(), data, output.size() * sizeof(XMFLOAT3));
        }
        else
        {
            for (std::size_t element = 0; element < accessor->count; ++element)
            {
                if ((element & 0x3fff) == 0 && Cancelled()) return false;
                std::memcpy(&output[element], data + element * stride, sizeof(XMFLOAT3));
            }
        }
        for (std::size_t element = 0; element < output.size(); ++element)
        {
            const XMFLOAT3& value = output[element];
            if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
            {
                error = L"A geometry accessor contains a non-finite value.";
                return false;
            }
        }
        return true;
    }

    bool ReadColors(int index, std::size_t expectedCount, std::vector<XMFLOAT4>& output, std::wstring& error) const
    {
        const std::uint8_t* data = nullptr;
        std::size_t stride = 0;
        const Accessor* accessor = nullptr;
        if (!GetAccessorSpan(index, 0, 0, data, stride, accessor, error)) return false;
        if ((accessor->components != 3 && accessor->components != 4) ||
            (accessor->componentType != 5121 && accessor->componentType != 5123 && accessor->componentType != 5126) ||
            accessor->count != expectedCount)
        {
            error = L"A vertex color accessor uses an unsupported layout.";
            return false;
        }
        if (accessor->count > kMaxOutputVertices)
        {
            error = L"A vertex color attribute exceeds the element-count limit for this preview slice.";
            return false;
        }
        output.resize(accessor->count, XMFLOAT4(1, 1, 1, 1));
        for (std::size_t element = 0; element < accessor->count; ++element)
        {
            const std::uint8_t* source = data + element * stride;
            for (int component = 0; component < accessor->components; ++component)
            {
                float value = 0.0f;
                if (accessor->componentType == 5121)
                {
                    value = static_cast<float>(source[component]) / 255.0f;
                }
                else if (accessor->componentType == 5123)
                {
                    std::uint16_t raw = 0;
                    std::memcpy(&raw, source + component * 2, 2);
                    value = static_cast<float>(raw) / 65535.0f;
                }
                else
                {
                    std::memcpy(&value, source + component * 4, 4);
                }
                (&output[element].x)[component] = value;
            }
        }
        return true;
    }

    bool ReadIndices(int index, std::size_t vertexCount, std::vector<std::uint32_t>& output, std::wstring& error) const
    {
        if (index < 0)
        {
            if (vertexCount % 3 != 0)
            {
                error = L"An unindexed triangle primitive has a vertex count that is not divisible by three.";
                return false;
            }
            output.resize(vertexCount);
            for (std::size_t value = 0; value < vertexCount; ++value) output[value] = static_cast<std::uint32_t>(value);
            return true;
        }
        const std::uint8_t* data = nullptr;
        std::size_t stride = 0;
        const Accessor* accessor = nullptr;
        if (!GetAccessorSpan(index, 1, 0, data, stride, accessor, error)) return false;
        if (accessor->componentType != 5121 && accessor->componentType != 5123 && accessor->componentType != 5125)
        {
            error = L"Triangle indices must use an unsigned integer component type.";
            return false;
        }
        if (accessor->count > kMaxOutputIndices)
        {
            error = L"An index accessor exceeds the element-count limit for this preview slice.";
            return false;
        }
        output.resize(accessor->count);
        if (accessor->componentType == 5125 && stride == sizeof(std::uint32_t))
        {
            // Fast path for tightly packed 32-bit indices: bulk copy, then validate.
            if (Cancelled()) return false;
            std::memcpy(output.data(), data, output.size() * sizeof(std::uint32_t));
        }
        else
        {
            for (std::size_t element = 0; element < accessor->count; ++element)
            {
                if ((element & 0x3fff) == 0 && Cancelled()) return false;
                const std::uint8_t* source = data + element * stride;
                std::uint32_t value = 0;
                if (accessor->componentType == 5121) value = *source;
                else if (accessor->componentType == 5123) { std::uint16_t small = 0; std::memcpy(&small, source, 2); value = small; }
                else std::memcpy(&value, source, 4);
                output[element] = value;
            }
        }
        for (std::size_t element = 0; element < output.size(); ++element)
        {
            if (output[element] >= vertexCount)
            {
                error = L"A triangle index is outside its vertex accessor.";
                return false;
            }
        }
        if (output.size() % 3 != 0)
        {
            error = L"A triangle primitive has an index count that is not divisible by three.";
            return false;
        }
        return true;
    }

    XMMATRIX NodeTransform(const JsonValue& node, std::wstring& error) const
    {
        const JsonValue* matrix = node.Find("matrix");
        if (matrix && matrix->type == JsonValue::Type::Array && matrix->array.size() == 16)
        {
            float values[16]{};
            for (int index = 0; index < 16; ++index)
            {
                if (matrix->array[index].type != JsonValue::Type::Number || !std::isfinite(matrix->array[index].number))
                {
                    error = L"A node matrix contains an invalid value.";
                    return XMMatrixIdentity();
                }
                values[index] = static_cast<float>(matrix->array[index].number);
            }
            return XMMATRIX(values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7],
                values[8], values[9], values[10], values[11], values[12], values[13], values[14], values[15]);
        }

        XMFLOAT3 scale(1, 1, 1);
        XMFLOAT4 rotation(0, 0, 0, 1);
        XMFLOAT3 translation(0, 0, 0);
        ReadArray(node.Find("scale"), &scale.x, 3);
        ReadArray(node.Find("rotation"), &rotation.x, 4);
        ReadArray(node.Find("translation"), &translation.x, 3);
        XMVECTOR quaternion = XMQuaternionNormalize(XMLoadFloat4(&rotation));
        return XMMatrixScaling(scale.x, scale.y, scale.z) * XMMatrixRotationQuaternion(quaternion) *
            XMMatrixTranslation(translation.x, translation.y, translation.z);
    }

    static void ReadArray(const JsonValue* value, float* output, std::size_t count)
    {
        if (!value || value->type != JsonValue::Type::Array || value->array.size() != count) return;
        for (std::size_t index = 0; index < count; ++index)
        {
            if (value->array[index].type == JsonValue::Type::Number && std::isfinite(value->array[index].number))
            {
                output[index] = static_cast<float>(value->array[index].number);
            }
        }
    }

    bool VisitNode(int index, FXMMATRIX parent, int depth, std::wstring& error)
    {
        if (Cancelled()) return false;
        if (depth > 256 || index < 0 || !nodes_ || static_cast<std::size_t>(index) >= nodes_->array.size())
        {
            error = L"The GLB scene graph is invalid or too deeply nested.";
            return false;
        }
        if (visitState_[index] == 1)
        {
            error = L"The GLB scene graph contains a cycle.";
            return false;
        }
        visitState_[index] = 1;
        const JsonValue& node = nodes_->array[index];
        XMMATRIX local = NodeTransform(node, error);
        if (!error.empty()) return false;
        XMMATRIX world = local * parent;
        if (const JsonValue* mesh = node.Find("mesh"))
        {
            if (!AppendMesh(mesh->Integer(), world, error)) return false;
        }
        const JsonValue* children = node.Find("children");
        if (children && children->type == JsonValue::Type::Array)
        {
            for (const JsonValue& child : children->array)
            {
                if (!VisitNode(child.Integer(), world, depth + 1, error)) return false;
            }
        }
        visitState_[index] = 2;
        return true;
    }

    bool AppendMesh(int index, FXMMATRIX world, std::wstring& error)
    {
        if (index < 0 || !meshes_ || static_cast<std::size_t>(index) >= meshes_->array.size())
        {
            error = L"A node refers to a mesh that does not exist.";
            return false;
        }
        const JsonValue* primitives = meshes_->array[index].Find("primitives");
        if (!primitives || primitives->type != JsonValue::Type::Array)
        {
            error = L"A mesh has no primitive list.";
            return false;
        }
        for (const JsonValue& primitive : primitives->array)
        {
            if (Cancelled()) return false;
            const JsonValue* modeValue = primitive.Find("mode");
            if (modeValue && modeValue->Integer(4) != 4)
            {
                skippedPrimitives_ = true;
                continue;
            }
            if (primitive.Find("extensions"))
            {
                const JsonValue* extensions = primitive.Find("extensions");
                if (extensions->Find("KHR_draco_mesh_compression") || extensions->Find("EXT_meshopt_compression"))
                {
                    error = L"This GLB uses compressed mesh data, which is deferred in this vertical slice.";
                    return false;
                }
            }
            const JsonValue* attributes = primitive.Find("attributes");
            const JsonValue* positionValue = attributes ? attributes->Find("POSITION") : nullptr;
            if (!positionValue)
            {
                skippedPrimitives_ = true;
                continue;
            }

            std::vector<XMFLOAT3> positions;
            std::vector<XMFLOAT3> normals;
            std::vector<XMFLOAT4> vertexColors;
            std::vector<std::uint32_t> indices;
            if (!ReadVec3(positionValue->Integer(), positions, error)) return false;
            if (const JsonValue* normal = attributes->Find("NORMAL"))
            {
                if (!ReadVec3(normal->Integer(), normals, error) || normals.size() != positions.size())
                {
                    if (error.empty()) error = L"The normal accessor count does not match POSITION.";
                    return false;
                }
            }
            if (const JsonValue* color = attributes->Find("COLOR_0"))
            {
                if (!ReadColors(color->Integer(), positions.size(), vertexColors, error)) return false;
                model_->stats.hasVertexColors = true;
            }
            if (attributes->Find("TEXCOORD_0")) model_->stats.hasUv0 = true;
            if (attributes->Find("TEXCOORD_1")) model_->stats.hasUv1 = true;
            const JsonValue* indexValue = primitive.Find("indices");
            if (!ReadIndices(indexValue ? indexValue->Integer() : -1, positions.size(), indices, error)) return false;

            if (positions.empty() || indices.empty()) continue;
            if (positions.size() > kMaxOutputVertices || indices.size() > kMaxOutputIndices ||
                model_->vertices.size() > kMaxOutputVertices - positions.size() ||
                model_->indices.size() > kMaxOutputIndices - indices.size())
            {
                error = L"This model expands beyond the geometry limit for this preview slice.";
                return false;
            }

            XMFLOAT4 material = colors_.front();
            if (const JsonValue* materialValue = primitive.Find("material"))
            {
                const int materialIndex = materialValue->Integer();
                if (materialIndex >= 0 && static_cast<std::size_t>(materialIndex) + 1 < colors_.size())
                    material = colors_[static_cast<std::size_t>(materialIndex) + 1];
            }

            // Reserve the exact per-primitive growth up front so large models
            // do not pay for repeated reallocation-and-copy cycles.
            model_->vertices.reserve(model_->vertices.size() + positions.size());
            model_->indices.reserve(model_->indices.size() + indices.size());

            const std::uint32_t base = static_cast<std::uint32_t>(model_->vertices.size());
            XMMATRIX normalMatrix = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
            for (std::size_t vertexIndex = 0; vertexIndex < positions.size(); ++vertexIndex)
            {
                ModelVertex vertex;
                XMStoreFloat3(&vertex.position, XMVector3TransformCoord(XMLoadFloat3(&positions[vertexIndex]), world));
                if (!normals.empty())
                {
                    XMStoreFloat3(&vertex.normal, XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&normals[vertexIndex]), normalMatrix)));
                }
                const XMFLOAT4 sourceColor = vertexColors.empty() ? XMFLOAT4(1, 1, 1, 1) : vertexColors[vertexIndex];
                vertex.color = XMFLOAT4(sourceColor.x * material.x, sourceColor.y * material.y,
                    sourceColor.z * material.z, sourceColor.w * material.w);
                model_->vertices.push_back(vertex);
                GrowBounds(vertex.position);
            }
            if (normals.empty()) GenerateNormals(base, indices);
            for (std::uint32_t value : indices) model_->indices.push_back(base + value);
            ++model_->stats.drawCallCount;
        }
        return true;
    }

    void GenerateNormals(std::uint32_t base, const std::vector<std::uint32_t>& indices)
    {
        for (std::size_t triangle = 0; triangle + 2 < indices.size(); triangle += 3)
        {
            ModelVertex& a = model_->vertices[base + indices[triangle]];
            ModelVertex& b = model_->vertices[base + indices[triangle + 1]];
            ModelVertex& c = model_->vertices[base + indices[triangle + 2]];
            const XMVECTOR edge1 = XMLoadFloat3(&b.position) - XMLoadFloat3(&a.position);
            const XMVECTOR edge2 = XMLoadFloat3(&c.position) - XMLoadFloat3(&a.position);
            const XMVECTOR face = XMVector3Cross(edge1, edge2);
            XMFLOAT3 faceValue{};
            XMStoreFloat3(&faceValue, face);
            for (ModelVertex* vertex : { &a, &b, &c })
            {
                vertex->normal.x += faceValue.x;
                vertex->normal.y += faceValue.y;
                vertex->normal.z += faceValue.z;
            }
        }
        const std::size_t count = model_->vertices.size() - base;
        for (std::size_t index = 0; index < count; ++index)
        {
            ModelVertex& vertex = model_->vertices[base + index];
            XMVECTOR normal = XMLoadFloat3(&vertex.normal);
            if (XMVectorGetX(XMVector3LengthSq(normal)) < 1e-12f) normal = XMVectorSet(0, 1, 0, 0);
            else normal = XMVector3Normalize(normal);
            XMStoreFloat3(&vertex.normal, normal);
        }
    }

    void GrowBounds(const XMFLOAT3& value)
    {
        if (!hasBounds_)
        {
            model_->boundsMin = value;
            model_->boundsMax = value;
            hasBounds_ = true;
            return;
        }
        model_->boundsMin.x = std::min(model_->boundsMin.x, value.x);
        model_->boundsMin.y = std::min(model_->boundsMin.y, value.y);
        model_->boundsMin.z = std::min(model_->boundsMin.z, value.z);
        model_->boundsMax.x = std::max(model_->boundsMax.x, value.x);
        model_->boundsMax.y = std::max(model_->boundsMax.y, value.y);
        model_->boundsMax.z = std::max(model_->boundsMax.z, value.z);
    }

    const JsonValue& root_;
    const std::uint8_t* binary_ = nullptr;
    std::size_t binarySize_ = 0;
    std::shared_ptr<std::atomic_bool> cancel_;
    const JsonValue* meshes_ = nullptr;
    const JsonValue* nodes_ = nullptr;
    std::vector<BufferView> views_;
    std::vector<Accessor> accessors_;
    std::vector<XMFLOAT4> colors_;
    std::vector<std::uint8_t> visitState_;
    ModelData* model_ = nullptr;
    bool hasBounds_ = false;
    bool ignoredTextures_ = false;
    bool skippedPrimitives_ = false;
};
}

LoadResult LoadGlb(const std::wstring& path, const std::shared_ptr<std::atomic_bool>& cancel,
    const LoadProgressCallback& progress)
{
    LoadResult result;
    auto cancelled = [&]() { return cancel && cancel->load(std::memory_order_relaxed); };
    if (cancelled()) { result.cancelled = true; return result; }

    progress(L"Opening GLB…");
    MappedFile file;
    std::wstring error;
    if (!file.Open(path, error))
    {
        result.summary = L"This file could not be opened.";
        result.details = error;
        return result;
    }
    if (file.Size() < 20)
    {
        result.summary = L"This file is not a valid GLB model.";
        result.details = L"The GLB header is incomplete.";
        return result;
    }
    const std::uint8_t* bytes = file.Data();
    const std::uint32_t declaredLength = ReadU32(bytes + 8);
    if (ReadU32(bytes) != kGlbMagic || ReadU32(bytes + 4) != 2 || declaredLength != file.Size())
    {
        result.summary = L"This file is not a valid GLB 2.0 model.";
        result.details = L"The magic, version, or declared file length is invalid.";
        return result;
    }

    std::size_t cursor = 12;
    const std::uint8_t* jsonData = nullptr;
    std::size_t jsonSize = 0;
    const std::uint8_t* binData = nullptr;
    std::size_t binSize = 0;
    while (cursor + 8 <= file.Size())
    {
        const std::uint32_t chunkLength = ReadU32(bytes + cursor);
        const std::uint32_t chunkType = ReadU32(bytes + cursor + 4);
        cursor += 8;
        if (chunkLength > file.Size() - cursor)
        {
            result.summary = L"This GLB file is damaged.";
            result.details = L"A chunk extends beyond the declared file length.";
            return result;
        }
        if (chunkType == kJsonChunk && !jsonData) { jsonData = bytes + cursor; jsonSize = chunkLength; }
        else if (chunkType == kBinChunk && !binData) { binData = bytes + cursor; binSize = chunkLength; }
        cursor += chunkLength;
    }
    if (!jsonData || !binData || jsonSize > kMaxJsonBytes)
    {
        result.summary = L"This GLB is missing required scene data.";
        result.details = jsonSize > kMaxJsonBytes ? L"The JSON chunk exceeds the 64 MiB metadata limit." : L"An embedded JSON or BIN chunk is missing.";
        return result;
    }

    progress(L"Reading scene structure…");
    JsonValue root;
    JsonParser parser(reinterpret_cast<const char*>(jsonData), reinterpret_cast<const char*>(jsonData + jsonSize), cancel);
    if (!parser.Parse(root, error))
    {
        if (cancelled()) { result.cancelled = true; return result; }
        result.summary = L"This GLB contains invalid scene data.";
        result.details = std::move(error);
        return result;
    }
    const JsonValue* asset = root.Find("asset");
    const JsonValue* version = asset ? asset->Find("version") : nullptr;
    if (!version || version->type != JsonValue::Type::String || version->string.rfind("2", 0) != 0)
    {
        result.summary = L"This GLB uses an unsupported glTF version.";
        result.details = L"Only glTF/GLB 2.x is supported by this vertical slice.";
        return result;
    }

    progress(L"Building preview geometry…");
    auto model = std::make_shared<ModelData>();
    GlbImporter importer(root, binData, binSize, cancel);
    if (!importer.Import(*model, error))
    {
        if (cancelled()) { result.cancelled = true; return result; }
        result.summary = L"This GLB could not be previewed.";
        result.details = std::move(error);
        return result;
    }

    if (cancelled()) { result.cancelled = true; return result; }

    model->sourceUpAxis = SourceUpAxis::Y;
    // glTF is right-handed, Y-up. Rotating +90 degrees about X maps +Y to
    // +Z (and +Z to -Y) — a pure rotation (determinant +1), so this
    // re-orients the model into the app's Z-up world without ever flipping
    // handedness.
    XMStoreFloat4x4(&model->upAxisCorrection, XMMatrixRotationX(XM_PIDIV2));

    result.succeeded = true;
    result.model = std::move(model);
    return result;
}

bool PickMesh(const ModelData& model, const XMFLOAT3& origin, const XMFLOAT3& direction, float& hitDistance)
{
    // Moller-Trumbore ray/triangle intersection over the world-space triangle
    // soup. This runs once per click (not per frame), so a linear scan keeps
    // picking exact without a spatial index to build and keep in sync.
    const XMVECTOR rayOrigin = XMLoadFloat3(&origin);
    const XMVECTOR rayDirection = XMVector3Normalize(XMLoadFloat3(&direction));
    const std::uint32_t vertexCount = static_cast<std::uint32_t>(model.vertices.size());
    const std::uint32_t triangleCount = static_cast<std::uint32_t>(model.indices.size() / 3);
    float best = std::numeric_limits<float>::infinity();

    for (std::uint32_t triangle = 0; triangle < triangleCount; ++triangle)
    {
        const std::uint32_t index[3] = {
            model.indices[triangle * 3],
            model.indices[triangle * 3 + 1],
            model.indices[triangle * 3 + 2]
        };
        if (index[0] >= vertexCount || index[1] >= vertexCount || index[2] >= vertexCount) continue;
        const XMVECTOR v0 = XMLoadFloat3(&model.vertices[index[0]].position);
        const XMVECTOR v1 = XMLoadFloat3(&model.vertices[index[1]].position);
        const XMVECTOR v2 = XMLoadFloat3(&model.vertices[index[2]].position);
        const XMVECTOR edge1 = XMVectorSubtract(v1, v0);
        const XMVECTOR edge2 = XMVectorSubtract(v2, v0);
        const XMVECTOR p = XMVector3Cross(rayDirection, edge2);
        const float determinant = XMVectorGetX(XMVector3Dot(edge1, p));
        if (std::abs(determinant) < 1e-12f) continue;
        const float inverse = 1.0f / determinant;
        const XMVECTOR t = XMVectorSubtract(rayOrigin, v0);
        const float u = XMVectorGetX(XMVector3Dot(t, p)) * inverse;
        if (u < 0.0f || u > 1.0f) continue;
        const XMVECTOR q = XMVector3Cross(t, edge1);
        const float v = XMVectorGetX(XMVector3Dot(rayDirection, q)) * inverse;
        if (v < 0.0f || u + v > 1.0f) continue;
        const float distance = XMVectorGetX(XMVector3Dot(edge2, q)) * inverse;
        if (distance > 1e-4f && distance < best) best = distance;
    }
    if (!std::isfinite(best)) return false;
    hitDistance = best;
    return true;
}
