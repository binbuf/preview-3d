// Valid-file coverage scoreboard (0.8.0 T01).
//
// This file deliberately contains no importer logic. It loads
// tests/compat/valid-file-manifest.json, resolves each fixture the same way the
// shipping viewer's automatic open gate does (catalog extension first, bounded
// content probe only as a diagnostic), runs the ones the gate admits through
// the product's real broker/worker or dedicated host, and records one of three
// outcomes:
//
//   opened    geometry published, no compatibility warning
//   degraded  geometry published, at least one compatibility warning
//   failed    the gate rejected the suffix/content, or the import returned a
//             typed error, or a successful import published no geometry
//
// The checked assertion is the [valid-file] cases below. The hidden
// [write-valid-file-report] case writes the same observations to
// artifacts/valid-file/<Configuration>.json as evidence. The report contains
// manifest ids and stable format names only -- never a source path or model
// name.
//
// This task measures; it does not fix importer behaviour. A later task flips
// the manifest entry it owns.

#include "CompatManifest.h"

#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "model_core/FormatCatalog.h"
#include "model_core/FormatSelection.h"
#include "model_core/FormatSniffer.h"
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

#ifndef PREVIEW3D_VALID_FILE_MANIFEST
#error "PREVIEW3D_VALID_FILE_MANIFEST must be defined by Tests.ImportIsolation.vcxproj"
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

using compat_test::JsonValue;
using compat_test::JoinPath;
using compat_test::ParseJsonFile;
using compat_test::Schema1Entries;
using compat_test::Utf8ToWide;
using compat_test::WideToUtf8;

// ---------------------------------------------------------------------------
// Manifest schema
// ---------------------------------------------------------------------------

enum class ExpectedOutcome { Opened, Degraded, Failed };

