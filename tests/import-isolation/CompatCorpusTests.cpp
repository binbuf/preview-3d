// Compatibility corpus scoreboard (0.7.0 T01).
//
// This file deliberately contains no importer logic. It loads
// tests/compat/manifest.json, runs every entry through the product's real
// broker/worker (or STEP host) path with import_broker::RunImportSession, and
// records one of three outcomes:
//
//   opened    geometry published, no compatibility warning
//   degraded  geometry published, at least one compatibility warning
//   failed    no geometry; result.errorCode is the typed failure
//
// The checked assertion is the [compat] case below. The hidden
// [write-compat-report] case writes the same observations to
// artifacts/compat/<Configuration>.json as evidence for the hand-off and later
// tasks. The report contains manifest ids only -- never a source path.
//
// The fixture manifest is seeded with the Preview3D corpus that already
// existed when 0.7.0 started. Breadth tasks append their own entries; this
// task measures, it does not fix importer behaviour.

#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "model_core/ImportError.h"
#include "model_core/WireFormat.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef PREVIEW3D_COMPAT_MANIFEST
#error "PREVIEW3D_COMPAT_MANIFEST must be defined by Tests.ImportIsolation.vcxproj"
#endif

#ifndef PREVIEW3D_REPO_ROOT
#error "PREVIEW3D_REPO_ROOT must be defined by Tests.ImportIsolation.vcxproj"
#endif

#ifndef PREVIEW3D_BUILD_CONFIGURATION
#error "PREVIEW3D_BUILD_CONFIGURATION must be defined by Tests.ImportIsolation.vcxproj"
#endif

#ifndef PREVIEW3D_IMPORT_WORKER_EXE
#error "PREVIEW3D_IMPORT_WORKER_EXE must be defined by Tests.ImportIsolation.vcxproj"
#endif

#ifndef PREVIEW3D_IMPORT_HOST_EXE
#error "PREVIEW3D_IMPORT_HOST_EXE must be defined by Tests.ImportIsolation.vcxproj"
#endif

#ifndef PREVIEW3D_STEP_HOST_EXE
#error "PREVIEW3D_STEP_HOST_EXE must be defined by Tests.ImportIsolation.vcxproj"
#endif

namespace {

// ---------------------------------------------------------------------------
// Minimal JSON reader. The repository has no general JSON dependency in the
// test binary, and the compatibility manifest is a small, closed schema; this
// parser is intentionally narrow (objects/arrays/strings/numbers/bools/null)
// rather than a second dependency.
// ---------------------------------------------------------------------------

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    const JsonValue& at(const std::string& key) const
    {
        const auto it = object.find(key);
        if (it == object.end()) {
            throw std::runtime_error("manifest is missing required field '" + key + "'");
        }
        return it->second;
    }

    const std::string& asString() const
    {
        if (type != Type::String) {
            throw std::runtime_error("manifest field is not a string");
        }
        return string;
    }

    bool asBool() const
    {
        if (type != Type::Bool) {
            throw std::runtime_error("manifest field is not a boolean");
        }
        return boolean;
    }

    const std::vector<JsonValue>& asArray() const
    {
        if (type != Type::Array) {
            throw std::runtime_error("manifest field is not an array");
        }
        return array;
    }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& text) : text_(text) {}

    JsonValue parse()
    {
        skipWhitespace();
        JsonValue value = parseValue();
        skipWhitespace();
        if (pos_ != text_.size()) {
            fail("trailing data after JSON value");
        }
        return value;
    }

private:
    const std::string& text_;
    size_t pos_ = 0;

    [[noreturn]] void fail(const char* message) const
    {
        throw std::runtime_error(std::string(message) + " at byte "
                                 + std::to_string(pos_));
    }

    void skipWhitespace()
    {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                break;
            }
            ++pos_;
        }
    }

    char peek() const
    {
        if (pos_ >= text_.size()) {
            fail("unexpected end of manifest");
        }
        return text_[pos_];
    }

    void expect(char c)
    {
        if (peek() != c) {
            fail("unexpected character");
        }
        ++pos_;
    }

    JsonValue parseValue()
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

    JsonValue parseLiteral(const char* keyword, bool value)
    {
        parseKeyword(keyword);
        JsonValue result;
        result.type = JsonValue::Type::Bool;
        result.boolean = value;
        return result;
    }

    void parseKeyword(const char* keyword)
    {
        for (const char* p = keyword; *p != '\0'; ++p) {
            if (peek() != *p) {
                fail("invalid literal");
            }
            ++pos_;
        }
    }

    JsonValue parseNumber()
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

    JsonValue parseObject()
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

    JsonValue parseArray()
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

    static void appendUtf8(std::string& out, uint32_t codePoint)
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

    uint32_t parseHex4()
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

    std::string parseString()
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
};

