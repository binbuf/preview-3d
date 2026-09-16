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
                REQUIRE(chunk.payload.size()==sizeof(ScanSummaryPayload));
                ScanSummaryPayload summary{};
                std::memcpy(&summary,chunk.payload.data(),sizeof(summary));
                CHECK(summary.fullPayloadChecksum==d.chunkChecksum);
                CHECK(summary.fullPayloadBytes==uint64_t(d.vertexCount)*VertexStrideForLayout(VertexLayoutId(d.vertexLayoutId))
                    +uint64_t(d.indexCount)*sizeof(uint32_t));
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


TEST_CASE("Pinned worker re-decodes selected immutable source ranges repeatedly", "[detail-budget]") {
    for (const auto* name:{L"A-small-glb.glb",L"A-small-stl.stl",L"A-small-ply-mesh-le.ply",
        L"A-small-ply-mesh-be.ply",L"A-small-ply-points-le.ply",L"A-small-ply-points-be.ply",
        L"sidecar-approved.gltf",L"draco_triangle.glb"}) {
        const std::wstring path=std::wstring(PREVIEW3D_TEST_ASSETS_DIR)+L"corpus/"+name;
        CAPTURE(path);
        auto request=Request(path,path.ends_with(L".stl") ? import_broker::ImportFormat::Stl
            : path.ends_with(L".ply") ? import_broker::ImportFormat::Ply : import_broker::ImportFormat::Gltf);
        request.sectionByteCapacity=4096;
        std::map<uint32_t,ChunkDescriptor> scans;
        std::map<uint32_t,uint64_t> fineBytes;
        bool complete=false, cancelled=false; unsigned replies=0;
        uint32_t selected=0;
        request.onInitialComplete=[&](const auto&) { complete=true; REQUIRE_FALSE(scans.empty()); selected=scans.rbegin()->first; };
        request.isCancelled=[&] { return cancelled; };
        request.nextDetail=[&] { return selected; };
        request.onBatch=[&](auto&& chunks) {
            if (complete) {
                REQUIRE(chunks.size()==1); const auto& d=chunks.front().descriptor;
                CHECK(d.chunkId==selected); CHECK(d.lodLevel==kFineLod);
                CHECK(d.chunkChecksum==scans.at(selected).chunkChecksum);
                CHECK(d.sourceRangeOffset==scans.at(selected).sourceRangeOffset);
                CHECK(d.sourceRangeLength==scans.at(selected).sourceRangeLength);
                CHECK(chunks.front().payload.size()==fineBytes.at(selected));
                if (++replies==2) cancelled=true;
            } else for (const auto& chunk:chunks) if (chunk.descriptor.lodLevel==kScanLod) {
                const auto id=chunk.descriptor.chunkId & ~kScanIdentity;
                ScanSummaryPayload summary{};
                REQUIRE(chunk.payload.size()==sizeof(summary));
                std::memcpy(&summary,chunk.payload.data(),sizeof(summary));
                scans.emplace(id,chunk.descriptor); fineBytes.emplace(id,summary.fullPayloadBytes);
            }
        };
        const auto result=import_broker::RunImportSession(request);
        CAPTURE(result.stage,result.errorCode);
        REQUIRE(complete); CHECK(replies==2); CHECK(result.stage==import_broker::ImportStage::Cancelled);
    }
}

TEST_CASE("Detail requests reject ids outside the validated source catalog", "[detail-budget]") {
    auto request=Request(std::wstring(PREVIEW3D_TEST_ASSETS_DIR)+L"corpus/A-small-stl.stl",import_broker::ImportFormat::Stl);
    request.onBatch=[](auto&&) {};
    request.onInitialComplete=[](const auto&) {};
    request.nextDetail=[] { return uint32_t(0x0ffffffe); };
    const auto result=import_broker::RunImportSession(request);
    CHECK_FALSE(result.ok); CHECK(result.stage==import_broker::ImportStage::UnexpectedReply);
}


TEST_CASE("PLY detail replay preserves chunks starting inside a polygon fan", "[detail-budget]") {
    const auto path=std::filesystem::temp_directory_path()/("Preview3D-detail-fan-"+std::to_string(GetCurrentProcessId())+".ply");
    {
        std::ofstream file(path,std::ios::binary);
        file << "ply\nformat binary_little_endian 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\nelement face 100\nproperty list uchar uint vertex_indices\nend_header\n";
        const float vertices[]={0,0,0,1,0,0,1,1,0,0,1,0};
        file.write(reinterpret_cast<const char*>(vertices),sizeof(vertices));
        for (unsigned i=0;i<100;++i) { file.put(4); const uint32_t indices[]={0,1,2,3}; file.write(reinterpret_cast<const char*>(indices),sizeof(indices)); }
    }
    auto request=Request(path.wstring(),import_broker::ImportFormat::Ply); request.sectionByteCapacity=4096;
    std::map<uint32_t,ChunkDescriptor> scans; bool complete=false,cancelled=false; uint32_t selected=0;
    request.isCancelled=[&] { return cancelled; };
    request.onInitialComplete=[&](const auto&) {
        complete=true;
        for (const auto& [id,scan]:scans) if (scan.sourceElementOffset) { selected=id; break; }
        REQUIRE(selected>0);
    };
    request.nextDetail=[&] { return selected; };
    request.onBatch=[&](auto&& chunks) {
        if (complete) {
            REQUIRE(chunks.size()==1); CHECK(chunks.front().descriptor.chunkId==selected);
            CHECK(chunks.front().descriptor.chunkChecksum==scans.at(selected).chunkChecksum);
            CHECK(chunks.front().descriptor.sourceElementOffset==1); cancelled=true;
        } else for (const auto& chunk:chunks) if (chunk.descriptor.lodLevel==kScanLod)
            scans.emplace(chunk.descriptor.chunkId & ~kScanIdentity,chunk.descriptor);
    };
    const auto result=import_broker::RunImportSession(request);
    // Job close terminates the sandbox asynchronously; allow its pinned file
    // handle to close before removing the fixture this test created.
    std::error_code cleanupError;
    for (unsigned retry=0;retry<50;++retry) {
        if (std::filesystem::remove(path,cleanupError) || !std::filesystem::exists(path)) break;
        Sleep(20);
    }
    CHECK_FALSE(std::filesystem::exists(path));
    CAPTURE(result.stage,result.errorCode); CHECK(complete); CHECK(cancelled); CHECK(result.stage==import_broker::ImportStage::Cancelled);
}

TEST_CASE("Product CPU admission can refuse a worker batch without GPU allocation", "[detail-budget]") {
    auto request=Request(std::wstring(PREVIEW3D_TEST_ASSETS_DIR)+L"corpus/A-small-stl.stl",import_broker::ImportFormat::Stl);
    request.cpuBudgetAllows=[](uint64_t bytes) { CHECK(bytes>0); return false; };
    const auto result=import_broker::RunImportSession(request);
    CHECK_FALSE(result.ok); CHECK(result.errorCode==ImportErrorCode::ResourceLimit);
}