std::optional<ExpectedOutcome> ParseExpected(const std::string& name)
{
    if (name == "opened") return ExpectedOutcome::Opened;
    if (name == "degraded") return ExpectedOutcome::Degraded;
    if (name == "failed") return ExpectedOutcome::Failed;
    return std::nullopt;
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

enum class ExpectedGeometry { Triangles, Lines, Points, Mixed, None };

std::optional<ExpectedGeometry> ParseGeometry(const std::string& name)
{
    if (name == "triangles") return ExpectedGeometry::Triangles;
    if (name == "lines") return ExpectedGeometry::Lines;
    if (name == "points") return ExpectedGeometry::Points;
    if (name == "mixed") return ExpectedGeometry::Mixed;
    if (name == "none") return ExpectedGeometry::None;
    return std::nullopt;
}

const char* GeometryName(ExpectedGeometry geometry)
{
    switch (geometry) {
    case ExpectedGeometry::Triangles: return "triangles";
    case ExpectedGeometry::Lines: return "lines";
    case ExpectedGeometry::Points: return "points";
    case ExpectedGeometry::Mixed: return "mixed";
    case ExpectedGeometry::None: return "none";
    }
    return "unknown";
}

enum class FailureClass { None, Extension, Primitive, Sniffer, Importer, Resource };

std::optional<FailureClass> ParseFailureClass(const std::string& name)
{
    if (name == "none") return FailureClass::None;
    if (name == "extension") return FailureClass::Extension;
    if (name == "primitive") return FailureClass::Primitive;
    if (name == "sniffer") return FailureClass::Sniffer;
    if (name == "importer") return FailureClass::Importer;
    if (name == "resource") return FailureClass::Resource;
    return std::nullopt;
}

const char* FailureClassName(FailureClass value)
{
    switch (value) {
    case FailureClass::None: return "none";
    case FailureClass::Extension: return "extension";
    case FailureClass::Primitive: return "primitive";
    case FailureClass::Sniffer: return "sniffer";
    case FailureClass::Importer: return "importer";
    case FailureClass::Resource: return "resource";
    }
    return "unknown";
}

struct ManifestEntry {
    std::string id;
    std::string relativePath;
    std::string formatName;
    const model_core::FormatDescriptor* format = nullptr;
    ExpectedOutcome expected = ExpectedOutcome::Opened;
    std::string expectedError = "none";
    ExpectedGeometry expectedGeometry = ExpectedGeometry::None;
    FailureClass failureClass = FailureClass::None;
    std::vector<std::string> expectedWarnings;
    bool requiresHost = false;
    bool requiresStepHost = false;
    std::vector<std::string> tags;
};

struct Manifest {
    std::vector<ManifestEntry> entries;
};

Manifest LoadManifest()
{
    const JsonValue root = ParseJsonFile(PREVIEW3D_VALID_FILE_MANIFEST);
    Manifest manifest;
    for (const JsonValue& item : Schema1Entries(root)) {
        ManifestEntry entry;
        entry.id = item.at("id").asString();
        entry.relativePath = item.at("path").asString();
        entry.formatName = item.at("format").asString();
        entry.format = model_core::FindByStableName(entry.formatName);
        if (!entry.format) {
            throw std::runtime_error("fixture '" + entry.id + "' has an unknown format");
        }
        const auto expected = ParseExpected(item.at("expected").asString());
        if (!expected) {
            throw std::runtime_error("fixture '" + entry.id + "' has an unknown expected outcome");
        }
        entry.expected = *expected;
        entry.expectedError = item.at("expectedError").asString();
        const auto geometry = ParseGeometry(item.at("expectedGeometry").asString());
        if (!geometry) {
            throw std::runtime_error("fixture '" + entry.id + "' has an unknown expected geometry");
        }
        entry.expectedGeometry = *geometry;
        const auto failureClass = ParseFailureClass(item.at("failureClass").asString());
        if (!failureClass) {
            throw std::runtime_error("fixture '" + entry.id + "' has an unknown failure class");
        }
        entry.failureClass = *failureClass;
        if (item.has("expectedWarnings")) {
            for (const JsonValue& warning : item.at("expectedWarnings").asArray()) {
                entry.expectedWarnings.push_back(warning.asString());
            }
        }
        if (item.has("requiresHost")) {
            entry.requiresHost = item.at("requiresHost").asBool();
        }
        if (item.has("requiresStepHost")) {
            entry.requiresStepHost = item.at("requiresStepHost").asBool();
        }
        if (item.has("tags")) {
            for (const JsonValue& tag : item.at("tags").asArray()) {
                entry.tags.push_back(tag.asString());
            }
        }
        manifest.entries.push_back(std::move(entry));
    }
    if (manifest.entries.empty()) {
        throw std::runtime_error("valid-file manifest has no entries");
    }
    return manifest;
}

// ---------------------------------------------------------------------------
// Dispatch: the shipping viewer's automatic open gate
// ---------------------------------------------------------------------------

std::wstring ExtensionOf(const std::wstring& path)
{
    const size_t dot = path.find_last_of(L'.');
    const size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
        return {};
    }
    std::wstring extension = path.substr(dot + 1);
    for (wchar_t& c : extension) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + (L'a' - L'A'));
    }
    return extension;
}

bool IsCadFamily(const model_core::FormatDescriptor& descriptor)
{
    return descriptor.family == model_core::FormatId::Step
        || descriptor.family == model_core::FormatId::Iges
        || descriptor.family == model_core::FormatId::Brep
        || descriptor.family == model_core::FormatId::Xbf
        || descriptor.family == model_core::FormatId::Fcstd;
}

bool IsAssimpFamily(const model_core::FormatDescriptor& descriptor)
{
    return descriptor.importer == model_core::ImporterKind::AssimpCompat;
}

