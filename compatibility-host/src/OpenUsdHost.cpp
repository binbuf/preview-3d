#define NOMINMAX

#include "OpenUsdHost.h"
#include "model_core/OpenUsdIdentifier.h"

#include "BoundedChunkWriter.h"
#include "ChunkBatchSink.h"
#include "ImageFormatSniff.h"
#include "SidecarFileClient.h"
#include "TextureDecodePolicy.h"
#include "TextureTranscodeAdapter.h"
#include "UsdZipPreflight.h"
#include "WebpDecodeAdapter.h"
#include "WicImageDecodeAdapter.h"
#include "model_core/MaterialPayload.h"
#include "model_core/MappedFile.h"
#include "model_core/TierALimits.h"
#include "model_core/VertexLayouts.h"
#include "platform/Sha256.h"
#include "platform/Win32Handle.h"

#include <windows.h>
#include <psapi.h>

#pragma warning(push, 0)
#include "pxr/base/plug/plugin.h"
#include "pxr/base/plug/registry.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/type.h"
#include "pxr/base/vt/array.h"
#include "pxr/usd/ar/asset.h"
#include "pxr/usd/ar/defineResolver.h"
#include "pxr/usd/ar/resolvedPath.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/primFlags.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/gprim.h"
#include "pxr/usd/usdGeom/imageable.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/pointInstancer.h"
#include "pxr/usd/usdGeom/primvar.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/subset.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usd/usdGeom/xformable.h"
#include "pxr/usd/usdGeom/xformCache.h"
#include "pxr/usd/usdShade/connectableAPI.h"
#include "pxr/usd/usdShade/input.h"
#include "pxr/usd/usdShade/material.h"
#include "pxr/usd/usdShade/materialBindingAPI.h"
#include "pxr/usd/usdShade/shader.h"
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <limits>
#include <numbers>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace compatibility_host {
namespace {

static_assert(kOpenUsdIdentifierCapacity == model_core::kMaxOpenUsdIdentifierBytes);

using Clock = std::chrono::steady_clock;

// Digests are computed over the line-ending-normalized (LF) resource text.
// The checked-in manifest is stored as LF, but Windows checkouts with
// core.autocrlf=true materialize it (and the build's copy) as CRLF, so
// hashing raw bytes made the audit depend on the developer's git config and
// rejected an otherwise pristine payload. Normalizing to LF makes the
// integrity check deterministic on every checkout and platform while still
// detecting any content change.
struct ExpectedResource { const wchar_t* path; const char* sha256; };
constexpr ExpectedResource kResources[] = {
    {L"plugInfo.json", "c42a8b0c30e7cc9f85349184a2273ad6f5b0f7e283b090f5497dc4668c1f1e40"},
    {L"ar/resources/plugInfo.json", "9e1f7de1980772441033787853be0dd5b7a6619c3abc86285ac242b8589e326c"},
    {L"sdf/resources/plugInfo.json", "39fbcb5a53d124d01454154d7097c15971adb6c8cd6fed17c6db17e56331f84f"},
    {L"usd/resources/plugInfo.json", "269f6837927dc9257cf25f25dce38721fac5e0ecfa19aa245668373f74ea0d89"},
    {L"usd/resources/generatedSchema.usda", "a09ceab63ab5491e33054cb2a9394af35c391cba2633fa60c3ef373addddaa4c"},
    {L"usd/resources/usd/schema.usda", "53706dd717cad34c05a567c320c1ecbc99acf327f65fcf703b5409e4c07c8e63"},
    {L"usdGeom/resources/plugInfo.json", "2b2752839f23c12cfdfac59ab5507a3c8758fba01beed1ed37900ef7470577cc"},
    {L"usdGeom/resources/generatedSchema.usda", "f5732dad7c7128bae308383820fea109e08affb6abd2a65fa7d4bde8fa09ca94"},
    {L"usdGeom/resources/usdGeom/schema.usda", "cf1f4615b7196d59a78f49b490a15bfe0c49c78d96f7bc5620075341263ca251"},
    {L"usdShade/resources/plugInfo.json", "12c232fba07cc967bec28ef275ab0200923d2f5f200b1f83d701b3e574698aef"},
    {L"usdShade/resources/generatedSchema.usda", "aef8c58040043e586bcf7f3b8dec116d0b5c0e5e224abc058235f33b6d77bbda"},
    {L"usdShade/resources/usdShade/schema.usda", "7f8ec41517c2f1bba669ca0fcd50f0435071a8bae18ba8af80e1d2ce1d428039"},
    {L"preview3d/resources/plugInfo.json", "06c9138c6376571196fa50f5e5d9c7a4fc800a6c94981e8a4d5eda917f10de53"},
};

std::string Hex(std::span<const std::byte> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned char>(byte);
        text.push_back(digits[value >> 4]); text.push_back(digits[value & 15]);
    }
    return text;
}

// Collapses CRLF to LF so the payload audit is independent of the checkout's
// git line-ending policy. A lone CR is preserved.
std::vector<std::byte> NormalizeLineEndings(std::span<const std::byte> bytes)
{
    std::vector<std::byte> normalized;
    normalized.reserve(bytes.size());
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (bytes[index] == std::byte{'\r'} && index + 1 < bytes.size()
            && bytes[index + 1] == std::byte{'\n'}) continue;
        normalized.push_back(bytes[index]);
    }
    return normalized;
}

std::optional<std::vector<std::byte>> ReadSmallFile(const std::filesystem::path& path)
{
    platform::Win32Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    if (!file || !GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024)
        return std::nullopt;
    std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!bytes.empty() && (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)
                           || read != bytes.size())) return std::nullopt;
    return bytes;
}

bool AuditResources(const std::filesystem::path& payloadDirectory)
{
    const auto root = payloadDirectory / L"usd";
    std::unordered_set<std::wstring> expected;
    std::unordered_set<std::wstring> expectedDirectories;
    for (const auto& resource : kResources) {
        const std::filesystem::path relative(resource.path);
        expected.insert(relative.generic_wstring());
        for (auto parent = relative.parent_path(); !parent.empty(); parent = parent.parent_path())
            expectedDirectories.insert(parent.generic_wstring());
        const auto bytes = ReadSmallFile(root / relative);
        if (!bytes) return false;
        const auto digest = platform::ComputeSha256(NormalizeLineEndings(*bytes));
        if (!digest || Hex(*digest) != resource.sha256) return false;
    }
    std::error_code error;
    std::size_t found = 0;
    for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto relative = iterator->path().lexically_relative(root);
        const auto attributes = GetFileAttributesW(iterator->path().c_str());
        if (relative.empty() || attributes == INVALID_FILE_ATTRIBUTES
            || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!expectedDirectories.contains(relative.generic_wstring())) return false;
        } else {
            if (!expected.contains(relative.generic_wstring())) return false;
            ++found;
        }
    }
    return !error && found == expected.size();
}

class ByteStore {
public:
    bool Initialize(OpenUsdSpikeSection& header, std::span<const std::byte> section)
    {
        if (!header.entryCount || header.entryCount > kOpenUsdMaxEntries
            || header.sectionByteLength != section.size()
            || header.maxApprovedAssetBytes > kOpenUsdMaxSectionBytes) return false;
        std::uint64_t total = 0;
        for (std::uint32_t i = 0; i < header.entryCount; ++i) {
            const auto& item = header.entries[i];
            const auto nameLength = strnlen_s(item.identifier, kOpenUsdIdentifierCapacity);
            if (!nameLength || nameLength == kOpenUsdIdentifierCapacity) return false;
            std::string name(item.identifier, nameLength);
            if (!model_core::IsSafeOpenUsdIdentifier(name) || assets_.contains(name) || item.byteOffset < sizeof(OpenUsdSpikeSection)
                || item.byteOffset > section.size() || item.byteLength > section.size() - item.byteOffset
                || item.byteLength > header.maxApprovedAssetBytes - total) return false;
            total += item.byteLength;
            auto bytes = std::make_shared<std::vector<char>>(static_cast<std::size_t>(item.byteLength));
            if (!bytes->empty()) std::memcpy(bytes->data(), section.data() + item.byteOffset, bytes->size());
            assets_.emplace(std::move(name), std::move(bytes));
        }
        header_ = &header; limit_ = header.maxResolverOpens;
        return true;
    }
    bool InitializeProduction(std::string rootIdentifier, std::span<const std::byte> rootBytes,
                              import_worker::SidecarFileClient* sidecars)
    {
        if (!model_core::IsSafeOpenUsdIdentifier(rootIdentifier) || rootBytes.empty()
            || rootBytes.size() > model_core::kTierBPrimarySourceBytes) return false;
        auto bytes = std::make_shared<std::vector<char>>(rootBytes.size());
        std::memcpy(bytes->data(), rootBytes.data(), rootBytes.size());
        totalBytes_ = rootBytes.size();
        assets_.emplace(std::move(rootIdentifier), std::move(bytes));
        sidecars_ = sidecars;
        limit_ = 64;
        return true;
    }
    std::shared_ptr<const std::vector<char>> Find(std::string_view name)
    {
        const std::string key(name);
        if (const auto found = assets_.find(key); found != assets_.end()) return found->second;
        if (!sidecars_ || !model_core::IsSafeOpenUsdIdentifier(key)) return nullptr;
        constexpr std::string_view prefix = "preview3d://";
        const std::string relative(key.substr(prefix.size()));
        if (relative.empty() || relative.size() > model_core::kMaxSidecarRelativePathBytes
            || assets_.size() >= 65) {
            productionError_ = model_core::ImportErrorCode::ResourceLimit;
            return nullptr;
        }
        const std::uint64_t remaining = totalBytes_ < model_core::kTierBAllSourceBytes
            ? model_core::kTierBAllSourceBytes - totalBytes_ : 0;
        if (!remaining) {
            productionError_ = model_core::ImportErrorCode::ResourceLimit;
            return nullptr;
        }
        auto result = sidecars_->RequestSidecarBytes(relative, remaining, false);
        if (!result.bytes) {
            productionError_ = result.errorCode == model_core::ImportErrorCode::None
                ? model_core::ImportErrorCode::FileUnavailable : result.errorCode;
            return nullptr;
        }
        if (result.bytes->size() > remaining) {
            productionError_ = model_core::ImportErrorCode::ResourceLimit;
            return nullptr;
        }
        auto bytes = std::make_shared<std::vector<char>>(result.bytes->size());
        if (!bytes->empty()) std::memcpy(bytes->data(), result.bytes->data(), bytes->size());
        totalBytes_ += bytes->size();
        assets_.emplace(key, bytes);
        return bytes;
    }
    bool OpenAllowed(bool found)
    {
        std::uint32_t& opens = header_ ? header_->resolverOpenCount : productionOpens_;
        std::uint32_t& denied = header_ ? header_->resolverDeniedCount : productionDenied_;
        if (!found || opens >= limit_) {
            ++denied;
            if (!found && !header_ && productionError_ == model_core::ImportErrorCode::None)
                productionError_ = model_core::ImportErrorCode::FileUnavailable;
            return false;
        }
        ++opens; return true;
    }
    model_core::ImportErrorCode ProductionError() const { return productionError_; }
    void Deny(model_core::ImportErrorCode error) { productionError_ = error; }
    void ClearOptionalError() { productionError_ = model_core::ImportErrorCode::None; }
    std::uint32_t DeniedCount() const { return productionDenied_; }
private:
    std::unordered_map<std::string, std::shared_ptr<std::vector<char>>> assets_;
    OpenUsdSpikeSection* header_{};
    std::uint32_t limit_{};
    import_worker::SidecarFileClient* sidecars_{};
    std::uint64_t totalBytes_{};
    std::uint32_t productionOpens_{};
    std::uint32_t productionDenied_{};
    model_core::ImportErrorCode productionError_ = model_core::ImportErrorCode::None;
};