std::string ReadFileUtf8(const std::wstring& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open compatibility manifest");
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

// ---------------------------------------------------------------------------
// Manifest schema
// ---------------------------------------------------------------------------

enum class CorpusFormat { Gltf, Stl, Ply, Obj, Fbx, Usd, ThreeMf, Step };

const char* FormatName(CorpusFormat format)
{
    switch (format) {
    case CorpusFormat::Gltf: return "gltf";
    case CorpusFormat::Stl: return "stl";
    case CorpusFormat::Ply: return "ply";
    case CorpusFormat::Obj: return "obj";
    case CorpusFormat::Fbx: return "fbx";
    case CorpusFormat::Usd: return "usd";
    case CorpusFormat::ThreeMf: return "3mf";
    case CorpusFormat::Step: return "step";
    }
    return "unknown";
}

std::optional<CorpusFormat> ParseFormat(const std::string& name)
{
    if (name == "gltf") return CorpusFormat::Gltf;
    if (name == "stl") return CorpusFormat::Stl;
    if (name == "ply") return CorpusFormat::Ply;
    if (name == "obj") return CorpusFormat::Obj;
    if (name == "fbx") return CorpusFormat::Fbx;
    if (name == "usd") return CorpusFormat::Usd;
    if (name == "3mf") return CorpusFormat::ThreeMf;
    if (name == "step") return CorpusFormat::Step;
    return std::nullopt;
}

import_broker::ImportFormat ToImportFormat(CorpusFormat format)
{
    switch (format) {
    case CorpusFormat::Gltf: return import_broker::ImportFormat::Gltf;
    case CorpusFormat::Stl: return import_broker::ImportFormat::Stl;
    case CorpusFormat::Ply: return import_broker::ImportFormat::Ply;
    case CorpusFormat::Obj: return import_broker::ImportFormat::Obj;
    case CorpusFormat::Fbx: return import_broker::ImportFormat::Fbx;
    case CorpusFormat::Usd: return import_broker::ImportFormat::Usd;
    case CorpusFormat::ThreeMf: return import_broker::ImportFormat::ThreeMf;
    case CorpusFormat::Step: return import_broker::ImportFormat::Step;
    }
    return import_broker::ImportFormat::Gltf;
}

enum class ExpectedOutcome { Opened, Degraded, Failed };

std::optional<ExpectedOutcome> ParseExpected(const std::string& name)
{
    if (name == "opened") return ExpectedOutcome::Opened;
    if (name == "degraded") return ExpectedOutcome::Degraded;
    if (name == "failed") return ExpectedOutcome::Failed;
    return std::nullopt;
}

struct ManifestEntry {
    std::string id;
    std::string relativePath;
    CorpusFormat format = CorpusFormat::Gltf;
    ExpectedOutcome expected = ExpectedOutcome::Opened;
    std::string expectedError = "none";
    std::vector<std::string> expectedWarnings;
    std::vector<std::string> tags;
    bool requiresHost = false;
    bool requiresStepHost = false;
};

struct Manifest {
    std::vector<ManifestEntry> entries;
};

Manifest LoadManifest()
{
    const JsonValue root = JsonReader(ReadFileUtf8(PREVIEW3D_COMPAT_MANIFEST)).parse();
    if (root.type != JsonValue::Type::Object) {
        throw std::runtime_error("compatibility manifest root is not an object");
    }
    const JsonValue& schema = root.at("schema");
    if (schema.type != JsonValue::Type::Number || schema.number != 1.0) {
        throw std::runtime_error("compatibility manifest schema must be 1");
    }

    Manifest manifest;
    for (const JsonValue& item : root.at("entries").asArray()) {
        ManifestEntry entry;
        entry.id = item.at("id").asString();
        entry.relativePath = item.at("path").asString();
        const auto format = ParseFormat(item.at("format").asString());
        if (!format) {
            throw std::runtime_error("fixture '" + entry.id + "' has an unknown format");
        }
        entry.format = *format;
        const auto expected = ParseExpected(item.at("expected").asString());
        if (!expected) {
            throw std::runtime_error("fixture '" + entry.id + "' has an unknown expected outcome");
        }
        entry.expected = *expected;
        entry.expectedError = item.at("expectedError").asString();
        if (item.object.contains("expectedWarnings")) {
            for (const JsonValue& warning : item.at("expectedWarnings").asArray()) {
                entry.expectedWarnings.push_back(warning.asString());
            }
        }
        if (item.object.contains("tags")) {
            for (const JsonValue& tag : item.at("tags").asArray()) {
                entry.tags.push_back(tag.asString());
            }
        }
        if (item.object.contains("requiresHost")) {
            entry.requiresHost = item.at("requiresHost").asBool();
        }
        if (item.object.contains("requiresStepHost")) {
            entry.requiresStepHost = item.at("requiresStepHost").asBool();
        }
        manifest.entries.push_back(std::move(entry));
    }
    if (manifest.entries.empty()) {
        throw std::runtime_error("compatibility manifest has no entries");
    }
    return manifest;
}

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

struct Observation {
    std::string id;
    std::string outcome;
    std::string error;
    std::vector<std::string> warnings;
    size_t chunks = 0;
    uint32_t batches = 0;
    uint64_t elapsedMs = 0;
    std::string skippedReason;
    import_broker::ImportStage stage = import_broker::ImportStage::Completed;
    model_core::ImportErrorCode code = model_core::ImportErrorCode::None;
};

const char* ErrorName(model_core::ImportErrorCode code)
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

const char* StageName(import_broker::ImportStage stage)
{
    using import_broker::ImportStage;
    switch (stage) {
    case ImportStage::Completed: return "Completed";
    case ImportStage::OpenSource: return "OpenSource";
    case ImportStage::DuplicateSourceHandle: return "DuplicateSourceHandle";
    case ImportStage::CreateOutputSection: return "CreateOutputSection";
    case ImportStage::CreateSandboxProfile: return "CreateSandboxProfile";
    case ImportStage::CreateControlChannel: return "CreateControlChannel";
    case ImportStage::LaunchWorker: return "LaunchWorker";
    case ImportStage::ResumeWorker: return "ResumeWorker";
    case ImportStage::SendRequest: return "SendRequest";
    case ImportStage::SidecarRequestLimit: return "SidecarRequestLimit";
    case ImportStage::AwaitReply: return "AwaitReply";
    case ImportStage::ReplyTimedOut: return "ReplyTimedOut";
    case ImportStage::Cancelled: return "Cancelled";
    case ImportStage::UnexpectedReply: return "UnexpectedReply";
    case ImportStage::MapOutputSection: return "MapOutputSection";
    case ImportStage::ValidateSection: return "ValidateSection";
    case ImportStage::WorkerReportedError: return "WorkerReportedError";
    case ImportStage::GenerationDeadline: return "GenerationDeadline";
    case ImportStage::ChunkBatchLimit: return "ChunkBatchLimit";
    case ImportStage::ChunkBatchOutOfOrder: return "ChunkBatchOutOfOrder";
    case ImportStage::ChunkCountLimit: return "ChunkCountLimit";
    case ImportStage::ChunkBatchAckFailed: return "ChunkBatchAckFailed";
    case ImportStage::StepProgressLimit: return "StepProgressLimit";
    case ImportStage::Upload: return "Upload";
    }
    return "unknown";
}

bool HasPublishedGeometry(const import_broker::ImportSessionResult& result)
{
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology == model_core::ChunkTopology::TriangleList
            || chunk.descriptor.topology == model_core::ChunkTopology::PointList) {
            return true;
        }
    }
    return false;
}

