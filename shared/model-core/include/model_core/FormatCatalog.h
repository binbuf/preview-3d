#pragma once

// Compile-time format catalog.
//
// This header is data plus pure lookups. It deliberately includes no parser,
// platform or third-party header: the trusted process, the import brokers and
// the Explorer thumbnail provider may all read it, but it never decides that a
// file is safe to parse. The sniffer in FormatSniffer.h produces a candidate;
// the real parser and its containment still own acceptance.
//
// FormatId values mirror the existing SourceFormatId identities 1..13 so the
// two enumerations can be converted without a table. Values after that are
// append-only reservations for later catalog tasks and have no descriptor yet.

#include "model_core/WireFormat.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace model_core {

enum class FormatId : uint32_t {
    Unknown = 0,
    Gltf = 1,
    Stl = 2,
    Ply = 3,
    Glb = 4,
    AsciiStl = 5,
    AsciiPly = 6,
    Obj = 7,
    Fbx = 8,
    Usda = 9,
    Usdc = 10,
    Usdz = 11,
    ThreeMf = 12,
    Step = 13,
    // Generic `.usd` extension identity. The concrete encoding (USDA, USDC or
    // USDZ) is a content decision, so this descriptor has no wire identity of
    // its own until a parser reports one.
    Usd = 14,
    // Reserved append-only identities for later 0.7.0 breadth tasks (T07-T10).
    // They intentionally have no descriptor yet; a format is not "supported"
    // until its adapter, preflight, fixtures and installer entry also land.
    Iges = 15,
    Brep = 16,
    Xbf = 17,
    Fcstd = 18,
    Dae = 19,
    ThreeDs = 20,
    Off = 21,
    Amf = 22,
    Vrml = 23,
    X3d = 24,
};

// Which product adapter owns the family. The catalog records identity only;
// adapter behavior, preflight and budgets stay in the worker/host sources.
enum class ImporterKind : uint8_t {
    Unknown = 0,
    GltfWorker,
    StlWorker,
    PlyWorker,
    ObjWorker,
    FbxWorker,
    ThreeMfWorker,
    UsdFastPath,          // TinyUSDZ subset in Preview3DImportWorker.exe
    UsdCompatibilityHost, // OpenUSD compatibility host fallback
    StepHost,             // Preview3DStepHost.exe
    CadHost,              // reserved: OCCT IGES/BREP/XBF/FCStd
    AssimpCompat,         // reserved: bounded Assimp compatibility adapter
};

enum class UnitPolicy : uint8_t {
    Unspecified = 0, // no implicit scale; the parser must author one
    Metres,
    Millimetres,
    Centimetres,
    Inches,
    Feet,
};

// The view-state default declared by the catalog for a format family. It is
// deliberately expressed in product terms, not in viewer enum values: the
// trusted viewer maps it to its own lighting/grid/background state, and the
// thumbnail provider may map it differently. PointCloud is the one profile
// that cannot be declared by format alone (a PLY may be a mesh or a point
// cloud), so ResolvePresentationProfile() below picks it from the imported
// summary instead of from a second switch.
enum class PresentationProfile : uint8_t {
    Default = 0,
    StudioPbr,   // textured/neutral PBR review (glTF, FBX, USD, OBJ, PLY mesh)
    StudioCad,   // neutral material + ground grid (STL, 3MF, STEP)
    PointCloud,  // unlit colored round splats, no ground grid (PLY point-only)
};

enum class ThumbnailFamily : uint8_t {
    None = 0,
    Gltf,
    Stl,
    Ply,
    Obj,
    Fbx,
    ThreeMf,
    Usd,
    Step,
};

enum class CapabilityFlags : uint32_t {
    None = 0,
    Sidecars = 1u << 0,    // may resolve broker-approved dependencies
    Archive = 1u << 1,     // package container with bounded expansion
    CoarseProxy = 1u << 2, // Tier A/B streaming proxy is meaningful
    Instancing = 1u << 3,
    Skeletal = 1u << 4,
    CadHost = 1u << 5,
};