ByteStore* gStore{};

class MemoryAsset final : public ArAsset {
public:
    explicit MemoryAsset(std::shared_ptr<const std::vector<char>> bytes) : bytes_(std::move(bytes)) {}
    std::size_t GetSize() const final { return bytes_->size(); }
    std::shared_ptr<const char> GetBuffer() const final
    { return bytes_->empty() ? nullptr : std::shared_ptr<const char>(bytes_, bytes_->data()); }
    std::size_t Read(void* out, std::size_t count, std::size_t offset) const final
    {
        if (!out || offset > bytes_->size() || count > bytes_->size() - offset) return 0;
        if (count) std::memcpy(out, bytes_->data() + offset, count);
        return count;
    }
    std::pair<FILE*, std::size_t> GetFileUnsafe() const final { return {nullptr, 0}; }
private:
    std::shared_ptr<const std::vector<char>> bytes_;
};

} // namespace

class BrokerResolver final : public ArResolver {
public: BrokerResolver() = default;
private:
    std::string _CreateIdentifier(const std::string& path, const ArResolvedPath& anchor) const final
    {
        if (gStore && model_core::IsSafeOpenUsdIdentifier(path) && gStore->Find(path)) {
            return path;
        }
        const auto id = model_core::AnchorOpenUsdIdentifier(path, anchor.GetPathString());
        if (!id && gStore) gStore->Deny(model_core::ImportErrorCode::UnsafeReference);
        return id.value_or(std::string());
    }
    std::string _CreateIdentifierForNewAsset(const std::string&, const ArResolvedPath&) const final { return {}; }
    ArResolvedPath _Resolve(const std::string& path) const final
    {
        if (!gStore) return ArResolvedPath();
        if (!gStore->Find(path)) {
            gStore->OpenAllowed(false);
            return ArResolvedPath();
        }
        return ArResolvedPath(path);
    }
    ArResolvedPath _ResolveForNewAsset(const std::string&) const final { return {}; }
    std::shared_ptr<ArAsset> _OpenAsset(const ArResolvedPath& path) const final
    {
        const auto key = path.GetPathString();
        const auto bytes = gStore ? gStore->Find(key) : nullptr;
        return gStore && gStore->OpenAllowed(bytes != nullptr) ? std::make_shared<MemoryAsset>(bytes) : nullptr;
    }
    std::shared_ptr<ArWritableAsset> _OpenAssetForWrite(const ArResolvedPath&, WriteMode) const final { return nullptr; }
};

AR_DEFINE_RESOLVER(BrokerResolver, ArResolver);

namespace {

struct Hash {
    std::uint64_t value{14695981039346656037ull};
    template<class T> void Scalar(const T& item)
    { for (const auto byte : std::as_bytes(std::span(&item, 1))) { value ^= std::to_integer<std::uint8_t>(byte); value *= 1099511628211ull; } }
    void String(std::string_view text)
    { const auto length = static_cast<std::uint64_t>(text.size()); Scalar(length); for (const char ch : text) Scalar(ch); }
    void Number(double number) { const auto q = static_cast<std::int64_t>(std::llround(number * 1'000'000.0)); Scalar(q); }
};

bool LoadPayloads(const UsdStageRefPtr& stage, OpenUsdSpikeSection& out,
                  const Clock::time_point deadline)
{
    for (;;) {
        std::vector<SdfPath> paths;
        for (const auto& prim : stage->TraverseAll())
            if (prim.HasPayload() && !prim.IsLoaded()) paths.push_back(prim.GetPath());
        if (paths.empty()) return true;
        std::sort(paths.begin(), paths.end(), [](const SdfPath& left, const SdfPath& right) {
            const auto leftDepth = left.GetPathElementCount();
            const auto rightDepth = right.GetPathElementCount();
            return leftDepth != rightDepth ? leftDepth < rightDepth : left < right;
        });
        for (const auto& path : paths) {
            if (Clock::now() >= deadline || out.payloadCount >= out.maxPayloads
                || path.GetPathElementCount() > out.maxGraphDepth) return false;
            stage->Load(path); ++out.payloadCount;
            if (!stage->GetPrimAtPath(path).IsLoaded()) return false;
        }
    }
}

bool Normalize(const UsdStageRefPtr& stage, OpenUsdSpikeSection& out)
{
    Hash hash;
    const auto axis = UsdGeomGetStageUpAxis(stage);
    out.upAxis = axis == UsdGeomTokens->x ? 'X' : axis == UsdGeomTokens->z ? 'Z' : 'Y';
    out.metersPerUnit = UsdGeomGetStageMetersPerUnit(stage);
    hash.Scalar(out.upAxis); hash.Number(out.metersPerUnit);
    bool bounded = false;
    std::array<double, 3> low{}, high{};
    UsdGeomXformCache xforms(UsdTimeCode::Default());
    for (const auto& prim : stage->Traverse()) {
        ++out.primCount; hash.String(prim.GetPath().GetString()); hash.String(prim.GetTypeName().GetString());
        if (prim.IsA<UsdShadeMaterial>()) ++out.materialCount;
        UsdGeomMesh mesh(prim);
        if (!mesh) continue;
        ++out.meshCount;
        VtArray<GfVec3f> points; VtArray<int> counts; VtArray<int> indices;
        if (!mesh.GetPointsAttr().Get(&points) || !mesh.GetFaceVertexCountsAttr().Get(&counts)
            || !mesh.GetFaceVertexIndicesAttr().Get(&indices)) return false;
        out.pointCount += static_cast<std::uint32_t>(points.size());
        out.faceCount += static_cast<std::uint32_t>(counts.size());
        const auto matrix = xforms.GetLocalToWorldTransform(prim);
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) hash.Number(matrix[r][c]);
        hash.Scalar(static_cast<std::uint64_t>(points.size()));
        for (const auto& point : points) {
            hash.Number(point[0]); hash.Number(point[1]); hash.Number(point[2]);
            const auto world = matrix.Transform(GfVec3d(point));
            for (std::size_t c = 0; c < 3; ++c) {
                if (!bounded) low[c] = high[c] = world[c];
                else { low[c] = (std::min)(low[c], world[c]); high[c] = (std::max)(high[c], world[c]); }
            }
            bounded = true;
        }
        for (const auto item : counts) hash.Scalar(item);
        for (const auto item : indices) hash.Scalar(item);
    }
    for (const auto& prim : stage->Traverse()) {
        UsdShadeShader shader(prim);
        if (!shader) continue;
        for (const auto& input : shader.GetInputs()) {
            SdfAssetPath asset;
            if (!input.Get(&asset) || asset.GetAssetPath().empty()) continue;
            const auto id = ArGetResolver().CreateIdentifier(asset.GetAssetPath(),
                ArResolvedPath(stage->GetRootLayer()->GetResolvedPath()));
            const auto resolved = ArGetResolver().Resolve(id);
            const auto opened = resolved.empty() ? nullptr : ArGetResolver().OpenAsset(resolved);
            if (!opened) return false;
            ++out.textureAssetCount; hash.String(id); hash.Scalar(static_cast<std::uint64_t>(opened->GetSize()));
        }
    }
    for (std::size_t c = 0; c < 3 && bounded; ++c) { out.worldBounds[c] = low[c]; out.worldBounds[c + 3] = high[c]; }
    out.semanticDigest = hash.value;
    return out.meshCount != 0;
}

void Memory(OpenUsdSpikeSection& out)
{
    PROCESS_MEMORY_COUNTERS_EX info{}; info.cb = sizeof(info);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof(info))) {
        out.privateBytes = info.PrivateUsage; out.peakWorkingSetBytes = info.PeakWorkingSetSize;
    }
}

bool Under(const std::filesystem::path& path, const std::filesystem::path& root)
{
    auto p = path.lexically_normal().wstring();
    auto r = root.lexically_normal().wstring();
    std::replace(p.begin(), p.end(), L'/', L'\\');
    std::replace(r.begin(), r.end(), L'/', L'\\');
    while (r.size() > 3 && r.ends_with(L'\\')) r.pop_back();
    return p.size() >= r.size() && _wcsnicmp(p.c_str(), r.c_str(), r.size()) == 0
        && (p.size() == r.size() || p[r.size()] == L'\\');
}

void SaveDiagnostics(const TfErrorMark& errors, OpenUsdSpikeSection& out)
{
    std::string diagnostic;
    for (const auto& error : errors) {
        if (!diagnostic.empty()) diagnostic += " | ";
        diagnostic += error.GetCommentary();
    }
    strncpy_s(out.diagnostic, diagnostic.c_str(), _TRUNCATE);
}

} // namespace