// Compatibility warnings currently cross the wire in two closed shapes:
// ImportStatusPayload carries saturated optional-feature/texture counters, and
// TextureWarning carries a bounded fallback count. Both are worker-owned facts;
// the corpus maps them to stable, path-free identifiers.
std::vector<std::string> CollectWarnings(const import_broker::ImportSessionResult& result)
{
    bool optionalFeature = false;
    bool texture = false;
    for (const auto& chunk : result.chunks) {
        if (chunk.descriptor.topology == model_core::ChunkTopology::ImportStatus
            && chunk.payload.size() >= sizeof(model_core::ImportStatusPayload)) {
            model_core::ImportStatusPayload status{};
            std::memcpy(&status, chunk.payload.data(), sizeof(status));
            optionalFeature = optionalFeature || status.optionalFeatureWarnings > 0;
            texture = texture || status.textureWarnings > 0;
        } else if (chunk.descriptor.topology == model_core::ChunkTopology::TextureWarning) {
            texture = true;
        }
    }

    std::vector<std::string> warnings;
    if (optionalFeature) {
        warnings.emplace_back("optional-feature");
    }
    if (texture) {
        warnings.emplace_back("texture-fallback");
    }
    std::sort(warnings.begin(), warnings.end());
    return warnings;
}

std::optional<Observation> Observe(const ManifestEntry& entry, std::string& skippedReason)
{
    std::wstring hostPath;
    if (entry.requiresStepHost) {
        hostPath = PREVIEW3D_STEP_HOST_EXE;
        if (!std::filesystem::exists(hostPath)) {
            skippedReason = "step-host-missing";
            return std::nullopt;
        }
    } else if (entry.requiresHost) {
        hostPath = PREVIEW3D_IMPORT_HOST_EXE;
        if (!std::filesystem::exists(hostPath)) {
            skippedReason = "host-missing";
            return std::nullopt;
        }
    } else if (entry.format == CorpusFormat::Step) {
        if (!std::filesystem::exists(PREVIEW3D_STEP_HOST_EXE)) {
            skippedReason = "step-host-missing";
            return std::nullopt;
        }
    } else if (entry.format == CorpusFormat::Usd) {
        if (!std::filesystem::exists(PREVIEW3D_IMPORT_HOST_EXE)) {
            skippedReason = "host-missing";
            return std::nullopt;
        }
    }

    import_broker::ImportSessionRequest request;
    request.workerExePath = PREVIEW3D_IMPORT_WORKER_EXE;
    request.sourcePath = JoinPath(PREVIEW3D_REPO_ROOT, Utf8ToWide(entry.relativePath));
    request.format = ToImportFormat(entry.format);
    request.generationId = 0x4300'0000ull + static_cast<uint64_t>(entry.id.size());
    request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = import_broker::kImportMaxChunkCount;
    request.maxSidecarRequestsPerGeneration = 64;
    request.maxSidecarFileBytes = 256ull * 1024 * 1024;
    request.compatibilityHostExePath = PREVIEW3D_IMPORT_HOST_EXE;
    request.stepHostExePath = PREVIEW3D_STEP_HOST_EXE;

    const auto started = std::chrono::steady_clock::now();
    import_broker::ImportSessionResult result = import_broker::RunImportSession(request);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    Observation observation;
    observation.id = entry.id;
    observation.chunks = result.chunks.size();
    observation.batches = result.batchCount;
    observation.elapsedMs = static_cast<uint64_t>(elapsed.count());
    observation.stage = result.stage;
    observation.code = result.errorCode;
    observation.error = ErrorName(result.errorCode);
    observation.warnings = result.ok ? CollectWarnings(result) : std::vector<std::string>{};
    if (!result.ok) {
        observation.outcome = "failed";
    } else if (!HasPublishedGeometry(result)) {
        // A successful broker result with no geometry is not "opened"; it is a
        // harness/schema failure the corpus must not hide.
        observation.outcome = "failed";
        observation.error = "NoGeometryChunks";
        observation.stage = import_broker::ImportStage::ValidateSection;
    } else if (!observation.warnings.empty()) {
        observation.outcome = "degraded";
    } else {
        observation.outcome = "opened";
    }
    return observation;
}

