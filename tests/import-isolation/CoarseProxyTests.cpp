#define NOMINMAX
#include "SandboxTestSupport.h"
#include "import_broker/ImportSession.h"
#include "import_broker/SharedSection.h"
#include "model_core/TierALimits.h"
#include "model_core/VertexLayouts.h"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

namespace {
using namespace model_core;
import_broker::ImportSessionRequest Request(std::wstring path, import_broker::ImportFormat format) {
    import_broker::ImportSessionRequest r; r.enableCoarseProxy=true;
    r.workerExePath=sandbox_test_support::WorkerExePath(); r.sourcePath=std::move(path); r.format=format;
    r.generationId=206; r.sectionByteCapacity=16ull*1024*1024; r.maxChunkCount=1024;
    r.maxChunksPerGeneration=kTierACatalogLimit; r.maxChunkBatchesPerGeneration=kTierABatchLimit;
    r.maxSidecarRequestsPerGeneration=128; r.maxSidecarFileBytes=kTierAPrimarySourceBytes;
    return r;
}
void Verify(import_broker::ImportSessionRequest request, uint64_t expected, bool points=false) {
    std::map<uint32_t,ChunkDescriptor> scans;
    std::set<uint32_t> coarse, fine;
    std::set<unsigned> components;
    uint64_t sampled=0, bytes=0, valid=0;
    bool complete=false;
    bool firstCoarse=true;
    bool firstPreview=true;
    request.onBatch=[&](std::vector<import_broker::ValidatedChunk>&& chunks) {
        std::set<unsigned> firstComponents;
        std::set<unsigned> previewComponents;
        bool coarseInBatch=false;
        for (const auto& chunk:chunks) {
            const auto& d=chunk.descriptor;
            if (d.topology==ChunkTopology::CoarseComplete) {
                REQUIRE_FALSE(complete); complete=true;
                CHECK(scans.size()==coarse.size()); CHECK(valid==expected); CHECK(components.size()==8);
                CHECK(sampled<=CoarsePrimitiveCap(valid,scans.size())); CHECK(bytes<=kCoarseReservedBytes);
                continue;
            }
            if (d.topology!=ChunkTopology::TriangleList && d.topology!=ChunkTopology::PointList) continue;
            const uint32_t identity=d.chunkId&0x0fffffffu;
            if (d.lodLevel==kPreviewLod) {
                CHECK_FALSE(complete); CHECK(scans.empty());
                const size_t stride=VertexStrideForLayout(VertexLayoutId(d.vertexLayoutId));
                for (uint32_t vertex=0;vertex<d.vertexCount;++vertex) {
                    float p[3]; std::memcpy(p,chunk.payload.data()+size_t(vertex)*stride,12);
                    previewComponents.insert(unsigned(d.origin[0]+p[0]>5) | (unsigned(d.origin[1]+p[1]>5)<<1)
                        | (unsigned(d.origin[2]+p[2]>5)<<2));
                }
                continue;
            }
            if (d.lodLevel==kScanLod) {
                CHECK_FALSE(complete); REQUIRE(scans.emplace(identity,d).second);
                valid+=points ? d.vertexCount : d.indexCount/3;
            } else if (d.lodLevel==kCoarseLod) {
                coarseInBatch=true;
                CHECK_FALSE(complete); REQUIRE(scans.contains(identity)); REQUIRE(coarse.insert(identity).second);
                sampled+=points ? d.vertexCount : d.indexCount/3; bytes+=d.byteSize;
                const size_t stride=VertexStrideForLayout(VertexLayoutId(d.vertexLayoutId));
                for (uint32_t vertex=0;vertex<d.vertexCount;++vertex) {
                    float p[3]; std::memcpy(p,chunk.payload.data()+size_t(vertex)*stride,12);
                    const unsigned component=unsigned(d.origin[0]+p[0]>5) | (unsigned(d.origin[1]+p[1]>5)<<1)
                                      | (unsigned(d.origin[2]+p[2]>5)<<2);
                    components.insert(component); firstComponents.insert(component);
                }
            } else {
                CHECK(complete); REQUIRE(coarse.contains(identity)); REQUIRE(fine.insert(identity).second);
                CHECK(d.chunkChecksum==scans.at(identity).chunkChecksum);
            }
        }
        if (coarseInBatch && firstCoarse) { CHECK(firstComponents.size()==8); firstCoarse=false; }
        if (!previewComponents.empty() && firstPreview) { CHECK(previewComponents.size()==8); firstPreview=false; }
    };
    const auto result=import_broker::RunImportSession(request);
    CAPTURE(result.stage,result.errorCode);
    REQUIRE(result.ok); REQUIRE(complete); CHECK(coarse==fine);
    CHECK(result.chunks.empty()); CHECK(result.sourceCatalog.size()==scans.size());
}
}