int RunOpenUsdSpike(OpenUsdSpikeSection& out, std::span<const std::byte> section,
                    const std::filesystem::path& payloadDirectory)
{
    const auto start = Clock::now();
    InterlockedExchange(&out.state, 1);
    out.status = OpenUsdSpikeStatus::InvalidRequest;
    if (out.magic != kOpenUsdSpikeMagic || out.version != kOpenUsdSpikeVersion) return 2;
    if (out.mode == OpenUsdSpikeMode::Hang) { Sleep(INFINITE); return 3; }
    if (out.mode == OpenUsdSpikeMode::Crash) { RaiseFailFastException(nullptr, nullptr, 0); return 3; }
    if (out.mode == OpenUsdSpikeMode::ConsumeMemory) {
        std::vector<std::unique_ptr<std::byte[]>> blocks;
        for (;;) { blocks.push_back(std::make_unique<std::byte[]>(16 * 1024 * 1024)); std::memset(blocks.back().get(), 1, 16 * 1024 * 1024); }
    }
    if (!AuditResources(payloadDirectory)) { out.status = OpenUsdSpikeStatus::PayloadIntegrityFailure; return 4; }
    ByteStore store;
    if (!store.Initialize(out, section)) return 2;
    const auto rootLength = strnlen_s(out.rootIdentifier, kOpenUsdIdentifierCapacity);
    if (!rootLength || rootLength == kOpenUsdIdentifierCapacity
        || !model_core::IsSafeOpenUsdIdentifier(
            std::string_view(out.rootIdentifier, rootLength))) return 2;
    gStore = &store;
    // The build has no ambient plug-in path. Register only the hash-verified
    // manifest that advertises the broker URI resolver.
    PlugRegistry::GetInstance().RegisterPlugins(
        (payloadDirectory / L"usd" / L"plugInfo.json").generic_string());
    out.registeredPluginCount = static_cast<std::uint32_t>(
        PlugRegistry::GetInstance().GetAllPlugins().size());
    TfErrorMark errors;
    const auto loadStart = Clock::now();
    const auto stage = UsdStage::Open(std::string(out.rootIdentifier, rootLength), UsdStage::LoadNone);
    out.loadNoneMicroseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - loadStart).count());
    if (!stage) {
        SaveDiagnostics(errors, out);
        out.status = out.resolverDeniedCount ? OpenUsdSpikeStatus::ResolverDenied
                                             : OpenUsdSpikeStatus::StageOpenFailure;
        return 5;
    }
    InterlockedExchange(&out.state, 2);
    const auto deadline = start + std::chrono::milliseconds(out.maxMilliseconds);
    if (!out.maxGraphDepth || !out.maxMilliseconds || !LoadPayloads(stage, out, deadline)) {
        out.status = out.resolverDeniedCount ? OpenUsdSpikeStatus::ResolverDenied
                                             : OpenUsdSpikeStatus::ResourceLimit;
        return 6;
    }
    const auto compositionErrors = stage->GetCompositionErrors();
    if (!compositionErrors.empty()) {
        if (compositionErrors.front())
            strncpy_s(out.diagnostic, compositionErrors.front()->ToString().c_str(), _TRUNCATE);
        out.status = out.resolverDeniedCount ? OpenUsdSpikeStatus::ResolverDenied
                                             : OpenUsdSpikeStatus::InternalFailure;
        return 7;
    }
    if (!Normalize(stage, out)) { out.status = out.resolverDeniedCount ? OpenUsdSpikeStatus::ResolverDenied : OpenUsdSpikeStatus::InternalFailure; return 7; }
    out.firstGeometryMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
    const auto plugins = PlugRegistry::GetInstance().GetAllPlugins();
    out.registeredPluginCount = static_cast<std::uint32_t>(plugins.size());
    for (const auto& plugin : plugins) {
        if (plugin && ((!plugin->GetPath().empty() && !Under(plugin->GetPath(), payloadDirectory))
            || (!plugin->GetResourcePath().empty() && !Under(plugin->GetResourcePath(), payloadDirectory / L"usd"))))
            ++out.externalPluginCount;
    }
    if (out.externalPluginCount) { out.status = OpenUsdSpikeStatus::PayloadIntegrityFailure; return 8; }
    Memory(out);
    out.totalMicroseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
    out.startupMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(loadStart - start).count());
    out.status = OpenUsdSpikeStatus::Success;
    InterlockedExchange(&out.state, 3);
    gStore = nullptr;
    return 0;
}

namespace {

using model_core::AlphaModeId;
using model_core::ChunkDescriptor;
using model_core::ChunkTopology;
using model_core::ColorSpaceId;
using model_core::ImportErrorCode;
using model_core::ImportFailurePhase;
using model_core::MaterialPayload;
using model_core::MeshInstancePayload;
using model_core::NodePayload;
using model_core::SceneMetadata;
using model_core::SourceFormatId;
using model_core::UpAxisId;
using model_core::VertexLayoutId;
using model_core::VertexPositionNormalUv0TangentColorF32;

constexpr std::uint32_t kMaxCompositionAssets = 64;
constexpr auto kCompositionDeadline = std::chrono::seconds(30);

struct ProductionState {
    const model_core::ParseOpenUsdFileRequest& request;
    HANDLE cancellation{};
    Clock::time_point deadline;
    ByteStore* store{};
    std::span<const std::byte> source;
    const import_worker::UsdzArchiveView* archive{};
    std::uint32_t optionalWarnings{};
    std::uint32_t textureWarnings{};
    ImportErrorCode error = ImportErrorCode::None;
    ImportFailurePhase phase = ImportFailurePhase::Geometry;

    bool Cancelled() const
    {
        return cancellation && WaitForSingleObject(cancellation, 0) == WAIT_OBJECT_0;
    }
    bool Expired() const { return Clock::now() >= deadline; }
    void Warn() { optionalWarnings = (std::min)(64u, optionalWarnings + 1); }
    void TextureWarn() { textureWarnings = (std::min)(64u, textureWarnings + 1); }
    bool Fail(ImportErrorCode code, ImportFailurePhase at = ImportFailurePhase::Geometry)
    {
        error = code; phase = at; return false;
    }
};

bool Finite(double value) { return std::isfinite(value); }

bool ValidMatrix(const GfMatrix4d& matrix)
{
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            if (!Finite(matrix[row][column]) || std::abs(matrix[row][column]) > 1e30) return false;
    return std::abs(matrix[0][3]) < 1e-12 && std::abs(matrix[1][3]) < 1e-12
        && std::abs(matrix[2][3]) < 1e-12 && std::abs(matrix[3][3] - 1.0) < 1e-12;
}

void CopyMatrix(const GfMatrix4d& source, double destination[16])
{
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            destination[row * 4 + column] = source[row][column];
}

SourceFormatId DetectEncoding(std::span<const std::byte> bytes)
{
    const auto starts = [&](std::string_view signature) {
        return bytes.size() >= signature.size()
            && std::memcmp(bytes.data(), signature.data(), signature.size()) == 0;
    };
    if (starts("PXR-USDC")) return SourceFormatId::Usdc;
    if (bytes.size() >= 4 && std::to_integer<unsigned char>(bytes[0]) == 'P'
        && std::to_integer<unsigned char>(bytes[1]) == 'K'
        && std::to_integer<unsigned char>(bytes[2]) == 3
        && std::to_integer<unsigned char>(bytes[3]) == 4) return SourceFormatId::Usdz;
    std::size_t offset = 0;
    if (bytes.size() >= 3 && std::to_integer<unsigned char>(bytes[0]) == 0xef
        && std::to_integer<unsigned char>(bytes[1]) == 0xbb
        && std::to_integer<unsigned char>(bytes[2]) == 0xbf) offset = 3;
    while (offset < bytes.size()) {
        const unsigned char ch = std::to_integer<unsigned char>(bytes[offset]);
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
        ++offset;
    }
    constexpr std::string_view usda = "#usda";
    if (offset <= bytes.size() && usda.size() <= bytes.size() - offset
        && std::memcmp(bytes.data() + offset, usda.data(), usda.size()) == 0)
        return SourceFormatId::Usda;
    return SourceFormatId::Unknown;
}

bool EncodingMatches(SourceFormatId format, std::uint32_t flags)
{
    const auto expected = flags & model_core::kImportRequestUsdExpectedMask;
    if (!expected) return true;
    return (expected == model_core::kImportRequestUsdExpectedUsda && format == SourceFormatId::Usda)
        || (expected == model_core::kImportRequestUsdExpectedUsdc && format == SourceFormatId::Usdc)
        || (expected == model_core::kImportRequestUsdExpectedUsdz && format == SourceFormatId::Usdz);
}

std::string RootIdentifier(SourceFormatId format)
{
    if (format == SourceFormatId::Usda) return "preview3d://root.usda";
    if (format == SourceFormatId::Usdc) return "preview3d://root.usdc";
    return "preview3d://root.usdz";
}

bool LoadProductionPayloads(const UsdStageRefPtr& stage, ProductionState& state)
{
    std::uint32_t loaded = 0;
    for (;;) {
        std::vector<SdfPath> paths;
        for (const auto& prim : stage->TraverseAll()) {
            if (prim.HasPayload() && !prim.IsLoaded()) paths.push_back(prim.GetPath());
            if (prim.GetPath().GetPathElementCount() > model_core::kMaxSceneHierarchyDepth)
                return state.Fail(ImportErrorCode::ResourceLimit);
        }
        if (paths.empty()) return true;
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        for (const auto& path : paths) {
            if (state.Cancelled()) return state.Fail(ImportErrorCode::Cancelled);
            if (state.Expired() || ++loaded > kMaxCompositionAssets)
                return state.Fail(ImportErrorCode::CompatibilityHostLimit);
            stage->Load(path);
            if (!stage->GetPrimAtPath(path).IsLoaded()) {
                const auto resolverError = state.store->ProductionError();
                return state.Fail(resolverError == ImportErrorCode::ResourceLimit
                    ? ImportErrorCode::CompatibilityHostLimit
                    : resolverError == ImportErrorCode::UnsafeReference
                        ? ImportErrorCode::UnsafeReference : ImportErrorCode::FileUnavailable);
            }
        }
    }
}

UsdTimeCode PolicyTime(const UsdStageRefPtr& stage, ProductionState& state)
{
    const double start = stage->HasAuthoredTimeCodeRange() ? stage->GetStartTimeCode() : 0.0;
    if (!Finite(start)) {
        state.Fail(ImportErrorCode::MalformedData);
        return UsdTimeCode(0.0);
    }
    return UsdTimeCode(start);
}

bool Animated(const UsdAttribute& attribute)
{
    std::vector<double> samples;
    return attribute && attribute.GetTimeSamples(&samples) && samples.size() > 1;
}

struct NodeRecord {
    UsdPrim prim;
    std::uint32_t parent = UINT32_MAX;
    GfMatrix4d local{1.0};
    GfMatrix4d world{1.0};
    bool visible = true;
    bool acceptedPurpose = true;
    std::uint32_t id{};
};

bool IsAcceptedPurpose(const UsdPrim& prim, ProductionState& state)
{
    const UsdGeomImageable imageable(prim);
    if (!imageable) return true;
    const TfToken purpose = imageable.ComputePurpose();
    if (purpose == UsdGeomTokens->proxy || purpose == UsdGeomTokens->guide) {
        state.Warn(); return false;
    }
    return purpose == UsdGeomTokens->default_ || purpose == UsdGeomTokens->render;
}

bool IsVisible(const UsdPrim& prim, const UsdTimeCode time, ProductionState& state)
{
    const UsdGeomImageable imageable(prim);
    if (!imageable) return true;
    if (Animated(imageable.GetVisibilityAttr())) state.Warn();
    return imageable.ComputeVisibility(time) != UsdGeomTokens->invisible;
}

bool RequiredUnsupportedSchema(const UsdPrim& prim)
{
    const std::string type = prim.GetTypeName().GetString();
    // Skeletal schemas (SkelRoot/Skeleton/SkelAnimation) are not rejected:
    // their meshes still carry an authored rest/bind pose, which is the static
    // preview the other formats already show (the FBX path bakes a start pose).
    // The skel prims are non-geometry and are skipped as ordinary non-Xformable
    // prims; the meshes render at rest. Volume/Field/Procedural/MaterialX
    // remain required-content rejections.
    return type.find("Volume") != std::string::npos
        || type.find("Field") != std::string::npos || type.find("Procedural") != std::string::npos
        || type.find("MaterialX") != std::string::npos;
}

std::vector<NodeRecord> CollectNodes(const UsdStageRefPtr& stage, const UsdTimeCode time,
                                     ProductionState& state)
{
    std::vector<NodeRecord> nodes;
    std::unordered_map<std::string, std::uint32_t> byPath;
    UsdGeomXformCache xforms(time);
    for (const UsdPrim& prim : stage->Traverse(UsdTraverseInstanceProxies())) {
        if (state.Cancelled() || state.Expired()) {
            state.Fail(state.Cancelled() ? ImportErrorCode::Cancelled
                                        : ImportErrorCode::CompatibilityHostLimit);
            return {};
        }
        if (prim.GetPath().GetPathElementCount() > model_core::kMaxSceneHierarchyDepth) {
            state.Fail(ImportErrorCode::ResourceLimit); return {};
        }
        if (RequiredUnsupportedSchema(prim)) {
            state.Fail(ImportErrorCode::UnsupportedRequiredFeature); return {};
        }
        const UsdGeomXformable xformable(prim);
        if (!xformable) {
            if (!prim.IsA<UsdShadeMaterial>() && !prim.IsA<UsdShadeShader>()
                && !prim.IsA<UsdGeomSubset>()) state.Warn();
            continue;
        }
        NodeRecord record;
        record.prim = prim;
        bool resets = false;
        xformable.GetLocalTransformation(&record.local, &resets, time);
        if (!ValidMatrix(record.local)) {
            state.Fail(ImportErrorCode::MalformedData); return {};
        }
        record.world = xforms.GetLocalToWorldTransform(prim);
        if (!ValidMatrix(record.world)) {
            state.Fail(ImportErrorCode::MalformedData); return {};
        }
        record.visible = IsVisible(prim, time, state);
        record.acceptedPurpose = IsAcceptedPurpose(prim, state);
        if (!resets) {
            for (SdfPath parent = prim.GetPath().GetParentPath(); !parent.IsEmpty();
                 parent = parent.GetParentPath()) {
                if (const auto found = byPath.find(parent.GetString()); found != byPath.end()) {
                    record.parent = found->second; break;
                }
            }
        }
        const auto index = static_cast<std::uint32_t>(nodes.size());
        byPath.emplace(prim.GetPath().GetString(), index);
        nodes.push_back(std::move(record));
        if (nodes.size() > model_core::kTierBObjectLimit) {
            state.Fail(ImportErrorCode::ResourceLimit); return {};
        }
    }
    return nodes;
}

template<class T>
bool GetInput(const UsdShadeShader& shader, const char* name, T& value, const UsdTimeCode time)
{
    const auto input = shader.GetInput(TfToken(name));
    return input && input.Get(&value, time);
}

bool ImageExtensionMatches(std::string_view asset, import_worker::SniffedImageFormat format)
{
    std::string extension = std::filesystem::path(asset).extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    using F = import_worker::SniffedImageFormat;
    switch (format) {
    case F::Png: return extension == ".png";
    case F::Jpeg: return extension == ".jpg" || extension == ".jpeg";
    case F::Bmp: return extension == ".bmp";
    case F::Tiff: return extension == ".tif" || extension == ".tiff";
    case F::WebP: return extension == ".webp";
    case F::Ktx2: return extension == ".ktx2";
    case F::Unknown: return true;
    }
    return false;
}

struct MaterialEmitter {
    import_worker::BoundedChunkWriter& writer;
    ProductionState& state;
    UsdTimeCode time;
    std::unordered_map<std::string, std::uint32_t> materials;
    std::unordered_map<std::string, std::uint32_t> images;
    std::uint64_t decodedBytes{};
    std::uint64_t decodedPixels{};