constexpr CapabilityFlags operator|(CapabilityFlags left, CapabilityFlags right) noexcept
{
    return static_cast<CapabilityFlags>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

constexpr bool HasCapability(CapabilityFlags value, CapabilityFlags probe) noexcept
{
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(probe)) != 0;
}

// Signature kinds are descriptive. FormatSniffer.cpp implements the bounded
// matching; the catalog never parses a document.
enum class MagicKind : uint8_t {
    HeadPrefix = 0,           // exact ASCII/byte prefix at offset
    HeadPrefixCaseInsensitive,
    GltfJsonAsset,            // JSON object with "asset" and "version"
    BinaryStl,                // 84-byte prefix + checked 50-byte facet count
    AsciiStlText,             // `solid` with facet/endsolid evidence
    PlyHeader,                // `ply` followed by a header line break
    ObjText,                  // vertex/face statement lines
    ZipEntryEquals,           // central-directory entry name equality
    ZipEntrySuffix,           // central-directory entry name suffix
};

struct MagicSignature {
    MagicKind kind = MagicKind::HeadPrefix;
    std::string_view bytes{};
    uint32_t offset = 0;
};

struct FormatDescriptor {
    FormatId id = FormatId::Unknown;
    // Families group content variants that classify to the same intent:
    // Gltf covers .gltf/.glb, Stl covers binary/ASCII STL, Usd covers
    // .usd/.usda/.usdc/.usdz.
    FormatId family = FormatId::Unknown;
    std::string_view stableName{};
    // Both point at string literals with static lifetime so they can be passed
    // directly to Loc()/UI code; no temporary key may reach the fallback cache.
    const char* displayNameKey = nullptr;
    const wchar_t* displayName = nullptr;
    std::span<const std::wstring_view> extensions{}; // lowercase, no leading dot
    std::span<const MagicSignature> signatures{};
    ImporterKind importer = ImporterKind::Unknown;
    uint8_t priority = 0; // higher wins when several signatures match
    UpAxisId defaultUpAxis = UpAxisId::Unknown;
    UnitPolicy unitPolicy = UnitPolicy::Unspecified;
    PresentationProfile presentation = PresentationProfile::Default;
    ThumbnailFamily thumbnail = ThumbnailFamily::None;
    CapabilityFlags capabilities = CapabilityFlags::None;
    SourceFormatId wireFormatId = SourceFormatId::Unknown;
};

