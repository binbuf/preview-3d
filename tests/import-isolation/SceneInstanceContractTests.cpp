#include "import_broker/SharedSectionValidator.h"
#include "model_core/Checksum.h"
#include "model_core/GeometryBounds.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace {
using namespace model_core;
constexpr uint64_t kGeneration = 0x1020304050607080ull;

struct Spec { ChunkDescriptor descriptor{}; std::vector<std::byte> payload; };

template<class T> std::vector<std::byte> Bytes(const T& value)
{
    const auto bytes=std::as_bytes(std::span(&value,1));
    return {bytes.begin(),bytes.end()};
}

Spec Geometry()
{
    const std::array<VertexPositionOnlyF32,3> vertices{{{0,0,0},{2,0,0},{0,1,0}}};
    const std::array<uint32_t,3> indices{{0,1,2}};
    Spec result;
    result.descriptor.topology=ChunkTopology::TriangleList;
    result.descriptor.chunkId=1;result.descriptor.vertexCount=3;result.descriptor.indexCount=3;
    result.descriptor.vertexLayoutId=uint32_t(VertexLayoutId::PositionOnly_F32);
    result.descriptor.boundsState=BoundsState::Verified;result.descriptor.meshId=1;
    result.descriptor.origin[0]=1.0e12;result.descriptor.origin[1]=-2.0e12;result.descriptor.origin[2]=3.0e12;
    const auto vb=std::as_bytes(std::span(vertices));const auto ib=std::as_bytes(std::span(indices));
    result.payload.assign(vb.begin(),vb.end());result.payload.insert(result.payload.end(),ib.begin(),ib.end());
    REQUIRE(SetLocalBounds(result.descriptor,std::span(result.payload).first(vb.size())));
    return result;
}

NodePayload Node(uint32_t id,uint32_t parent,double x,bool mirrored=false)
{
    NodePayload node{};node.nodeId=id;node.parentNodeId=parent;node.flags=kSceneRecordVisible;
    node.localTransform[0]=mirrored?-1.0:1.0;node.localTransform[5]=node.localTransform[10]=node.localTransform[15]=1;
    node.localTransform[12]=x;
    return node;
}

Spec NodeSpec(const NodePayload& node)
{
    Spec result;result.descriptor.topology=ChunkTopology::Node;result.descriptor.chunkId=node.nodeId;
    if(node.parentNodeId){result.descriptor.dependencyIds[0]=node.parentNodeId;result.descriptor.dependencyCount=1;}
    result.payload=Bytes(node);return result;
}

MeshInstancePayload Instance(uint32_t id,uint32_t nodeId,const NodePayload& node)
{
    MeshInstancePayload instance{};instance.instanceId=id;instance.nodeId=nodeId;
    instance.geometryChunkId=1;instance.flags=kSceneRecordVisible;
    const double x0=1.0e12,x1=1.0e12+2.0,y0=-2.0e12,y1=-2.0e12+1.0,z=3.0e12;
    if(node.localTransform[0]<0){instance.worldMin[0]=-x1+node.localTransform[12];instance.worldMax[0]=-x0+node.localTransform[12];}
    else {instance.worldMin[0]=x0+node.localTransform[12];instance.worldMax[0]=x1+node.localTransform[12];}
    instance.worldMin[1]=y0;instance.worldMax[1]=y1;instance.worldMin[2]=instance.worldMax[2]=z;
    return instance;
}

Spec InstanceSpec(const MeshInstancePayload& instance)
{
    Spec result;result.descriptor.topology=ChunkTopology::MeshInstance;result.descriptor.chunkId=instance.instanceId;
    result.descriptor.dependencyIds[0]=instance.geometryChunkId;
    result.descriptor.dependencyIds[2]=instance.nodeId;result.descriptor.dependencyCount=2;
    result.payload=Bytes(instance);return result;
}