    void ApplyUvTransform(const UsdShadeInput& colorInput, MaterialPayload& payload)
    {
        UsdShadeConnectableAPI textureSource;
        TfToken sourceName; UsdShadeAttributeType sourceType{};
        if (!colorInput || !colorInput.GetConnectedSource(&textureSource, &sourceName, &sourceType)) return;
        const UsdShadeShader texture(textureSource.GetPrim());
        if (!texture) return;
        const UsdShadeInput st = texture.GetInput(TfToken("st"));
        UsdShadeConnectableAPI transformSource;
        if (!st || !st.GetConnectedSource(&transformSource, &sourceName, &sourceType)) return;
        const UsdShadeShader transform(transformSource.GetPrim());
        TfToken id;
        if (!transform || !transform.GetIdAttr().Get(&id) || id != TfToken("UsdTransform2d")) return;
        GfVec2f value;
        if (GetInput(transform, "translation", value, time)) {
            payload.uvOffset[0] = value[0]; payload.uvOffset[1] = value[1];
        }
        if (GetInput(transform, "scale", value, time)) {
            payload.uvScale[0] = value[0]; payload.uvScale[1] = value[1];
        }
        float rotation = 0;
        if (GetInput(transform, "rotation", rotation, time))
            payload.uvRotation = rotation * std::numbers::pi_v<float> / 180.0f;
    }

    std::uint32_t EmitImage(const UsdShadeInput& input, ColorSpaceId colorSpace,
                            import_worker::TextureSemantic semantic)
    {
        UsdShadeConnectableAPI source;
        TfToken sourceName;
        UsdShadeAttributeType sourceType{};
        if (!input || !input.GetConnectedSource(&source, &sourceName, &sourceType)) return 0;
        UsdShadeShader texture(source.GetPrim());
        TfToken id;
        if (!texture || !texture.GetIdAttr().Get(&id) || id != TfToken("UsdUVTexture")) {
            state.Warn(); return 0;
        }
        SdfAssetPath path;
        if (!GetInput(texture, "file", path, time) || path.GetAssetPath().empty()) {
            state.TextureWarn(); return EmitFallback(colorSpace, semantic);
        }
        std::string identifier = path.GetResolvedPath();
        const bool wasResolved = !identifier.empty();
        if (identifier.empty()) {
            const auto anchored = model_core::AnchorOpenUsdIdentifier(path.GetAssetPath(),
                texture.GetPrim().GetStage()->GetRootLayer()->GetResolvedPath().GetPathString());
            if (!anchored) {
                state.Fail(ImportErrorCode::UnsafeReference, ImportFailurePhase::Textures);
                return 0;
            }
            identifier = *anchored;
        }
        const std::string cacheKey = identifier + "#" + std::to_string(static_cast<unsigned>(colorSpace))
            + "#" + std::to_string(static_cast<unsigned>(semantic));
        if (const auto found = images.find(cacheKey); found != images.end()) return found->second;
        std::span<const std::byte> encoded;
        std::shared_ptr<ArAsset> asset;
        std::shared_ptr<const char> buffer;
        if (state.archive) {
            std::string entryName = path.GetAssetPath();
            const auto packageEntry = [](std::string_view value) {
                const auto bracket = value.rfind('[');
                return bracket != std::string_view::npos && value.ends_with(']')
                    ? std::string(value.substr(bracket + 1, value.size() - bracket - 2))
                    : std::string(value);
            };
            entryName = packageEntry(entryName);
            if (std::ranges::none_of(state.archive->entries,
                    [&](const import_worker::UsdzEntryView& candidate) {
                        return candidate.name == entryName;
                    }))
                entryName = packageEntry(identifier);
            while (entryName.starts_with("./")) entryName.erase(0, 2);
            const import_worker::UsdzEntryView* matched = nullptr;
            for (const auto& candidate : state.archive->entries) {
                const std::string packageSuffix = "[" + candidate.name + "]";
                const bool match = candidate.name == entryName
                    || std::string_view(identifier).ends_with(packageSuffix);
                if (!match) continue;
                if (matched) { matched = nullptr; break; }
                matched = &candidate;
            }
            if (matched && matched->dataOffset <= state.source.size()
                && matched->byteSize <= state.source.size() - matched->dataOffset)
                encoded = state.source.subspan(static_cast<std::size_t>(matched->dataOffset),
                                               static_cast<std::size_t>(matched->byteSize));
        }
        if (encoded.empty()) {
            // Only reach the resolver when the archive did not supply the
            // bytes. Resolving a USDZ-internal asset up front would emit a
            // broker sidecar request for every texture even though the entry
            // is already in the package, burning the bounded request budget.
            const auto resolved = identifier.empty() ? ArResolvedPath()
                : wasResolved ? ArResolvedPath(identifier) : ArGetResolver().Resolve(identifier);
            asset = resolved.empty() ? nullptr : ArGetResolver().OpenAsset(resolved);
            if (!asset || asset->GetSize() > 256ull * 1024 * 1024) {
                state.store->ClearOptionalError(); state.TextureWarn();
                return EmitFallback(colorSpace, semantic);
            }
            buffer = asset->GetBuffer();
            if (!buffer && asset->GetSize()) {
                state.TextureWarn(); return EmitFallback(colorSpace, semantic);
            }
            encoded = std::span(reinterpret_cast<const std::byte*>(buffer.get()), asset->GetSize());
        }
        const auto imageFormat = import_worker::SniffImageFormat(encoded);
        if (!ImageExtensionMatches(path.GetAssetPath(), imageFormat)) {
            state.Fail(ImportErrorCode::UnsafeReference, ImportFailurePhase::Textures); return 0;
        }
        import_worker::TextureDecodeOptions options;
        options.isCancelled = [&] { return state.Cancelled(); };
        options.semantic = semantic;
        options.maxDecodedBytes = (std::min)(options.maxDecodedBytes,
            model_core::kMaxAggregateTextureBytes - decodedBytes);
        options.maxPixels = (std::min)(options.maxPixels,
            model_core::kMaxAggregateTexturePixels - decodedPixels);
        model_core::PixelFormatId pixelFormat = model_core::PixelFormatId::Unknown;
        ColorSpaceId decodedColorSpace = colorSpace;
        std::uint32_t width = 0, height = 0, mipLevels = 0;
        std::vector<std::byte> pixelBytes;
        switch (imageFormat) {
        case import_worker::SniffedImageFormat::Ktx2:
            if (auto decoded = import_worker::TranscodeKtx2BasisImage(encoded, options)) {
                pixelFormat = decoded->pixelFormat; width = decoded->width; height = decoded->height;
                mipLevels = decoded->mipLevels; pixelBytes = std::move(decoded->pixelBytes);
            }
            break;
        case import_worker::SniffedImageFormat::WebP:
            if (auto decoded = import_worker::DecodeWebpImage(encoded, colorSpace, options)) {
                pixelFormat = decoded->pixelFormat; decodedColorSpace = decoded->colorSpace;
                width = decoded->width; height = decoded->height;
                mipLevels = decoded->mipLevels; pixelBytes = std::move(decoded->pixelBytes);
            }
            break;
        default:
            if (auto decoded = import_worker::DecodeRasterImageWic(encoded, colorSpace, options)) {
                pixelFormat = decoded->pixelFormat; decodedColorSpace = decoded->colorSpace;
                width = decoded->width; height = decoded->height;
                mipLevels = decoded->mipLevels; pixelBytes = std::move(decoded->pixelBytes);
            }
            break;
        }
        if (state.Cancelled()) {
            state.Fail(ImportErrorCode::Cancelled); return 0;
        }
        if (pixelBytes.empty()) {
            state.TextureWarn(); return EmitFallback(colorSpace, semantic);
        }
        const std::uint64_t pixels = std::uint64_t(width) * height;
        if (pixelBytes.size() > model_core::kMaxAggregateTextureBytes - decodedBytes
            || pixels > model_core::kMaxAggregateTexturePixels - decodedPixels) {
            state.Fail(ImportErrorCode::ResourceLimit, ImportFailurePhase::Textures); return 0;
        }
        decodedBytes += pixelBytes.size(); decodedPixels += pixels;
        model_core::ImagePayloadHeader header{};
        header.pixelFormat = static_cast<std::uint32_t>(pixelFormat);
        header.width = width; header.height = height; header.mipLevels = mipLevels;
        header.colorSpace = static_cast<std::uint32_t>(decodedColorSpace);
        header.pixelDataByteSize = pixelBytes.size();
        ChunkDescriptor descriptor{};
        descriptor.topology = ChunkTopology::Image;
        descriptor.chunkId = writer.NextId();
        if (!writer.Add(descriptor, import_worker::ChunkBytes(header), pixelBytes)) {
            state.Fail(writer.Error(), ImportFailurePhase::Textures); return 0;
        }
        images.emplace(cacheKey, descriptor.chunkId);
        return descriptor.chunkId;
    }

