#include "import_broker/SidecarPathResolver.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <optional>

namespace import_broker {

namespace {

using model_core::ImportErrorCode;

SidecarResolution Reject(ImportErrorCode code, std::wstring message)
{
    SidecarResolution result;
    result.rejectionCode = code;
    result.diagnosticMessage = std::move(message);
    return result;
}

std::wstring Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    int required = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), required);
    return wide;
}

std::wstring LowerCopy(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

bool HasAllowedSidecarExtension(const std::filesystem::path& path)
{
    static const std::wstring kAllowed[] = { L".bin", L".mtl", L".png", L".jpg", L".jpeg",
                                             L".bmp", L".tif", L".tiff", L".webp", L".ktx2",
                                             L".usd", L".usda", L".usdc" };
    std::wstring ext = LowerCopy(path.extension().wstring());
    for (const auto& allowed : kAllowed) {
        if (ext == allowed) {
            return true;
        }
    }
    return false;
}

// The package search is deliberately image-only: a missing buffer, MTL file,
// or USD layer must stay exactly where the document references it, while a
// model's texture maps are routinely reorganized into a texture folder.
bool HasImageTextureExtension(const std::filesystem::path& path)
{
    static const std::wstring kImage[] = { L".png", L".jpg", L".jpeg", L".bmp",
                                           L".tif", L".tiff", L".webp", L".ktx2" };
    const std::wstring ext = LowerCopy(path.extension().wstring());
    for (const auto& allowed : kImage) {
        if (ext == allowed) {
            return true;
        }
    }
    return false;
}

// Opens one candidate path and runs the full handle-based validation every
// accepted sidecar must pass: canonicalize the opened handle, confirm the
// canonical path stays inside `allowedDirectoryPrefix` (lowercase, trailing
// separator), and enforce a nonzero size within the cap. Never trusts the
// candidate text itself.
SidecarResolution AcceptCandidate(const std::filesystem::path& candidate,
                                  const std::wstring& allowedDirectoryPrefix,
                                  uint64_t maxSidecarFileBytes)
{
    HANDLE rawFile = CreateFileW(candidate.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (rawFile == INVALID_HANDLE_VALUE) {
        return Reject(ImportErrorCode::FileUnavailable,
                       L"sidecar file could not be opened (Windows error " + std::to_wstring(GetLastError())
                           + L")");
    }
    platform::Win32Handle file(rawFile);

    DWORD requiredLength = GetFinalPathNameByHandleW(file.get(), nullptr, 0, FILE_NAME_NORMALIZED);
    if (requiredLength == 0) {
        return Reject(ImportErrorCode::FileUnavailable, L"sidecar canonical path could not be determined");
    }
    std::wstring canonicalPath(requiredLength, L'\0');
    DWORD writtenLength = GetFinalPathNameByHandleW(file.get(), canonicalPath.data(), requiredLength,
                                                      FILE_NAME_NORMALIZED);
    if (writtenLength == 0 || writtenLength >= requiredLength) {
        return Reject(ImportErrorCode::FileUnavailable, L"sidecar canonical path could not be determined");
    }
    canonicalPath.resize(writtenLength);

    // Containment to the allowed directory tree, not just the immediate
    // directory. Both sides here are GetFinalPathNameByHandleW-canonicalized
    // (FILE_NAME_NORMALIZED), so a reparse point that resolves outside the
    // allowed directory surfaces as a canonical path outside it. Compare with
    // a trailing separator -- a plain string prefix would let a sibling like
    // "C:\Foo2" satisfy "C:\Foo".
    std::wstring sidecarCanonicalPath = LowerCopy(canonicalPath);
    if (sidecarCanonicalPath.size() <= allowedDirectoryPrefix.size()
        || sidecarCanonicalPath.compare(0, allowedDirectoryPrefix.size(), allowedDirectoryPrefix) != 0) {
        return Reject(ImportErrorCode::UnsafeReference,
                       L"sidecar reference resolved outside the primary file's directory");
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0) {
        return Reject(ImportErrorCode::FileUnavailable, L"sidecar file is empty or its size could not be read");
    }
    if (static_cast<uint64_t>(size.QuadPart) > maxSidecarFileBytes) {
        return Reject(ImportErrorCode::FileUnavailable, L"sidecar file exceeds the size cap");
    }

    SidecarResolution result;
    result.file = std::move(file);
    result.canonicalPath = std::move(canonicalPath);
    result.fileSizeBytes = static_cast<uint64_t>(size.QuadPart);
    return result;
}

std::wstring DirectoryPrefix(std::filesystem::path directory)
{
    std::wstring prefix = LowerCopy(directory.wstring());
    if (prefix.empty() || prefix.back() != L'\\') {
        prefix.push_back(L'\\');
    }
    return prefix;
}

// The package root a downloaded model's sibling textures live under: the
// primary file's directory when that parent would itself be a drive root
// (searching an entire volume is not "a model package"), otherwise the parent.
std::filesystem::path PackageRootFor(const std::filesystem::path& primaryDirectory)
{
    std::filesystem::path parent = primaryDirectory.parent_path();
    if (parent.empty() || parent == parent.root_path()) {
        return primaryDirectory;
    }
    return parent;
}

// A downloaded package's texture files are often renamed from the names the
// model references: spaces become underscores, case changes, `.jpeg` becomes
// `.jpg`, and the material role is abbreviated (`..._BaseColor` -> `..._B`,
// `..._Normal` -> `..._N`). Compare on a canonical key so those renames still
// match without inventing an association: the non-role stem must be identical
// and, when both sides name a role, the roles must be equivalent. A trailing
// download annotation such as `_(Personalizado)` is ignored.
std::wstring CanonicalImageExtension(std::wstring extension)
{
    if (extension == L".jpeg" || extension == L".jpe") return L".jpg";
    if (extension == L".tiff") return L".tif";
    return extension;
}

std::wstring CanonicalImageKey(std::wstring name)
{
    std::wstring lower = LowerCopy(std::move(name));
    for (wchar_t& c : lower) {
        if (c == L' ') {
            c = L'_';
        }
    }
    const size_t dot = lower.rfind(L'.');
    std::wstring stem = dot == std::wstring::npos ? lower : lower.substr(0, dot);
    const std::wstring extension = dot == std::wstring::npos
        ? std::wstring{} : CanonicalImageExtension(lower.substr(dot));

    // Ignore a trailing site annotation only when it is the final segment
    // (`..._diffuse_(Personalizado)`), never a mid-name parenthetical such as
    // `..._Buttons-Detail(text)_B`.
    const size_t annotation = stem.rfind(L"_(");
    if (annotation != std::wstring::npos && stem.find(L'_', annotation + 2) == std::wstring::npos) {
        stem = stem.substr(0, annotation);
    }

    struct RoleAlias { const wchar_t* suffix; const wchar_t* role; };
    static const RoleAlias kRoles[] = {
        { L"_base_color", L"basecolor" }, { L"_basecolor", L"basecolor" }, { L"_base", L"basecolor" },
        { L"_diffuse", L"basecolor" }, { L"_albedo", L"basecolor" }, { L"_color", L"basecolor" },
        { L"_b", L"basecolor" },
        { L"_normal", L"normal" }, { L"_nrm", L"normal" }, { L"_nor", L"normal" }, { L"_n", L"normal" },
        { L"_roughness", L"roughness" }, { L"_rough", L"roughness" }, { L"_rgh", L"roughness" },
        { L"_r", L"roughness" },
        { L"_metallic", L"metallic" }, { L"_metalness", L"metallic" }, { L"_metal", L"metallic" },
        { L"_m", L"metallic" },
        { L"_emissive", L"emissive" }, { L"_emission", L"emissive" }, { L"_emit", L"emissive" },
        { L"_e", L"emissive" },
        { L"_glossiness", L"glossiness" }, { L"_gloss", L"glossiness" }, { L"_g", L"glossiness" },
        { L"_specular", L"specular" }, { L"_spec", L"specular" },
        { L"_occlusion", L"occlusion" }, { L"_ao", L"occlusion" },
        { L"_opacity", L"opacity" }, { L"_alpha", L"opacity" },
    };
    std::wstring role;
    for (const auto& alias : kRoles) {
        const size_t length = wcslen(alias.suffix);
        if (stem.size() > length && stem.compare(stem.size() - length, length, alias.suffix) == 0) {
            role = alias.role;
            stem.resize(stem.size() - length);
            break;
        }
    }
    return stem + L"|" + role + extension;
}

// Scan one candidate directory for an image whose canonical key matches. The
// scan is entry-bounded, skips directories and reparse points, and treats two
// files that canonicalize to the same key as ambiguous (a miss) rather than
// guessing. `.bin`, `.mtl`, and layer extensions are never considered.
std::optional<std::filesystem::path> FindNormalizedImage(const std::filesystem::path& directory,
                                                         const std::wstring& canonicalLeaf)
{
    constexpr size_t kMaxDirectoryScanEntries = 4096;
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW((directory / L"*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    std::optional<std::filesystem::path> match;
    size_t scanned = 0;
    bool ambiguous = false;
    do {
        if (++scanned > kMaxDirectoryScanEntries) {
            break;
        }
        if ((entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            continue;
        }
        const std::filesystem::path candidate = directory / entry.cFileName;
        if (!HasImageTextureExtension(candidate)) {
            continue;
        }
        if (CanonicalImageKey(entry.cFileName) != canonicalLeaf) {
            continue;
        }
        if (match && _wcsicmp(match->c_str(), candidate.c_str()) != 0) {
            ambiguous = true;
            break;
        }
        match = candidate;
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return ambiguous ? std::nullopt : match;
}

// The exact directories a downloaded package may keep textures in, in the
// priority order the viewer promises:
//
//   1. beside the model
//   2. <model dir>/texture
//   3. <model dir>/textures
//   4. <parent>/texture
//   5. <parent>/textures
//
// Each directory is checked for the exact file name first, then for a
// canonical-name match (case, space/underscore, jpg/jpeg, tif/tiff). The first
// directory that has the image wins, so the closest copy is preferred and two
// model packages in the same parent cannot collide. Only image files are
// considered, and a name containing wildcard characters is never used as a
// pattern.
std::optional<std::filesystem::path> FindPackagedImage(const std::filesystem::path& primaryDirectory,
                                                       const std::wstring& leafName)
{
    if (leafName.empty()
        || leafName.find(L'*') != std::wstring::npos
        || leafName.find(L'?') != std::wstring::npos) {
        return std::nullopt;
    }
    const std::filesystem::path parent = PackageRootFor(primaryDirectory);
    const std::filesystem::path candidates[] = {
        primaryDirectory,
        primaryDirectory / L"texture",
        primaryDirectory / L"textures",
        parent / L"texture",
        parent / L"textures",
    };
    const std::wstring canonicalLeaf = CanonicalImageKey(leafName);
    for (const auto& directory : candidates) {
        WIN32_FIND_DATAW entry{};
        HANDLE find = FindFirstFileW((directory / leafName).c_str(), &entry);
        if (find != INVALID_HANDLE_VALUE) {
            FindClose(find);
            if ((entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0) {
                const std::filesystem::path exact = directory / entry.cFileName;
                if (HasImageTextureExtension(exact)) {
                    return exact;
                }
            }
        }
        if (const auto normalized = FindNormalizedImage(directory, canonicalLeaf)) {
            return normalized;
        }
    }
    return std::nullopt;
}

// A user-chosen asset root: match the reference's leaf file name in the root
// itself and its `texture`/`textures` subfolders only. The authored relative
// path is deliberately ignored here -- the folder the user points at is the
// one that holds the assets, not a mirror of the document's directory tree.
// Images still get the canonical-name match; every other allowed sidecar
// (.bin, .mtl, USD layer) must match its exact file name.
std::optional<std::filesystem::path> FindAssetInUserRoot(const std::filesystem::path& root,
                                                         const std::wstring& leafName,
                                                         bool image)
{
    if (leafName.empty()
        || leafName.find(L'*') != std::wstring::npos
        || leafName.find(L'?') != std::wstring::npos) {
        return std::nullopt;
    }
    const std::filesystem::path candidates[] = { root, root / L"texture", root / L"textures" };
    const std::optional<std::wstring> canonicalLeaf = image
        ? std::optional<std::wstring>(CanonicalImageKey(leafName)) : std::nullopt;
    for (const auto& directory : candidates) {
        WIN32_FIND_DATAW entry{};
        HANDLE find = FindFirstFileW((directory / leafName).c_str(), &entry);
        if (find != INVALID_HANDLE_VALUE) {
            FindClose(find);
            if ((entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0) {
                const std::filesystem::path exact = directory / entry.cFileName;
                if (image ? HasImageTextureExtension(exact) : HasAllowedSidecarExtension(exact)) {
                    return exact;
                }
            }
        }
        if (canonicalLeaf) {
            if (const auto normalized = FindNormalizedImage(directory, *canonicalLeaf)) {
                return normalized;
            }
        }
    }
    return std::nullopt;
}

} // namespace

SidecarResolution ResolveSidecarPath(const std::wstring& primaryCanonicalPath,
                                      const std::string& relativeReferenceUtf8,
                                      uint64_t maxSidecarFileBytes,
                                      bool allowPackageBasenameLookup,
                                      const std::vector<std::wstring>& additionalSearchRoots)
{
    if (relativeReferenceUtf8.empty()) {
        return Reject(ImportErrorCode::UnsafeReference, L"empty sidecar reference");
    }

    // One conservative rule covering three distinct Input-boundary concerns
    // at once: a drive-relative path ("C:foo"), an alternate data stream
    // ("file:stream"), and any URI scheme ("data:", "http:", "file:", ...).
    // Stricter than the doc's letter, strictly safer than handling each
    // separately.
    if (relativeReferenceUtf8.find(':') != std::string::npos) {
        return Reject(ImportErrorCode::UnsafeReference, L"sidecar reference contains ':'");
    }

    std::wstring reference = Utf8ToWide(relativeReferenceUtf8);
    if (reference.empty()) {
        return Reject(ImportErrorCode::UnsafeReference, L"sidecar reference failed to decode as UTF-8");
    }

    // UNC/device-path prefix ("\\server\share", "\\.\", "\\?\") -- none of
    // these contain ':' immediately, so this is a separate check.
    if (reference.size() >= 2 && (reference[0] == L'\\' || reference[0] == L'/')
        && (reference[1] == L'\\' || reference[1] == L'/')) {
        return Reject(ImportErrorCode::UnsafeReference, L"sidecar reference is a UNC/device path");
    }

    std::filesystem::path referencePath(reference);
    // The primary canonical path carries the "\\?\" extended-length prefix
    // (GetFinalPathNameByHandleW's normalized form), and that prefix disables
    // Windows path normalization: a forward slash in a glTF URI
    // ("textures/foo.jpg") is not accepted by CreateFileW once joined under
    // it. The reference is already validated as relative with no "..", so
    // converting its separators to the platform's preferred form is safe.
    referencePath.make_preferred();
    if (referencePath.is_absolute() || referencePath.has_root_name() || referencePath.has_root_directory()) {
        return Reject(ImportErrorCode::UnsafeReference, L"sidecar reference is absolute/rooted");
    }
    for (const auto& component : referencePath) {
        if (component == L"..") {
            return Reject(ImportErrorCode::UnsafeReference, L"sidecar reference contains a '..' component");
        }
    }
    if (!HasAllowedSidecarExtension(referencePath)) {
        return Reject(ImportErrorCode::UnsafeReference, L"sidecar reference has a disallowed extension");
    }

    std::filesystem::path primaryDirectory = std::filesystem::path(primaryCanonicalPath).parent_path();
    std::filesystem::path joined = primaryDirectory / referencePath;

    SidecarResolution direct = AcceptCandidate(joined, DirectoryPrefix(primaryDirectory), maxSidecarFileBytes);
    if (direct.file || direct.rejectionCode != ImportErrorCode::FileUnavailable) {
        return direct;
    }

    // Package image fallback, image-only. The authored text was already checked
    // as relative, local, and free of ".." / ":" / rooting, so matching its
    // file name in a texture folder cannot expand what the reference may
    // address -- it only widens *where* a safe image name may be found. The
    // five candidate directories are the model's own, its `texture/` and
    // `textures/` subfolders, and the same under the parent package root, in
    // that order; the first exact image match wins. Required buffers, MTL
    // files and USD layers are never resolved this way.
    if (allowPackageBasenameLookup && HasImageTextureExtension(referencePath)) {
        if (const auto match = FindPackagedImage(primaryDirectory, referencePath.filename().wstring())) {
            if (SidecarResolution packaged = AcceptCandidate(*match, DirectoryPrefix(PackageRootFor(primaryDirectory)),
                                                              maxSidecarFileBytes);
                packaged.file) {
                return packaged;
            }
        }
    }

    // User-chosen asset roots, tried last. The trusted UI picked each one, but
    // every match still runs the full canonical-containment check against that
    // same root, so a reparse point inside it cannot escape -- exactly the
    // boundary the primary directory already enforces.
    if (HasAllowedSidecarExtension(referencePath)) {
        const bool image = HasImageTextureExtension(referencePath);
        const std::wstring leafName = referencePath.filename().wstring();
        for (const auto& root : additionalSearchRoots) {
            const std::filesystem::path rootPath(root);
            if (rootPath.empty()) {
                continue;
            }
            const auto match = FindAssetInUserRoot(rootPath, leafName, image);
            if (!match) {
                continue;
            }
            SidecarResolution userResolved = AcceptCandidate(*match, DirectoryPrefix(rootPath), maxSidecarFileBytes);
            if (userResolved.file) {
                return userResolved;
            }
        }
    }
    return direct;
}

} // namespace import_broker