#pragma once

// T07 product-owned reader registry.
//
// A reader is the product identity a user can list and force. It is not a
// plugin: this is a compile-time table of readers shipped in this build, and it
// cannot load code, a path or a URL. Each reader names the catalog family (or
// families, via its extensions), the closed ImporterKind the worker/host
// receives, the extension suffixes it owns, a product-policy score and the
// closed option names it honors.
//
// Reader selection is a trusted-process decision. The worker/host never sees a
// reader name; the viewer maps the selection to the existing closed
// SourceFormat/ImportFormat discriminator and the normal admission,
// containment and budgets still apply. Scores are product policy, not parser
// output: a compatibility reader always scores below its native counterpart,
// and ties resolve by stable name.

#include "model_core/ControlProtocol.h"
#include "model_core/FormatCatalog.h"
#include "model_core/FormatSniffer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace model_core {

struct ReaderDescriptor {
    // Stable ASCII product identity, lowercase, never localized or derived
    // from a path.
    std::string_view name{};
    const wchar_t* displayName = nullptr;
    // Primary catalog family. Multi-family readers (the OCCT CAD host and the
    // Assimp compatibility adapter) declare their first family here; use
    // ReaderFamilies()/ReaderCoversFamily() for the complete coverage.
    FormatId family = FormatId::Unknown;
    ImporterKind importer = ImporterKind::Unknown;
    std::span<const std::wstring_view> extensions{}; // lowercase, no leading dot
    uint8_t score = 0; // product policy; higher wins
    bool isCompatibility = false;
    std::span<const std::string_view> optionNames{}; // closed option names
};