    std::uint32_t EmitFallback(ColorSpaceId colorSpace, import_worker::TextureSemantic semantic)
    {
        const std::string key = "fallback-" + std::to_string(static_cast<unsigned>(semantic));
        if (const auto found = images.find(key); found != images.end()) return found->second;
        const std::array<std::byte, 16> checker{
            std::byte{0x80},std::byte{0x80},std::byte{0x80},std::byte{0xff},
            std::byte{0x30},std::byte{0x30},std::byte{0x30},std::byte{0xff},
            std::byte{0x30},std::byte{0x30},std::byte{0x30},std::byte{0xff},
            std::byte{0x80},std::byte{0x80},std::byte{0x80},std::byte{0xff}};
        std::array<std::byte, 4> neutral{
            std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff}};
        if (semantic == import_worker::TextureSemantic::Normal)
            neutral = {std::byte{0x80},std::byte{0x80},std::byte{0xff},std::byte{0xff}};
        else if (semantic == import_worker::TextureSemantic::Emissive)
            neutral = {std::byte{0},std::byte{0},std::byte{0},std::byte{0xff}};
        const bool color = semantic == import_worker::TextureSemantic::Color;
        const std::span<const std::byte> pixels = color ? std::span<const std::byte>(checker)
                                                       : std::span<const std::byte>(neutral);
        model_core::ImagePayloadHeader header{};
        header.pixelFormat = static_cast<std::uint32_t>(model_core::PixelFormatId::RGBA8_UNORM);
        header.width = header.height = color ? 2u : 1u;
        header.mipLevels = 1; header.colorSpace = static_cast<std::uint32_t>(colorSpace);
        header.pixelDataByteSize = pixels.size();
        ChunkDescriptor descriptor{}; descriptor.topology = ChunkTopology::Image;
        descriptor.chunkId = writer.NextId();
        if (!writer.Add(descriptor, import_worker::ChunkBytes(header), pixels)) {
            state.Fail(writer.Error(), ImportFailurePhase::Textures); return 0;
        }
        images.emplace(key, descriptor.chunkId);
        return descriptor.chunkId;
    }

    std::uint32_t Emit(const UsdShadeMaterial& material, bool doubleSided)
    {
        if (!material) return 0;
        const std::string key = material.GetPath().GetString() + (doubleSided ? "#2" : "#1");
        if (const auto found = materials.find(key); found != materials.end()) return found->second;
        if (materials.size() >= model_core::kTierBMaterialLimit) {
            state.Fail(ImportErrorCode::ResourceLimit, ImportFailurePhase::Textures); return 0;
        }
        MaterialPayload payload{};
        payload.baseColorFactor[0] = payload.baseColorFactor[1] = payload.baseColorFactor[2] = 0.18f;
        payload.baseColorFactor[3] = 1.0f; payload.roughnessFactor = 0.5f;
        payload.uvScale[0] = payload.uvScale[1] = 1.0f; payload.alphaCutoff = 0.5f;
        payload.flags = model_core::kMaterialFlagFlipV;
        if (doubleSided) payload.flags |= model_core::kMaterialFlagDoubleSided;
        const UsdShadeShader shader = material.ComputeSurfaceSource();
        TfToken shaderId;
        if (!shader || !shader.GetIdAttr().Get(&shaderId) || shaderId != TfToken("UsdPreviewSurface")) {
            state.Warn();
        } else {
            GfVec3f color, emissive;
            float scalar = 0;
            if (GetInput(shader, "diffuseColor", color, time))
                for (int i = 0; i < 3; ++i) payload.baseColorFactor[i] = color[i];
            if (GetInput(shader, "emissiveColor", emissive, time))
                for (int i = 0; i < 3; ++i) payload.emissiveFactor[i] = emissive[i];
            if (GetInput(shader, "metallic", scalar, time)) payload.metallicFactor = scalar;
            if (GetInput(shader, "roughness", scalar, time)) payload.roughnessFactor = scalar;
            if (GetInput(shader, "opacity", scalar, time)) payload.baseColorFactor[3] = scalar;
            const bool hasThreshold = GetInput(shader, "opacityThreshold", scalar, time);
            if (hasThreshold && scalar > 0) payload.alphaCutoff = scalar;
            UsdShadeConnectableAPI opacitySource;
            TfToken opacityName;
            UsdShadeAttributeType opacityType{};
            const auto opacity = shader.GetInput(TfToken("opacity"));
            const bool opacityTexture = opacity
                && opacity.GetConnectedSource(&opacitySource, &opacityName, &opacityType);
            payload.alphaMode = static_cast<std::uint32_t>(hasThreshold && scalar > 0
                ? AlphaModeId::Mask : opacityTexture || payload.baseColorFactor[3] < 1
                    ? AlphaModeId::Blend : AlphaModeId::Opaque);
        }
        for (float value : payload.baseColorFactor) if (!std::isfinite(value)) {
            state.Fail(ImportErrorCode::MalformedData, ImportFailurePhase::Textures); return 0;
        }
        if (!std::isfinite(payload.metallicFactor) || !std::isfinite(payload.roughnessFactor)
            || !std::isfinite(payload.alphaCutoff)) {
            state.Fail(ImportErrorCode::MalformedData, ImportFailurePhase::Textures); return 0;
        }
        for (float value : payload.emissiveFactor) if (!std::isfinite(value)) {
            state.Fail(ImportErrorCode::MalformedData, ImportFailurePhase::Textures); return 0;
        }
        std::uint32_t textureIds[4]{};
        if (shader) {
            const auto diffuse = shader.GetInput(TfToken("diffuseColor"));
            textureIds[0] = EmitImage(diffuse, ColorSpaceId::Srgb, import_worker::TextureSemantic::Color);
            if (textureIds[0])
                payload.baseColorFactor[0] = payload.baseColorFactor[1]
                    = payload.baseColorFactor[2] = 1.0f;
            ApplyUvTransform(diffuse, payload);
            const auto metallic = shader.GetInput(TfToken("metallic"));
            const auto roughness = shader.GetInput(TfToken("roughness"));
            textureIds[1] = EmitImage(roughness, ColorSpaceId::Linear, import_worker::TextureSemantic::Data);
            if (textureIds[1]) {
                payload.roughnessFactor = 1.0f;
            } else {
                textureIds[1] = EmitImage(metallic, ColorSpaceId::Linear,
                                          import_worker::TextureSemantic::Data);
                if (textureIds[1]) payload.metallicFactor = 1.0f;
            }
            textureIds[2] = EmitImage(shader.GetInput(TfToken("normal")), ColorSpaceId::Linear, import_worker::TextureSemantic::Normal);
            textureIds[3] = EmitImage(shader.GetInput(TfToken("emissiveColor")), ColorSpaceId::Srgb, import_worker::TextureSemantic::Emissive);
            if (textureIds[3])
                payload.emissiveFactor[0] = payload.emissiveFactor[1]
                    = payload.emissiveFactor[2] = 1.0f;
            if (state.error != ImportErrorCode::None) return 0;
        }
        const float uvValues[]{payload.uvOffset[0], payload.uvOffset[1], payload.uvScale[0],
            payload.uvScale[1], payload.uvRotation};
        if (std::ranges::any_of(uvValues, [](float value) { return !std::isfinite(value); })) {
            state.Fail(ImportErrorCode::MalformedData, ImportFailurePhase::Textures); return 0;
        }
        ChunkDescriptor descriptor{}; descriptor.topology = ChunkTopology::Material;
        descriptor.chunkId = writer.NextId();
        for (std::size_t i = 0; i < 4; ++i) descriptor.dependencyIds[i] = textureIds[i];
        descriptor.dependencyCount = static_cast<std::uint32_t>(std::ranges::count_if(
            textureIds, [](std::uint32_t id) { return id != 0; }));
        if (!writer.Add(descriptor, import_worker::ChunkBytes(payload))) {
            state.Fail(writer.Error(), ImportFailurePhase::Textures); return 0;
        }
        materials.emplace(key, descriptor.chunkId);
        return descriptor.chunkId;
    }
};

std::size_t InterpolationIndex(const TfToken& interpolation, std::size_t point,
                               std::size_t face, std::size_t corner)
{
    if (interpolation == UsdGeomTokens->constant) return 0;
    if (interpolation == UsdGeomTokens->uniform) return face;
    if (interpolation == UsdGeomTokens->faceVarying) return corner;
    return point;
}

struct TriangleCorner { std::uint32_t point{}; std::uint32_t face{}; std::uint32_t corner{}; };
struct GeometryRecord { std::uint32_t id{}; ChunkDescriptor descriptor{}; int materialGroup{-1}; };
struct MeshGeometry { std::vector<GeometryRecord> records; };

GfVec3f FaceNormal(const GfVec3f& a, const GfVec3f& b, const GfVec3f& c)
{
    GfVec3f value = GfCross(b - a, c - a);
    const float length = value.GetLength();
    return length > 1e-20f && std::isfinite(length) ? value / length : GfVec3f(0, 0, 1);
}