namespace catalog_detail {

inline constexpr std::wstring_view kGltfExtensions[] = { L"gltf" };
inline constexpr std::wstring_view kGlbExtensions[] = { L"glb" };
inline constexpr std::wstring_view kStlExtensions[] = { L"stl" };
inline constexpr std::wstring_view kPlyExtensions[] = { L"ply" };
inline constexpr std::wstring_view kObjExtensions[] = { L"obj" };
inline constexpr std::wstring_view kFbxExtensions[] = { L"fbx" };
inline constexpr std::wstring_view kUsdExtensions[] = { L"usd" };
inline constexpr std::wstring_view kUsdaExtensions[] = { L"usda" };
inline constexpr std::wstring_view kUsdcExtensions[] = { L"usdc" };
inline constexpr std::wstring_view kUsdzExtensions[] = { L"usdz" };
inline constexpr std::wstring_view kThreeMfExtensions[] = { L"3mf" };
inline constexpr std::wstring_view kStepExtensions[] = { L"step", L"stp" };

inline constexpr MagicSignature kGltfSignatures[] = {
    { MagicKind::GltfJsonAsset, {}, 0 },
};
inline constexpr MagicSignature kGlbSignatures[] = {
    { MagicKind::HeadPrefix, "glTF", 0 },
};
inline constexpr MagicSignature kStlSignatures[] = {
    { MagicKind::BinaryStl, {}, 0 },
    { MagicKind::AsciiStlText, {}, 0 },
};
inline constexpr MagicSignature kPlySignatures[] = {
    { MagicKind::PlyHeader, {}, 0 },
};
inline constexpr MagicSignature kObjSignatures[] = {
    { MagicKind::ObjText, {}, 0 },
};
inline constexpr MagicSignature kFbxSignatures[] = {
    { MagicKind::HeadPrefix, "Kaydara FBX Binary", 0 },
    { MagicKind::HeadPrefixCaseInsensitive, "; FBX", 0 },
};
inline constexpr MagicSignature kUsdSignatures[] = {
    { MagicKind::HeadPrefix, "#usda", 0 },
    { MagicKind::HeadPrefix, "PXR-USDC", 0 },
    { MagicKind::ZipEntrySuffix, ".usdc", 0 },
    { MagicKind::ZipEntrySuffix, ".usda", 0 },
    { MagicKind::ZipEntrySuffix, ".usd", 0 },
};
inline constexpr MagicSignature kUsdaSignatures[] = {
    { MagicKind::HeadPrefix, "#usda", 0 },
};
inline constexpr MagicSignature kUsdcSignatures[] = {
    { MagicKind::HeadPrefix, "PXR-USDC", 0 },
};
inline constexpr MagicSignature kUsdzSignatures[] = {
    { MagicKind::ZipEntrySuffix, ".usdc", 0 },
    { MagicKind::ZipEntrySuffix, ".usda", 0 },
    { MagicKind::ZipEntrySuffix, ".usd", 0 },
};
inline constexpr MagicSignature kThreeMfSignatures[] = {
    { MagicKind::ZipEntryEquals, "[Content_Types].xml", 0 },
    { MagicKind::ZipEntryEquals, "3D/3dmodel.model", 0 },
};
inline constexpr MagicSignature kStepSignatures[] = {
    { MagicKind::HeadPrefix, "ISO-10303-21", 0 },
};

inline constexpr std::span<const std::wstring_view> Extensions(std::span<const std::wstring_view> value) noexcept
{
    return value;
}

inline constexpr std::span<const MagicSignature> Signatures(std::span<const MagicSignature> value) noexcept
{
    return value;
}

// The table is ordered by the numeric FormatId for deterministic iteration.
// Empty extension/signature spans mark wire-only content variants (ASCII
// STL/PLY) or the generic extension identity (`.usd`).
inline constexpr FormatDescriptor kFormats[] = {
    { FormatId::Gltf, FormatId::Gltf, "gltf", "format.gltf", L"glTF",
      Extensions(kGltfExtensions), Signatures(kGltfSignatures),
      ImporterKind::GltfWorker, 80, UpAxisId::Y, UnitPolicy::Metres,
      PresentationProfile::StudioPbr, ThumbnailFamily::Gltf,
      CapabilityFlags::Sidecars | CapabilityFlags::CoarseProxy,
      SourceFormatId::Gltf },
    { FormatId::Stl, FormatId::Stl, "stl", "format.stl", L"STL",
      Extensions(kStlExtensions), Signatures(kStlSignatures),
      ImporterKind::StlWorker, 60, UpAxisId::Z, UnitPolicy::Unspecified,
      PresentationProfile::StudioCad, ThumbnailFamily::Stl,
      CapabilityFlags::CoarseProxy, SourceFormatId::Stl },
    { FormatId::Ply, FormatId::Ply, "ply", "format.ply", L"PLY",
      Extensions(kPlyExtensions), Signatures(kPlySignatures),
      ImporterKind::PlyWorker, 70, UpAxisId::Unknown, UnitPolicy::Unspecified,
      PresentationProfile::StudioPbr, ThumbnailFamily::Ply,
      CapabilityFlags::CoarseProxy, SourceFormatId::Ply },
    { FormatId::Glb, FormatId::Gltf, "glb", "format.glb", L"GLB",
      Extensions(kGlbExtensions), Signatures(kGlbSignatures),
      ImporterKind::GltfWorker, 100, UpAxisId::Y, UnitPolicy::Metres,
      PresentationProfile::StudioPbr, ThumbnailFamily::Gltf,
      CapabilityFlags::CoarseProxy, SourceFormatId::Glb },
    { FormatId::AsciiStl, FormatId::Stl, "stl-ascii", "format.stl.ascii", L"STL (ASCII)",
      {}, Signatures(kStlSignatures),
      ImporterKind::StlWorker, 0, UpAxisId::Z, UnitPolicy::Unspecified,
      PresentationProfile::StudioCad, ThumbnailFamily::Stl,
      CapabilityFlags::CoarseProxy, SourceFormatId::AsciiStl },
    { FormatId::AsciiPly, FormatId::Ply, "ply-ascii", "format.ply.ascii", L"PLY (ASCII)",
      {}, Signatures(kPlySignatures),
      ImporterKind::PlyWorker, 0, UpAxisId::Unknown, UnitPolicy::Unspecified,
      PresentationProfile::StudioPbr, ThumbnailFamily::Ply,
      CapabilityFlags::CoarseProxy, SourceFormatId::AsciiPly },
    { FormatId::Obj, FormatId::Obj, "obj", "format.obj", L"OBJ",
      Extensions(kObjExtensions), Signatures(kObjSignatures),
      // OBJ has no standard up axis. Y-up is the ecosystem default (and the
      // F3D convention); the toolbar ground-axis override still wins.
      ImporterKind::ObjWorker, 50, UpAxisId::Y, UnitPolicy::Unspecified,
      PresentationProfile::StudioPbr, ThumbnailFamily::Obj,
      CapabilityFlags::Sidecars | CapabilityFlags::CoarseProxy,
      SourceFormatId::Obj },
    { FormatId::Fbx, FormatId::Fbx, "fbx", "format.fbx", L"FBX",
      Extensions(kFbxExtensions), Signatures(kFbxSignatures),
      ImporterKind::FbxWorker, 90, UpAxisId::Y, UnitPolicy::Unspecified,
      PresentationProfile::StudioPbr, ThumbnailFamily::Fbx,
      CapabilityFlags::Sidecars | CapabilityFlags::Skeletal | CapabilityFlags::CoarseProxy,
      SourceFormatId::Fbx },
    { FormatId::Usda, FormatId::Usd, "usda", "format.usda", L"USD",
      Extensions(kUsdaExtensions), Signatures(kUsdaSignatures),
      ImporterKind::UsdFastPath, 75, UpAxisId::Y, UnitPolicy::Centimetres,
      PresentationProfile::StudioPbr, ThumbnailFamily::Usd,
      CapabilityFlags::Sidecars | CapabilityFlags::CoarseProxy,
      SourceFormatId::Usda },
    { FormatId::Usdc, FormatId::Usd, "usdc", "format.usdc", L"USD",
      Extensions(kUsdcExtensions), Signatures(kUsdcSignatures),
      ImporterKind::UsdFastPath, 95, UpAxisId::Y, UnitPolicy::Centimetres,
      PresentationProfile::StudioPbr, ThumbnailFamily::Usd,
      CapabilityFlags::Sidecars | CapabilityFlags::CoarseProxy,
      SourceFormatId::Usdc },
    { FormatId::Usdz, FormatId::Usd, "usdz", "format.usdz", L"USDZ",
      Extensions(kUsdzExtensions), Signatures(kUsdzSignatures),
      ImporterKind::UsdFastPath, 85, UpAxisId::Y, UnitPolicy::Centimetres,
      PresentationProfile::StudioPbr, ThumbnailFamily::Usd,
      CapabilityFlags::Sidecars | CapabilityFlags::Archive | CapabilityFlags::Instancing
          | CapabilityFlags::CoarseProxy,
      SourceFormatId::Usdz },
    { FormatId::ThreeMf, FormatId::ThreeMf, "3mf", "format.3mf", L"3MF",
      Extensions(kThreeMfExtensions), Signatures(kThreeMfSignatures),
      ImporterKind::ThreeMfWorker, 90, UpAxisId::Z, UnitPolicy::Millimetres,
      PresentationProfile::StudioCad, ThumbnailFamily::ThreeMf,
      CapabilityFlags::Archive | CapabilityFlags::Instancing | CapabilityFlags::CoarseProxy,
      SourceFormatId::ThreeMf },
    { FormatId::Step, FormatId::Step, "step", "format.step", L"STEP",
      Extensions(kStepExtensions), Signatures(kStepSignatures),
      ImporterKind::StepHost, 100, UpAxisId::Unknown, UnitPolicy::Unspecified,
      PresentationProfile::StudioCad, ThumbnailFamily::Step,
      CapabilityFlags::CadHost | CapabilityFlags::CoarseProxy,
      SourceFormatId::Step },
    { FormatId::Usd, FormatId::Usd, "usd", "format.usd", L"USD",
      Extensions(kUsdExtensions), Signatures(kUsdSignatures),
      ImporterKind::UsdFastPath, 70, UpAxisId::Y, UnitPolicy::Centimetres,
      PresentationProfile::StudioPbr, ThumbnailFamily::Usd,
      CapabilityFlags::Sidecars | CapabilityFlags::CoarseProxy,
      SourceFormatId::Unknown },
};

constexpr wchar_t FoldExtensionChar(wchar_t value) noexcept
{
    return (value >= L'A' && value <= L'Z') ? static_cast<wchar_t>(value + (L'a' - L'A')) : value;
}

constexpr bool EqualExtension(std::wstring_view left, std::wstring_view right) noexcept
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (FoldExtensionChar(left[index]) != FoldExtensionChar(right[index])) return false;
    }
    return true;
}

