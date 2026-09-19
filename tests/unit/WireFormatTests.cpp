// Pure data-layout tests for the Gate 2 workstream A wire format -- no
// process launch, no I/O. Proves the wire structs are exactly the
// documented byte layout, the checksum is deterministic/corruption-
// sensitive, closed-enum lookups reject unknown IDs, and checked arithmetic
// never silently wraps. See .docs/design/03-file-formats-and-ingestion.md
// ("Wire format") and shared/model-core/include/model_core/.

#include "model_core/Checksum.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "model_core/GeometryBounds.h"
#include "InfoPanel.h"
#include "platform/CheckedMath.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <cstring>
#include <vector>

TEST_CASE("SectionHeader and ChunkDescriptor match the documented fixed-width wire layout",
          "[wire-format]")
{
    REQUIRE(sizeof(model_core::SectionHeader) == 88);
    REQUIRE(sizeof(model_core::ChunkDescriptor) == 160);
    REQUIRE(model_core::kCurrentProtocolVersion == 10);
    REQUIRE(sizeof(model_core::NodePayload) == 144);
    REQUIRE(sizeof(model_core::MeshInstancePayload) == 80);

    CHECK(offsetof(model_core::SectionHeader, magic) == 0);
    CHECK(offsetof(model_core::SectionHeader, protocolVersion) == 4);
    CHECK(offsetof(model_core::SectionHeader, generationId) == 8);
    CHECK(offsetof(model_core::SectionHeader, sectionChecksum) == 32);

    CHECK(offsetof(model_core::ChunkDescriptor, topology) == 32);
    CHECK(offsetof(model_core::ChunkDescriptor, byteSize) == 56);
    CHECK(offsetof(model_core::ChunkDescriptor, chunkChecksum) == 84);
    CHECK(offsetof(model_core::ChunkDescriptor, origin) == 92);
    CHECK(offsetof(model_core::ChunkDescriptor, localMin) == 116);
    CHECK(offsetof(model_core::ChunkDescriptor, boundsState) == 152);
    CHECK(offsetof(model_core::ChunkDescriptor, sourceElementOffset) == 156);
    CHECK(sizeof(model_core::SceneMetadata) == 48);
    CHECK(sizeof(double) == 8);
    CHECK(std::numeric_limits<double>::is_iec559);
}

TEST_CASE("Double cluster origins preserve tiny local geometry and finite exact bounds", "[wire-format][bounds][precision]")
{
    using namespace model_core;
    std::array<VertexPositionOnlyF32,3> points{{ {0,0,0}, {1e-6f,2e-6f,0}, {-1e-6f,0,3e-6f} }};
    ChunkDescriptor descriptor{};
    descriptor.vertexCount = 3; descriptor.vertexLayoutId = uint32_t(VertexLayoutId::PositionOnly_F32);
    descriptor.origin[0] = 1e12;
    auto bytes = std::as_writable_bytes(std::span(points));
    REQUIRE(RebasePositions(descriptor,bytes));
    CHECK(descriptor.origin[0] == 1e12);
    CHECK(descriptor.localMin[0] == -1e-6f); CHECK(descriptor.localMax[0] == 1e-6f);
    CHECK(descriptor.localMax[1] == 2e-6f); CHECK(descriptor.localMax[2] == 3e-6f);
    CHECK(descriptor.boundsState == BoundsState::Verified);
    points[1].x = std::numeric_limits<float>::infinity();
    CHECK_FALSE(SetLocalBounds(descriptor,bytes));
    points[1].x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(SetLocalBounds(descriptor,bytes));
    descriptor.vertexCount = 0; CHECK_FALSE(SetLocalBounds(descriptor,bytes));
}

