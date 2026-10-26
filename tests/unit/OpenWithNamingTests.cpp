#include "OpenWithNaming.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Open With naming cleans raw executable labels", "[open-with]")
{
    CHECK(open_with::NormalizeDisplayName(L"f3d.exe", L"C:\\Program Files\\F3D\\bin\\f3d.exe", L"", L"") == L"F3D");
    CHECK(open_with::NormalizeDisplayName(L"bambu-studio.exe", L"C:\\Program Files\\Bambu Studio\\bambu-studio.exe", L"", L"") == L"Bambu Studio");
    CHECK(open_with::NormalizeDisplayName(L"meshlab.exe", L"C:\\Program Files\\VCG\\MeshLab\\meshlab.exe", L"", L"") == L"Meshlab");
}

TEST_CASE("Open With naming prefers version-resource descriptions", "[open-with]")
{
    CHECK(open_with::NormalizeDisplayName(L"meshlab.exe",
        L"C:\\Program Files\\VCG\\MeshLab\\meshlab.exe", L"MeshLab", L"MeshLab") == L"MeshLab");
    CHECK(open_with::NormalizeDisplayName(L"app.exe",
        L"C:\\Apps\\app.exe", L"", L"Friendly Product") == L"Friendly Product");
}

TEST_CASE("Open With naming leaves friendly labels alone", "[open-with]")
{
    CHECK(open_with::NormalizeDisplayName(L"BambuStudio", L"C:\\Program Files\\Bambu Studio\\bambu-studio.exe", L"", L"") == L"BambuStudio");
    CHECK(open_with::NormalizeDisplayName(L"Babylon.js Sandbox", L"C:\\Apps\\Babylon.js Sandbox.exe", L"", L"") == L"Babylon.js Sandbox");
    CHECK(open_with::NormalizeDisplayName(L"3D Viewer", L"C:\\Apps\\3DViewer.exe", L"", L"") == L"3D Viewer");
}

TEST_CASE("Open With naming sanitizes and bounds fallback labels", "[open-with]")
{
    const std::wstring label = open_with::NormalizeDisplayName(L"\x01\x02.exe", L"C:\\Apps\\tool.exe", L"", L"");
    CHECK(label == L"Tool");
    CHECK(open_with::FriendlyFileStem(L"C:\\Apps\\my-model_v2.exe") == L"My Model V2");
    CHECK(open_with::LooksLikeFileName(L"f3d.exe", L"C:\\Program Files\\F3D\\bin\\f3d.exe"));
    CHECK_FALSE(open_with::LooksLikeFileName(L"BambuStudio", L"C:\\Program Files\\Bambu Studio\\bambu-studio.exe"));
}