constexpr bool EqualAscii(std::string_view left, std::string_view right) noexcept
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const char a = (left[index] >= 'A' && left[index] <= 'Z')
            ? static_cast<char>(left[index] + ('a' - 'A')) : left[index];
        const char b = (right[index] >= 'A' && right[index] <= 'Z')
            ? static_cast<char>(right[index] + ('a' - 'A')) : right[index];
        if (a != b) return false;
    }
    return true;
}

} // namespace catalog_detail

constexpr std::span<const FormatDescriptor> Formats() noexcept
{
    return catalog_detail::kFormats;
}

// Imported facts the profile decision is allowed to see. Counts are the
// validated scene totals, never a worker's unchecked claim.
struct PresentationSummary {
    uint64_t pointCount = 0;
    uint64_t triangleCount = 0;
};

// Pure, deterministic catalog -> preset mapping. A PLY is the only current
// family whose profile depends on content: a point-only/3DGS file selects the
// point preset, while a triangle mesh keeps the declared Studio PBR default.
// Calling this twice with the same inputs always returns the same profile.
constexpr PresentationProfile ResolvePresentationProfile(
    const FormatDescriptor& descriptor, PresentationSummary summary) noexcept
{
    if (descriptor.family == FormatId::Ply && summary.pointCount > 0
        && summary.triangleCount == 0)
        return PresentationProfile::PointCloud;
    return descriptor.presentation;
}


