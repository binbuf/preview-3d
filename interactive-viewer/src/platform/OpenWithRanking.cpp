#include "OpenWithRanking.h"

#include <algorithm>
#include <cwctype>
#include <unordered_map>

namespace open_with
{
namespace
{
bool IsMeshExtension(const std::wstring& extension)
{
    return extension == L".stl" || extension == L".3mf" || extension == L".obj" || extension == L".ply";
}

bool IsSceneExtension(const std::wstring& extension)
{
    return extension == L".glb" || extension == L".gltf" || extension == L".fbx";
}

bool IsUsdExtension(const std::wstring& extension)
{
    return extension == L".usd" || extension == L".usda" || extension == L".usdc" || extension == L".usdz";
}

bool IsCadExtension(const std::wstring& extension)
{
    return extension == L".step" || extension == L".stp";
}

void MergeInto(RankedHandler& target, const RankedHandler& candidate)
{
    target.recommended = target.recommended || candidate.recommended;
    if (target.iconPath.empty() && !candidate.iconPath.empty())
    {
        target.iconPath = candidate.iconPath;
        target.iconIndex = candidate.iconIndex;
    }
    const bool candidateBetter = candidate.affinity > target.affinity ||
        (candidate.affinity == target.affinity && candidate.curated && !target.curated);
    if (candidateBetter)
    {
        target.affinity = candidate.affinity;
        target.group = candidate.group;
        target.catalogId = candidate.catalogId;
        target.catalogOrder = candidate.catalogOrder;
        target.curated = candidate.curated;
    }
    else if (target.catalogId.empty() && !candidate.catalogId.empty())
    {
        target.catalogId = candidate.catalogId;
        target.group = candidate.group;
        target.catalogOrder = candidate.catalogOrder;
        target.curated = candidate.curated;
    }
    if (target.displayName.empty()) target.displayName = candidate.displayName;
}
}

std::wstring FoldOpenWithText(std::wstring value)
{
    for (auto& character : value) character = static_cast<wchar_t>(towlower(character));
    return value;
}

std::wstring FileNameOf(const std::wstring& path)
{
    const std::size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

int ExtensionAffinity(const std::wstring& extension, OpenWithGroup group)
{
    const std::wstring ext = FoldOpenWithText(extension);
    if (IsCadExtension(ext))
    {
        if (group == OpenWithGroup::Cad) return 3;
        if (group == OpenWithGroup::Modeling) return 1;
        return 0;
    }
    if (IsSceneExtension(ext))
    {
        if (group == OpenWithGroup::Modeling) return 3;
        if (group == OpenWithGroup::Cad) return 1;
        return 0;
    }
    if (IsUsdExtension(ext))
    {
        if (group == OpenWithGroup::Modeling) return 3;
        if (group == OpenWithGroup::Cad) return 2;
        return 0;
    }
    if (ext == L".stl")
    {
        if (group == OpenWithGroup::Printing) return 3;
        if (group == OpenWithGroup::Modeling) return 2;
        if (group == OpenWithGroup::Cad) return 1;
        return 0;
    }
    if (ext == L".3mf")
    {
        if (group == OpenWithGroup::Printing) return 3;
        if (group == OpenWithGroup::Cad || group == OpenWithGroup::Modeling) return 1;
        return 0;
    }
    if (ext == L".obj")
    {
        if (group == OpenWithGroup::Modeling) return 3;
        if (group == OpenWithGroup::Printing) return 2;
        if (group == OpenWithGroup::Cad) return 1;
        return 0;
    }
    if (ext == L".ply")
    {
        if (group == OpenWithGroup::Modeling) return 3;
        if (group == OpenWithGroup::Printing) return 1;
        if (group == OpenWithGroup::Cad) return 1;
        return 0;
    }
    if (IsMeshExtension(ext)) return 1;
    return 1;
}

std::vector<RankedHandler> RankHandlers(std::vector<RankedHandler> handlers, const RankOptions& options)
{
    const std::wstring selfIdentity = FoldOpenWithText(options.selfIdentity);
    const std::wstring selfFileName = FoldOpenWithText(options.selfFileName);
    auto isSelfName = [&](const std::wstring& value) {
        const std::wstring folded = FoldOpenWithText(value);
        for (const auto& name : options.selfNames)
            if (folded == FoldOpenWithText(name)) return true;
        return false;
    };

    std::vector<RankedHandler> merged;
    merged.reserve(handlers.size());
    std::unordered_map<std::wstring, std::size_t> byIdentity;
    std::unordered_map<std::wstring, std::size_t> byName;

    for (auto& handler : handlers)
    {
        const std::wstring identityKey = FoldOpenWithText(
            handler.identity.empty() ? handler.handlerName : handler.identity);
        const std::wstring fileName = FoldOpenWithText(FileNameOf(handler.handlerName));
        if ((!selfIdentity.empty() && identityKey == selfIdentity) ||
            (!selfFileName.empty() && fileName == selfFileName) ||
            isSelfName(handler.handlerName) || isSelfName(handler.displayName))
        {
            continue;
        }

        if (handler.affinity == 0 && !handler.curated) handler.affinity = 1;

        const std::wstring nameKey = FoldOpenWithText(handler.displayName.empty()
            ? identityKey : handler.displayName);

        auto identityIt = byIdentity.find(identityKey);
        std::size_t target = merged.size();
        bool found = false;
        if (identityIt != byIdentity.end())
        {
            target = identityIt->second;
            found = true;
        }
        if (!found)
        {
            auto nameIt = byName.find(nameKey);
            if (nameIt != byName.end())
            {
                target = nameIt->second;
                found = true;
            }
        }

        if (found)
        {
            MergeInto(merged[target], handler);
            byIdentity[identityKey] = target;
            byName[nameKey] = target;
            continue;
        }

        const std::size_t index = merged.size();
        merged.push_back(std::move(handler));
        byIdentity[identityKey] = index;
        byName[nameKey] = index;
    }

    std::stable_sort(merged.begin(), merged.end(), [](const RankedHandler& left, const RankedHandler& right) {
        if (left.affinity != right.affinity) return left.affinity > right.affinity;
        if (left.recommended != right.recommended) return left.recommended && !right.recommended;
        if (left.curated != right.curated) return left.curated && !right.curated;
        if (left.catalogOrder != right.catalogOrder) return left.catalogOrder < right.catalogOrder;
        const std::wstring leftName = FoldOpenWithText(left.displayName);
        const std::wstring rightName = FoldOpenWithText(right.displayName);
        if (leftName != rightName) return leftName < rightName;
        if (left.displayName != right.displayName) return left.displayName < right.displayName;
        return FoldOpenWithText(left.identity) < FoldOpenWithText(right.identity);
    });
    return merged;
}
}