namespace reader_catalog_detail {

inline constexpr std::wstring_view kGltfReaderExtensions[] = { L"gltf", L"glb" };
inline constexpr std::wstring_view kStlReaderExtensions[] = { L"stl" };
inline constexpr std::wstring_view kPlyReaderExtensions[] = { L"ply" };
inline constexpr std::wstring_view kObjReaderExtensions[] = { L"obj" };
inline constexpr std::wstring_view kFbxReaderExtensions[] = { L"fbx" };
inline constexpr std::wstring_view kThreeMfReaderExtensions[] = { L"3mf" };
inline constexpr std::wstring_view kUsdReaderExtensions[] = {
    L"usd", L"usda", L"usdc", L"usdz" };
inline constexpr std::wstring_view kStepReaderExtensions[] = {
    L"step", L"stp", L"stpnc", L"p21", L"210" };
inline constexpr std::wstring_view kCadReaderExtensions[] = {
    L"igs", L"iges", L"brep", L"xbf", L"fcstd" };
inline constexpr std::wstring_view kAssimpReaderExtensions[] = {
    L"dae", L"3ds", L"dxf", L"x", L"off", L"amf", L"x3d" };
// T08 single-family compatibility readers declare the same suffixes as their
// native counterpart. The extension is catalog identity; the reader name is
// the product identity a user can force.
inline constexpr std::wstring_view kGltfAssimpReaderExtensions[] = { L"gltf", L"glb" };
inline constexpr std::wstring_view kObjAssimpReaderExtensions[] = { L"obj" };
inline constexpr std::wstring_view kFbxAssimpReaderExtensions[] = { L"fbx" };
inline constexpr std::wstring_view kThreeMfAssimpReaderExtensions[] = { L"3mf" };

inline constexpr std::string_view kCadReaderOptions[] = {
    "quality", "read_wire", "linear_deflection", "angular_deflection",
    "relative_deflection", "unit_scale" };
inline constexpr std::string_view kUsdReaderOptions[] = { "subdivision_level" };

// Stable name order. --list-readers prints this order verbatim, so a new reader
// must be inserted at its sorted position rather than appended.
inline constexpr ReaderDescriptor kReaders[] = {
    { "3mf", L"3MF", FormatId::ThreeMf, ImporterKind::ThreeMfWorker,
      std::span<const std::wstring_view>(kThreeMfReaderExtensions), 90, false, {} },
    { "3mf-assimp", L"3MF (Assimp compatibility)", FormatId::ThreeMf,
      ImporterKind::AssimpCompat,
      std::span<const std::wstring_view>(kThreeMfAssimpReaderExtensions), 35, true, {} },
    { "assimp", L"Assimp compatibility", FormatId::Dae, ImporterKind::AssimpCompat,
      std::span<const std::wstring_view>(kAssimpReaderExtensions), 30, true, {} },
    { "cad", L"OCCT CAD", FormatId::Iges, ImporterKind::CadHost,
      std::span<const std::wstring_view>(kCadReaderExtensions), 100, false,
      std::span<const std::string_view>(kCadReaderOptions) },
    { "fbx", L"FBX", FormatId::Fbx, ImporterKind::FbxWorker,
      std::span<const std::wstring_view>(kFbxReaderExtensions), 90, false, {} },
    { "fbx-assimp", L"FBX (Assimp compatibility)", FormatId::Fbx,
      ImporterKind::AssimpCompat,
      std::span<const std::wstring_view>(kFbxAssimpReaderExtensions), 35, true, {} },
    { "gltf", L"glTF", FormatId::Gltf, ImporterKind::GltfWorker,
      std::span<const std::wstring_view>(kGltfReaderExtensions), 100, false, {} },
    { "gltf-assimp", L"glTF (Assimp compatibility)", FormatId::Gltf,
      ImporterKind::AssimpCompat,
      std::span<const std::wstring_view>(kGltfAssimpReaderExtensions), 35, true, {} },
    { "obj", L"OBJ", FormatId::Obj, ImporterKind::ObjWorker,
      std::span<const std::wstring_view>(kObjReaderExtensions), 50, false, {} },
    { "obj-assimp", L"OBJ (Assimp compatibility)", FormatId::Obj,
      ImporterKind::AssimpCompat,
      std::span<const std::wstring_view>(kObjAssimpReaderExtensions), 35, true, {} },
    { "ply", L"PLY", FormatId::Ply, ImporterKind::PlyWorker,
      std::span<const std::wstring_view>(kPlyReaderExtensions), 70, false, {} },
    { "step", L"STEP", FormatId::Step, ImporterKind::StepHost,
      std::span<const std::wstring_view>(kStepReaderExtensions), 100, false,
      std::span<const std::string_view>(kCadReaderOptions) },
    { "stl", L"STL", FormatId::Stl, ImporterKind::StlWorker,
      std::span<const std::wstring_view>(kStlReaderExtensions), 60, false, {} },
    { "usd", L"USD", FormatId::Usd, ImporterKind::UsdFastPath,
      std::span<const std::wstring_view>(kUsdReaderExtensions), 95, false,
      std::span<const std::string_view>(kUsdReaderOptions) },
    { "usd-compat", L"USD (OpenUSD compatibility)", FormatId::Usd,
      ImporterKind::UsdCompatibilityHost,
      std::span<const std::wstring_view>(kUsdReaderExtensions), 40, true,
      std::span<const std::string_view>(kUsdReaderOptions) },
};

inline std::string AsciiFromWide(std::wstring_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        result.push_back(character >= 0 && character <= 0x7f
            ? static_cast<char>(character) : '?');
    }
    return result;
}

} // namespace reader_catalog_detail

// The shipped reader table in stable name order.
inline std::span<const ReaderDescriptor> Readers() noexcept
{
    return reader_catalog_detail::kReaders;
}

// Case-insensitive stable-name lookup. A reader that is not in this build's
// table returns nullptr; there is no fallback or dynamic discovery.
inline const ReaderDescriptor* FindReaderByName(std::string_view name) noexcept
{
    for (const auto& reader : Readers()) {
        if (catalog_detail::EqualAscii(reader.name, name)) return &reader;
    }
    return nullptr;
}

// True when the reader owns the extension (with or without a leading dot).
inline bool ReaderOwnsExtension(const ReaderDescriptor& reader,
                                std::wstring_view extension) noexcept
{
    if (!extension.empty() && extension.front() == L'.') extension.remove_prefix(1);
    if (extension.empty()) return false;
    for (const auto candidate : reader.extensions) {
        if (catalog_detail::EqualExtension(candidate, extension)) return true;
    }
    return false;
}

// True when the reader owns at least one suffix of the descriptor's family.
// Wire-only content variants (ASCII STL/PLY) declare no extension and are
// served by the reader that owns their family's extension.
inline bool ReaderCoversDescriptor(const ReaderDescriptor& reader,
                                   const FormatDescriptor* descriptor) noexcept
{
    if (!descriptor) return false;
    for (const auto extension : descriptor->extensions) {
        if (ReaderOwnsExtension(reader, extension)) return true;
    }
    if (descriptor->extensions.empty()) {
        for (const auto& candidate : Formats()) {
            if (candidate.family != descriptor->family) continue;
            for (const auto extension : candidate.extensions) {
                if (ReaderOwnsExtension(reader, extension)) return true;
            }
        }
    }
    return false;
}