std::optional<import_broker::ImportFormat> ToImportFormat(
    const model_core::FormatDescriptor& descriptor)
{
    using model_core::FormatId;
    using import_broker::ImportFormat;
    switch (descriptor.family) {
    case FormatId::Gltf: return ImportFormat::Gltf;
    case FormatId::Stl: return ImportFormat::Stl;
    case FormatId::Ply: return ImportFormat::Ply;
    case FormatId::Obj: return ImportFormat::Obj;
    case FormatId::Fbx: return ImportFormat::Fbx;
    case FormatId::Usd: return ImportFormat::Usd;
    case FormatId::ThreeMf: return ImportFormat::ThreeMf;
    case FormatId::Step: return ImportFormat::Step;
    case FormatId::Iges: return ImportFormat::Iges;
    case FormatId::Brep: return ImportFormat::Brep;
    case FormatId::Xbf: return ImportFormat::Xbf;
    case FormatId::Fcstd: return ImportFormat::Fcstd;
    case FormatId::Dae: case FormatId::ThreeDs: case FormatId::Dxf:
    case FormatId::DirectX: case FormatId::Off: case FormatId::Amf:
    case FormatId::X3d:
        return ImportFormat::Assimp;
    default:
        return std::nullopt;
    }
}

std::optional<model_core::AssimpFormat> ToAssimpFormat(
    const model_core::FormatDescriptor& descriptor)
{
    using model_core::FormatId;
    using model_core::AssimpFormat;
    switch (descriptor.id) {
    case FormatId::Dae: return AssimpFormat::Dae;
    case FormatId::ThreeDs: return AssimpFormat::ThreeDs;
    case FormatId::Dxf: return AssimpFormat::Dxf;
    case FormatId::DirectX: return AssimpFormat::DirectX;
    case FormatId::Off: return AssimpFormat::Off;
    case FormatId::Amf: return AssimpFormat::Amf;
    case FormatId::X3d: return AssimpFormat::X3d;
    default: return std::nullopt;
    }
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

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

struct GeometryCounts {
    uint32_t triangleChunks = 0;
    uint32_t lineChunks = 0;
    uint32_t pointChunks = 0;
    uint64_t triangles = 0;
    uint64_t lines = 0;
    uint64_t points = 0;

    std::string Kind() const
    {
        const int kinds = (triangleChunks > 0 ? 1 : 0) + (lineChunks > 0 ? 1 : 0)
            + (pointChunks > 0 ? 1 : 0);
        if (kinds == 0) return "none";
        if (kinds > 1) return "mixed";
        if (triangleChunks > 0) return "triangles";
        if (lineChunks > 0) return "lines";
        return "points";
    }
};

GeometryCounts CountGeometry(const import_broker::ImportSessionResult& result)
{
    GeometryCounts counts;
    for (const auto& chunk : result.chunks) {
        switch (chunk.descriptor.topology) {
        case model_core::ChunkTopology::TriangleList:
            ++counts.triangleChunks;
            counts.triangles += chunk.descriptor.indexCount / 3;
            break;
        case model_core::ChunkTopology::LineList:
            ++counts.lineChunks;
            counts.lines += chunk.descriptor.indexCount / 2;
            break;
        case model_core::ChunkTopology::PointList:
            ++counts.pointChunks;
            counts.points += chunk.descriptor.vertexCount;
            break;
        default:
            break;
        }
    }
    return counts;
}

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
    if (optionalFeature) warnings.emplace_back("optional-feature");
    if (texture) warnings.emplace_back("texture-fallback");
    std::sort(warnings.begin(), warnings.end());
    return warnings;
}

bool IsResourceError(model_core::ImportErrorCode code)
{
    using model_core::ImportErrorCode;
    switch (code) {
    case ImportErrorCode::ResourceLimit:
    case ImportErrorCode::OutOfMemory:
    case ImportErrorCode::WorkerTimedOut:
    case ImportErrorCode::PrimarySourceLimit:
    case ImportErrorCode::AggregateSourceLimit:
    case ImportErrorCode::ScratchLimit:
    case ImportErrorCode::ChunkCatalogLimit:
    case ImportErrorCode::DracoPrimitiveLimit:
    case ImportErrorCode::CompatibilityHostLimit:
    case ImportErrorCode::ArchiveLimit:
    case ImportErrorCode::StepHostLimit:
        return true;
    default:
        return false;
    }
}