std::vector<std::byte> Section(std::vector<Spec> specs,uint32_t nodeCount,uint32_t version=kCurrentProtocolVersion)
{
    uint64_t offset=kSectionHeaderSize+specs.size()*kChunkDescriptorSize;
    for(auto& spec:specs){spec.descriptor.normalizedRangeOffset=offset;spec.descriptor.byteSize=spec.payload.size();
        spec.descriptor.normalizedRangeLength=spec.payload.size();spec.descriptor.chunkChecksum=WireChecksum64(spec.payload);offset+=spec.payload.size();}
    std::vector<std::byte> bytes(static_cast<size_t>(offset));
    for(size_t i=0;i<specs.size();++i){std::memcpy(bytes.data()+kSectionHeaderSize+i*kChunkDescriptorSize,&specs[i].descriptor,sizeof(ChunkDescriptor));
        std::memcpy(bytes.data()+specs[i].descriptor.normalizedRangeOffset,specs[i].payload.data(),specs[i].payload.size());}
    SectionHeader header{};header.magic=kSectionMagic;header.protocolVersion=version;header.generationId=kGeneration;
    header.sectionLength=bytes.size();header.chunkCount=uint32_t(specs.size());header.scene.generationId=kGeneration;
    header.scene.meshCount=1;header.scene.nodeCount=nodeCount;
    header.sectionChecksum=WireChecksum64(std::span(bytes).subspan(kSectionHeaderSize));
    std::memcpy(bytes.data(),&header,sizeof(header));return bytes;
}

std::vector<Spec> ManyInstances(uint32_t count)
{
    std::vector<Spec> specs;specs.push_back(Geometry());
    const auto root=Node(100,0,0);specs.push_back(NodeSpec(root));
    for(uint32_t i=1;i<count;++i){const auto node=Node(100+i,100,double(i)*16.0,i==count-1);
        specs.push_back(NodeSpec(node));specs.push_back(InstanceSpec(Instance(1000+i,100+i,node)));}
    specs.push_back(InstanceSpec(Instance(1000,100,root)));return specs;
}
}

TEST_CASE("Protocol v10 validates one geometry shared by many hierarchical instances", "[fbx-002][scene-contract]")
{
    auto section=Section(ManyInstances(64),64);
    import_broker::KnownSceneCatalog catalog;
    auto result=import_broker::ValidateAndCopySection(section,kGeneration,256,nullptr,false,nullptr,0,0,&catalog);
    REQUIRE(result.ok);
    CHECK(catalog.geometry.size()==1);CHECK(catalog.nodes.size()==64);CHECK(catalog.instances.size()==64);
    CHECK(catalog.nodes.at(163).depth==2);CHECK(catalog.instances.at(1063).instanceId==1063);
    CHECK(catalog.nodes.at(163).worldTransform[0]==-1.0);

    // The accepted records are private copies: changing the source section
    // after the first read cannot change any record admitted downstream.
    auto accepted=result.chunks.back().payload;
    std::fill(section.begin(),section.end(),std::byte{0xff});
    CHECK(result.chunks.back().payload==accepted);
}

TEST_CASE("Scene references cross progressive batches only through the bounded generation catalog", "[fbx-002][scene-contract]")
{
    const auto root=Node(100,0,0);
    import_broker::KnownSceneCatalog scenes;
    auto first=import_broker::ValidateAndCopySection(Section({Geometry(),NodeSpec(root)},1),kGeneration,8,
        nullptr,false,nullptr,0,0,&scenes);
    REQUIRE(first.ok);
    import_broker::KnownChunkCatalog chunks{{1,ChunkTopology::TriangleList},{100,ChunkTopology::Node}};
    auto second=import_broker::ValidateAndCopySection(Section({InstanceSpec(Instance(1000,100,root))},1),
        kGeneration,8,&chunks,false,nullptr,0,0,&scenes);
    REQUIRE(second.ok);CHECK(scenes.instances.size()==1);

    auto foreign=Instance(1001,777,root);auto rejected=import_broker::ValidateAndCopySection(
        Section({InstanceSpec(foreign)},1),kGeneration,8,&chunks,true,nullptr,0,0,&scenes);
    CHECK_FALSE(rejected.ok);
}