bool TransformBounds(const ChunkDescriptor& geometry, const GfMatrix4d& world,
                     double minimum[3], double maximum[3])
{
    if (!ValidMatrix(world)) return false;
    std::fill(minimum, minimum + 3, (std::numeric_limits<double>::max)());
    std::fill(maximum, maximum + 3, -(std::numeric_limits<double>::max)());
    for (std::uint32_t corner = 0; corner < 8; ++corner) {
        const GfVec3d point(
            geometry.origin[0] + (corner & 1 ? geometry.localMax[0] : geometry.localMin[0]),
            geometry.origin[1] + (corner & 2 ? geometry.localMax[1] : geometry.localMin[1]),
            geometry.origin[2] + (corner & 4 ? geometry.localMax[2] : geometry.localMin[2]));
        const GfVec3d transformed = world.Transform(point);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!Finite(transformed[axis])) return false;
            minimum[axis] = (std::min)(minimum[axis], transformed[axis]);
            maximum[axis] = (std::max)(maximum[axis], transformed[axis]);
        }
    }
    return true;
}

std::optional<MeshGeometry> EmitMeshGeometry(import_worker::BoundedChunkWriter& writer,
    const UsdGeomMesh& mesh, std::uint32_t meshId, const UsdTimeCode time,
    ProductionState& state, std::uint64_t& triangleTotal, std::uint64_t& vertexTotal)
{
    VtVec3fArray points; VtIntArray counts, indices, holes;
    if (!mesh.GetPointsAttr().Get(&points, time) || !mesh.GetFaceVertexCountsAttr().Get(&counts, time)
        || !mesh.GetFaceVertexIndicesAttr().Get(&indices, time) || points.empty()) {
        state.Fail(ImportErrorCode::MalformedData); return std::nullopt;
    }
    mesh.GetHoleIndicesAttr().Get(&holes, time);
    std::unordered_set<int> holeSet(holes.begin(), holes.end());
    TfToken subdivision;
    mesh.GetSubdivisionSchemeAttr().Get(&subdivision, time);
    if (subdivision != UsdGeomTokens->none) {
        VtIntArray creaseIndices, creaseLengths, cornerIndices;
        VtFloatArray creaseSharpnesses, cornerSharpnesses;
        mesh.GetCreaseIndicesAttr().Get(&creaseIndices, time);
        mesh.GetCreaseLengthsAttr().Get(&creaseLengths, time);
        mesh.GetCreaseSharpnessesAttr().Get(&creaseSharpnesses, time);
        mesh.GetCornerIndicesAttr().Get(&cornerIndices, time);
        mesh.GetCornerSharpnessesAttr().Get(&cornerSharpnesses, time);
        if (!holes.empty() || !creaseIndices.empty() || !creaseLengths.empty()
            || !creaseSharpnesses.empty() || !cornerIndices.empty() || !cornerSharpnesses.empty()) {
            state.Fail(ImportErrorCode::UnsupportedRequiredFeature); return std::nullopt;
        }
        if (subdivision != UsdGeomTokens->catmullClark && subdivision != UsdGeomTokens->loop
            && subdivision != UsdGeomTokens->bilinear) {
            state.Fail(ImportErrorCode::UnsupportedRequiredFeature); return std::nullopt;
        }
        state.Warn();
    }
    std::vector<int> faceGroups(counts.size(), -1);
    const auto subsets = UsdGeomSubset::GetGeomSubsets(mesh, UsdGeomTokens->face,
                                                       UsdShadeTokens->materialBind);
    for (std::size_t subsetIndex = 0; subsetIndex < subsets.size(); ++subsetIndex) {
        VtIntArray faces;
        if (!subsets[subsetIndex].GetIndicesAttr().Get(&faces, time)) continue;
        for (int face : faces) {
            if (face < 0 || static_cast<std::size_t>(face) >= faceGroups.size()
                || faceGroups[face] != -1) {
                state.Fail(ImportErrorCode::MalformedData); return std::nullopt;
            }
            faceGroups[face] = static_cast<int>(subsetIndex);
        }
    }
    std::vector<std::vector<TriangleCorner>> groups(subsets.size() + 1);
    std::size_t cursor = 0;
    bool leftHanded = false;
    TfToken orientation;
    if (mesh.GetOrientationAttr().Get(&orientation, time)) leftHanded = orientation == UsdGeomTokens->leftHanded;
    for (std::size_t face = 0; face < counts.size(); ++face) {
        const int count = counts[face];
        if (count < 0 || cursor > indices.size() || static_cast<std::size_t>(count) > indices.size() - cursor) {
            state.Fail(ImportErrorCode::MalformedData); return std::nullopt;
        }
        if (count >= 3 && !holeSet.contains(static_cast<int>(face))) {
            auto& output = groups[static_cast<std::size_t>(faceGroups[face] + 1)];
            for (int corner = 1; corner + 1 < count; ++corner) {
                const int local[3]{0, corner, corner + 1};
                for (int outCorner = 0; outCorner < 3; ++outCorner) {
                    const int selected = leftHanded ? local[2 - outCorner] : local[outCorner];
                    const int point = indices[cursor + selected];
                    if (point < 0 || static_cast<std::size_t>(point) >= points.size()) {
                        state.Fail(ImportErrorCode::MalformedData); return std::nullopt;
                    }
                    output.push_back(TriangleCorner{static_cast<std::uint32_t>(point),
                        static_cast<std::uint32_t>(face), static_cast<std::uint32_t>(cursor + selected)});
                }
            }
        }
        cursor += static_cast<std::size_t>(count);
    }
    if (cursor != indices.size()) { state.Fail(ImportErrorCode::MalformedData); return std::nullopt; }
    std::uint64_t meshTriangles = 0;
    for (const auto& group : groups) meshTriangles += group.size() / 3;
    if (!meshTriangles) return MeshGeometry{};
    if (meshTriangles > model_core::kTierBTriangleLimit - triangleTotal
        || meshTriangles * 3 > model_core::kTierBVertexLimit - vertexTotal) {
        state.Fail(ImportErrorCode::ResourceLimit); return std::nullopt;
    }

    VtVec3fArray normals, colors;
    VtFloatArray opacities;
    TfToken normalsInterpolation = mesh.GetNormalsInterpolation();
    mesh.GetNormalsAttr().Get(&normals, time);
    const UsdGeomPrimvarsAPI primvars(mesh.GetPrim());
    UsdGeomPrimvar st;
    VtVec2fArray uvs;
    for (const char* preferred : {"st", "st0"}) {
        const auto candidate = primvars.GetPrimvar(TfToken(preferred));
        VtVec2fArray values;
        if (candidate && candidate.ComputeFlattened(&values, time) && !values.empty()) {
            st = candidate; uvs = std::move(values); break;
        }
    }
    if (!st) {
        for (const auto& candidate : primvars.GetPrimvarsWithAuthoredValues()) {
            const TfToken name = candidate.GetPrimvarName();
            if (name == TfToken("displayColor") || name == TfToken("displayOpacity")) continue;
            VtVec2fArray values;
            if (candidate.ComputeFlattened(&values, time) && !values.empty()) {
                st = candidate; uvs = std::move(values); break;
            }
        }
    }
    const auto displayColor = UsdGeomGprim(mesh.GetPrim()).GetDisplayColorPrimvar();
    if (displayColor) displayColor.ComputeFlattened(&colors, time);
    const auto displayOpacity = UsdGeomGprim(mesh.GetPrim()).GetDisplayOpacityPrimvar();
    if (displayOpacity) displayOpacity.ComputeFlattened(&opacities, time);
    const TfToken uvInterpolation = st ? st.GetInterpolation() : TfToken();
    const TfToken colorInterpolation = displayColor ? displayColor.GetInterpolation() : TfToken();
    const TfToken opacityInterpolation = displayOpacity ? displayOpacity.GetInterpolation() : TfToken();
    for (const auto& primvar : primvars.GetPrimvarsWithAuthoredValues()) {
        const TfToken name = primvar.GetPrimvarName();
        if ((!st || name != st.GetPrimvarName()) && name != TfToken("displayColor")
            && name != TfToken("displayOpacity")) state.Warn();
    }
    if (Animated(mesh.GetPointsAttr()) || Animated(mesh.GetNormalsAttr())) state.Warn();

    if (writer.Length() + model_core::kChunkDescriptorSize >= state.request.sectionByteCapacity) {
        state.Fail(ImportErrorCode::ResourceLimit); return std::nullopt;
    }
    constexpr std::uint64_t perTriangle = 3 * sizeof(VertexPositionNormalUv0TangentColorF32)
        + 3 * sizeof(std::uint32_t);
    const std::uint32_t sectionLimit = static_cast<std::uint32_t>((std::min<std::uint64_t>)(
        model_core::kTierAClusterTriangles,
        (state.request.sectionByteCapacity - model_core::kSectionHeaderSize
            - model_core::kChunkDescriptorSize) / perTriangle));
    if (!sectionLimit) { state.Fail(ImportErrorCode::ResourceLimit); return std::nullopt; }

    MeshGeometry geometry;
    for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        auto& corners = groups[groupIndex];
        for (std::size_t first = 0; first < corners.size();) {
            if (state.Cancelled()) { state.Fail(ImportErrorCode::Cancelled); return std::nullopt; }
            const std::size_t count = (std::min<std::size_t>)(
                static_cast<std::size_t>(sectionLimit) * 3, corners.size() - first);
            std::vector<VertexPositionNormalUv0TangentColorF32> vertices(count);
            std::vector<std::uint32_t> normalized(count);
            const GfVec3f originPoint = points[corners[first].point];
            const double origin[3]{originPoint[0], originPoint[1], originPoint[2]};
            bool hasUv = false, hasColor = false;
            for (std::size_t index = 0; index < count; ++index) {
                const auto& corner = corners[first + index];
                const GfVec3f point = points[corner.point];
                auto& vertex = vertices[index];
                vertex.px = static_cast<float>(double(point[0]) - origin[0]);
                vertex.py = static_cast<float>(double(point[1]) - origin[1]);
                vertex.pz = static_cast<float>(double(point[2]) - origin[2]);
                const std::size_t normalIndex = InterpolationIndex(normalsInterpolation,
                    corner.point, corner.face, corner.corner);
                GfVec3f normal;
                if (normalIndex < normals.size()) normal = normals[normalIndex];
                else {
                    const std::size_t triangleStart = first + (index / 3) * 3;
                    normal = FaceNormal(points[corners[triangleStart].point],
                        points[corners[triangleStart + 1].point], points[corners[triangleStart + 2].point]);
                }
                const float normalLength = normal.GetLength();
                if (!(normalLength > 1e-20f) || !std::isfinite(normalLength)) normal = GfVec3f(0,0,1);
                else normal /= normalLength;
                vertex.nx = normal[0]; vertex.ny = normal[1]; vertex.nz = normal[2];
                const std::size_t uvIndex = InterpolationIndex(uvInterpolation,
                    corner.point, corner.face, corner.corner);
                if (uvIndex < uvs.size() && std::isfinite(uvs[uvIndex][0]) && std::isfinite(uvs[uvIndex][1])) {
                    vertex.u = uvs[uvIndex][0]; vertex.v = uvs[uvIndex][1]; hasUv = true;
                }
                const std::size_t colorIndex = InterpolationIndex(colorInterpolation,
                    corner.point, corner.face, corner.corner);
                vertex.r = vertex.g = vertex.b = vertex.a = 1.0f;
                if (colorIndex < colors.size()) {
                    vertex.r = colors[colorIndex][0]; vertex.g = colors[colorIndex][1];
                    vertex.b = colors[colorIndex][2];
                    hasColor = hasColor || colorInterpolation != UsdGeomTokens->constant;
                }
                const std::size_t opacityIndex = InterpolationIndex(opacityInterpolation,
                    corner.point, corner.face, corner.corner);
                if (opacityIndex < opacities.size()) {
                    vertex.a = opacities[opacityIndex];
                    hasColor = hasColor || opacityInterpolation != UsdGeomTokens->constant;
                }
                vertex.tx = vertex.ty = vertex.tz = 0;
                vertex.tw = leftHanded ? -1.0f : 1.0f;
                const float values[]{vertex.px,vertex.py,vertex.pz,vertex.nx,vertex.ny,vertex.nz,
                    vertex.u,vertex.v,vertex.r,vertex.g,vertex.b,vertex.a};
                if (std::ranges::any_of(values, [](float v) { return !std::isfinite(v); })) {
                    state.Fail(ImportErrorCode::MalformedData); return std::nullopt;
                }
                normalized[index] = static_cast<std::uint32_t>(index);
            }
            ChunkDescriptor descriptor{};
            descriptor.topology = ChunkTopology::TriangleList;
            descriptor.vertexCount = descriptor.indexCount = static_cast<std::uint32_t>(count);
            descriptor.vertexLayoutId = static_cast<std::uint32_t>(VertexLayoutId::PositionNormalUv0TangentColor_F32);
            descriptor.chunkId = writer.NextId(); descriptor.meshId = meshId;
            descriptor.geometryFlags = model_core::kGeometryDeindexed
                | model_core::kGeometryReusableInstanceSource
                | (hasUv ? model_core::kGeometryHasUv0 : 0)
                | (hasColor ? model_core::kGeometryHasColors : 0);
            descriptor.sourceRangeOffset = (std::uint64_t(meshId - 1) << 32)
                | static_cast<std::uint32_t>(first);
            descriptor.sourceRangeLength = count;
            std::copy(std::begin(origin), std::end(origin), descriptor.origin);
            if (!model_core::SetLocalBounds(descriptor, import_worker::ChunkBytes(vertices))
                || !writer.Add(descriptor, import_worker::ChunkBytes(vertices),
                               import_worker::ChunkBytes(normalized))) {
                state.Fail(writer.Error() == ImportErrorCode::None ? ImportErrorCode::MalformedData
                                                                  : writer.Error());
                return std::nullopt;
            }
            geometry.records.push_back(GeometryRecord{descriptor.chunkId, descriptor,
                static_cast<int>(groupIndex) - 1});
            first += count;
        }
    }
    triangleTotal += meshTriangles; vertexTotal += meshTriangles * 3;
    return geometry;
}