const char* ExpectedName(ExpectedOutcome outcome)
{
    switch (outcome) {
    case ExpectedOutcome::Opened: return "opened";
    case ExpectedOutcome::Degraded: return "degraded";
    case ExpectedOutcome::Failed: return "failed";
    }
    return "unknown";
}

std::string Describe(const Observation& observation)
{
    std::ostringstream text;
    text << "id=" << observation.id
         << " outcome=" << observation.outcome
         << " error=" << observation.error
         << " stage=" << StageName(observation.stage)
         << " chunks=" << observation.chunks
         << " batches=" << observation.batches
         << " elapsedMs=" << observation.elapsedMs;
    if (!observation.warnings.empty()) {
        text << " warnings=";
        for (size_t i = 0; i < observation.warnings.size(); ++i) {
            text << (i == 0 ? "" : ",") << observation.warnings[i];
        }
    }
    return text.str();
}

std::string EscapeJson(const std::string& value)
{
    std::string out;
    for (const char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += '?';
            } else {
                out.push_back(c);
            }
        }
    }
    return out;
}

void WriteReport(const std::vector<Observation>& observations)
{
    const std::filesystem::path directory
        = std::filesystem::path(PREVIEW3D_REPO_ROOT) / L"artifacts" / L"compat";
    std::filesystem::create_directories(directory);
    const std::filesystem::path path
        = directory / (std::wstring(PREVIEW3D_BUILD_CONFIGURATION) + L".json");

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("cannot write compatibility report");
    }
    stream << "{\n  \"schema\": 1,\n  \"configuration\": \""
           << EscapeJson(WideToUtf8(PREVIEW3D_BUILD_CONFIGURATION))
           << "\",\n  \"entries\": [\n";
    for (size_t i = 0; i < observations.size(); ++i) {
        const Observation& observation = observations[i];
        stream << "    {\"id\": \"" << EscapeJson(observation.id)
               << "\", \"outcome\": \"" << observation.outcome
               << "\", \"error\": \"" << EscapeJson(observation.error)
               << "\", \"warnings\": [";
        for (size_t w = 0; w < observation.warnings.size(); ++w) {
            stream << (w == 0 ? "" : ", ") << "\"" << EscapeJson(observation.warnings[w]) << "\"";
        }
        stream << "], \"chunks\": " << observation.chunks
               << ", \"batches\": " << observation.batches
               << ", \"elapsedMs\": " << observation.elapsedMs;
        if (!observation.skippedReason.empty()) {
            stream << ", \"skipped\": \"" << EscapeJson(observation.skippedReason) << "\"";
        }
        stream << "}" << (i + 1 == observations.size() ? "" : ",") << "\n";
    }
    stream << "  ]\n}\n";
}