TEST_CASE("Protocol v10 rejects hostile scene graph and instance records before publication", "[fbx-002][scene-contract][hostile-worker]")
{
    SECTION("invalid parent topology") { auto specs=ManyInstances(2);auto node=Node(101,1,16);specs[2]=NodeSpec(node);
        CHECK_FALSE(import_broker::ValidateAndCopySection(Section(std::move(specs),2),kGeneration,16).ok); }
    SECTION("cycle") { auto specs=ManyInstances(2);auto a=Node(100,101,0),b=Node(101,100,16);specs[1]=NodeSpec(a);specs[2]=NodeSpec(b);
        CHECK_FALSE(import_broker::ValidateAndCopySection(Section(std::move(specs),2),kGeneration,16).ok); }
    SECTION("NaN matrix") { auto specs=ManyInstances(2);NodePayload node{};std::memcpy(&node,specs[2].payload.data(),sizeof(node));
        node.localTransform[0]=std::numeric_limits<double>::quiet_NaN();specs[2]=NodeSpec(node);
        CHECK_FALSE(import_broker::ValidateAndCopySection(Section(std::move(specs),2),kGeneration,16).ok); }
    SECTION("infinite matrix") { auto specs=ManyInstances(2);NodePayload node{};std::memcpy(&node,specs[2].payload.data(),sizeof(node));
        node.localTransform[12]=std::numeric_limits<double>::infinity();specs[2]=NodeSpec(node);
        CHECK_FALSE(import_broker::ValidateAndCopySection(Section(std::move(specs),2),kGeneration,16).ok); }
    SECTION("illegal geometry topology") { auto specs=ManyInstances(2);MeshInstancePayload instance{};std::memcpy(&instance,specs[3].payload.data(),sizeof(instance));
        instance.geometryChunkId=100;specs[3]=InstanceSpec(instance);specs[3].descriptor.dependencyIds[0]=100;
        CHECK_FALSE(import_broker::ValidateAndCopySection(Section(std::move(specs),2),kGeneration,16).ok); }
    SECTION("duplicate record id") { auto specs=ManyInstances(2);specs[3].descriptor.chunkId=100;
        CHECK_FALSE(import_broker::ValidateAndCopySection(Section(std::move(specs),2),kGeneration,16).ok); }
    SECTION("oversized table") { auto bytes=Section(ManyInstances(4),4);
        auto result=import_broker::ValidateAndCopySection(bytes,kGeneration,2);CHECK_FALSE(result.ok);
        CHECK(result.errorCode==ImportErrorCode::ResourceLimit); }
    SECTION("late unresolved reference") { auto specs=ManyInstances(2);MeshInstancePayload instance{};std::memcpy(&instance,specs[3].payload.data(),sizeof(instance));
        instance.nodeId=9999;specs[3]=InstanceSpec(instance);
        CHECK_FALSE(import_broker::ValidateAndCopySection(Section(std::move(specs),2),kGeneration,16,nullptr,true).ok); }
    SECTION("fabricated transformed bounds") { auto specs=ManyInstances(2);MeshInstancePayload instance{};std::memcpy(&instance,specs[3].payload.data(),sizeof(instance));
        instance.worldMax[0]+=1;specs[3]=InstanceSpec(instance);
        CHECK_FALSE(import_broker::ValidateAndCopySection(Section(std::move(specs),2),kGeneration,16).ok); }
    SECTION("unknown flag") { auto specs=ManyInstances(2);NodePayload node{};std::memcpy(&node,specs[2].payload.data(),sizeof(node));node.flags|=0x80000000u;specs[2]=NodeSpec(node);
        CHECK_FALSE(import_broker::ValidateAndCopySection(Section(std::move(specs),2),kGeneration,16).ok); }
    SECTION("old protocol") { auto bytes=Section(ManyInstances(2),2,9);auto result=import_broker::ValidateAndCopySection(bytes,kGeneration,16);
        CHECK_FALSE(result.ok);CHECK(result.errorCode==ImportErrorCode::ImportProtocolViolation); }
}
