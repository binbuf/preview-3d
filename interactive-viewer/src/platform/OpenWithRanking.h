#pragma once

#include <climits>
#include <string>
#include <vector>

enum class OpenWithGroup
{
    Cad,
    Modeling,
    Printing,
    Recommended,
    Status,
    Fallback,
};

namespace open_with
{
// One discoverable handler after naming and catalog matching, before the
// self-filter/dedupe/rank pass. `identity` is the caller's canonical handler
// key (normally a case-folded absolute path); ranking is pure and never
// touches the filesystem or COM.
struct RankedHandler
{
    std::wstring extension;
    std::wstring handlerName;
    std::wstring displayName;
    std::wstring catalogId;
    std::wstring identity;
    std::wstring iconPath;
    int iconIndex = 0;
    int catalogOrder = INT_MAX;
    bool recommended = false;
    bool curated = false;
    OpenWithGroup group = OpenWithGroup::Recommended;
    int affinity = 1;
};

struct RankOptions
{
    std::wstring selfIdentity;
    std::wstring selfFileName;
    // Product names that also identify this viewer when a shell handler
    // exposes a package/ProgID name instead of an executable path.
    std::vector<std::wstring> selfNames;
};

// Relevance of a curated app category to an extension. Non-curated handlers
// use affinity 1; the caller leaves `group` as Recommended for them.
int ExtensionAffinity(const std::wstring& extension, OpenWithGroup group);

// Removes the viewer's own handlers, collapses duplicate identities and
// duplicate visible names, and returns the stable relevance order described in
// docs/open-with-popup-proposal.md.
std::vector<RankedHandler> RankHandlers(std::vector<RankedHandler> handlers, const RankOptions& options);

std::wstring FoldOpenWithText(std::wstring value);
std::wstring FileNameOf(const std::wstring& path);
}