void CheckObservation(const ManifestEntry& entry, const Observation& observation)
{
    INFO(Describe(observation));
    CAPTURE(entry.id, entry.relativePath, FormatName(entry.format),
            ExpectedName(entry.expected), entry.expectedError);

    CHECK(observation.outcome == ExpectedName(entry.expected));

    if (entry.expected == ExpectedOutcome::Failed) {
        CHECK(observation.outcome == "failed");
        CHECK(observation.error == entry.expectedError);
        CHECK(observation.error != "none");
    } else {
        CHECK(observation.code == model_core::ImportErrorCode::None);
        CHECK(observation.outcome != "failed");
        CHECK(observation.chunks > 0);
    }

    if (entry.expected == ExpectedOutcome::Degraded) {
        CHECK_FALSE(observation.warnings.empty());
    } else if (entry.expected == ExpectedOutcome::Opened) {
        CHECK(observation.warnings.empty());
    }

    if (!entry.expectedWarnings.empty()) {
        CHECK(observation.warnings == entry.expectedWarnings);
    }
}

} // namespace

TEST_CASE("The compatibility corpus manifest is readable and self-consistent", "[compat]")
{
    INFO("manifest: " << WideToUtf8(PREVIEW3D_COMPAT_MANIFEST));
    Manifest manifest;
    REQUIRE_NOTHROW(manifest = LoadManifest());
    REQUIRE_FALSE(manifest.entries.empty());

    std::map<std::string, size_t> ids;
    for (const auto& entry : manifest.entries) {
        CAPTURE(entry.id);
        CHECK_FALSE(entry.id.empty());
        CHECK_FALSE(entry.relativePath.empty());
        CHECK(ids.emplace(entry.id, ids.size()).second);
        INFO("fixture path: " << entry.relativePath);
        CHECK(std::filesystem::exists(
            JoinPath(PREVIEW3D_REPO_ROOT, Utf8ToWide(entry.relativePath))));
    }
}

TEST_CASE("Every compatibility corpus fixture reaches its recorded outcome", "[compat]")
{
    Manifest manifest;
    REQUIRE_NOTHROW(manifest = LoadManifest());

    size_t opened = 0;
    size_t degraded = 0;
    size_t failed = 0;
    size_t skipped = 0;
    for (const auto& entry : manifest.entries) {
        std::string skipReason;
        const auto observation = Observe(entry, skipReason);
        if (!observation) {
            ++skipped;
            WARN("skipping " << entry.id << ": " << skipReason);
            continue;
        }
        if (observation->outcome == "opened") {
            ++opened;
        } else if (observation->outcome == "degraded") {
            ++degraded;
        } else {
            ++failed;
        }
        CheckObservation(entry, *observation);
    }
    INFO("compat baseline: opened=" << opened << " degraded=" << degraded
                                    << " failed=" << failed << " skipped=" << skipped);
}

TEST_CASE("Write the compatibility corpus report", "[write-compat-report]")
{
    Manifest manifest;
    REQUIRE_NOTHROW(manifest = LoadManifest());

    std::vector<Observation> observations;
    for (const auto& entry : manifest.entries) {
        std::string skipReason;
        const auto observation = Observe(entry, skipReason);
        if (!observation) {
            Observation skippedObservation;
            skippedObservation.id = entry.id;
            skippedObservation.outcome = "skipped";
            skippedObservation.error = "none";
            skippedObservation.skippedReason = skipReason;
            observations.push_back(std::move(skippedObservation));
            continue;
        }
        observations.push_back(*observation);
    }
    REQUIRE_NOTHROW(WriteReport(observations));
    for (const auto& observation : observations) {
        INFO(Describe(observation));
        CHECK_FALSE(observation.id.empty());
    }
}