inline bool ReaderCoversFamily(const ReaderDescriptor& reader, FormatId family) noexcept
{
    for (const auto& descriptor : Formats()) {
        if (descriptor.family != family) continue;
        if (ReaderCoversDescriptor(reader, &descriptor)) return true;
    }
    return false;
}

// Unique catalog families the reader serves, in catalog order.
inline std::vector<FormatId> ReaderFamilies(const ReaderDescriptor& reader)
{
    std::vector<FormatId> families;
    for (const auto& descriptor : Formats()) {
        if (descriptor.extensions.empty()) continue;
        if (!ReaderCoversDescriptor(reader, &descriptor)) continue;
        if (std::find(families.begin(), families.end(), descriptor.family) == families.end())
            families.push_back(descriptor.family);
    }
    return families;
}

// Readers serving a family, default first: score descending, then stable name.
inline std::vector<const ReaderDescriptor*> ReadersForFamily(FormatId family)
{
    std::vector<const ReaderDescriptor*> result;
    for (const auto& reader : Readers()) {
        if (ReaderCoversFamily(reader, family)) result.push_back(&reader);
    }
    std::sort(result.begin(), result.end(), [](const ReaderDescriptor* left,
                                               const ReaderDescriptor* right) {
        if (left->score != right->score) return left->score > right->score;
        return left->name < right->name;
    });
    return result;
}

// The highest-scoring shipped reader for a family, or nullptr.
inline const ReaderDescriptor* DefaultReaderForFamily(FormatId family)
{
    const auto readers = ReadersForFamily(family);
    return readers.empty() ? nullptr : readers.front();
}

// The catalog descriptor that represents a family for an Open As selection:
// the first importable descriptor with an extension, in catalog order.
inline const FormatDescriptor* DefaultFormatForFamily(FormatId family) noexcept
{
    for (const auto& descriptor : Formats()) {
        if (descriptor.family != family) continue;
        if (descriptor.extensions.empty()) continue;
        if (descriptor.importer == ImporterKind::Unknown) continue;
        return &descriptor;
    }
    return nullptr;
}

struct ReaderChoice {
    const ReaderDescriptor* reader = nullptr;
    bool isDefault = true;
};

// Default plus shipped alternates for one family, default first.
inline std::vector<ReaderChoice> ReaderChoicesForFamily(FormatId family)
{
    std::vector<ReaderChoice> result;
    const auto readers = ReadersForFamily(family);
    for (std::size_t index = 0; index < readers.size(); ++index)
        result.push_back({ readers[index], index == 0 });
    return result;
}

// The single shipped compatibility reader for a family, or nullptr. The
// multi-family `assimp` reader is deliberately not returned: it owns formats
// that have no native counterpart, so it is a default reader, not an alternate.
inline const ReaderDescriptor* CompatibilityReaderForFamily(FormatId family) noexcept
{
    for (const auto& reader : Readers()) {
        if (!reader.isCompatibility) continue;
        if (reader.name == "assimp") continue;
        if (ReaderCoversFamily(reader, family)) return &reader;
    }
    return nullptr;
}

// Closed mapping from a single-family compatibility reader to the worker's
// Assimp hint. Returns false for every native or multi-family reader. This is
// the table/discriminator sync point: a new compatibility reader must add a
// closed AssimpFormat value, and a new hint must appear here.
constexpr bool AssimpFormatForCompatibilityReader(const ReaderDescriptor& reader,
                                                  AssimpFormat& format) noexcept
{
    if (!reader.isCompatibility) return false;
    if (reader.name == "gltf-assimp") { format = AssimpFormat::Gltf; return true; }
    if (reader.name == "obj-assimp") { format = AssimpFormat::Obj; return true; }
    if (reader.name == "fbx-assimp") { format = AssimpFormat::Fbx; return true; }
    if (reader.name == "3mf-assimp") { format = AssimpFormat::ThreeMf; return true; }
    return false;
}