struct Observation {
    std::string id;
    std::string format;
    std::string route; // extension, content, reject
    std::string detectedFormat;
    std::string outcome;
    std::string error;
    std::string failureClass;
    std::string stage;
    std::vector<std::string> warnings;
    size_t chunks = 0;
    uint32_t batches = 0;
    uint64_t elapsedMs = 0;
    GeometryCounts geometry;
    std::string skippedReason;
};

Observation GateFailure(const ManifestEntry& entry,
                        const model_core::FormatClassification& classification,
                        const std::string& route)
{
    Observation observation;
    observation.id = entry.id;
    observation.format = entry.formatName;
    observation.route = route;
    observation.detectedFormat = classification.content
        ? std::string(classification.content->stableName) : "unknown";
    observation.outcome = "failed";
    observation.error = "UnsupportedFormat";
    // A known suffix that disagrees with strong content, or an unknown suffix
    // with a detected candidate, is an extension/dispatch gap. Only a suffix
    // that is both unknown and content-unknown is a sniffer gap.
    observation.failureClass = classification.content ? "extension" : "sniffer";
    observation.stage = "OpenGate";
    return observation;
}

std::optional<Observation> Observe(const ManifestEntry& entry, std::string& skippedReason)
{
    const std::wstring sourcePath
        = JoinPath(PREVIEW3D_REPO_ROOT, Utf8ToWide(entry.relativePath));
    const std::wstring extension = ExtensionOf(sourcePath);

    const model_core::SniffResult content = model_core::SniffFile(sourcePath);
    const model_core::FormatClassification classification
        = model_core::ClassifyFormat(extension, content);
    const model_core::OpenFormatDecision decision = model_core::DecideOpenFormat(classification);
    if (decision.action != model_core::OpenFormatAction::OpenExtension) {
        return GateFailure(entry, classification,
            classification.content ? "content" : "reject");
    }

    const model_core::FormatDescriptor* descriptor = decision.format;
    if (!descriptor) {
        return GateFailure(entry, classification, "reject");
    }

    if (entry.requiresStepHost || IsCadFamily(*descriptor)) {
        if (!std::filesystem::exists(PREVIEW3D_STEP_HOST_EXE)) {
            skippedReason = "step-host-missing";
            return std::nullopt;
        }
    } else if (entry.requiresHost || descriptor->family == model_core::FormatId::Usd) {
        if (!std::filesystem::exists(PREVIEW3D_IMPORT_HOST_EXE)) {
            skippedReason = "host-missing";
            return std::nullopt;
        }
    }

    const auto brokerFormat = ToImportFormat(*descriptor);
    if (!brokerFormat) {
        return GateFailure(entry, classification, "extension");
    }

    import_broker::ImportSessionRequest request{};
    request.workerExePath = PREVIEW3D_IMPORT_WORKER_EXE;
    request.sourcePath = sourcePath;
    request.format = *brokerFormat;
    if (IsAssimpFamily(*descriptor)) {
        const auto assimpFormat = ToAssimpFormat(*descriptor);
        if (!assimpFormat) {
            return GateFailure(entry, classification, "extension");
        }
        request.assimpFormat = *assimpFormat;
    }
    request.generationId = 0x5600'0000ull + static_cast<uint64_t>(entry.id.size());
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
    observation.format = entry.formatName;
    observation.route = classification.reason == model_core::ClassificationReason::Both
        || classification.reason == model_core::ClassificationReason::Extension
        ? "extension" : "content";
    observation.detectedFormat = classification.content
        ? std::string(classification.content->stableName) : "unknown";
    observation.chunks = result.chunks.size();
    observation.batches = result.batchCount;
    observation.elapsedMs = static_cast<uint64_t>(elapsed.count());
    observation.stage = StageName(result.stage);
    observation.geometry = CountGeometry(result);
    observation.warnings = result.ok ? CollectWarnings(result) : std::vector<std::string>{};

    if (!result.ok) {
        observation.outcome = "failed";
        observation.error = compat_test::ImportErrorName(result.errorCode);
        if (result.errorCode == model_core::ImportErrorCode::EmptyGeometry) {
            observation.failureClass = "primitive";
        } else if (IsResourceError(result.errorCode)) {
            observation.failureClass = "resource";
        } else {
            observation.failureClass = "importer";
        }
    } else if (observation.geometry.Kind() == "none") {
        // A successful broker result with no geometry is not "opened"; it is a
        // harness/schema failure the corpus must not hide.
        observation.outcome = "failed";
        observation.error = "NoGeometryChunks";
        observation.failureClass = "importer";
        observation.stage = "ValidateSection";
    } else if (!observation.warnings.empty()) {
        observation.outcome = "degraded";
        observation.error = "none";
        observation.failureClass = "none";
    } else {
        observation.outcome = "opened";
        observation.error = "none";
        observation.failureClass = "none";
    }
    return observation;
}