TEST_CASE("Product coarse catalogs cover every manifest component before fine delivery", "[coarse-proxy]") {
    for (const auto* name:{L"A-small-glb.glb",L"A-small-stl.stl",L"A-small-ply-mesh-le.ply",
                          L"A-small-ply-mesh-be.ply",L"A-small-ply-points-le.ply",L"A-small-ply-points-be.ply"}) {
        const std::wstring path=std::wstring(PREVIEW3D_TEST_ASSETS_DIR)+L"corpus/"+name;
        const auto format=path.ends_with(L".glb") ? import_broker::ImportFormat::Gltf
            : path.ends_with(L".stl") ? import_broker::ImportFormat::Stl : import_broker::ImportFormat::Ply;
        CAPTURE(path); Verify(Request(path,format),240,path.find(L"points")!=std::wstring::npos);
    }
}

TEST_CASE("Cancellation before coarse handoff abandons scans and permits a later valid request", "[coarse-proxy]") {
    auto request=Request(std::wstring(PREVIEW3D_TEST_ASSETS_DIR)+L"corpus/A-small-stl.stl",import_broker::ImportFormat::Stl);
    request.sectionByteCapacity=4096; bool cancelled=false;
    request.isCancelled=[&] { return cancelled; };
    request.onBatch=[&](auto&& chunks) {
        REQUIRE_FALSE(chunks.empty()); CHECK(chunks.front().descriptor.lodLevel==kPreviewLod); cancelled=true;
    };
    auto result=import_broker::RunImportSession(request); CHECK_FALSE(result.ok);
    CHECK(result.stage==import_broker::ImportStage::Cancelled);
    Verify(Request(std::wstring(PREVIEW3D_TEST_ASSETS_DIR)+L"corpus/A-small-stl.stl",import_broker::ImportFormat::Stl),240);
}

TEST_CASE("Draw-heavy retains every single-triangle occurrence under the mandatory coverage floor", "[coarse-proxy]") {
    auto request=Request(std::wstring(PREVIEW3D_TEST_ASSETS_DIR)+L"corpus/Draw-heavy.glb",import_broker::ImportFormat::Gltf);
    std::set<uint32_t> scans,coarse,fine;
    uint64_t primitives=0,bytes=0; bool complete=false;
    request.onBatch=[&](auto&& chunks) {
        for (const auto& chunk:chunks) {
            const auto& d=chunk.descriptor;
            if (d.topology==ChunkTopology::CoarseComplete) { complete=true; CHECK(coarse==scans); continue; }
            if (d.topology!=ChunkTopology::TriangleList || d.lodLevel==kPreviewLod) continue;
            const auto id=d.chunkId&0x0fffffffu;
            if (d.lodLevel==kScanLod) scans.insert(id);
            else if (d.lodLevel==kCoarseLod) { coarse.insert(id); primitives+=d.indexCount/3; bytes+=d.byteSize; }
            else { CHECK(complete); fine.insert(id); }
        }
    };
    const auto result=import_broker::RunImportSession(request);
    REQUIRE(result.ok); CHECK(complete); CHECK(scans.size()==2048); CHECK(coarse==fine);
    CHECK(primitives==2048); CHECK(primitives==CoarsePrimitiveCap(2048,scans.size()));
    CHECK(bytes<=kCoarseReservedBytes);
}