TEST_CASE("Info uses real counts units exact axis dimensions and provisional bounds", "[bounds][metadata]")
{
    ModelData metadata;
    metadata.source.format = model_core::SourceFormatId::Ply;
    metadata.vertexCount = metadata.pointCount = 1234;
    metadata.relativeMax[0] = 2; metadata.relativeMax[1] = 3; metadata.relativeMax[2] = 4;
    auto sections = BuildInfoPanelSections(metadata,false);
    CHECK(sections[0].rows[0].value == L"2.000 units");
    CHECK(sections[0].rows[3].value == L"Provisional (loading)");
    CHECK(sections[1].rows[0].value == L"0");
    CHECK(sections[1].rows[1].value == L"1,234"); CHECK(sections[1].rows[2].value == L"1,234");
    metadata.source.format = model_core::SourceFormatId::Glb;
    metadata.source.upAxis = model_core::UpAxisId::Y; metadata.source.metersPerUnit = 1;
    metadata.boundsVerified = true;
    sections = BuildInfoPanelSections(metadata,false);
    CHECK(sections[0].rows[1].value == L"4.000 m"); CHECK(sections[0].rows[2].value == L"3.000 m");
    CHECK(sections[0].rows[3].value == L"Verified");
    sections = BuildInfoPanelSections(metadata,true);
    CHECK(sections[0].rows[1].value == L"3.000 m");
    metadata.relativeMax[0] = 1e-6;
    CHECK(BuildInfoPanelSections(metadata,true)[0].rows[0].value == L"1.000e-06 m");
    metadata.source.format = model_core::SourceFormatId::AsciiStl;
    CHECK(BuildInfoPanelSections(metadata, false)[5].rows[2].value == L"STL (ASCII)");
    metadata.source.format = model_core::SourceFormatId::AsciiPly;
    CHECK(BuildInfoPanelSections(metadata, false)[5].rows[2].value == L"PLY (ASCII)");
    metadata.source.format = model_core::SourceFormatId::Obj;
    CHECK(BuildInfoPanelSections(metadata, false)[5].rows[2].value == L"OBJ");
    metadata.source.format = model_core::SourceFormatId::Fbx;
    CHECK(BuildInfoPanelSections(metadata, false)[5].rows[2].value == L"FBX");
    metadata.source.format = model_core::SourceFormatId::Usda;
    CHECK(BuildInfoPanelSections(metadata, false)[5].rows[2].value == L"USD (ASCII)");
    metadata.source.format = model_core::SourceFormatId::Usdc;
    CHECK(BuildInfoPanelSections(metadata, false)[5].rows[2].value == L"USD (crate)");
    metadata.source.format = model_core::SourceFormatId::Usdz;
    CHECK(BuildInfoPanelSections(metadata, false)[5].rows[2].value == L"USDZ");
    metadata.source.format = model_core::SourceFormatId::ThreeMf;
    CHECK(BuildInfoPanelSections(metadata, false)[5].rows[2].value == L"3MF");
    metadata.source.format = model_core::SourceFormatId::Step;
    CHECK(BuildInfoPanelSections(metadata, false)[5].rows[2].value == L"STEP");
    CHECK(metadata.vertices.empty()); CHECK(metadata.indices.empty());
}

TEST_CASE("Ground-axis selection cycles and applies exact axis permutations", "[bounds][metadata][ground-axis]")
{
    CHECK(ResolveGroundAxis(GroundAxis::Automatic, model_core::UpAxisId::Y) == GroundAxis::Y);
    CHECK(ResolveGroundAxis(GroundAxis::Automatic, model_core::UpAxisId::X) == GroundAxis::X);
    CHECK(ResolveGroundAxis(GroundAxis::Automatic, model_core::UpAxisId::Z) == GroundAxis::Z);
    CHECK(ResolveGroundAxis(GroundAxis::Automatic, model_core::UpAxisId::Unknown) == GroundAxis::Z);
    CHECK(NextGroundAxis(GroundAxis::Z, model_core::UpAxisId::Unknown) == GroundAxis::Y);
    CHECK(NextGroundAxis(GroundAxis::Y, model_core::UpAxisId::Unknown) == GroundAxis::X);
    CHECK(NextGroundAxis(GroundAxis::X, model_core::UpAxisId::Unknown) == GroundAxis::Z);

    double dimensions[3] = { 2.0, 3.0, 4.0 };
    PermuteGroundedDimensions(dimensions, GroundAxis::X, model_core::UpAxisId::Unknown, false);
    CHECK(dimensions[0] == 4.0);
    CHECK(dimensions[1] == 3.0);
    CHECK(dimensions[2] == 2.0);

    DirectX::XMFLOAT3 grounded{};
    DirectX::XMStoreFloat3(&grounded, DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(1, 0, 0, 0),
        GroundAxisTransform(GroundAxis::X, model_core::UpAxisId::Unknown, false)));
    CHECK(grounded.x == 0.0f);
    CHECK(grounded.y == 0.0f);
    CHECK(grounded.z == 1.0f);

    DirectX::XMStoreFloat3(&grounded, DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(-1, 0, 0, 0),
        GroundAxisTransform(GroundAxis::X, model_core::UpAxisId::Unknown, false, true)));
    CHECK(grounded.x == 0.0f);
    CHECK(grounded.y == 0.0f);
    CHECK(grounded.z == 1.0f);

    DirectX::XMStoreFloat3(&grounded, DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(0, 1, 0, 0),
        GroundAxisTransform(GroundAxis::Y, model_core::UpAxisId::Unknown, false)));
    CHECK(grounded.x == 0.0f);
    CHECK(grounded.y == 0.0f);
    CHECK(grounded.z == 1.0f);

    DirectX::XMStoreFloat3(&grounded, DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(0, -1, 0, 0),
        GroundAxisTransform(GroundAxis::Y, model_core::UpAxisId::Unknown, false, true)));
    CHECK(grounded.x == 0.0f);
    CHECK(grounded.y == 0.0f);
    CHECK(grounded.z == 1.0f);

    DirectX::XMStoreFloat3(&grounded, DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(0, 0, -1, 0),
        GroundAxisTransform(GroundAxis::Z, model_core::UpAxisId::Unknown, false, true)));
    CHECK(grounded.x == 0.0f);
    CHECK(grounded.y == 0.0f);
    CHECK(grounded.z == 1.0f);
}

