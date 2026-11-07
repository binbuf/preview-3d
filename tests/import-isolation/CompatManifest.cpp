#include "CompatManifest.h"

#include <windows.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace compat_test {

const JsonValue& JsonValue::at(const std::string& key) const
{
    const auto it = object.find(key);
    if (it == object.end()) {
        throw std::runtime_error("manifest is missing required field '" + key + "'");
    }
    return it->second;
}

const std::string& JsonValue::asString() const
{
    if (type != Type::String) {
        throw std::runtime_error("manifest field is not a string");
    }
    return string;
}

bool JsonValue::asBool() const
{
    if (type != Type::Bool) {
        throw std::runtime_error("manifest field is not a boolean");
    }
    return boolean;
}

const std::vector<JsonValue>& JsonValue::asArray() const
{
    if (type != Type::Array) {
        throw std::runtime_error("manifest field is not an array");
    }
    return array;
}

JsonValue JsonReader::parse()
{
    skipWhitespace();
    JsonValue value = parseValue();
    skipWhitespace();
    if (pos_ != text_.size()) {
        fail("trailing data after JSON value");
    }
    return value;
}

void JsonReader::fail(const char* message) const
{
    throw std::runtime_error(std::string(message) + " at byte "
                             + std::to_string(pos_));
}

void JsonReader::skipWhitespace()
{
    while (pos_ < text_.size()) {
        const char c = text_[pos_];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        ++pos_;
    }
}

char JsonReader::peek() const
{
    if (pos_ >= text_.size()) {
        fail("unexpected end of manifest");
    }
    return text_[pos_];
}

void JsonReader::expect(char c)
{
    if (peek() != c) {
        fail("unexpected character");
    }
    ++pos_;
}

JsonValue JsonReader::parseValue()
{
    skipWhitespace();
    switch (peek()) {
    case '{': return parseObject();
    case '[': return parseArray();
    case '"': {
        JsonValue value;
        value.type = JsonValue::Type::String;
        value.string = parseString();
        return value;
    }
    case 't': return parseLiteral("true", true);
    case 'f': return parseLiteral("false", false);
    case 'n': {
        parseKeyword("null");
        return JsonValue{};
    }
    default:
        if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
            return parseNumber();
        }
        fail("unexpected value");
    }
}

JsonValue JsonReader::parseLiteral(const char* keyword, bool value)
{
    parseKeyword(keyword);
    JsonValue result;
    result.type = JsonValue::Type::Bool;
    result.boolean = value;
    return result;
}

void JsonReader::parseKeyword(const char* keyword)
{
    for (const char* p = keyword; *p != '\0'; ++p) {
        if (peek() != *p) {
            fail("invalid literal");
        }
        ++pos_;
    }
}

JsonValue JsonReader::parseNumber()
{
    const size_t start = pos_;
    if (peek() == '-') {
        ++pos_;
    }
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
        ++pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
        }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
        ++pos_;
        if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
            ++pos_;
        }
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
        }
    }
    JsonValue value;
    value.type = JsonValue::Type::Number;
    value.number = std::stod(text_.substr(start, pos_ - start));
    return value;
}

JsonValue JsonReader::parseObject()
{
    expect('{');
    JsonValue value;
    value.type = JsonValue::Type::Object;
    skipWhitespace();
    if (peek() == '}') {
        ++pos_;
        return value;
    }
    for (;;) {
        skipWhitespace();
        const std::string key = parseString();
        skipWhitespace();
        expect(':');
        value.object.emplace(key, parseValue());
        skipWhitespace();
        if (peek() == ',') {
            ++pos_;
            continue;
        }
        expect('}');
        return value;
    }
}

JsonValue JsonReader::parseArray()
{
    expect('[');
    JsonValue value;
    value.type = JsonValue::Type::Array;
    skipWhitespace();
    if (peek() == ']') {
        ++pos_;
        return value;
    }
    for (;;) {
        value.array.push_back(parseValue());
        skipWhitespace();
        if (peek() == ',') {
            ++pos_;
            continue;
        }
        expect(']');
        return value;
    }
}

void JsonReader::appendUtf8(std::string& out, uint32_t codePoint)
{
    if (codePoint <= 0x7F) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

uint32_t JsonReader::parseHex4()
{
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = peek();
        ++pos_;
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= static_cast<uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= static_cast<uint32_t>(c - 'A' + 10);
        } else {
            fail("invalid unicode escape");
        }
    }
    return value;
}

