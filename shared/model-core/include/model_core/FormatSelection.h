#pragma once

// Pure open-routing policy on top of the format catalog.
//
// This header owns the decisions shared by the viewer, the CLI parser and the
// unit tests: which descriptor an extension/content classification may open,
// when the user must choose (Open As), and the stable registration identities
// the installer must match. It reads no file and parses no document, so it is
// safe to use from the trusted process and from tests without a GPU or parser.

#include "model_core/FormatCatalog.h"
#include "model_core/FormatSniffer.h"

#include <string>
#include <string_view>
#include <vector>

namespace model_core {

enum class OpenFormatAction : uint8_t {
    Reject = 0,
    OpenExtension,
    AskUser,
};

struct OpenFormatDecision {
    // The descriptor the open path should use when the action is
    // OpenExtension. For AskUser this is the strongest candidate (content),
    // with `extensionCandidate` kept for the "show both" prompt.
    const FormatDescriptor* format = nullptr;
    const FormatDescriptor* extensionCandidate = nullptr;
    OpenFormatAction action = OpenFormatAction::Reject;
    ClassificationReason reason = ClassificationReason::None;
};

// Applies the user-override policy:
//   * a known extension, whether or not content agrees, opens with the
//     extension's importer (existing behaviour stays identical);
//   * content-only evidence or a cross-family disagreement never opens
//     silently: it asks the user through Open As and reports both candidates;
//   * no evidence rejects with the typed unsupported-format path.
inline OpenFormatDecision DecideOpenFormat(const FormatClassification& classification) noexcept
{
    OpenFormatDecision result;
    result.reason = classification.reason;
    result.extensionCandidate = classification.extension;
    switch (classification.reason) {
    case ClassificationReason::Both:
    case ClassificationReason::Extension:
        result.format = classification.extension;
        result.action = OpenFormatAction::OpenExtension;
        break;
    case ClassificationReason::Content:
        result.format = classification.content;
        result.action = OpenFormatAction::AskUser;
        break;
    case ClassificationReason::Mismatch:
        result.format = classification.content ? classification.content : classification.extension;
        result.action = OpenFormatAction::AskUser;
        break;
    case ClassificationReason::None:
        result.action = OpenFormatAction::Reject;
        break;
    }
    return result;
}

inline std::string StableNameOrUnknown(const FormatDescriptor* descriptor)
{
    return descriptor ? std::string(descriptor->stableName) : std::string("unknown");
}

// Path-free "Detected as X; suffix Y" text for diagnostics and the Open As
// prompt. Stable names are ASCII and never localized.
inline std::string FormatChoiceSummary(const FormatClassification& classification)
{
    std::string text = "extension=" + StableNameOrUnknown(classification.extension);
    text += " content=" + StableNameOrUnknown(classification.content);
    text += " selected=" + StableNameOrUnknown(classification.selected);
    switch (classification.reason) {
    case ClassificationReason::Extension: text += " reason=extension"; break;
    case ClassificationReason::Content: text += " reason=content"; break;
    case ClassificationReason::Both: text += " reason=both"; break;
    case ClassificationReason::Mismatch: text += " reason=mismatch"; break;
    case ClassificationReason::None: text += " reason=none"; break;
    }
    return text;
}

// Stable application ProgID owned by a catalog family. The installer, the
// registration test and any future registry writer all read this one table;
// a family without an installer ProgID returns nullptr.
inline const char* ProgIdForFamily(FormatId family) noexcept
{
    switch (family) {
    case FormatId::Gltf: return "Binbuf.Preview3D.glTF.1";
    case FormatId::Stl: return "Binbuf.Preview3D.STL.1";
    case FormatId::Ply: return "Binbuf.Preview3D.PLY.1";
    case FormatId::Obj: return "Binbuf.Preview3D.OBJ.1";
    case FormatId::Fbx: return "Binbuf.Preview3D.FBX.1";
    case FormatId::ThreeMf: return "Binbuf.Preview3D.ThreeMF.1";
    case FormatId::Usd: return "Binbuf.Preview3D.USD.1";
    case FormatId::Step: return "Binbuf.Preview3D.STEP.1";
    default: return nullptr;
    }
}

inline const char* ProgIdForFormat(const FormatDescriptor& descriptor) noexcept
{
    return ProgIdForFamily(descriptor.family);
}

// True when the descriptor names an importer this product actually ships.
// Reserved catalog identities (CadHost, AssimpCompat) are rejected until
// their adapter task lands.
inline bool IsImportableFormat(const FormatDescriptor* descriptor) noexcept
{
    if (!descriptor) return false;
    switch (descriptor->importer) {
    case ImporterKind::GltfWorker:
    case ImporterKind::StlWorker:
    case ImporterKind::PlyWorker:
    case ImporterKind::ObjWorker:
    case ImporterKind::FbxWorker:
    case ImporterKind::ThreeMfWorker:
    case ImporterKind::UsdFastPath:
    case ImporterKind::UsdCompatibilityHost:
    case ImporterKind::StepHost:
        return true;
    default:
        return false;
    }
}

// Catalog descriptors that own at least one extension and have a shipped
// importer, in catalog order. This is the Open As list and the set the open
// dialog/installer must cover.
inline std::vector<const FormatDescriptor*> ImportableFormatDescriptors()
{
    std::vector<const FormatDescriptor*> result;
    for (const auto& descriptor : Formats()) {
        if (!descriptor.extensions.empty() && IsImportableFormat(&descriptor)) {
            result.push_back(&descriptor);
        }
    }
    return result;
}

// Lowercase extensions with a leading dot, deduplicated, in catalog order.
inline std::vector<std::wstring> DirectOpenExtensions()
{
    std::vector<std::wstring> result;
    for (const auto& descriptor : Formats()) {
        if (!IsImportableFormat(&descriptor)) continue;
        for (const auto extension : descriptor.extensions) {
            const std::wstring candidate = L"." + std::wstring(extension);
            bool seen = false;
            for (const auto& existing : result) {
                if (existing == candidate) { seen = true; break; }
            }
            if (!seen) result.push_back(candidate);
        }
    }
    return result;
}

} // namespace model_core