UsdShadeMaterial BoundMaterial(const UsdPrim& prim)
{
    return UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
}

UsdShadeMaterial GroupMaterial(const UsdGeomMesh& mesh, int group)
{
    if (group < 0) return BoundMaterial(mesh.GetPrim());
    const auto subsets = UsdGeomSubset::GetGeomSubsets(mesh, UsdGeomTokens->face,
                                                       UsdShadeTokens->materialBind);
    return static_cast<std::size_t>(group) < subsets.size()
        ? BoundMaterial(subsets[static_cast<std::size_t>(group)].GetPrim()) : UsdShadeMaterial();
}

struct PointExpansionInfo {
    std::size_t originalNodeCount{};
    std::vector<SdfPath> prototypeRoots;
};

bool UnderPrototype(const SdfPath& path, const std::vector<SdfPath>& roots)
{
    return std::ranges::any_of(roots, [&](const SdfPath& root) { return path.HasPrefix(root); });
}

bool ExpandPointInstancers(std::vector<NodeRecord>& nodes, const UsdTimeCode time,
                           ProductionState& state, PointExpansionInfo& info)
{
    info.originalNodeCount = nodes.size();
    for (std::size_t nodeIndex = 0; nodeIndex < info.originalNodeCount; ++nodeIndex) {
        const UsdGeomPointInstancer instancer(nodes[nodeIndex].prim);
        if (!instancer) continue;
        SdfPathVector prototypes;
        if (!instancer.GetPrototypesRel().GetTargets(&prototypes) || prototypes.empty())
            return state.Fail(ImportErrorCode::UnsupportedRequiredFeature);
        info.prototypeRoots.insert(info.prototypeRoots.end(), prototypes.begin(), prototypes.end());
        VtIntArray protoIndices;
        if (!instancer.GetProtoIndicesAttr().Get(&protoIndices, time))
            return state.Fail(ImportErrorCode::MalformedData);
        VtMatrix4dArray transforms;
        if (!instancer.ComputeInstanceTransformsAtTime(&transforms, time, time,
                UsdGeomPointInstancer::IncludeProtoXform,
                UsdGeomPointInstancer::IgnoreMask)
            || transforms.size() != protoIndices.size())
            return state.Fail(ImportErrorCode::MalformedData);
        VtInt64Array ids, invisibleIds;
        instancer.GetIdsAttr().Get(&ids, time);
        instancer.GetInvisibleIdsAttr().Get(&invisibleIds, time);
        if (!ids.empty() && ids.size() != protoIndices.size())
            return state.Fail(ImportErrorCode::MalformedData);
        const std::unordered_set<std::int64_t> invisible(invisibleIds.begin(), invisibleIds.end());
        if (Animated(instancer.GetProtoIndicesAttr()) || Animated(instancer.GetPositionsAttr())
            || Animated(instancer.GetOrientationsAttr()) || Animated(instancer.GetScalesAttr())) state.Warn();
        for (std::size_t instance = 0; instance < protoIndices.size(); ++instance) {
            if (state.Cancelled()) return state.Fail(ImportErrorCode::Cancelled);
            const int prototypeIndex = protoIndices[instance];
            if (prototypeIndex < 0 || static_cast<std::size_t>(prototypeIndex) >= prototypes.size())
                return state.Fail(ImportErrorCode::MalformedData);
            const std::int64_t id = ids.empty() ? static_cast<std::int64_t>(instance) : ids[instance];
            if (invisible.contains(id)) continue;
            const SdfPath& rootPath = prototypes[static_cast<std::size_t>(prototypeIndex)];
            const auto root = std::find_if(nodes.begin(), nodes.begin() + info.originalNodeCount,
                [&](const NodeRecord& node) { return node.prim.GetPath() == rootPath; });
            if (root == nodes.begin() + info.originalNodeCount)
                return state.Fail(ImportErrorCode::UnsupportedRequiredFeature);
            const GfMatrix4d inverseRoot = root->world.GetInverse();
            for (std::size_t prototypeNode = 0; prototypeNode < info.originalNodeCount; ++prototypeNode) {
                if (!nodes[prototypeNode].prim.IsA<UsdGeomMesh>()
                    || !nodes[prototypeNode].prim.GetPath().HasPrefix(rootPath)) continue;
                NodeRecord expanded = nodes[prototypeNode];
                const GfMatrix4d relative = nodes[prototypeNode].world * inverseRoot;
                expanded.local = relative * transforms[instance];
                expanded.world = expanded.local * nodes[nodeIndex].world;
                expanded.parent = static_cast<std::uint32_t>(nodeIndex);
                expanded.visible = nodes[nodeIndex].visible && nodes[prototypeNode].visible;
                expanded.acceptedPurpose = nodes[nodeIndex].acceptedPurpose
                    && nodes[prototypeNode].acceptedPurpose;
                if (!ValidMatrix(expanded.local) || !ValidMatrix(expanded.world))
                    return state.Fail(ImportErrorCode::MalformedData);
                nodes.push_back(std::move(expanded));
                if (nodes.size() > model_core::kTierBObjectLimit)
                    return state.Fail(ImportErrorCode::ResourceLimit);
            }
        }
    }
    std::sort(info.prototypeRoots.begin(), info.prototypeRoots.end());
    info.prototypeRoots.erase(std::unique(info.prototypeRoots.begin(), info.prototypeRoots.end()),
                              info.prototypeRoots.end());
    return true;
}

