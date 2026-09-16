#include "CoarseSampler.h"
#include "D3D12ViewerPath.h"
#include <catch2/catch_test_macros.hpp>
#include <array>

TEST_CASE("Spatial coarse sampling preserves eight components, boundaries and attributes after reorder", "[coarse-proxy]") {
    using namespace model_core;
    std::vector<VertexPositionOnlyF32> vertices;
    for (unsigned component=0;component<8;++component) for (unsigned repeat=0;repeat<30;++repeat)
        vertices.push_back({float(component&1)*10,float((component>>1)&1)*10,float((component>>2)&1)*10});
    ChunkDescriptor d{}; d.chunkId=1; d.topology=ChunkTopology::PointList;
    d.vertexLayoutId=uint32_t(VertexLayoutId::PositionOnly_F32); d.vertexCount=uint32_t(vertices.size());
    REQUIRE(SetLocalBounds(d,std::as_bytes(std::span(vertices))));
    auto check=[&] {
        auto sample=import_worker::SampleCoarse(d,std::as_bytes(std::span(vertices)),{},12);
        CHECK(sample.Primitives()==12);
        std::array<bool,8> covered{};
        for (size_t at=0;at<sample.vertices.size();at+=12) {
            float position[3]; std::memcpy(position,sample.vertices.data()+at,12);
            covered[unsigned(position[0]>5) | (unsigned(position[1]>5)<<1) | (unsigned(position[2]>5)<<2)]=true;
        }
        for (bool component:covered) CHECK(component);
        CHECK(sample.descriptor.chunkId==(kCoarseIdentity|1));
        CHECK(sample.descriptor.lodLevel==kCoarseLod);
        for (unsigned axis=0;axis<3;++axis) { CHECK(sample.descriptor.localMin[axis]==0); CHECK(sample.descriptor.localMax[axis]==10); }
    };
    check(); std::reverse(vertices.begin(),vertices.end()); check();
    for (uint64_t count: {0ull,1ull,19ull,20ull,240ull,100000000ull})
        CHECK(CoarsePrimitiveCap(count)==(count<20 ? count : (std::min)(2000000ull,count/20)));
}

TEST_CASE("A fine publication hides exactly its coarse parent and restores it on eviction", "[coarse-proxy]") {
    using namespace model_core;
    D3D12ViewerPath path;
    auto mesh=[](uint32_t id,uint32_t lod) { D3D12ViewerPath::GpuMesh m; m.chunkId=id; m.sourceGeometry.lodLevel=lod; return m; };
    path.model.meshes.push_back(mesh(kCoarseIdentity|1,kCoarseLod));
    path.model.meshes.push_back(mesh(kCoarseIdentity|2,kCoarseLod));
    D3D12ViewerPath::UpdateCoarseVisibility(path.model);
    CHECK(path.model.meshes[0].drawEnabled); CHECK(path.model.meshes[1].drawEnabled);
    path.model.meshes.push_back(mesh(1,kFineLod));
    D3D12ViewerPath::UpdateCoarseVisibility(path.model);
    CHECK_FALSE(path.model.meshes[0].drawEnabled); CHECK(path.model.meshes[1].drawEnabled); CHECK(path.model.meshes[2].drawEnabled);
    path.frames[0].fenceValue=42;
    const std::array<uint32_t,1> evict{1}; path.EvictFineChunks(evict);
    REQUIRE(path.model.meshes.size()==2); CHECK(path.model.meshes[0].drawEnabled); CHECK(path.model.meshes[1].drawEnabled);
    REQUIRE(path.retiredModels.size()==1); CHECK(path.retiredModels[0].directFenceValue==42);
    CHECK(path.retiredModels[0].copyFenceValue==0);
    path.model.meshes.push_back(mesh(1,kFineLod));
    D3D12ViewerPath::UpdateCoarseVisibility(path.model); CHECK_FALSE(path.model.meshes[0].drawEnabled);
    CHECK(path.model.meshes[1].drawEnabled);
}
