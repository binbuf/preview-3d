#include "model_core/TierALimits.h"
#include "D3D12ImportBridge.h"
#include "Localization.h"
#include <cstdio>

#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "model_core/PixelFormats.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <functional>
#include <unordered_map>

namespace d3d12_import_bridge {

namespace {

std::wstring ToLower(std::wstring s)
{
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(towlower(c));
    }
    return s;
}

// Sidecar references cross the protocol as the worker's own UTF-8 bytes.
// User-facing text is wide; an undecodable reference is shown as its raw
// bytes rather than silently dropped, so the user still sees that *something*
// referenced by the model could not be found.
std::wstring Utf8ReferenceToWide(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (required <= 0) {
        return std::wstring(utf8.begin(), utf8.end());
    }
    std::wstring wide(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), required);
    return wide;
}

std::wstring ExtensionOf(const std::wstring& path)
{
    auto dot = path.find_last_of(L'.');
    auto slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
        return {};
    }
    return ToLower(path.substr(dot + 1));
}

std::wstring ResolveWorkerExePath()
{
    wchar_t modulePath[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring path(modulePath, length);
    auto lastSlash = path.find_last_of(L"\\/");
    std::wstring directory = (lastSlash == std::wstring::npos) ? L"." : path.substr(0, lastSlash);
    // Portable releases keep the AppContainer payload in its own directory.
    // ImportSession grants read/execute to the worker executable's directory,
    // so this layout avoids granting the sandbox access to the viewer, docs,
    // or cleanup tooling. Developer/test builds retain the shared-OutDir
    // fallback used by the solution.
    const std::wstring packaged = directory + L"\\worker\\Preview3DImportWorker.exe";
    if (GetFileAttributesW(packaged.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return packaged;
    }
    return directory + L"\\Preview3DImportWorker.exe";
}

std::wstring ResolveCompatibilityHostExePath()
{
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    const std::wstring path(modulePath, length);
    const auto lastSlash = path.find_last_of(L"\\/");
    const std::wstring directory = (lastSlash == std::wstring::npos) ? L"." : path.substr(0, lastSlash);
    // Both packaged and solution builds keep OpenUSD's executable, DLLs, and
    // hash-verified resources in this private sibling directory.
    return directory + L"\\OpenUsdHost\\Preview3DImportHost.exe";
}

std::wstring ResolveStepHostExePath()
{
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    const std::wstring path(modulePath, length);
    const auto lastSlash = path.find_last_of(L"\\/");
    const std::wstring directory = (lastSlash == std::wstring::npos) ? L"." : path.substr(0, lastSlash);
    // The dedicated OCCT STEP host and its signed payload closure live in
    // their own sibling directory, separate from the viewer, the general
    // worker, and the USD compatibility host.
    return directory + L"\\StepHost\\Preview3DStepHost.exe";
}

void DescribeImportError(model_core::ImportErrorCode code, std::wstring& summary, std::wstring& details)
{
    switch (code) {
    case model_core::ImportErrorCode::UnsupportedEncoding:
        summary = Loc("fb.summary.this.encoding.is.not.supported", L"This encoding is not supported.");
        details = Loc("fb.details.use.a.supported.gltf.stl.ply.obj.fbx.3mf.usd.or", L"Use a supported glTF, STL, PLY, OBJ, FBX, 3MF, USD, or clear-text STEP encoding."); return;
    case model_core::ImportErrorCode::WorkerCrashed:
        summary = Loc("fb.summary.the.sandboxed.importer.stopped.unexpectedly", L"The sandboxed importer stopped unexpectedly.");
        details = Loc("fb.details.the.worker.exited.before.completing.this.model.r", L"The worker exited before completing this model. Retry or open another model."); return;
    case model_core::ImportErrorCode::UnsupportedRequiredFeature:
        summary = Loc("fb.summary.this.model.requires.an.unsupported.feature", L"This model requires an unsupported feature.");
        details = Loc("fb.details.export.a.static.model.using.the.documented.suppo", L"Export a static model using the documented supported subset."); return;
    case model_core::ImportErrorCode::UnsupportedComposition:
        summary = Loc("fb.summary.this.usd.stage.requires.compatibility.import", L"This USD stage requires compatibility import.");
        details = Loc("fb.details.the.fast.importer.requested.the.isolated.compati", L"The fast importer requested the isolated compatibility host."); return;
    case model_core::ImportErrorCode::CompatibilityHostFailure:
        summary = Loc("fb.summary.the.usd.compatibility.importer.stopped.unexpecte", L"The USD compatibility importer stopped unexpectedly.");
        details = Loc("fb.details.the.isolated.compatibility.host.could.not.comple", L"The isolated compatibility host could not complete this model."); return;
    case model_core::ImportErrorCode::CompatibilityHostLimit:
        summary = Loc("fb.summary.this.usd.stage.is.too.large.or.complex.to.previe", L"This USD stage is too large or complex to preview.");
        details = Loc("fb.details.the.compatibility.host.reached.a.bounded.resourc", L"The compatibility host reached a bounded resource limit."); return;
    case model_core::ImportErrorCode::StepHostFailure:
        summary = Loc("fb.summary.the.step.importer.stopped.unexpectedly", L"The STEP importer stopped unexpectedly.");
        details = Loc("fb.details.the.isolated.step.host.could.not.complete.this.m", L"The isolated STEP host could not complete this model."); return;
    case model_core::ImportErrorCode::StepHostLimit:
        summary = Loc("fb.summary.this.step.model.is.too.large.or.complex.to.previ", L"This STEP model is too large or complex to preview.");
        details = Loc("fb.details.the.step.host.reached.a.bounded.resource.limit", L"The STEP host reached a bounded resource limit."); return;
    case model_core::ImportErrorCode::TessellationFailed:
        summary = Loc("fb.summary.this.step.model.could.not.be.tessellated", L"This STEP model could not be tessellated.");
        details = Loc("fb.details.one.or.more.shapes.exceeded.the.supported.cad.te", L"One or more shapes exceeded the supported CAD tessellation budget. Export a simpler solid or assembly."); return;
    case model_core::ImportErrorCode::ArchiveLimit:
        summary = Loc("fb.summary.this.model.archive.is.not.supported", L"This model archive is not supported.");
        details = Loc("fb.details.the.archive.violates.a.path.structure.compressio", L"The archive violates a path, structure, compression, or expansion limit."); return;
    case model_core::ImportErrorCode::EmptyGeometry:
        summary = Loc("fb.summary.this.model.has.no.displayable.geometry", L"This model has no displayable geometry.");
        details = Loc("fb.details.no.valid.triangles.or.points.remain.in.the.selec", L"No valid triangles or points remain in the selected scene."); return;
    case model_core::ImportErrorCode::OutOfMemory:
        summary = Loc("fb.summary.there.is.not.enough.memory.to.preview.this.model", L"There is not enough memory to preview this model.");
        details = Loc("fb.details.close.other.applications.or.export.a.smaller.mod", L"Close other applications or export a smaller model, then retry."); return;
    case model_core::ImportErrorCode::FileChanged:
        summary = Loc("fb.summary.the.source.changed.during.import", L"The source changed during import.");
        details = Loc("fb.details.save.a.stable.local.copy.of.the.model.and.its.si", L"Save a stable local copy of the model and its sidecars, then retry."); return;
    case model_core::ImportErrorCode::ImportProtocolViolation:
        summary = Loc("fb.summary.the.importer.returned.invalid.data", L"The importer returned invalid data.");
        details = Loc("fb.details.the.sandbox.response.failed.validation.and.was.d", L"The sandbox response failed validation and was discarded."); return;
    case model_core::ImportErrorCode::MalformedData:
        summary = Loc("fb.summary.this.file.could.not.be.read", L"This file could not be read.");
        details = Loc("fb.details.the.importer.found.data.that.doesn.t.match.the.e", L"The importer found data that doesn't match the expected file format.");
        return;
    case model_core::ImportErrorCode::PrimarySourceLimit:
        summary = Loc("fb.summary.the.primary.source.exceeds.the.import.limit", L"The primary source exceeds the import limit.");
        details = Loc("fb.details.ascii.stl.ply.files.are.limited.to.2.gib.tier.a", L"ASCII STL/PLY files are limited to 2 GiB; Tier A primary files are limited to 8 GiB.");
        return;
    case model_core::ImportErrorCode::AggregateSourceLimit:
        summary = Loc("fb.summary.the.model.and.sidecars.exceed.the.import.limit", L"The model and sidecars exceed the import limit.");
        details = Loc("fb.details.combined.local.source.files.are.limited.to.12.gi", L"Combined local source files are limited to 12 GiB.");
        return;
    case model_core::ImportErrorCode::ScratchLimit:
        summary = Loc("fb.summary.the.importer.scratch.budget.was.exceeded", L"The importer scratch budget was exceeded.");
        details = Loc("fb.details.import.scratch.is.limited.to.1.gib.or.25.of.phys", L"Import scratch is limited to 1 GiB or 25% of physical RAM, whichever is lower.");
        return;
    case model_core::ImportErrorCode::ChunkCatalogLimit:
        summary = Loc("fb.summary.the.model.has.too.many.source.ranges", L"The model has too many source ranges.");
        details = Loc("fb.details.the.bounded.geometry.material.and.image.catalog", L"The bounded geometry, material and image catalog cannot accept more entries.");
        return;
    case model_core::ImportErrorCode::DracoPrimitiveLimit:
        summary = Loc("fb.summary.a.compressed.primitive.exceeds.the.decode.limit", L"A compressed primitive exceeds the decode limit.");
        details = Loc("fb.details.one.draco.primitive.is.limited.to.512.mib.of.est", L"One Draco primitive is limited to 512 MiB of estimated decode work and 10 million triangles.");
        return;
    case model_core::ImportErrorCode::ResourceLimit:
        summary = Loc("fb.summary.this.model.is.too.large.to.preview", L"This model is too large to preview.");
        details = Loc("fb.details.the.file.exceeds.a.resource.limit.the.sandboxed", L"The file exceeds a resource limit the sandboxed importer enforces.");
        return;
    case model_core::ImportErrorCode::UnsafeReference:
        summary = Loc("fb.summary.this.model.could.not.be.previewed", L"This model could not be previewed.");
        details = Loc("fb.details.the.file.references.another.file.in.a.way.that.i", L"The file references another file in a way that isn't allowed.");
        return;
    case model_core::ImportErrorCode::FileUnavailable:
        summary = Loc("fb.summary.this.model.could.not.be.previewed.2", L"This model could not be previewed.");
        details = Loc("fb.details.a.file.this.model.depends.on.could.not.be.opened", L"A file this model depends on could not be opened.");
        return;
    case model_core::ImportErrorCode::InternalImporterFailure:
    case model_core::ImportErrorCode::None:
    default:
        summary = Loc("fb.summary.this.model.could.not.be.previewed.3", L"This model could not be previewed.");
        details = Loc("fb.details.the.importer.stopped.unexpectedly.while.reading", L"The importer stopped unexpectedly while reading the file.");
        return;
    }
}

// Turns a typed session failure into the user-facing pair. Every host-side
// plumbing stage keeps the distinct wording it had when this sequence lived
// inline here; only the two stages that carry a worker/validator error code
// defer to DescribeImportError.
void DescribeSessionFailureInternal(const import_broker::ImportSessionResult& session, std::wstring& summary,
                             std::wstring& details)
{
    using import_broker::ImportStage;
    switch (session.stage) {
    case ImportStage::OpenSource:
        summary = Loc("fb.summary.this.file.could.not.be.opened", L"This file could not be opened.");
        details = session.openError;
        return;
    case ImportStage::DuplicateSourceHandle:
        summary = Loc("fb.summary.this.file.could.not.be.prepared.for.preview", L"This file could not be prepared for preview.");
        details = Loc("fb.details.the.file.handle.could.not.be.shared.with.the.san", L"The file handle could not be shared with the sandboxed importer.");
        return;
    case ImportStage::CreateOutputSection:
        summary = Loc("fb.summary.this.model.could.not.be.previewed.4", L"This model could not be previewed.");
        details = Loc("fb.details.a.shared.memory.section.for.the.importer.s.outpu", L"A shared memory section for the importer's output could not be created.");
        return;
    case ImportStage::CreateSandboxProfile:
        summary = Loc("fb.summary.this.model.could.not.be.previewed.5", L"This model could not be previewed.");
        details = Loc("fb.details.the.sandbox.container.for.the.importer.could.not", L"The sandbox container for the importer could not be created.");
        return;
    case ImportStage::CreateControlChannel:
        summary = Loc("fb.summary.this.model.could.not.be.previewed.6", L"This model could not be previewed.");
        details = Loc("fb.details.the.control.channel.to.the.sandboxed.importer.co", L"The control channel to the sandboxed importer could not be created.");
        return;
    case ImportStage::LaunchWorker:
        summary = Loc("fb.summary.this.model.could.not.be.previewed.7", L"This model could not be previewed.");
        details = Loc("fb.details.the.sandboxed.importer.process.could.not.be.star", L"The sandboxed importer process could not be started.");
        return;
    case ImportStage::ResumeWorker:
        summary = Loc("fb.summary.this.model.could.not.be.previewed.8", L"This model could not be previewed.");
        details = Loc("fb.details.the.sandboxed.importer.process.could.not.be.resu", L"The sandboxed importer process could not be resumed.");
        return;
    case ImportStage::SendRequest:
        summary = Loc("fb.summary.this.model.could.not.be.previewed.9", L"This model could not be previewed.");
        details = Loc("fb.details.the.import.request.could.not.be.sent.to.the.sand", L"The import request could not be sent to the sandboxed importer.");
        return;
    case ImportStage::SidecarRequestLimit:
        summary = Loc("fb.summary.this.model.could.not.be.previewed.10", L"This model could not be previewed.");
        details = Loc("fb.details.the.sandboxed.importer.made.too.many.file.reques", L"The sandboxed importer made too many file requests.");
        return;
    case ImportStage::AwaitReply:
        summary = Loc("fb.summary.this.model.could.not.be.previewed.11", L"This model could not be previewed.");
        details = Loc("fb.details.the.sandboxed.importer.did.not.respond", L"The sandboxed importer did not respond.");
        return;
    case ImportStage::ReplyTimedOut:
        summary = Loc("fb.summary.this.model.could.not.be.previewed.12", L"This model could not be previewed.");
        details = Loc("fb.details.the.sandboxed.importer.stopped.responding.and.wa", L"The sandboxed importer stopped responding and was shut down.");
        return;
    case ImportStage::Cancelled:
        // Superseded or closing: the caller drops the result rather than
        // showing it, so this text exists only so no path returns empty.
        summary = Loc("fb.summary.this.preview.was.cancelled", L"This preview was cancelled.");
        details = Loc("fb.details.a.newer.file.was.opened.or.the.window.was.closed", L"A newer file was opened, or the window was closed.");
        return;
    case ImportStage::UnexpectedReply:
        summary = Loc("fb.summary.this.model.could.not.be.previewed.13", L"This model could not be previewed.");
        details = Loc("fb.details.the.sandboxed.importer.returned.an.unexpected.re", L"The sandboxed importer returned an unexpected response.");
        return;
    case ImportStage::MapOutputSection:
        summary = Loc("fb.summary.this.model.could.not.be.previewed.14", L"This model could not be previewed.");
        details = Loc("fb.details.the.importer.s.output.could.not.be.read", L"The importer's output could not be read.");
        return;
    case ImportStage::WorkerReportedError:
    case ImportStage::ValidateSection:
    case ImportStage::Completed:
    default:
        DescribeImportError(session.errorCode, summary, details);
        return;
    }
}

import_broker::ImportFormat ToBrokerFormat(SourceFormat format)
{
    switch (format) {
    case SourceFormat::Stl:
        return import_broker::ImportFormat::Stl;
    case SourceFormat::Ply:
        return import_broker::ImportFormat::Ply;
    case SourceFormat::Obj:
        return import_broker::ImportFormat::Obj;
    case SourceFormat::Fbx:
        return import_broker::ImportFormat::Fbx;
    case SourceFormat::ThreeMf:
        return import_broker::ImportFormat::ThreeMf;
    case SourceFormat::Usd:
        return import_broker::ImportFormat::Usd;
    case SourceFormat::Step:
        return import_broker::ImportFormat::Step;
    case SourceFormat::Glb:
    default:
        return import_broker::ImportFormat::Gltf;
    }
}

constexpr uint32_t kMaxSidecarRequestsPerGeneration = 64;
constexpr uint64_t kMaxSidecarFileBytes = 8ull * 1024ull * 1024ull * 1024ull;

// Derived from normalized expansion, logical occurrence tails, material slots
// and immutable texture replacements; also safe for one-chunk test windows.
constexpr uint32_t kMaxChunkBatchesPerGeneration = model_core::kTierABatchLimit;

} // namespace

void DescribeSessionFailure(const import_broker::ImportSessionResult& session, std::wstring& summary, std::wstring& details)
{
    DescribeSessionFailureInternal(session, summary, details);
    if (session.stage == import_broker::ImportStage::ValidateSection
        && session.errorCode == model_core::ImportErrorCode::MalformedData)
        DescribeImportError(model_core::ImportErrorCode::ImportProtocolViolation, summary, details);
    // Codes with a specific document explanation override generic stage text.
    if (session.errorCode == model_core::ImportErrorCode::FileChanged ||
        session.errorCode == model_core::ImportErrorCode::OutOfMemory ||
        session.errorCode == model_core::ImportErrorCode::WorkerCrashed ||
        session.errorCode == model_core::ImportErrorCode::ResourceLimit ||
        session.errorCode == model_core::ImportErrorCode::StepHostFailure ||
        session.errorCode == model_core::ImportErrorCode::StepHostLimit ||
        session.errorCode == model_core::ImportErrorCode::TessellationFailed ||
        (session.errorCode >= model_core::ImportErrorCode::PrimarySourceLimit &&
         session.errorCode <= model_core::ImportErrorCode::ArchiveLimit))
        DescribeImportError(session.errorCode, summary, details);
    // Every non-resource compatibility-host fault collapses to one closed code
    // by design, so the product-owned stage/phase is the only remaining detail.
    // Surface it rather than the generic one-liner.
    if (session.errorCode == model_core::ImportErrorCode::CompatibilityHostFailure) {
        using import_broker::ImportStage;
        switch (session.stage) {
        case ImportStage::WorkerReportedError:
            summary = Loc("fb.summary.the.usd.compatibility.importer.could.not.complet", L"The USD compatibility importer could not complete this stage.");
            switch (session.errorPhase) {
            case model_core::ImportFailurePhase::Geometry:
                details = Loc("fb.details.the.compatibility.host.stopped.while.parsing.or", L"The compatibility host stopped while parsing or normalizing the stage geometry."); break;
            case model_core::ImportFailurePhase::Sidecars:
                details = Loc("fb.details.the.compatibility.host.stopped.while.resolving.t", L"The compatibility host stopped while resolving the stage's referenced layers or sidecars."); break;
            case model_core::ImportFailurePhase::Textures:
                details = Loc("fb.details.the.compatibility.host.stopped.while.decoding.th", L"The compatibility host stopped while decoding the stage's textures."); break;
            default:
                details = Loc("fb.details.the.compatibility.host.stopped.before.producing", L"The compatibility host stopped before producing preview geometry."); break;
            }
            break;
        case ImportStage::ValidateSection:
        case ImportStage::UnexpectedReply:
            summary = Loc("fb.summary.the.usd.compatibility.importer.returned.invalid", L"The USD compatibility importer returned invalid data.");
            details = Loc("fb.details.the.compatibility.host.response.failed.validatio", L"The compatibility host response failed validation and was discarded.");
            break;
        case ImportStage::AwaitReply:
        case ImportStage::ReplyTimedOut:
            summary = Loc("fb.summary.the.usd.compatibility.importer.stopped.respondin", L"The USD compatibility importer stopped responding.");
            details = Loc("fb.details.the.compatibility.host.was.shut.down.before.it.c", L"The compatibility host was shut down before it completed this stage.");
            break;
        case ImportStage::CreateOutputSection:
        case ImportStage::CreateSandboxProfile:
        case ImportStage::CreateControlChannel:
        case ImportStage::LaunchWorker:
        case ImportStage::ResumeWorker:
        case ImportStage::SendRequest:
            summary = Loc("fb.summary.the.usd.compatibility.importer.could.not.be.star", L"The USD compatibility importer could not be started.");
            details = Loc("fb.details.the.isolated.compatibility.host.process.or.its.s", L"The isolated compatibility host process or its shared resources could not be prepared.");
            break;
        default:
            break;
        }
    }
}

std::wstring SourceFormatLabel(const std::wstring& path)
{
    const auto ext = ExtensionOf(path);
    if (ext == L"gltf") return L"glTF";
    if (ext == L"glb") return L"GLB";
    if (ext == L"stl") return L"STL";
    if (ext == L"ply") return L"PLY";
    if (ext == L"obj") return L"OBJ";
    if (ext == L"fbx") return L"FBX";
    if (ext == L"3mf") return L"3MF";
    if (ext == L"step" || ext == L"stp") return L"STEP";
    if (ext == L"usd" || ext == L"usda" || ext == L"usdc") return L"USD";
    if (ext == L"usdz") return L"USDZ";
    // Extension only, capped and restricted to printable alphanumerics.
    if (ext.empty() || ext.size() > 16) return Loc("format.unknown", L"Unknown");
    std::wstring label;
    for (auto c : ext) {
        if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'))) return Loc("format.unknown", L"Unknown");
        label += wchar_t(towupper(c));
    }
    return label;
}

std::wstring StageLabel(import_broker::ImportStage stage)
{
    using import_broker::ImportStage;
    switch (stage) {
    case ImportStage::Completed: return Loc("stage.complete", L"complete");
    case ImportStage::OpenSource: return Loc("stage.openingSource", L"opening source");
    case ImportStage::WorkerReportedError: return Loc("stage.parsingDecoding", L"parsing / decoding");
    case ImportStage::ValidateSection: case ImportStage::UnexpectedReply:
    case ImportStage::ChunkBatchOutOfOrder: return Loc("stage.validatingImport", L"validating import");
    case ImportStage::ReplyTimedOut: case ImportStage::AwaitReply: return Loc("stage.waitingForImporter", L"waiting for importer");
    case ImportStage::SidecarRequestLimit: return Loc("stage.resolvingSidecars", L"resolving sidecars");
    case ImportStage::ChunkBatchLimit: case ImportStage::ChunkCountLimit: return Loc("stage.acceptingBatches", L"accepting batches");
    case ImportStage::ChunkBatchAckFailed: return Loc("stage.acknowledgingBatch", L"acknowledging batch");
    case ImportStage::Upload: return Loc("stage.uploadingGeometry", L"uploading geometry / textures");
    case ImportStage::Cancelled: return Loc("stage.cancelled", L"cancelled");
    default: return Loc("stage.preparingSandbox", L"preparing sandbox");
    }
}

std::wstring DiagnosticDetails(const std::wstring& path, const ImportResult& result)
{
    std::wstring text = result.errorSummary + L"\r\n\r\n" + result.errorDetails + L"\r\n\r\n";
    text += LocalizedJoin(Loc("diagnostics.format", L"Format:"), SourceFormatLabel(path));
    text += L"\r\n" + LocalizedJoin(Loc("diagnostics.phase", L"Phase:"), FailurePhaseLabel(result));
    text += L"\r\n" + LocalizedJoin(Loc("diagnostics.code", L"Code:"), std::to_wstring(uint32_t(result.errorCode)));
    return text;
}

std::wstring FailurePhaseLabel(const ImportResult& result)
{
    if (result.errorStage == import_broker::ImportStage::WorkerReportedError) {
        switch (result.errorPhase) {
        case model_core::ImportFailurePhase::Geometry: return Loc("phase.geometry", L"parsing geometry");
        case model_core::ImportFailurePhase::Sidecars: return Loc("phase.sidecars", L"resolving sidecars");
        case model_core::ImportFailurePhase::Textures: return Loc("phase.textures", L"decoding textures");
        default: break;
        }
    }
    return StageLabel(result.errorStage);
}

std::optional<SourceFormat> ClassifyByExtension(const std::wstring& path)
{
    std::wstring ext = ExtensionOf(path);
    if (ext == L"glb" || ext == L"gltf") return SourceFormat::Glb;
    if (ext == L"stl") return SourceFormat::Stl;
    if (ext == L"ply") return SourceFormat::Ply;
    if (ext == L"obj") return SourceFormat::Obj;
    if (ext == L"fbx") return SourceFormat::Fbx;
    if (ext == L"3mf") return SourceFormat::ThreeMf;
    // `.step`/`.stp` select the dedicated STEP host, but the extension alone
    // never bypasses STEP-002 admission: StepPart21Preflight verifies the
    // ISO 10303-21 byte envelope on the inherited handle before OCCT runs.
    if (ext == L"step" || ext == L"stp") return SourceFormat::Step;
    if (ext == L"usd" || ext == L"usda" || ext == L"usdc" || ext == L"usdz")
        return SourceFormat::Usd;
    return std::nullopt;
}

void EnsureImportSandboxPrepared()
{
    import_broker::PrepareImportWorkerPoolAsync(ResolveWorkerExePath());
}

ImportResult RunImport(SourceFormat format, const std::wstring& path, uint64_t generationId,
                        std::function<bool()> isCancelled, std::function<void(ImportResult)> onBatch, uint64_t sectionBytes, bool delayBatchesForTesting, uint32_t faultForTesting,
                        std::function<uint32_t()> nextDetail,
                        std::function<void(const model_core::FileIdentity&)> onInitialComplete, std::function<bool(uint64_t)> cpuBudgetAllows,
                        std::function<void(const model_core::StepProgressNotice&)> onStepProgress,
                        std::vector<std::wstring> additionalSidecarSearchRoots)
{
    ImportResult result;
    // Aggregated on the import thread only: the onSidecarUnavailable callback
    // and every onBatch call run sequentially on that same thread, so no
    // synchronization is needed. Each batch carries the snapshot discovered so
    // far, and the terminal result carries the complete list.
    std::vector<std::wstring> missingAssets;
    auto recordMissing = [&missingAssets](const std::string& referenceUtf8) {
        std::wstring reference = Utf8ReferenceToWide(referenceUtf8);
        if (reference.empty()) return;
        if (std::find(missingAssets.begin(), missingAssets.end(), reference) == missingAssets.end())
            missingAssets.push_back(std::move(reference));
    };
    // Startup normally prewarms this pool, but direct/retry callers must not
    // depend on that timing. The coordinator makes repeated preparation cheap.
    EnsureImportSandboxPrepared();

    import_broker::ImportSessionRequest sessionRequest;
    sessionRequest.additionalSidecarSearchRoots = std::move(additionalSidecarSearchRoots);
    sessionRequest.onSidecarUnavailable = recordMissing;
    sessionRequest.enableCoarseProxy = !delayBatchesForTesting && format != SourceFormat::Obj
        && format != SourceFormat::Fbx && format != SourceFormat::ThreeMf
        && format != SourceFormat::Usd && format != SourceFormat::Step;
    sessionRequest.useWorkerPool = !faultForTesting;
    sessionRequest.cpuBudgetAllows=std::move(cpuBudgetAllows);
    if (!delayBatchesForTesting && !faultForTesting && format != SourceFormat::ThreeMf
        && format != SourceFormat::Usd && format != SourceFormat::Step) {
        sessionRequest.nextDetail = std::move(nextDetail);
        sessionRequest.onInitialComplete = std::move(onInitialComplete);
    }
    sessionRequest.isCancelled = std::move(isCancelled);
    sessionRequest.workerExePath = ResolveWorkerExePath();
    if (format == SourceFormat::Usd)
        sessionRequest.compatibilityHostExePath = ResolveCompatibilityHostExePath();
    if (format == SourceFormat::Step) {
        sessionRequest.stepHostExePath = ResolveStepHostExePath();
        sessionRequest.onStepProgress = std::move(onStepProgress);
    }
    sessionRequest.sourcePath = path;
    sessionRequest.format = ToBrokerFormat(format);
    sessionRequest.generationId = generationId;
    sessionRequest.sectionByteCapacity = sectionBytes;
    if (delayBatchesForTesting && format == SourceFormat::Glb)
        sessionRequest.workerArgumentsOverride = L"--parse-gltf-delayed-batches";
    sessionRequest.maxChunksPerGeneration = model_core::kTierACatalogLimit;
    sessionRequest.maxChunkCount = import_broker::kImportMaxChunkCount;
    sessionRequest.maxSidecarRequestsPerGeneration = kMaxSidecarRequestsPerGeneration;
    sessionRequest.maxSidecarFileBytes = kMaxSidecarFileBytes;
    sessionRequest.maxChunkBatchesPerGeneration = kMaxChunkBatchesPerGeneration;
    if (faultForTesting == 1) sessionRequest.workerArgumentsOverride = L"--child-noop";
    if (faultForTesting == 2) {
        sessionRequest.workerArgumentsOverride = L"--test-hang-import";
        sessionRequest.replyTimeoutMs = 500;
    }
    if (faultForTesting == 3) sessionRequest.maxChunkCount = 0;
    if (faultForTesting == 5) sessionRequest.workerArgumentsOverride = L"--test-invalid-import-reply";
    if (faultForTesting == 6) {
        sessionRequest.commitLimitBytes = 1ull * 1024 * 1024;
        sessionRequest.replyTimeoutMs = 500;
    }
    // The dedicated STEP host owns its own attack-mode flags; the general
    // worker override above never reaches it.
    if (format == SourceFormat::Step) {
        if (faultForTesting == 1) sessionRequest.stepHostArgumentsOverride = L"--pool-crash";
        if (faultForTesting == 2) {
            sessionRequest.stepHostArgumentsOverride = L"--pool-hang";
            sessionRequest.replyTimeoutMs = 500;
        }
        if (faultForTesting == 6) {
            sessionRequest.stepHostArgumentsOverride = L"--pool-overallocate";
            sessionRequest.stepHostCommitLimitBytes = 1ull * 1024 * 1024;
            sessionRequest.replyTimeoutMs = 500;
        }
    }
    import_broker::KnownChunkCatalog catalog;
    std::unordered_map<uint32_t, model_core::NodePayload> nodeCatalog;
    struct ResolvedNode { double world[16]{}; bool visible=false; bool active=false; };
    std::unordered_map<uint32_t,ResolvedNode> resolvedNodes;
    model_core::FileIdentity openedIdentity;
    bool initialComplete = false;
    if (sessionRequest.onInitialComplete) {
        auto complete = std::move(sessionRequest.onInitialComplete);
        sessionRequest.onInitialComplete = [&, complete](const auto& identity) {
            initialComplete = true; complete(identity);
        };
    }
    sessionRequest.onSourceOpened=[&](const auto& identity) { openedIdentity=identity; };
    auto unpack = [&](std::vector<import_broker::ValidatedChunk> chunks) {
        ImportResult result;
        result.detail = initialComplete;
        result.sourceIdentity=openedIdentity;
        result.forceUploadFailureForTesting = faultForTesting == 4;
        if (!chunks.empty()) result.scene = chunks.front().scene;
        for (const auto& chunk : chunks) catalog.emplace(chunk.descriptor.chunkId, chunk.descriptor.topology);
        for (auto& chunk : chunks) {
            switch (chunk.descriptor.topology) {
            case model_core::ChunkTopology::Node: {
                ImportedNode node;
                std::memcpy(&node.data, chunk.payload.data(), sizeof(node.data));
                nodeCatalog.emplace(node.data.nodeId,node.data);
                result.nodes.push_back(node);
                break;
            }
            case model_core::ChunkTopology::MeshInstance: {
                ImportedInstance instance;
                std::memcpy(&instance.data, chunk.payload.data(), sizeof(instance.data));
                result.instances.push_back(instance);
                break;
            }
            case model_core::ChunkTopology::CoarseComplete:
                result.coarseComplete = true;
                break;
            case model_core::ChunkTopology::TriangleList:
            case model_core::ChunkTopology::PointList: {
                ImportedMesh mesh;
                mesh.geometry = chunk.descriptor;
                mesh.chunkId = chunk.descriptor.chunkId;
                mesh.topology = chunk.descriptor.topology;
                mesh.vertexLayoutId = static_cast<model_core::VertexLayoutId>(chunk.descriptor.vertexLayoutId);
                mesh.vertexCount = chunk.descriptor.vertexCount;
                mesh.indexCount = chunk.descriptor.indexCount;
                mesh.payload = std::move(chunk.payload);
                // Per WireFormat.h: a mesh's dependencyIds[0] is only a
                // material reference when the target chunk's own topology is
                // Material -- the same slot predates this and is also used for
                // an unrelated LOD/derivation relationship elsewhere, so the
                // target's topology (not the slot position) determines meaning.
                if (chunk.descriptor.dependencyCount >= 1) {
                    uint32_t targetId = chunk.descriptor.dependencyIds[0];
                    auto target = catalog.find(targetId);
                    if (target == catalog.end() || target->second == model_core::ChunkTopology::Material)
                        mesh.materialChunkId = targetId;
                }
                result.meshes.push_back(std::move(mesh));
                break;
            }
            case model_core::ChunkTopology::Material: {
                ImportedMaterial material;
                material.chunkId = chunk.descriptor.chunkId;
                if (chunk.payload.size() == sizeof(model_core::MaterialPayload)) {
                    std::memcpy(&material.data, chunk.payload.data(), sizeof(material.data));
                }
                if (chunk.descriptor.dependencyIds[0]) material.baseColorImageChunkId = chunk.descriptor.dependencyIds[0];
                if (chunk.descriptor.dependencyIds[1]) material.metallicRoughnessImageChunkId = chunk.descriptor.dependencyIds[1];
                if (chunk.descriptor.dependencyIds[2]) material.normalImageChunkId = chunk.descriptor.dependencyIds[2];
                if (chunk.descriptor.dependencyIds[3]) material.emissiveImageChunkId = chunk.descriptor.dependencyIds[3];
                result.materials.push_back(std::move(material));
                break;
            }
            case model_core::ChunkTopology::Image: {
                ImportedImage image;
                image.chunkId = chunk.descriptor.chunkId;
                if (chunk.payload.size() >= sizeof(model_core::ImagePayloadHeader)) {
                    model_core::ImagePayloadHeader header{};
                    std::memcpy(&header, chunk.payload.data(), sizeof(header));
                    image.logicalChunkId=header.reserved0;
                    image.pixelFormat = static_cast<model_core::PixelFormatId>(header.pixelFormat);
                    image.width = header.width;
                    image.height = header.height;
                    image.mipLevels = header.mipLevels;
                    image.colorSpace = static_cast<model_core::ColorSpaceId>(header.colorSpace);
                    image.pixelBytes.assign(chunk.payload.begin() + sizeof(header), chunk.payload.end());
                }
                result.images.push_back(std::move(image));
                break;
            }
            case model_core::ChunkTopology::ImportStatus:
                std::memcpy(&result.status,chunk.payload.data(),sizeof(result.status));
                break;
            case model_core::ChunkTopology::TextureWarning:
                std::memcpy(&result.textureWarningCount,chunk.payload.data(),sizeof(uint32_t));
                break;
            default:
                break; // unrecognized topology already rejected by the validator; never reached
            }
        }
        std::function<bool(uint32_t)> resolveNode=[&](uint32_t id) {
            if(auto found=resolvedNodes.find(id);found!=resolvedNodes.end()&&!found->second.active)return true;
            auto source=nodeCatalog.find(id);if(source==nodeCatalog.end())return false;
            auto& resolved=resolvedNodes[id];if(resolved.active)return false;resolved.active=true;
            std::memcpy(resolved.world,source->second.localTransform,sizeof(resolved.world));
            resolved.visible=(source->second.flags&model_core::kSceneRecordVisible)!=0;
            if(source->second.parentNodeId){if(!resolveNode(source->second.parentNodeId))return false;
                double world[16]{};const auto& parent=resolvedNodes.at(source->second.parentNodeId);
                for(uint32_t row=0;row<4;++row)for(uint32_t column=0;column<4;++column)for(uint32_t k=0;k<4;++k)
                    world[row*4+column]+=source->second.localTransform[row*4+k]*parent.world[k*4+column];
                std::memcpy(resolved.world,world,sizeof(world));resolved.visible=resolved.visible&&parent.visible;}
            resolved.active=false;return true;
        };
        for(auto& instance:result.instances){if(resolveNode(instance.data.nodeId)){const auto& node=resolvedNodes.at(instance.data.nodeId);
            std::memcpy(instance.worldTransform,node.world,sizeof(instance.worldTransform));
            instance.resolvedVisible=node.visible&&(instance.data.flags&model_core::kSceneRecordVisible);
            const auto* m=instance.worldTransform;const double determinant=m[0]*(m[5]*m[10]-m[6]*m[9])
                -m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]);instance.mirrored=determinant<0;}}
        result.ok = true;
        return result;
    };
    if (onBatch) sessionRequest.onBatch = [&](auto chunks) {
        auto batch = unpack(std::move(chunks));
        batch.missingAssets = missingAssets;
        onBatch(std::move(batch));
    };
    auto session = import_broker::RunImportSession(sessionRequest);
    if (!session.ok) {
        if (delayBatchesForTesting && session.stage!=import_broker::ImportStage::Cancelled) std::fprintf(stderr,"Import smoke failure: stage %u code %u\n",unsigned(session.stage),unsigned(session.errorCode));
        result.errorCode = session.errorCode;
        if (session.stage == import_broker::ImportStage::ValidateSection && result.errorCode == model_core::ImportErrorCode::MalformedData)
            result.errorCode = model_core::ImportErrorCode::ImportProtocolViolation;
        result.errorStage = session.stage;
        result.errorPhase = session.errorPhase;
        DescribeSessionFailure(session, result.errorSummary, result.errorDetails);
        if (format == SourceFormat::Fbx) {
            if (session.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature) {
                result.errorDetails = Loc("fb.detail.export.or.bake.this.fbx.as.static.polygon.geomet", L"Export or bake this FBX as static polygon geometry using the supported material and deformation subset.");
            } else if (session.errorCode == model_core::ImportErrorCode::PrimarySourceLimit) {
                result.errorDetails = Loc("fb.detail.fbx.files.are.limited.to.the.bounded.tier.b.sour", L"FBX files are limited to the bounded Tier B source size.");
            } else if (session.errorCode == model_core::ImportErrorCode::ScratchLimit) {
                result.errorDetails = Loc("fb.detail.fbx.parsing.or.static.pose.evaluation.exceeded.t", L"FBX parsing or static-pose evaluation exceeded the bounded importer scratch budget.");
            }
        } else if (format == SourceFormat::ThreeMf) {
            if (session.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature) {
                result.errorDetails = Loc("fb.detail.this.viewer.supports.static.3mf.core.materials.a", L"This viewer supports static 3MF Core, Materials and Properties, Production, and bounded Beam Lattice content. Other required extensions or features cannot be previewed.");
            } else if (session.errorCode == model_core::ImportErrorCode::PrimarySourceLimit) {
                result.errorDetails = Loc("fb.detail.3mf.files.are.limited.to.the.bounded.tier.b.prim", L"3MF files are limited to the bounded Tier B primary-source size.");
            } else if (session.errorCode == model_core::ImportErrorCode::ArchiveLimit) {
                result.errorDetails = Loc("fb.detail.the.3mf.package.exceeded.a.bounded.archive.relat", L"The 3MF package exceeded a bounded archive, relationship, or expansion limit.");
            } else if (session.errorCode == model_core::ImportErrorCode::ScratchLimit) {
                result.errorDetails = Loc("fb.detail.3mf.package.parsing.or.normalization.exceeded.th", L"3MF package parsing or normalization exceeded the bounded Tier B scratch budget.");
            }
        } else if (format == SourceFormat::Usd) {
            if (session.errorCode == model_core::ImportErrorCode::UnsupportedEncoding) {
                result.errorDetails = Loc("fb.detail.use.usda.usdc.or.usdz.content.whose.encoding.mat", L"Use USDA, USDC, or USDZ content whose encoding matches the explicit suffix; .usd is detected by bytes.");
            } else if (session.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature) {
                result.errorDetails = Loc("fb.detail.export.a.static.usd.stage.using.supported.meshes", L"Export a static USD stage using supported meshes, primvars, instances, and USD Preview Surface materials.");
            } else if (session.errorCode == model_core::ImportErrorCode::PrimarySourceLimit) {
                result.errorDetails = Loc("fb.detail.usd.files.are.limited.to.the.bounded.tier.b.prim", L"USD files are limited to the bounded Tier B primary-source size.");
            } else if (session.errorCode == model_core::ImportErrorCode::ScratchLimit) {
                result.errorDetails = Loc("fb.detail.usd.parsing.composition.or.normalization.exceede", L"USD parsing, composition, or normalization exceeded the bounded Tier B scratch budget.");
            }
        } else if (format == SourceFormat::Step) {
            if (session.errorCode == model_core::ImportErrorCode::UnsupportedRequiredFeature) {
                result.errorDetails = Loc("fb.detail.export.a.self.contained.iso.10303.21.step.file.r", L"Export a self-contained ISO 10303-21 STEP file. Required external STEP documents are not supported yet.");
            } else if (session.errorCode == model_core::ImportErrorCode::PrimarySourceLimit) {
                result.errorDetails = Loc("fb.detail.step.files.are.limited.to.the.bounded.tier.b.pri", L"STEP files are limited to the bounded Tier B primary-source size.");
            } else if (session.errorCode == model_core::ImportErrorCode::ScratchLimit) {
                result.errorDetails = Loc("fb.detail.step.parsing.or.tessellation.exceeded.the.bounde", L"STEP parsing or tessellation exceeded the bounded Tier B scratch budget.");
            }
        }
        return result;
    }
    if (!onBatch)
    {
        result = unpack(std::move(session.chunks));
        result.sourceIdentity = session.sourceIdentity;
        result.missingAssets = missingAssets;
        return result;
    }
    result.sourceIdentity = session.sourceIdentity;
    result.missingAssets = missingAssets;
    result.ok = true;
    return result;
}

} // namespace d3d12_import_bridge