std::string Describe(const Observation& observation)
{
    std::ostringstream text;
    text << "id=" << observation.id
         << " format=" << observation.format
         << " route=" << observation.route
         << " detected=" << observation.detectedFormat
         << " outcome=" << observation.outcome
         << " error=" << observation.error
         << " failureClass=" << observation.failureClass
         << " stage=" << observation.stage
         << " chunks=" << observation.chunks
         << " batches=" << observation.batches
         << " geometry=" << observation.geometry.Kind()
         << " triangles=" << observation.geometry.triangles
         << " lines=" << observation.geometry.lines
         << " points=" << observation.geometry.points
         << " elapsedMs=" << observation.elapsedMs;
    if (!observation.warnings.empty()) {
        text << " warnings=";
        for (size_t i = 0; i < observation.warnings.size(); ++i) {
            text << (i == 0 ? "" : ",") << observation.warnings[i];
        }
    }
    return text.str();
}

// Returns every mismatch as a path-free, fixture-id-prefixed sentence. The
// checked cases assert this is empty; the self-check case proves a wrong
// expectation produces a non-empty result instead of silently passing.
std::vector<std::string> CheckObservation(const ManifestEntry& entry,
                                          const Observation& observation)
{
    std::vector<std::string> failures;
    auto fail = [&](const std::string& message) {
        failures.push_back("fixture '" + entry.id + "': " + message);
    };

    if (observation.outcome != ExpectedName(entry.expected)) {
        fail(std::string("expected outcome ") + ExpectedName(entry.expected)
             + " but observed " + observation.outcome);
    }
    if (observation.failureClass != FailureClassName(entry.failureClass)) {
        fail(std::string("expected failureClass ") + FailureClassName(entry.failureClass)
             + " but observed " + observation.failureClass);
    }

    if (entry.expected == ExpectedOutcome::Failed) {
        if (observation.error != entry.expectedError) {
            fail("expected error " + entry.expectedError
                 + " but observed " + observation.error);
        }
        if (observation.error == "none") {
            fail("a failed entry must observe a typed error, not none");
        }
    } else {
        if (observation.error != "none") {
            fail("expected no error but observed " + observation.error);
        }
        if (observation.geometry.Kind() != GeometryName(entry.expectedGeometry)) {
            fail(std::string("expected geometry ") + GeometryName(entry.expectedGeometry)
                 + " but observed " + observation.geometry.Kind());
        }
    }

    if (entry.expected == ExpectedOutcome::Degraded && observation.warnings.empty()) {
        fail("expected a compatibility warning but observed none");
    }
    if (entry.expected == ExpectedOutcome::Opened && !observation.warnings.empty()) {
        fail("expected no compatibility warning but observed "
             + std::to_string(observation.warnings.size()));
    }
    if (!entry.expectedWarnings.empty() && observation.warnings != entry.expectedWarnings) {
        std::ostringstream text;
        text << "expected warnings [";
        for (size_t i = 0; i < entry.expectedWarnings.size(); ++i) {
            text << (i == 0 ? "" : ",") << entry.expectedWarnings[i];
        }
        text << "] but observed [";
        for (size_t i = 0; i < observation.warnings.size(); ++i) {
            text << (i == 0 ? "" : ",") << observation.warnings[i];
        }
        text << "]";
        fail(text.str());
    }
    return failures;
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

void WriteReport(const Manifest& manifest, const std::vector<Observation>& observations)
{
    const std::filesystem::path directory
        = std::filesystem::path(PREVIEW3D_REPO_ROOT) / L"artifacts" / L"valid-file";
    std::filesystem::create_directories(directory);
    const std::filesystem::path path
        = directory / (std::wstring(PREVIEW3D_BUILD_CONFIGURATION) + L".json");

    std::map<std::string, size_t> byFormat;
    std::map<std::string, size_t> byFailureClass;
    size_t opened = 0;
    size_t degraded = 0;
    size_t failed = 0;
    size_t skipped = 0;
    for (const auto& observation : observations) {
        ++byFormat[observation.format];
        ++byFailureClass[observation.failureClass];
        if (observation.outcome == "opened") ++opened;
        else if (observation.outcome == "degraded") ++degraded;
        else if (observation.outcome == "failed") ++failed;
        else ++skipped;
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("cannot write valid-file report");
    }
    stream << "{\n  \"schema\": 1,\n  \"configuration\": \""
           << EscapeJson(WideToUtf8(PREVIEW3D_BUILD_CONFIGURATION))
           << "\",\n  \"manifestEntries\": " << manifest.entries.size()
           << ",\n  \"summary\": {\"opened\": " << opened << ", \"degraded\": "
           << degraded << ", \"failed\": " << failed << ", \"skipped\": " << skipped
           << "},\n  \"byFormat\": {";
    bool first = true;
    for (const auto& [format, count] : byFormat) {
        stream << (first ? "" : ", ") << "\"" << EscapeJson(format) << "\": " << count;
        first = false;
    }
    stream << "},\n  \"byFailureClass\": {";
    first = true;
    for (const auto& [failureClass, count] : byFailureClass) {
        stream << (first ? "" : ", ") << "\"" << EscapeJson(failureClass)
               << "\": " << count;
        first = false;
    }
    stream << "},\n  \"entries\": [\n";
    for (size_t i = 0; i < observations.size(); ++i) {
        const Observation& observation = observations[i];
        stream << "    {\"id\": \"" << EscapeJson(observation.id)
               << "\", \"format\": \"" << EscapeJson(observation.format)
               << "\", \"route\": \"" << EscapeJson(observation.route)
               << "\", \"detectedFormat\": \"" << EscapeJson(observation.detectedFormat)
               << "\", \"outcome\": \"" << observation.outcome
               << "\", \"error\": \"" << EscapeJson(observation.error)
               << "\", \"failureClass\": \"" << EscapeJson(observation.failureClass)
               << "\", \"stage\": \"" << EscapeJson(observation.stage)
               << "\", \"warnings\": [";
        for (size_t w = 0; w < observation.warnings.size(); ++w) {
            stream << (w == 0 ? "" : ", ") << "\""
                   << EscapeJson(observation.warnings[w]) << "\"";
        }
        stream << "], \"chunks\": " << observation.chunks
               << ", \"batches\": " << observation.batches
               << ", \"triangles\": " << observation.geometry.triangles
               << ", \"lines\": " << observation.geometry.lines
               << ", \"points\": " << observation.geometry.points
               << ", \"elapsedMs\": " << observation.elapsedMs;
        if (!observation.skippedReason.empty()) {
            stream << ", \"skipped\": \"" << EscapeJson(observation.skippedReason) << "\"";
        }
        stream << "}" << (i + 1 == observations.size() ? "" : ",") << "\n";
    }
    stream << "  ]\n}\n";
}

} // namespace