TEST_CASE("Reordered qualification proxies retain component usefulness", "[.][coarse-reordered]") {
    wchar_t root[32768]{};
    const DWORD length=GetEnvironmentVariableW(L"PREVIEW3D_TSK206_FIXTURES",root,32768);
    REQUIRE(length>0); REQUIRE(length<32768);
    for (const auto& entry:std::filesystem::directory_iterator(root)) {
        const auto path=entry.path().wstring(); if (path.ends_with(L".json")) continue;
        const auto format=path.ends_with(L".glb") ? import_broker::ImportFormat::Gltf
            : path.ends_with(L".stl") ? import_broker::ImportFormat::Stl : import_broker::ImportFormat::Ply;
        CAPTURE(path); Verify(Request(path,format),100000,path.find(L"points")!=std::wstring::npos);
    }
}

TEST_CASE("Hostile coarse catalogs cannot hand off incomplete or inconsistent representations", "[coarse-proxy][hostile-worker]") {
    for (const auto* mode:{L"--coarse-premature",L"--coarse-missing-region",L"--coarse-duplicate",L"--coarse-changed-fine",
                          L"--coarse-outside-bounds",L"--coarse-unknown-role",L"--coarse-false-totals",L"--coarse-terminal-scan",L"--coarse-missing-fine",L"--coarse-mixed-preview"}) {
        auto request=Request(std::wstring(PREVIEW3D_TEST_ASSETS_DIR)+L"tri_tight.glb",import_broker::ImportFormat::Gltf);
        request.workerExePath=sandbox_test_support::HostileWorkerExePath(); request.workerArgumentsOverride=mode;
        request.onBatch=[](auto&&) {};
        CAPTURE(mode); auto result=import_broker::RunImportSession(request);
        CHECK_FALSE(result.ok); CHECK(result.errorCode==ImportErrorCode::MalformedData);
    }
}

TEST_CASE("Multi-GiB sources emit scene-wide preview geometry before full normalization", "[.][coarse-large-preview]") {
    wchar_t root[32768]{}; REQUIRE(GetEnvironmentVariableW(L"PREVIEW3D_TSK205_FIXTURES",root,32768)>0);
    for (const auto* format:{L"glb",L"stl",L"ply-mesh-le",L"ply-mesh-be",L"ply-points-le",L"ply-points-be"}) {
        const std::wstring name=L"A-large-"+std::wstring(format)+(std::wstring(format).starts_with(L"ply") ? L".ply" : L"."+std::wstring(format));
        const auto path=std::filesystem::path(root)/name; REQUIRE(std::filesystem::file_size(path)>2ull*1024*1024*1024);
        auto request=Request(path.wstring(),std::wstring(format)==L"glb" ? import_broker::ImportFormat::Gltf
            : std::wstring(format)==L"stl" ? import_broker::ImportFormat::Stl : import_broker::ImportFormat::Ply);
        bool cancelled=false; std::set<unsigned> components; uint64_t primitives=0,bytes=0;
        request.isCancelled=[&] { return cancelled; };
        const auto start=GetTickCount64();
        request.onBatch=[&](auto&& chunks) {
            for (const auto& chunk:chunks) {
                const auto& d=chunk.descriptor; REQUIRE(d.lodLevel==kPreviewLod);
                primitives+=d.topology==ChunkTopology::PointList ? d.vertexCount : d.indexCount/3;
                bytes+=d.byteSize;
                const auto stride=VertexStrideForLayout(VertexLayoutId(d.vertexLayoutId));
                for (uint32_t vertex=0;vertex<d.vertexCount;++vertex) {
                    float p[3]; std::memcpy(p,chunk.payload.data()+size_t(vertex)*stride,12);
                    components.insert(unsigned(d.origin[0]+p[0]>5) | (unsigned(d.origin[1]+p[1]>5)<<1)
                        | (unsigned(d.origin[2]+p[2]>5)<<2));
                }
            }
            CHECK(components.size()==8); CHECK(primitives<=kPreviewPrimitiveLimit); CHECK(bytes<=kPreviewByteLimit); cancelled=true;
        };
        CAPTURE(name); auto result=import_broker::RunImportSession(request);
        CHECK_FALSE(result.ok); CHECK(result.stage==import_broker::ImportStage::Cancelled);
        std::string asciiName; for (wchar_t character:name) asciiName.push_back(static_cast<char>(character));
        std::cout << "coarse-large-preview " << asciiName << ": " << primitives
                  << " primitives " << bytes << " bytes first=" << GetTickCount64()-start << "ms\n";
    }
}