std::string JsonReader::parseString()
{
    expect('"');
    std::string result;
    for (;;) {
        const char c = peek();
        ++pos_;
        if (c == '"') {
            return result;
        }
        if (c != '\\') {
            result.push_back(c);
            continue;
        }
        const char escape = peek();
        ++pos_;
        switch (escape) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u': {
            uint32_t codePoint = parseHex4();
            if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                if (peek() != '\\') {
                    fail("unpaired high surrogate");
                }
                ++pos_;
                if (peek() != 'u') {
                    fail("unpaired high surrogate");
                }
                ++pos_;
                const uint32_t low = parseHex4();
                if (low < 0xDC00 || low > 0xDFFF) {
                    fail("invalid low surrogate");
                }
                codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
            }
            appendUtf8(result, codePoint);
            break;
        }
        default:
            fail("invalid string escape");
        }
    }
}

std::string ReadFileUtf8(const std::wstring& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open manifest");
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        throw std::runtime_error("manifest contains invalid UTF-8");
    }
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), wide.data(), length);
    return wide;
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) {
        throw std::runtime_error("cannot encode report configuration");
    }
    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        utf8.data(), length, nullptr, nullptr);
    return utf8;
}

std::wstring JoinPath(const std::wstring& root, const std::wstring& relative)
{
    if (root.empty()) {
        return relative;
    }
    if (root.back() == L'\\' || root.back() == L'/') {
        return root + relative;
    }
    return root + L"\\" + relative;
}

JsonValue ParseJsonFile(const std::wstring& path)
{
    return JsonReader(ReadFileUtf8(path)).parse();
}

const std::vector<JsonValue>& Schema1Entries(const JsonValue& root)
{
    if (root.type != JsonValue::Type::Object) {
        throw std::runtime_error("manifest root is not an object");
    }
    const JsonValue& schema = root.at("schema");
    if (schema.type != JsonValue::Type::Number || schema.number != 1.0) {
        throw std::runtime_error("manifest schema must be 1");
    }
    return root.at("entries").asArray();
}

const char* ImportErrorName(model_core::ImportErrorCode code)
{
    switch (code) {
    case model_core::ImportErrorCode::None: return "none";
    case model_core::ImportErrorCode::MalformedData: return "MalformedData";
    case model_core::ImportErrorCode::ResourceLimit: return "ResourceLimit";
    case model_core::ImportErrorCode::ImportProtocolViolation: return "ImportProtocolViolation";
    case model_core::ImportErrorCode::InternalImporterFailure: return "InternalImporterFailure";
    case model_core::ImportErrorCode::UnsafeReference: return "UnsafeReference";
    case model_core::ImportErrorCode::FileUnavailable: return "FileUnavailable";
    case model_core::ImportErrorCode::Cancelled: return "Cancelled";
    case model_core::ImportErrorCode::UnsupportedFormat: return "UnsupportedFormat";
    case model_core::ImportErrorCode::UnsupportedEncoding: return "UnsupportedEncoding";
    case model_core::ImportErrorCode::UnsupportedRequiredFeature: return "UnsupportedRequiredFeature";
    case model_core::ImportErrorCode::EmptyGeometry: return "EmptyGeometry";
    case model_core::ImportErrorCode::OutOfMemory: return "OutOfMemory";
    case model_core::ImportErrorCode::FileChanged: return "FileChanged";
    case model_core::ImportErrorCode::WorkerCrashed: return "WorkerCrashed";
    case model_core::ImportErrorCode::WorkerTimedOut: return "WorkerTimedOut";
    case model_core::ImportErrorCode::UploadFailure: return "UploadFailure";
    case model_core::ImportErrorCode::PrimarySourceLimit: return "PrimarySourceLimit";
    case model_core::ImportErrorCode::AggregateSourceLimit: return "AggregateSourceLimit";
    case model_core::ImportErrorCode::ScratchLimit: return "ScratchLimit";
    case model_core::ImportErrorCode::ChunkCatalogLimit: return "ChunkCatalogLimit";
    case model_core::ImportErrorCode::DracoPrimitiveLimit: return "DracoPrimitiveLimit";
    case model_core::ImportErrorCode::UnsupportedComposition: return "UnsupportedComposition";
    case model_core::ImportErrorCode::CompatibilityHostFailure: return "CompatibilityHostFailure";
    case model_core::ImportErrorCode::CompatibilityHostLimit: return "CompatibilityHostLimit";
    case model_core::ImportErrorCode::ArchiveLimit: return "ArchiveLimit";
    case model_core::ImportErrorCode::StepHostFailure: return "StepHostFailure";
    case model_core::ImportErrorCode::StepHostLimit: return "StepHostLimit";
    case model_core::ImportErrorCode::TessellationFailed: return "TessellationFailed";
    }
    return "unknown";
}

} // namespace compat_test