TEST_CASE("The valid-file manifest is readable and self-consistent", "[valid-file]")
{
    INFO("manifest: " << WideToUtf8(PREVIEW3D_VALID_FILE_MANIFEST));
    Manifest manifest;
    REQUIRE_NOTHROW(manifest = LoadManifest());
    REQUIRE_FALSE(manifest.entries.empty());

    std::map<std::string, size_t> ids;
    for (const auto& entry : manifest.entries) {
        CAPTURE(entry.id);
        CHECK_FALSE(entry.id.empty());
        CHECK_FALSE(entry.relativePath.empty());
        CHECK(ids.emplace(entry.id, ids.size()).second);
        CHECK(entry.format != nullptr);
        INFO("fixture path: " << entry.relativePath);
        CHECK(std::filesystem::exists(
            JoinPath(PREVIEW3D_REPO_ROOT, Utf8ToWide(entry.relativePath))));

        // Closed schema consistency: an opened/degraded entry publishes
        // geometry and has no failure class; a failed entry is typed and
        // publishes no geometry.
        if (entry.expected == ExpectedOutcome::Failed) {
            CHECK(entry.failureClass != FailureClass::None);
            CHECK(entry.expectedGeometry == ExpectedGeometry::None);
            CHECK(entry.expectedError != "none");
        } else {
            CHECK(entry.failureClass == FailureClass::None);
            CHECK(entry.expectedGeometry != ExpectedGeometry::None);
            CHECK(entry.expectedError == "none");
        }
    }
}

