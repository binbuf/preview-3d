#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <catch2/catch_test_macros.hpp>
#include "../../interactive-viewer/test-assets/corpus/Expectations.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {
struct HashHandles {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
    ~HashHandles() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0); }
};
}

TEST_CASE("Every immutable routine fixture matches its manifest SHA-256", "[fixtures]")
{
    struct File { const wchar_t* path; const char* sha256; };
    std::vector<File> files;
    for (const auto& fixture:fixture_manifest::files) files.push_back({fixture.path,fixture.sha256});
    files.push_back({L"basisu_sample.ktx2","fa87151224822c1464dc503ef1afe49868fc6a1363a13089f622951b2f54646c"});
    files.push_back({L"textures/gray.jpg","e8feecbf5e2bb99d2ef7b0f4be28b486456140ea82e8a205e0ff7dba2fe9b168"});
    files.push_back({L"textures/gray.png","52a3910db5af63aaa73083768b7c0949568c88900818080f085718cbe6e57a3e"});
    for (const auto& fixture:files) {
        CAPTURE(fixture.sha256);
        HashHandles handles;
        REQUIRE(BCryptOpenAlgorithmProvider(&handles.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0) == 0);
        DWORD objectBytes=0,returned=0;
        REQUIRE(BCryptGetProperty(handles.algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectBytes),
                                 sizeof(objectBytes),&returned,0) == 0);
        handles.object.resize(objectBytes);
        REQUIRE(BCryptCreateHash(handles.algorithm,&handles.hash,handles.object.data(),objectBytes,nullptr,0,0) == 0);
        const auto path = std::filesystem::path(PREVIEW3D_TEST_ASSETS_DIR) / fixture.path;
        std::ifstream source(path,std::ios::binary);
        REQUIRE(source.is_open());
        std::array<unsigned char,65536> block{};
        while (source.read(reinterpret_cast<char*>(block.data()),block.size()) || source.gcount()) {
            REQUIRE(BCryptHashData(handles.hash,block.data(),static_cast<ULONG>(source.gcount()),0) == 0);
        }
        REQUIRE(source.eof());
        std::array<unsigned char,32> digest{};
        REQUIRE(BCryptFinishHash(handles.hash,digest.data(),static_cast<ULONG>(digest.size()),0) == 0);
        std::string hex;
        for (auto value : digest) { hex += "0123456789abcdef"[value>>4]; hex += "0123456789abcdef"[value&15]; }
        CHECK(hex == fixture.sha256);
    }
}
