#include "OpenWithRanking.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace
{
open_with::RankedHandler MakeHandler(std::wstring handlerName, std::wstring displayName,
    OpenWithGroup group, bool curated, int catalogOrder, bool recommended,
    std::wstring identity = {})
{
    open_with::RankedHandler handler;
    handler.extension = L".stl";
    handler.handlerName = std::move(handlerName);
    handler.displayName = std::move(displayName);
    handler.group = group;
    handler.curated = curated;
    handler.catalogOrder = catalogOrder;
    handler.recommended = recommended;
    handler.identity = identity.empty() ? handler.handlerName : std::move(identity);
    handler.affinity = curated ? open_with::ExtensionAffinity(handler.extension, group) : 1;
    return handler;
}
}

TEST_CASE("Open With ranking orders by extension affinity", "[open-with]")
{
    std::vector<open_with::RankedHandler> handlers;
    handlers.push_back(MakeHandler(L"C:\\Apps\\freecad.exe", L"FreeCAD", OpenWithGroup::Cad, true, 0, true));
    handlers.push_back(MakeHandler(L"C:\\Apps\\blender.exe", L"Blender", OpenWithGroup::Modeling, true, 6, true));
    handlers.push_back(MakeHandler(L"C:\\Apps\\prusa.exe", L"PrusaSlicer", OpenWithGroup::Printing, true, 13, true));
    handlers.push_back(MakeHandler(L"C:\\Apps\\generic.exe", L"Generic Viewer", OpenWithGroup::Recommended, false, INT_MAX, true));

    const auto ranked = open_with::RankHandlers(std::move(handlers), {});
    REQUIRE(ranked.size() == 4);
    CHECK(ranked[0].displayName == L"PrusaSlicer");
    CHECK(ranked[1].displayName == L"Blender");
    // At affinity 1 both are recommended, so curated membership breaks the
    // tie before display name.
    CHECK(ranked[2].displayName == L"FreeCAD");
    CHECK(ranked[3].displayName == L"Generic Viewer");
}

TEST_CASE("Open With ranking removes every viewer build by file name", "[open-with]")
{
    std::vector<open_with::RankedHandler> handlers;
    handlers.push_back(MakeHandler(L"D:\\build\\Preview3D.exe", L"3D Preview", OpenWithGroup::Recommended, false, INT_MAX, true));
    handlers.push_back(MakeHandler(L"C:\\Program Files\\Binbuf\\Preview 3D\\Preview3D.exe", L"Preview 3D", OpenWithGroup::Recommended, false, INT_MAX, true));
    handlers.push_back(MakeHandler(L"C:\\Apps\\f3d.exe", L"F3D", OpenWithGroup::Recommended, false, INT_MAX, true));

    open_with::RankOptions options;
    options.selfFileName = L"preview3d.exe";
    const auto ranked = open_with::RankHandlers(std::move(handlers), options);
    REQUIRE(ranked.size() == 1);
    CHECK(ranked[0].displayName == L"F3D");
}

TEST_CASE("Open With ranking removes package-style viewer handlers by product name", "[open-with]")
{
    std::vector<open_with::RankedHandler> handlers;
    handlers.push_back(MakeHandler(L"Preview 3D", L"Preview 3D", OpenWithGroup::Recommended, false, INT_MAX, true));
    handlers.push_back(MakeHandler(L"3D Preview", L"3D Preview", OpenWithGroup::Recommended, false, INT_MAX, true));
    handlers.push_back(MakeHandler(L"C:\\Apps\\f3d.exe", L"F3D", OpenWithGroup::Recommended, false, INT_MAX, true));

    open_with::RankOptions options;
    options.selfNames = { L"Preview 3D", L"3D Preview" };
    const auto ranked = open_with::RankHandlers(std::move(handlers), options);
    REQUIRE(ranked.size() == 1);
    CHECK(ranked[0].displayName == L"F3D");
}

TEST_CASE("Open With ranking dedupes executable paths and display names", "[open-with]")
{
    std::vector<open_with::RankedHandler> handlers;
    auto first = MakeHandler(L"C:\\Apps\\blender.exe", L"Blender", OpenWithGroup::Modeling, true, 6, false,
        L"c:\\apps\\blender.exe");
    auto second = MakeHandler(L"C:\\Program Files\\Blender\\blender.exe", L"Blender", OpenWithGroup::Modeling, true, 6, true,
        L"c:\\apps\\blender.exe");
    handlers.push_back(first);
    handlers.push_back(second);
    handlers.push_back(MakeHandler(L"C:\\Apps\\f3d.exe", L"F3D", OpenWithGroup::Recommended, false, INT_MAX, true));

    const auto ranked = open_with::RankHandlers(std::move(handlers), {});
    REQUIRE(ranked.size() == 2);
    CHECK(ranked[0].displayName == L"Blender");
    CHECK(ranked[0].recommended);
    CHECK(ranked[1].displayName == L"F3D");
}

TEST_CASE("Open With ranking is deterministic for duplicate labels", "[open-with]")
{
    std::vector<open_with::RankedHandler> handlers;
    handlers.push_back(MakeHandler(L"C:\\B\\viewer.exe", L"Viewer", OpenWithGroup::Recommended, false, INT_MAX, true,
        L"c:\\b\\viewer.exe"));
    handlers.push_back(MakeHandler(L"C:\\A\\viewer.exe", L"Viewer", OpenWithGroup::Recommended, false, INT_MAX, true,
        L"c:\\a\\viewer.exe"));

    const auto ranked = open_with::RankHandlers(std::move(handlers), {});
    REQUIRE(ranked.size() == 1); // same display name dedupe
    CHECK(ranked[0].identity == L"c:\\b\\viewer.exe");
}

TEST_CASE("Open With affinity table covers the supported families", "[open-with]")
{
    CHECK(open_with::ExtensionAffinity(L".stl", OpenWithGroup::Printing) == 3);
    CHECK(open_with::ExtensionAffinity(L".stl", OpenWithGroup::Modeling) == 2);
    CHECK(open_with::ExtensionAffinity(L".obj", OpenWithGroup::Modeling) == 3);
    CHECK(open_with::ExtensionAffinity(L".fbx", OpenWithGroup::Modeling) == 3);
    CHECK(open_with::ExtensionAffinity(L".step", OpenWithGroup::Cad) == 3);
    CHECK(open_with::ExtensionAffinity(L".usdz", OpenWithGroup::Cad) == 2);
    CHECK(open_with::ExtensionAffinity(L".unknown", OpenWithGroup::Recommended) == 1);
}