TEST_CASE("Every valid-file corpus entry reaches its recorded outcome", "[valid-file]")
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
        if (observation->outcome == "opened") ++opened;
        else if (observation->outcome == "degraded") ++degraded;
        else ++failed;

        INFO(Describe(*observation));
        const std::vector<std::string> failures = CheckObservation(entry, *observation);
        for (const auto& failure : failures) {
            FAIL_CHECK(failure);
        }
    }
    INFO("valid-file baseline: opened=" << opened << " degraded=" << degraded
                                        << " failed=" << failed << " skipped=" << skipped);
}

TEST_CASE("The valid-file checker rejects a deliberately wrong expectation",
    "[valid-file-self-check]")
{
    ManifestEntry entry;
    entry.id = "self-check-fixture";
    entry.formatName = "stl";
    entry.expected = ExpectedOutcome::Opened;
    entry.expectedError = "none";
    entry.expectedGeometry = ExpectedGeometry::Triangles;
    entry.failureClass = FailureClass::None;

    Observation observation;
    observation.id = entry.id;
    observation.format = entry.formatName;
    observation.outcome = "failed";
    observation.error = "MalformedData";
    observation.failureClass = "importer";
    observation.geometry.triangles = 0;

    const auto failures = CheckObservation(entry, observation);
    REQUIRE_FALSE(failures.empty());
    const std::string combined = [&] {
        std::ostringstream text;
        for (const auto& failure : failures) text << failure << "\n";
        return text.str();
    }();
    CHECK(combined.find("self-check-fixture") != std::string::npos);
    CHECK(combined.find("MalformedData") != std::string::npos);

    // A wrong geometry kind on an otherwise successful observation must also
    // be reported with the observed value.
    Observation geometryMismatch;
    geometryMismatch.id = entry.id;
    geometryMismatch.format = entry.formatName;
    geometryMismatch.outcome = "opened";
    geometryMismatch.error = "none";
    geometryMismatch.failureClass = "none";
    geometryMismatch.geometry.triangleChunks = 0;
    geometryMismatch.geometry.lineChunks = 1;
    geometryMismatch.geometry.lines = 3;
    const auto geometryFailures = CheckObservation(entry, geometryMismatch);
    REQUIRE_FALSE(geometryFailures.empty());
    bool namedObserved = false;
    for (const auto& failure : geometryFailures) {
        if (failure.find("observed lines") != std::string::npos) namedObserved = true;
    }
    CHECK(namedObserved);
}

TEST_CASE("Write the valid-file corpus report", "[write-valid-file-report]")
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
            skippedObservation.format = entry.formatName;
            skippedObservation.outcome = "skipped";
            skippedObservation.error = "none";
            skippedObservation.failureClass = "none";
            skippedObservation.skippedReason = skipReason;
            observations.push_back(std::move(skippedObservation));
            continue;
        }
        observations.push_back(*observation);
    }
    REQUIRE_NOTHROW(WriteReport(manifest, observations));
    for (const auto& observation : observations) {
        INFO(Describe(observation));
        CHECK_FALSE(observation.id.empty());
    }
}
