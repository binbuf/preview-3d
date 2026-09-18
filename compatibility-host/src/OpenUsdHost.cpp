#define NOMINMAX

#include "OpenUsdHost.h"

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
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/xformCache.h"
#include "pxr/usd/usdShade/material.h"
#include "pxr/usd/usdShade/shader.h"
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace compatibility_host {
namespace {

using Clock = std::chrono::steady_clock;

struct ExpectedResource { const wchar_t* path; const char* sha256; };
constexpr ExpectedResource kResources[] = {
    {L"plugInfo.json", "c285a05d159154e3ae201fa383c61766bc57c58aae6d5a70b7463b49e2dcbb2d"},
    {L"ar/resources/plugInfo.json", "b37fc05c907b7756bec87a500893a325731b9c39ef0ad0f73c4d9b6351183ba2"},
    {L"sdf/resources/plugInfo.json", "73d500266d66794ebdd1ee9a486cd75adca2665e61f436d0f448900ca7dacd2b"},
    {L"usd/resources/plugInfo.json", "dbfc3fc05484155e5a552e5a002ea57f4407be1ed379a8fc165c4c5d49231721"},
    {L"usd/resources/generatedSchema.usda", "a09ceab63ab5491e33054cb2a9394af35c391cba2633fa60c3ef373addddaa4c"},
    {L"usd/resources/usd/schema.usda", "53706dd717cad34c05a567c320c1ecbc99acf327f65fcf703b5409e4c07c8e63"},
    {L"usdGeom/resources/plugInfo.json", "ecf2ce5596a544af2bfe3628eac1f9233947b42091f80e271aff51844e98ac4c"},
    {L"usdGeom/resources/generatedSchema.usda", "f5732dad7c7128bae308383820fea109e08affb6abd2a65fa7d4bde8fa09ca94"},
    {L"usdGeom/resources/usdGeom/schema.usda", "cf1f4615b7196d59a78f49b490a15bfe0c49c78d96f7bc5620075341263ca251"},
    {L"usdShade/resources/plugInfo.json", "76e44cccb791be06ef6de8967d11cbdfd9538ef0dde0e43dfacd72f39faf9747"},
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
        const auto digest = platform::ComputeSha256(*bytes);
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

bool Safe(std::string_view id)
{
    constexpr std::string_view prefix = "preview3d://";
    if (!id.starts_with(prefix) || id.size() >= kOpenUsdIdentifierCapacity
        || id.find('\\') != id.npos || id.find(':', prefix.size()) != id.npos
        || id.ends_with('/')) return false;
    for (std::size_t start = prefix.size(); start < id.size();) {
        const auto slash = id.find('/', start);
        const auto part = id.substr(start, slash == id.npos ? id.size() - start : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = slash == id.npos ? id.size() : slash + 1;
    }
    return true;
}

std::optional<std::string> Anchor(std::string_view asset, std::string_view anchor)
{
    if (asset.empty() || asset.find('\\') != asset.npos
        || (!asset.starts_with("preview3d://") && asset.find(':') != asset.npos)
        || asset.starts_with('/')) return std::nullopt;
    std::string candidate;
    if (asset.starts_with("preview3d://")) candidate.assign(asset);
    else {
        // OpenUSD supplies an empty anchor for composition arcs originating
        // in a layer backed only by ArAsset bytes. Treat that as the brokered
        // namespace root; a non-empty anchor remains directory-relative.
        if (anchor.empty()) candidate = "preview3d://";
        else {
            if (!Safe(anchor)) return std::nullopt;
            candidate.assign(anchor.substr(0, anchor.find_last_of('/') + 1));
        }
        candidate.append(asset);
    }
    std::vector<std::string> parts;
    constexpr std::string_view prefix = "preview3d://";
    for (std::size_t start = prefix.size(); start <= candidate.size();) {
        const auto slash = candidate.find('/', start);
        const auto part = candidate.substr(start, slash == candidate.npos ? candidate.size() - start : slash - start);
        if (part == "..") { if (parts.empty()) return std::nullopt; parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        if (slash == candidate.npos) break;
        start = slash + 1;
    }
    std::string result(prefix);
    for (const auto& part : parts) {
        if (result.size() > prefix.size()) result += '/';
        result += part;
    }
    return Safe(result) ? std::optional<std::string>(result) : std::nullopt;
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
            if (!Safe(name) || assets_.contains(name) || item.byteOffset < sizeof(OpenUsdSpikeSection)
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
    std::shared_ptr<const std::vector<char>> Find(std::string_view name) const
    {
        const auto found = assets_.find(std::string(name));
        return found == assets_.end() ? nullptr : found->second;
    }
    bool OpenAllowed(bool found)
    {
        if (!found || header_->resolverOpenCount >= limit_) {
            ++header_->resolverDeniedCount; return false;
        }
        ++header_->resolverOpenCount; return true;
    }
private:
    std::unordered_map<std::string, std::shared_ptr<std::vector<char>>> assets_;
    OpenUsdSpikeSection* header_{};
    std::uint32_t limit_{};
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
        if (gStore && gStore->Find(path)) {
            return path;
        }
        const auto id = Anchor(path, anchor.GetPathString());
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
        std::sort(paths.begin(), paths.end());
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
        || !Safe(std::string_view(out.rootIdentifier, rootLength))) return 2;
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