constexpr const char* ImporterKindName(ImporterKind importer) noexcept
{
    switch (importer) {
    case ImporterKind::GltfWorker: return "gltf-worker";
    case ImporterKind::StlWorker: return "stl-worker";
    case ImporterKind::PlyWorker: return "ply-worker";
    case ImporterKind::ObjWorker: return "obj-worker";
    case ImporterKind::FbxWorker: return "fbx-worker";
    case ImporterKind::ThreeMfWorker: return "3mf-worker";
    case ImporterKind::UsdFastPath: return "usd-fast";
    case ImporterKind::UsdCompatibilityHost: return "usd-compat-host";
    case ImporterKind::StepHost: return "step-host";
    case ImporterKind::CadHost: return "cad-host";
    case ImporterKind::AssimpCompat: return "assimp-compat";
    default: return "unknown";
    }
}

enum class ReaderSelectionError : uint8_t {
    None = 0,
    // The forced reader's multi-family table cannot serve this file and there
    // is no single-family fallback. Rejected before any open.
    ReaderCannotOpenFormat,
    // The classification named no importable family, so no default reader
    // exists. The normal unsupported-format path owns this decision.
    NoReaderForFormat,
};

struct ReaderSelection {
    const ReaderDescriptor* reader = nullptr;
    const FormatDescriptor* format = nullptr;
    bool forced = false;
    // The trusted viewer must route a USD selection to the OpenUSD
    // compatibility host instead of the TinyUSDZ fast path. Still a closed
    // product value; the host validates the bytes and all budgets apply.
    bool forceCompatibilityHost = false;
    ReaderSelectionError error = ReaderSelectionError::None;
    // Stable ASCII, path-free diagnostic. Includes the reader name whenever one
    // is known so a forced-reader failure can name it.
    std::string diagnostic{};

    explicit operator bool() const noexcept
    {
        return error == ReaderSelectionError::None && format != nullptr;
    }
};

// Extension/content classification remains the default selector. The default
// reader is the highest-scoring shipped reader that covers the selected
// family.
inline ReaderSelection SelectDefaultReader(const FormatClassification& classification)
{
    ReaderSelection result;
    if (!classification.selected) {
        result.error = ReaderSelectionError::NoReaderForFormat;
        result.diagnostic = "no reader for an unclassified file";
        return result;
    }
    result.format = classification.selected;
    result.reader = DefaultReaderForFamily(classification.selected->family);
    if (!result.reader) {
        result.error = ReaderSelectionError::NoReaderForFormat;
        result.diagnostic = "no shipped reader for family '"
            + std::string(classification.selected->stableName) + "'";
    }
    return result;
}

// Resolve a forced reader against the file's extension/content classification.
// The reader owns the file when it owns the suffix (or the content family);
// otherwise a single-family reader falls back to its own family and lets the
// worker/host reject the bytes. A multi-family reader has no honest fallback,
// so that selection fails typed before any open.
inline ReaderSelection ResolveForcedReader(const ReaderDescriptor& reader,
                                           const FormatClassification& classification)
{
    ReaderSelection result;
    result.reader = &reader;
    result.forced = true;
    result.forceCompatibilityHost =
        reader.importer == ImporterKind::UsdCompatibilityHost;

    if (ReaderCoversDescriptor(reader, classification.extension)) {
        result.format = classification.extension;
    } else if (ReaderCoversDescriptor(reader, classification.content)) {
        result.format = classification.content;
    } else {
        const auto families = ReaderFamilies(reader);
        if (families.size() == 1)
            result.format = DefaultFormatForFamily(families.front());
    }

    if (result.format) return result;

    result.error = ReaderSelectionError::ReaderCannotOpenFormat;
    result.diagnostic = "reader '" + std::string(reader.name) + "' cannot open ";
    if (classification.extension) {
        result.diagnostic += "'" + std::string(classification.extension->stableName)
            + "' files";
    } else if (classification.content) {
        result.diagnostic += "'" + std::string(classification.content->stableName)
            + "' content";
    } else {
        result.diagnostic += "an unclassified file";
    }
    result.diagnostic += "; supported extensions: ";
    bool first = true;
    for (const auto extension : reader.extensions) {
        if (!first) result.diagnostic += ',';
        result.diagnostic += reader_catalog_detail::AsciiFromWide(extension);
        first = false;
    }
    return result;
}

} // namespace model_core