TEST_CASE("Fnv1a64 is deterministic and detects single-byte corruption", "[wire-format]")
{
    std::array<std::byte, 8> data{ std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 }, std::byte{ 4 },
                                    std::byte{ 5 }, std::byte{ 6 }, std::byte{ 7 }, std::byte{ 8 } };

    uint64_t first = model_core::Fnv1a64(data);
    uint64_t second = model_core::Fnv1a64(data);
    CHECK(first == second);

    data[3] = std::byte{ 0xFF };
    uint64_t corrupted = model_core::Fnv1a64(data);
    CHECK(corrupted != first);
}

TEST_CASE("Protocol-v10 wire checksum is split-invariant across parallel block boundaries", "[wire-format]")
{
    std::vector<std::byte> data(11*1024*1024+37);
    for (size_t i=0;i<data.size();++i) data[i]=std::byte((i*131+17)&0xff);
    const uint64_t contiguous=model_core::WireChecksum64(data);
    for (const size_t split:{size_t(1),size_t(256*1024-1),size_t(256*1024),
                             size_t(10*1024*1024+13),data.size()-1})
        CHECK(model_core::WireChecksum64(std::span(data).first(split),
                                         std::span(data).subspan(split))==contiguous);
    data[10*1024*1024+9]^=std::byte{0x80};
    CHECK(model_core::WireChecksum64(data)!=contiguous);
}

TEST_CASE("VertexStrideForLayout returns 0 for Unknown and any value outside the closed enumeration",
          "[wire-format]")
{
    CHECK(model_core::VertexStrideForLayout(model_core::VertexLayoutId::Unknown) == 0);
    CHECK(model_core::VertexStrideForLayout(model_core::VertexLayoutId::PositionOnly_F32) == 12);
    CHECK(model_core::VertexStrideForLayout(model_core::VertexLayoutId::PositionNormalUv0_F32)
          == 32);
    CHECK(model_core::VertexStrideForLayout(model_core::VertexLayoutId::PositionNormalUv0TangentColor_F32)
          == 64);

    auto outOfRange = static_cast<model_core::VertexLayoutId>(0xFFFFFFFFu);
    CHECK(model_core::VertexStrideForLayout(outOfRange) == 0);
}

TEST_CASE("CheckedAdd and CheckedMultiply detect overflow instead of wrapping", "[wire-format]")
{
    constexpr uint64_t kMax = (std::numeric_limits<uint64_t>::max)();

    CHECK_FALSE(platform::CheckedAdd(kMax, 1).has_value());
    CHECK_FALSE(platform::CheckedMultiply(kMax, 2).has_value());

    auto sum = platform::CheckedAdd(40ULL, 92ULL);
    REQUIRE(sum.has_value());
    CHECK(*sum == 132ULL);

    auto product = platform::CheckedMultiply(8ULL, 92ULL);
    REQUIRE(product.has_value());
    CHECK(*product == 736ULL);

    CHECK(platform::CheckedMultiply(0ULL, kMax).has_value());
}