inline const FormatDescriptor* FindById(FormatId id) noexcept
{
    for (const auto& descriptor : Formats()) {
        if (descriptor.id == id) return &descriptor;
    }
    return nullptr;
}

inline const FormatDescriptor* FindByStableName(std::string_view name) noexcept
{
    for (const auto& descriptor : Formats()) {
        if (catalog_detail::EqualAscii(descriptor.stableName, name)) return &descriptor;
    }
    return nullptr;
}

inline const FormatDescriptor* FindBySourceFormatId(SourceFormatId wire) noexcept
{
    if (wire == SourceFormatId::Unknown) return nullptr;
    for (const auto& descriptor : Formats()) {
        if (descriptor.wireFormatId == wire) return &descriptor;
    }
    return nullptr;
}

// Accepts the extension with or without a leading dot, case-insensitively.
inline const FormatDescriptor* FindByExtension(std::wstring_view extension) noexcept
{
    if (!extension.empty() && extension.front() == L'.') extension.remove_prefix(1);
    if (extension.empty()) return nullptr;
    for (const auto& descriptor : Formats()) {
        for (const auto candidate : descriptor.extensions) {
            if (catalog_detail::EqualExtension(candidate, extension)) return &descriptor;
        }
    }
    return nullptr;
}

} // namespace model_core