bool NormalizeProduction(const UsdStageRefPtr& stage, SourceFormatId format,
                         std::span<std::byte> destination, import_worker::ChunkBatchSink& sink,
                         ProductionState& state, OpenUsdImportResult& result)
{
    const auto time = PolicyTime(stage, state);
    if (state.error != ImportErrorCode::None) return false;
    const TfToken axis = UsdGeomGetStageUpAxis(stage);
    const UpAxisId upAxis = axis == UsdGeomTokens->x ? UpAxisId::X
        : axis == UsdGeomTokens->z ? UpAxisId::Z : axis == UsdGeomTokens->y ? UpAxisId::Y
                                                                               : UpAxisId::Unknown;
    const double units = UsdGeomGetStageMetersPerUnit(stage);
    if (upAxis == UpAxisId::Unknown || !Finite(units) || units <= 0)
        return state.Fail(ImportErrorCode::MalformedData);
    auto nodes = CollectNodes(stage, time, state);
    if (state.error != ImportErrorCode::None) return false;
    PointExpansionInfo pointExpansions;
    if (!ExpandPointInstancers(nodes, time, state, pointExpansions)) return false;

    std::set<std::string> geometryKeys;
    for (const auto& node : nodes) if (node.prim.IsA<UsdGeomMesh>()) {
        const UsdPrim source = node.prim.IsInstanceProxy() ? node.prim.GetPrimInPrototype() : node.prim;
        geometryKeys.insert(source.GetPath().GetString());
    }
    if (geometryKeys.empty()) return state.Fail(ImportErrorCode::EmptyGeometry);
    if (geometryKeys.size() > model_core::kTierBObjectLimit)
        return state.Fail(ImportErrorCode::ResourceLimit);
    SceneMetadata metadata{};
    metadata.generationId = state.request.generationId; metadata.format = format;
    metadata.upAxis = upAxis; metadata.metersPerUnit = units;
    metadata.meshCount = static_cast<std::uint32_t>(geometryKeys.size());
    metadata.nodeCount = static_cast<std::uint32_t>(nodes.size());
    import_worker::BoundedChunkWriter writer(destination, state.request.generationId,
        state.request.maxChunkCount, metadata, &sink);

    std::unordered_map<std::string, MeshGeometry> geometry;
    std::uint64_t triangles = 0, vertices = 0;
    std::uint32_t meshId = 0;
    for (const std::string& key : geometryKeys) {
        const UsdPrim prim = stage->GetPrimAtPath(SdfPath(key));
        auto emitted = EmitMeshGeometry(writer, UsdGeomMesh(prim), ++meshId, time,
                                        state, triangles, vertices);
        if (!emitted) return false;
        geometry.emplace(key, std::move(*emitted));
    }
    if (!triangles) return state.Fail(ImportErrorCode::EmptyGeometry);

    const std::uint32_t firstNodeId = writer.NextId();
    if (std::uint64_t(firstNodeId) + nodes.size() > UINT32_MAX)
        return state.Fail(ImportErrorCode::ChunkCatalogLimit);
    for (std::size_t index = 0; index < nodes.size(); ++index)
        nodes[index].id = firstNodeId + static_cast<std::uint32_t>(index);
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        NodePayload payload{}; payload.nodeId = nodes[index].id;
        payload.flags = nodes[index].visible && nodes[index].acceptedPurpose
            ? model_core::kSceneRecordVisible : 0;
        if (nodes[index].parent != UINT32_MAX)
            payload.parentNodeId = nodes[nodes[index].parent].id;
        CopyMatrix(nodes[index].local, payload.localTransform);
        if (!writer.AddNode(payload)) return state.Fail(writer.Error());
    }

    MaterialEmitter materialEmitter{writer, state, time};
    std::uint64_t instanceCount = 0;
    for (std::size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
        const auto& node = nodes[nodeIndex];
        const UsdGeomMesh occurrence(node.prim);
        if (!occurrence || !node.visible || !node.acceptedPurpose) continue;
        if (nodeIndex < pointExpansions.originalNodeCount
            && UnderPrototype(node.prim.GetPath(), pointExpansions.prototypeRoots)) continue;
        const UsdPrim sourcePrim = node.prim.IsInstanceProxy() ? node.prim.GetPrimInPrototype() : node.prim;
        const auto found = geometry.find(sourcePrim.GetPath().GetString());
        if (found == geometry.end()) return state.Fail(ImportErrorCode::MalformedData);
        bool doubleSided = false; occurrence.GetDoubleSidedAttr().Get(&doubleSided, time);
        for (const auto& record : found->second.records) {
            if (++instanceCount > model_core::kTierBObjectLimit)
                return state.Fail(ImportErrorCode::ResourceLimit);
            const std::uint32_t materialId = materialEmitter.Emit(
                GroupMaterial(occurrence, record.materialGroup), doubleSided);
            if (state.error != ImportErrorCode::None) return false;
            MeshInstancePayload payload{}; payload.instanceId = writer.NextId();
            payload.nodeId = node.id; payload.geometryChunkId = record.id;
            payload.materialChunkId = materialId; payload.flags = model_core::kSceneRecordVisible;
            if (!TransformBounds(record.descriptor, node.world, payload.worldMin, payload.worldMax))
                return state.Fail(ImportErrorCode::MalformedData);
            if (!writer.AddInstance(payload)) return state.Fail(writer.Error());
        }
    }
    if (!instanceCount) return state.Fail(ImportErrorCode::UnsupportedRequiredFeature);
    if (state.optionalWarnings || state.textureWarnings) {
        model_core::ImportStatusPayload status{};
        status.optionalFeatureWarnings = state.optionalWarnings;
        status.textureWarnings = state.textureWarnings;
        ChunkDescriptor descriptor{}; descriptor.topology = ChunkTopology::ImportStatus;
        descriptor.chunkId = writer.NextId();
        if (!writer.Add(descriptor, import_worker::ChunkBytes(status)))
            return state.Fail(writer.Error());
    }
    if (!writer.Complete()) return state.Fail(writer.Error());
    result.chunkCount = writer.Count(); result.sectionBytesWritten = writer.Length();
    result.errorCode = ImportErrorCode::None;
    return true;
}

} // namespace

int RunOpenUsdProduction(const model_core::ParseOpenUsdFileRequest& request,
                         std::span<std::byte> destination, HANDLE controlInput,
                         HANDLE controlOutput, const std::filesystem::path& payloadDirectory,
                         OpenUsdImportResult& result)
{
    result = {};
    const auto fail = [&](ImportErrorCode code, ImportFailurePhase phase = ImportFailurePhase::Geometry) {
        result.errorCode = code; result.phase = phase; return 0;
    };
    HANDLE duplicate = nullptr;
    const HANDLE sourceHandle = reinterpret_cast<HANDLE>(
        static_cast<std::uintptr_t>(request.sourceFileHandleValue));
    if (!DuplicateHandle(GetCurrentProcess(), sourceHandle, GetCurrentProcess(), &duplicate,
                         0, FALSE, DUPLICATE_SAME_ACCESS))
        return fail(ImportErrorCode::CompatibilityHostFailure);
    auto opened = model_core::MappedFile::FromHandle(platform::Win32Handle(duplicate));
    if (!opened.file) return fail(ImportErrorCode::CompatibilityHostFailure);
    if (opened.file->SizeBytes() > model_core::kTierBPrimarySourceBytes)
        return fail(ImportErrorCode::PrimarySourceLimit);
    std::wstring mapError;
    auto source = opened.file->MapWhole(mapError);
    if (!source) return fail(ImportErrorCode::CompatibilityHostFailure);
    const auto format = DetectEncoding(source.Bytes());
    if (format == SourceFormatId::Unknown) return fail(ImportErrorCode::MalformedData);
    if (!EncodingMatches(format, request.requestFlags)) return fail(ImportErrorCode::UnsupportedEncoding);
    import_worker::UsdzArchiveView archive;
    if (format == SourceFormatId::Usdz) {
        const auto preflight = import_worker::InspectUsdz(source.Bytes(), &archive, {}, [&] {
            return WaitForSingleObject(reinterpret_cast<HANDLE>(
                static_cast<std::uintptr_t>(request.cancellationEventHandleValue)), 0) == WAIT_OBJECT_0;
        });
        if (preflight == import_worker::UsdzPreflightError::Cancelled)
            return fail(ImportErrorCode::Cancelled);
        if (preflight != import_worker::UsdzPreflightError::None)
            return fail(ImportErrorCode::ArchiveLimit);
    }
    import_worker::SidecarFileClient sidecars(controlInput, controlOutput, request.generationId);
    sidecars.EnablePinnedReplay();
    ByteStore store;
    const std::string rootIdentifier = RootIdentifier(format);
    if (!store.InitializeProduction(rootIdentifier, source.Bytes(), &sidecars))
        return fail(ImportErrorCode::CompatibilityHostFailure);
    gStore = &store;
    struct ResetStore { ~ResetStore() { gStore = nullptr; } } reset;
    PlugRegistry::GetInstance().RegisterPlugins(
        (payloadDirectory / L"usd" / L"plugInfo.json").generic_string());
    TfErrorMark errors;
    const std::string verifiedIdentifier = ArGetResolver().CreateIdentifier(rootIdentifier);
    if (verifiedIdentifier.empty()) return fail(ImportErrorCode::CompatibilityHostFailure);
    const ArResolvedPath verifiedPath = ArGetResolver().Resolve(verifiedIdentifier);
    if (verifiedPath.empty()) return fail(ImportErrorCode::FileUnavailable);
    const auto verifiedAsset = ArGetResolver().OpenAsset(verifiedPath);
    if (!verifiedAsset || verifiedAsset->GetSize() != source.Bytes().size())
        return fail(ImportErrorCode::CompatibilityHostFailure);
    const auto stage = UsdStage::Open(rootIdentifier, UsdStage::LoadNone);
    if (!stage) {
        const auto resolver = store.ProductionError();
        return fail(resolver == ImportErrorCode::ResourceLimit ? ImportErrorCode::CompatibilityHostLimit
            : resolver == ImportErrorCode::UnsafeReference ? ImportErrorCode::UnsafeReference
                                                           : ImportErrorCode::MalformedData);
    }
    ProductionState state{request,
        reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(request.cancellationEventHandleValue)),
        Clock::now() + kCompositionDeadline, &store, source.Bytes(),
        format == SourceFormatId::Usdz ? &archive : nullptr};
    if (!LoadProductionPayloads(stage, state)) return fail(state.error, state.phase);
    if (!stage->GetCompositionErrors().empty()) {
        const auto resolver = store.ProductionError();
        return fail(resolver == ImportErrorCode::ResourceLimit ? ImportErrorCode::CompatibilityHostLimit
            : resolver == ImportErrorCode::UnsafeReference ? ImportErrorCode::UnsafeReference
            : resolver == ImportErrorCode::FileUnavailable ? ImportErrorCode::FileUnavailable
                                                            : ImportErrorCode::MalformedData);
    }
    import_worker::ChunkBatchSink sink(controlInput, controlOutput, request.generationId,
                                       request.requestFlags, state.cancellation);
    try {
        if (!NormalizeProduction(stage, format, destination, sink, state, result))
            return fail(state.error == ImportErrorCode::None ? ImportErrorCode::InternalImporterFailure
                                                             : state.error, state.phase);
    } catch (const std::bad_alloc&) {
        return fail(ImportErrorCode::OutOfMemory);
    } catch (...) {
        return fail(ImportErrorCode::InternalImporterFailure);
    }
    return 0;
}

} // namespace compatibility_host

extern "C" __declspec(dllexport) int __cdecl Preview3DRunOpenUsdSpike(
    compatibility_host::OpenUsdSpikeSection* output, const std::byte* section,
    std::size_t sectionSize, const wchar_t* payloadDirectory)
{
    if (!output || !section || !payloadDirectory) return 1;
    return compatibility_host::RunOpenUsdSpike(
        *output, std::span<const std::byte>(section, sectionSize), payloadDirectory);
}

extern "C" __declspec(dllexport) int __cdecl Preview3DAuditOpenUsdPayload(
    const wchar_t* payloadDirectory)
{
    if (!payloadDirectory) return 1;
    return compatibility_host::AuditResources(payloadDirectory) ? 0 : 2;
}

extern "C" __declspec(dllexport) int __cdecl Preview3DRunOpenUsdImport(
    const model_core::ParseOpenUsdFileRequest* request,
    std::byte* outputSection, std::size_t outputSectionSize,
    void* controlInput, void* controlOutput,
    const wchar_t* payloadDirectory, compatibility_host::OpenUsdImportResult* result)
{
    if (!request || !outputSection || !outputSectionSize || !controlInput || !controlOutput
        || !payloadDirectory || !result) return 1;
    return compatibility_host::RunOpenUsdProduction(*request,
        std::span<std::byte>(outputSection, outputSectionSize),
        static_cast<HANDLE>(controlInput), static_cast<HANDLE>(controlOutput),
        payloadDirectory, *result);
}
