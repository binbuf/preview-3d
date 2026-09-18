#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace model_core {

inline constexpr std::string_view kOpenUsdIdentifierPrefix = "preview3d://";
inline constexpr std::size_t kMaxOpenUsdIdentifierBytes = 192;

// Pure identifier policy shared by the compatibility resolver and its
// standalone fuzz target. This grants no path authority: accepted identifiers
// still resolve only through the compatibility host's bounded broker store.
inline bool IsSafeOpenUsdIdentifier(std::string_view identifier)
{
    if (!identifier.starts_with(kOpenUsdIdentifierPrefix)
        || identifier.size() >= kMaxOpenUsdIdentifierBytes
        || identifier.find('\\') != identifier.npos
        || identifier.find(':', kOpenUsdIdentifierPrefix.size()) != identifier.npos
        || identifier.ends_with('/')) return false;
    for (const char value : identifier) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte < 0x20u || byte == 0x7fu) return false;
    }
    for (std::size_t start = kOpenUsdIdentifierPrefix.size(); start < identifier.size();) {
        const auto slash = identifier.find('/', start);
        const auto part = identifier.substr(
            start, slash == identifier.npos ? identifier.size() - start : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = slash == identifier.npos ? identifier.size() : slash + 1;
    }
    return true;
}

inline std::optional<std::string> AnchorOpenUsdIdentifier(
    std::string_view asset, std::string_view anchor)
{
    if (asset.empty() || asset.find('\\') != asset.npos
        || (!asset.starts_with(kOpenUsdIdentifierPrefix) && asset.find(':') != asset.npos)
        || asset.starts_with('/')) return std::nullopt;
    std::string candidate;
    if (asset.starts_with(kOpenUsdIdentifierPrefix)) candidate.assign(asset);
    else {
        // OpenUSD supplies an empty anchor for composition arcs originating
        // in a layer backed only by ArAsset bytes. Treat it as the brokered
        // namespace root; a non-empty anchor remains directory-relative.
        if (anchor.empty()) candidate.assign(kOpenUsdIdentifierPrefix);
        else {
            if (!IsSafeOpenUsdIdentifier(anchor)) return std::nullopt;
            candidate.assign(anchor.substr(0, anchor.find_last_of('/') + 1));
        }
        candidate.append(asset);
    }
    std::vector<std::string> parts;
    for (std::size_t start = kOpenUsdIdentifierPrefix.size(); start <= candidate.size();) {
        const auto slash = candidate.find('/', start);
        const auto part = candidate.substr(
            start, slash == candidate.npos ? candidate.size() - start : slash - start);
        if (part == "..") return std::nullopt;
        if (!part.empty() && part != ".") parts.emplace_back(part);
        if (slash == candidate.npos) break;
        start = slash + 1;
    }
    std::string result(kOpenUsdIdentifierPrefix);
    for (const auto& part : parts) {
        if (result.size() > kOpenUsdIdentifierPrefix.size()) result += '/';
        result += part;
    }
    return IsSafeOpenUsdIdentifier(result)
        ? std::optional<std::string>(std::move(result)) : std::nullopt;
}

} // namespace model_core
