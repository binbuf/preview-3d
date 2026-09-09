// Gate 3 slice 3: real binary-PLY (Stanford Polygon) parsing behind the
// sandbox. The third complete format wired through the same AppContainer-
// sandboxed pipeline GltfImportTests.cpp/StlImportTests.cpp already proved.
// Binary little- and big-endian only (mesh or point cloud); ASCII PLY is
// Tier B, explicitly out of scope -- see PlyAdapter.cpp's header comment.

#include "SandboxTestSupport.h"
#include "import_broker/SandboxLauncher.h"
#include "import_broker/SharedSection.h"
#include "import_broker/SharedSectionValidator.h"
#include "import_broker/SourceFileAccess.h"
#include "model_core/ControlChannelIo.h"
#include "model_core/ControlProtocol.h"
#include "model_core/VertexLayouts.h"
#include "model_core/WireFormat.h"
#include "platform/MappedView.h"
#include "platform/Win32Handle.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#ifndef PREVIEW3D_TEST_ASSETS_DIR
#error "PREVIEW3D_TEST_ASSETS_DIR must be defined by Tests.ImportIsolation.vcxproj"
#endif

namespace {

// ---- Binary-body byte construction ----

uint32_t TestByteSwap32(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8)
        | ((v & 0xFF000000u) >> 24);
}

uint16_t TestByteSwap16(uint16_t v)
{
    return static_cast<uint16_t>((v << 8) | (v >> 8));
}

void AppendU8(std::vector<std::byte>& out, uint8_t v)
{
    out.push_back(static_cast<std::byte>(v));
}

void AppendU16(std::vector<std::byte>& out, uint16_t v, bool bigEndian)
{
    if (bigEndian) {
        v = TestByteSwap16(v);
    }
    std::byte bytes[2];
    std::memcpy(bytes, &v, 2);
    out.insert(out.end(), bytes, bytes + 2);
}

void AppendU32(std::vector<std::byte>& out, uint32_t v, bool bigEndian)
{
    if (bigEndian) {
        v = TestByteSwap32(v);
    }
    std::byte bytes[4];
    std::memcpy(bytes, &v, 4);
    out.insert(out.end(), bytes, bytes + 4);
}

void AppendF32(std::vector<std::byte>& out, float f, bool bigEndian)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    AppendU32(out, bits, bigEndian);
}

// PLY's on-disk shape genuinely is "ASCII header text, verbatim, followed
// by a separately-built binary blob" -- mirrored directly here rather than
// forced through one inline C struct the way StlImportTests.cpp's
// BuildBinaryStl could for STL's trivial fixed-record layout.
std::vector<std::byte> BuildBinaryPly(const std::string& headerText, const std::vector<std::byte>& body)
{
    std::vector<std::byte> out(headerText.size());
    std::memcpy(out.data(), headerText.data(), headerText.size());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::string FormatLine(bool bigEndian)
{
    return bigEndian ? "binary_big_endian" : "binary_little_endian";
}

// A mesh: vertex(x,y,z[,nx,ny,nz]) + face(list uchar int vertex_indices).
// Faces are arbitrary-length index lists, triangulated by the adapter.
std::vector<std::byte> BuildMeshPly(bool bigEndian, bool includeNormals,
                                     const std::vector<std::array<float, 3>>& positions,
                                     const std::vector<std::array<float, 3>>& normals,
                                     const std::vector<std::vector<uint32_t>>& faces)
{
    std::string header = "ply\nformat " + FormatLine(bigEndian) + " 1.0\n";
    header += "element vertex " + std::to_string(positions.size()) + "\n";
    header += "property float x\nproperty float y\nproperty float z\n";
    if (includeNormals) {
        header += "property float nx\nproperty float ny\nproperty float nz\n";
    }
    header += "element face " + std::to_string(faces.size()) + "\n";
    header += "property list uchar int vertex_indices\n";
    header += "end_header\n";

    std::vector<std::byte> body;
    for (size_t i = 0; i < positions.size(); ++i) {
        AppendF32(body, positions[i][0], bigEndian);
        AppendF32(body, positions[i][1], bigEndian);
        AppendF32(body, positions[i][2], bigEndian);
        if (includeNormals) {
            AppendF32(body, normals[i][0], bigEndian);
            AppendF32(body, normals[i][1], bigEndian);
            AppendF32(body, normals[i][2], bigEndian);
        }
    }
    for (const auto& face : faces) {
        AppendU8(body, static_cast<uint8_t>(face.size()));
        for (uint32_t idx : face) {
            AppendU32(body, idx, bigEndian);
        }
    }
    return BuildBinaryPly(header, body);
}

std::vector<std::byte> BuildPointCloudPly(bool bigEndian, const std::vector<std::array<float, 3>>& positions,
                                           bool declareEmptyFaceElement)
{
    std::string header = "ply\nformat " + FormatLine(bigEndian) + " 1.0\n";
    header += "element vertex " + std::to_string(positions.size()) + "\n";
    header += "property float x\nproperty float y\nproperty float z\n";
    if (declareEmptyFaceElement) {
        header += "element face 0\nproperty list uchar int vertex_indices\n";
    }
    header += "end_header\n";

    std::vector<std::byte> body;
    for (const auto& p : positions) {
        AppendF32(body, p[0], false);
        AppendF32(body, p[1], false);
        AppendF32(body, p[2], false);
    }
    return BuildBinaryPly(header, body);
}

// A scratch on-disk file holding pre-built bytes -- mirrors
// StlImportTests.cpp's ScratchStlFile (itself mirroring
// tests/unit/MappedFileTests.cpp's ScratchFile pattern), duplicated here
// rather than shared, same precedent used throughout this suite.
struct ScratchPlyFile {
    std::wstring path;

    explicit ScratchPlyFile(const std::vector<std::byte>& bytes)
    {
        wchar_t tempDir[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
        wchar_t tempPath[MAX_PATH]{};
        REQUIRE(GetTempFileNameW(tempDir, L"ply", 0, tempPath) != 0);
        path = tempPath;

        platform::Win32Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                                FILE_ATTRIBUTE_NORMAL, nullptr));
        REQUIRE(file);
        DWORD written = 0;
        REQUIRE(WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
        REQUIRE(written == bytes.size());
    }

    ~ScratchPlyFile() { DeleteFileW(path.c_str()); }

    ScratchPlyFile(const ScratchPlyFile&) = delete;
    ScratchPlyFile& operator=(const ScratchPlyFile&) = delete;
};

struct PlyImportLaunch {
    import_broker::SandboxProcess proc;
    platform::Win32Handle controlInWrite;
    platform::Win32Handle controlOutRead;
};

// Launches the real worker in --parse-ply mode. Duplicates
// StlImportTests.cpp's LaunchStlImportWorker shape rather than sharing it,
// same "small deliberate duplication over cross-file coupling" precedent
// used throughout this suite.
std::optional<PlyImportLaunch> LaunchPlyImportWorker(const platform::AppContainerSid& sid,
                                                       HANDLE sourceFileHandle, HANDLE outputSectionHandle)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE inReadRaw = nullptr;
    HANDLE inWriteRaw = nullptr;
    if (!CreatePipe(&inReadRaw, &inWriteRaw, &sa, 0)) {
        return std::nullopt;
    }
    platform::Win32Handle controlInRead(inReadRaw);
    platform::Win32Handle controlInWrite(inWriteRaw);
    SetHandleInformation(controlInWrite.get(), HANDLE_FLAG_INHERIT, 0);

    HANDLE outReadRaw = nullptr;
    HANDLE outWriteRaw = nullptr;
    if (!CreatePipe(&outReadRaw, &outWriteRaw, &sa, 0)) {
        return std::nullopt;
    }
    platform::Win32Handle controlOutRead(outReadRaw);
    platform::Win32Handle controlOutWrite(outWriteRaw);
    SetHandleInformation(controlOutRead.get(), HANDLE_FLAG_INHERIT, 0);

    std::vector<HANDLE> inherited{ controlInRead.get(), controlOutWrite.get(), sourceFileHandle,
                                    outputSectionHandle };
    std::wstring cmdLine = L"\"" + std::wstring(sandbox_test_support::WorkerExePath()) + L"\" --parse-ply";

    import_broker::SandboxLimits limits{};
    auto proc = import_broker::LaunchSuspendedSandboxed(
        sandbox_test_support::WorkerExePath(), cmdLine, inherited, controlOutWrite.get(), limits, sid,
        controlInRead.get());
    if (!proc) {
        return std::nullopt;
    }

    controlInRead.reset();
    controlOutWrite.reset();

    PlyImportLaunch launch;
    launch.proc = std::move(*proc);
    launch.controlInWrite = std::move(controlInWrite);
    launch.controlOutRead = std::move(controlOutRead);
    return launch;
}

struct PlyImportRun {
    bool ready = false;
    import_broker::ValidationResult validation;
    model_core::GenerationErrorNotice errorNotice{};
};

PlyImportRun RunPlyImportFromRealFile(const platform::AppContainerSid& sid, const std::wstring& path,
                                       uint64_t generationId, uint32_t maxChunkCount)
{
    PlyImportRun run;

    auto opened = import_broker::OpenAndCanonicalizeSourceFile(path);
    REQUIRE(opened.file);

    auto duplicated = import_broker::DuplicateInheritableHandle(opened.file.get());
    REQUIRE(duplicated.has_value());

    auto outputSection = import_broker::CreateSharedSection(import_broker::kSyntheticSectionBytes);
    REQUIRE(outputSection);

    auto launch = LaunchPlyImportWorker(sid, duplicated->get(), outputSection.get());
    REQUIRE(launch.has_value());
    REQUIRE(import_broker::ResumeSandboxProcess(launch->proc));

    model_core::ParsePlyFileRequest request{};
    request.generationId = generationId;
    request.sourceFileHandleValue = reinterpret_cast<uint64_t>(duplicated->get());
    request.sectionHandleValue = reinterpret_cast<uint64_t>(outputSection.get());
    request.sectionByteCapacity = import_broker::kSyntheticSectionBytes;
    request.maxChunkCount = maxChunkCount;
    REQUIRE(model_core::WriteControlMessage(launch->controlInWrite.get(),
                                             model_core::ControlOpcode::StartPlyImportFromFile, &request,
                                             sizeof(request)));

    auto received = model_core::ReadControlMessage(launch->controlOutRead.get());
    REQUIRE(received.has_value());

    WaitForSingleObject(launch->proc.process.get(), 5000);

    if (received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::GenerationError)) {
        REQUIRE(received->payload.size() == sizeof(run.errorNotice));
        std::memcpy(&run.errorNotice, received->payload.data(), sizeof(run.errorNotice));
        return run;
    }

    REQUIRE(received->header.opcode == static_cast<uint32_t>(model_core::ControlOpcode::ChunksReady));
    run.ready = true;

    auto view = platform::MappedView::Map(outputSection.get(), FILE_MAP_READ, import_broker::kSyntheticSectionBytes);
    REQUIRE(view);
    run.validation = import_broker::ValidateAndCopySection(view.bytes(), generationId, maxChunkCount);
    return run;
}

// Two disjoint triangles in the XY plane, CCW winding -> flat normal
// (0,0,1) for both -- same convention StlImportTests.cpp's MakeTriangle
// already established.
std::vector<std::array<float, 3>> TwoTrianglePositions()
{
    return { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
             { 2.0f, 0.0f, 0.0f }, { 3.0f, 0.0f, 0.0f }, { 2.0f, 1.0f, 0.0f } };
}

std::vector<std::vector<uint32_t>> TwoTriangleFaces()
{
    return { { 0, 1, 2 }, { 3, 4, 5 } };
}

} // namespace

TEST_CASE("A valid little-endian binary PLY mesh round-trips through the real sandboxed pipeline",
          "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::vector<std::array<float, 3>> normals(6, std::array<float, 3>{ 0.0f, 0.0f, 1.0f });
    ScratchPlyFile file(BuildMeshPly(false, true, TwoTrianglePositions(), normals, TwoTriangleFaces()));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/1, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks.size() == 1);

    const auto& chunk = run.validation.chunks[0];
    CHECK(chunk.descriptor.topology == model_core::ChunkTopology::TriangleList);
    CHECK(chunk.descriptor.vertexCount == 6);
    CHECK(chunk.descriptor.indexCount == 6);
    CHECK(chunk.descriptor.vertexLayoutId
          == static_cast<uint32_t>(model_core::VertexLayoutId::PositionNormalUv0_F32));

    REQUIRE(chunk.payload.size() >= 6 * sizeof(model_core::VertexPositionNormalUv0F32));
    model_core::VertexPositionNormalUv0F32 vertices[6]{};
    std::memcpy(vertices, chunk.payload.data(), sizeof(vertices));
    for (const auto& v : vertices) {
        CHECK(v.nx == Catch::Approx(0.0f).margin(1e-5));
        CHECK(v.ny == Catch::Approx(0.0f).margin(1e-5));
        CHECK(v.nz == Catch::Approx(1.0f).margin(1e-5));
    }
}

TEST_CASE("The same geometry as binary_big_endian produces numerically identical output", "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::vector<std::array<float, 3>> normals(6, std::array<float, 3>{ 0.0f, 0.0f, 1.0f });
    ScratchPlyFile file(BuildMeshPly(true, true, TwoTrianglePositions(), normals, TwoTriangleFaces()));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/2, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks[0].payload.size() >= 6 * sizeof(model_core::VertexPositionNormalUv0F32));

    model_core::VertexPositionNormalUv0F32 vertices[6]{};
    std::memcpy(vertices, run.validation.chunks[0].payload.data(), sizeof(vertices));
    CHECK(vertices[1].px == Catch::Approx(1.0f).margin(1e-5));
    CHECK(vertices[2].py == Catch::Approx(1.0f).margin(1e-5));
    for (const auto& v : vertices) {
        CHECK(v.nz == Catch::Approx(1.0f).margin(1e-5));
    }
}

TEST_CASE("A vertex-only PLY (no face element) emits a PointList chunk", "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::vector<std::array<float, 3>> positions{ { 1.0f, 2.0f, 3.0f }, { 4.0f, 5.0f, 6.0f } };
    ScratchPlyFile file(BuildPointCloudPly(false, positions, /*declareEmptyFaceElement=*/false));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/3, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);

    const auto& chunk = run.validation.chunks[0];
    CHECK(chunk.descriptor.topology == model_core::ChunkTopology::PointList);
    CHECK(chunk.descriptor.vertexCount == 2);
    CHECK(chunk.descriptor.indexCount == 0);
    CHECK(chunk.descriptor.vertexLayoutId == static_cast<uint32_t>(model_core::VertexLayoutId::PositionOnly_F32));

    REQUIRE(chunk.payload.size() >= 2 * sizeof(model_core::VertexPositionOnlyF32));
    model_core::VertexPositionOnlyF32 points[2]{};
    std::memcpy(points, chunk.payload.data(), sizeof(points));
    CHECK(points[1].x == Catch::Approx(4.0f));
}

TEST_CASE("A declared-but-empty face element is treated as a point cloud, not a mesh", "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::vector<std::array<float, 3>> positions{ { 1.0f, 2.0f, 3.0f } };
    ScratchPlyFile file(BuildPointCloudPly(false, positions, /*declareEmptyFaceElement=*/true));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/4, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    CHECK(run.validation.chunks[0].descriptor.topology == model_core::ChunkTopology::PointList);
}

TEST_CASE("A quad face is fan-triangulated into 2 triangles with the expected index pattern",
          "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::vector<std::array<float, 3>> positions{
        { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }
    };
    std::vector<std::array<float, 3>> normals(4, std::array<float, 3>{ 0.0f, 0.0f, 1.0f });
    std::vector<std::vector<uint32_t>> faces{ { 0, 1, 2, 3 } };
    ScratchPlyFile file(BuildMeshPly(false, true, positions, normals, faces));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/5, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);

    const auto& chunk = run.validation.chunks[0];
    CHECK(chunk.descriptor.vertexCount == 4);
    CHECK(chunk.descriptor.indexCount == 6);
    REQUIRE(chunk.payload.size()
            >= 4 * sizeof(model_core::VertexPositionNormalUv0F32) + 6 * sizeof(uint32_t));
    uint32_t indices[6]{};
    std::memcpy(indices, chunk.payload.data() + 4 * sizeof(model_core::VertexPositionNormalUv0F32),
                sizeof(indices));
    CHECK(std::vector<uint32_t>(indices, indices + 6) == std::vector<uint32_t>{ 0, 1, 2, 0, 2, 3 });
}

TEST_CASE("A pentagon face is fan-triangulated into 3 triangles", "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::vector<std::array<float, 3>> positions{ { 0.0f, 0.0f, 0.0f },
                                                  { 1.0f, 0.0f, 0.0f },
                                                  { 1.5f, 1.0f, 0.0f },
                                                  { 0.5f, 1.5f, 0.0f },
                                                  { -0.5f, 1.0f, 0.0f } };
    std::vector<std::array<float, 3>> normals(5, std::array<float, 3>{ 0.0f, 0.0f, 1.0f });
    std::vector<std::vector<uint32_t>> faces{ { 0, 1, 2, 3, 4 } };
    ScratchPlyFile file(BuildMeshPly(false, true, positions, normals, faces));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/6, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    CHECK(run.validation.chunks[0].descriptor.indexCount == 9); // 3 triangles
}

TEST_CASE("An unrecognized/skippable extra vertex property doesn't corrupt subsequently-read fields",
          "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::string header = "ply\nformat binary_little_endian 1.0\n"
                          "element vertex 1\n"
                          "property float x\n"
                          "property float y\n"
                          "property float z\n"
                          "property float confidence\n"
                          "property float nx\n"
                          "property float ny\n"
                          "property float nz\n"
                          "element face 0\n"
                          "property list uchar int vertex_indices\n"
                          "end_header\n";
    std::vector<std::byte> body;
    AppendF32(body, 1.0f, false);
    AppendF32(body, 2.0f, false);
    AppendF32(body, 3.0f, false);
    AppendF32(body, 0.75f, false); // "confidence" -- skipped
    AppendF32(body, 0.0f, false);
    AppendF32(body, 0.0f, false);
    AppendF32(body, 1.0f, false);
    ScratchPlyFile file(BuildBinaryPly(header, body));

    // element face 0 with 0 records: hasFace is false (declared count is 0),
    // so this is validated as a point-cloud file -- exercises cursor
    // correctness for the "confidence" skip without needing a real face.
    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/7, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    REQUIRE(run.validation.chunks[0].payload.size() >= sizeof(model_core::VertexPositionOnlyF32));
    model_core::VertexPositionOnlyF32 point{};
    std::memcpy(&point, run.validation.chunks[0].payload.data(), sizeof(point));
    CHECK(point.x == Catch::Approx(1.0f));
    CHECK(point.y == Catch::Approx(2.0f));
    CHECK(point.z == Catch::Approx(3.0f));
}

TEST_CASE("Vertex red/green/blue color is parsed-and-dropped without corrupting positions",
          "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::string header = "ply\nformat binary_little_endian 1.0\n"
                          "element vertex 1\n"
                          "property float x\n"
                          "property float y\n"
                          "property float z\n"
                          "property uchar red\n"
                          "property uchar green\n"
                          "property uchar blue\n"
                          "end_header\n";
    std::vector<std::byte> body;
    AppendF32(body, 5.0f, false);
    AppendF32(body, 6.0f, false);
    AppendF32(body, 7.0f, false);
    AppendU8(body, 255);
    AppendU8(body, 128);
    AppendU8(body, 0);
    ScratchPlyFile file(BuildBinaryPly(header, body));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/8, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    model_core::VertexPositionOnlyF32 point{};
    std::memcpy(&point, run.validation.chunks[0].payload.data(), sizeof(point));
    CHECK(point.x == Catch::Approx(5.0f));
    CHECK(point.y == Catch::Approx(6.0f));
    CHECK(point.z == Catch::Approx(7.0f));
}

TEST_CASE("A face's index-list length over the per-face sanity cap is rejected as ResourceLimit",
          "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::string header = "ply\nformat binary_little_endian 1.0\n"
                          "element vertex 3\n"
                          "property float x\n"
                          "property float y\n"
                          "property float z\n"
                          "element face 1\n"
                          "property list ushort int vertex_indices\n"
                          "end_header\n";
    std::vector<std::byte> body;
    for (int i = 0; i < 3; ++i) {
        AppendF32(body, static_cast<float>(i), false);
        AppendF32(body, static_cast<float>(i), false);
        AppendF32(body, static_cast<float>(i), false);
    }
    AppendU16(body, 256, false); // over kMaxPolygonVerticesPerFace (255) -- no index bytes needed,
                                  // the adapter fails right after reading this declared count
    ScratchPlyFile file(BuildBinaryPly(header, body));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/9, /*maxChunkCount=*/4);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::ResourceLimit));
}

TEST_CASE("A declared vertex count over the sanity cap is rejected as ResourceLimit before any body scan",
          "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::string header = "ply\nformat binary_little_endian 1.0\n"
                          "element vertex 20000001\n"
                          "property float x\n"
                          "property float y\n"
                          "property float z\n"
                          "end_header\n";
    ScratchPlyFile file(BuildBinaryPly(header, {})); // no body needed -- rejected before any body scan

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/10, /*maxChunkCount=*/4);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::ResourceLimit));
}

TEST_CASE("A header exceeding the line-count sanity limit is rejected as ResourceLimit", "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::string header = "ply\n";
    for (int i = 0; i < 5000; ++i) {
        header += "comment x\n";
    }
    header += "format binary_little_endian 1.0\n"
              "element vertex 1\n"
              "property float x\n"
              "property float y\n"
              "property float z\n"
              "end_header\n";
    ScratchPlyFile file(BuildBinaryPly(header, {}));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/11, /*maxChunkCount=*/4);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::ResourceLimit));
}

TEST_CASE("A file truncated mid-vertex-record relative to the declared count is rejected as MalformedData",
          "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::string header = "ply\nformat binary_little_endian 1.0\n"
                          "element vertex 2\n"
                          "property float x\n"
                          "property float y\n"
                          "property float z\n"
                          "end_header\n";
    std::vector<std::byte> body;
    AppendF32(body, 1.0f, false);
    AppendF32(body, 2.0f, false);
    AppendF32(body, 3.0f, false);
    AppendF32(body, 4.0f, false);
    AppendF32(body, 5.0f, false);
    // Missing the second vertex's z -- declared count is 2, only 1.67
    // vertices' worth of bytes present.
    ScratchPlyFile file(BuildBinaryPly(header, body));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/12, /*maxChunkCount=*/4);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::MalformedData));
}

TEST_CASE("A file missing the ply magic first line is rejected as MalformedData", "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::string notPly = "this is not a ply file at all\n";
    std::vector<std::byte> bytes(notPly.size());
    std::memcpy(bytes.data(), notPly.data(), notPly.size());
    ScratchPlyFile file(bytes);

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/13, /*maxChunkCount=*/4);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::MalformedData));
}

TEST_CASE("A format ascii PLY is rejected as MalformedData (ASCII is out of scope)", "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::string header = "ply\nformat ascii 1.0\n"
                          "element vertex 1\n"
                          "property float x\n"
                          "property float y\n"
                          "property float z\n"
                          "end_header\n"
                          "1.0 2.0 3.0\n";
    std::vector<std::byte> bytes(header.size());
    std::memcpy(bytes.data(), header.data(), header.size());
    ScratchPlyFile file(bytes);

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/14, /*maxChunkCount=*/4);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::MalformedData));
}

TEST_CASE("A face referencing an out-of-range vertex index is dropped; a following good face survives",
          "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::vector<std::array<float, 3>> normals(6, std::array<float, 3>{ 0.0f, 0.0f, 1.0f });
    std::vector<std::vector<uint32_t>> faces{ { 0, 1, 99 }, { 3, 4, 5 } }; // first face out of range
    ScratchPlyFile file(BuildMeshPly(false, true, TwoTrianglePositions(), normals, faces));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/15, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    CHECK(run.validation.chunks[0].descriptor.indexCount == 3); // only the good triangle survives
}

TEST_CASE("A degenerate (fewer than 3 indices) face is dropped without corrupting the rest",
          "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::vector<std::array<float, 3>> normals(6, std::array<float, 3>{ 0.0f, 0.0f, 1.0f });
    std::vector<std::vector<uint32_t>> faces{ { 0, 1 }, { 3, 4, 5 } }; // first face degenerate
    ScratchPlyFile file(BuildMeshPly(false, true, TwoTrianglePositions(), normals, faces));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/16, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    CHECK(run.validation.chunks[0].descriptor.indexCount == 3);
}

TEST_CASE("A vertex element missing a required x/y/z property is rejected as MalformedData",
          "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::string header = "ply\nformat binary_little_endian 1.0\n"
                          "element vertex 1\n"
                          "property float x\n"
                          "property float y\n"
                          "end_header\n"; // missing z
    ScratchPlyFile file(BuildBinaryPly(header, {}));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/17, /*maxChunkCount=*/4);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::MalformedData));
}

TEST_CASE("A non-finite vertex position rejects the whole file as MalformedData", "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    auto positions = TwoTrianglePositions();
    positions[0][0] = std::numeric_limits<float>::quiet_NaN();
    std::vector<std::array<float, 3>> normals(6, std::array<float, 3>{ 0.0f, 0.0f, 1.0f });
    ScratchPlyFile file(BuildMeshPly(false, true, positions, normals, TwoTriangleFaces()));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/18, /*maxChunkCount=*/4);
    CHECK_FALSE(run.ready);
    CHECK(run.errorNotice.errorCode == static_cast<uint32_t>(model_core::ImportErrorCode::MalformedData));
}

TEST_CASE("A non-finite supplied vertex normal falls back per-vertex to (0,0,1) without failing the file",
          "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    std::vector<std::array<float, 3>> normals(6, std::array<float, 3>{ 0.0f, 0.0f, 1.0f });
    float nan = std::numeric_limits<float>::quiet_NaN();
    normals[0] = { nan, nan, nan };
    ScratchPlyFile file(BuildMeshPly(false, true, TwoTrianglePositions(), normals, TwoTriangleFaces()));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/19, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);

    model_core::VertexPositionNormalUv0F32 vertices[6]{};
    std::memcpy(vertices, run.validation.chunks[0].payload.data(), sizeof(vertices));
    CHECK(vertices[0].nx == Catch::Approx(0.0f).margin(1e-5));
    CHECK(vertices[0].ny == Catch::Approx(0.0f).margin(1e-5));
    CHECK(vertices[0].nz == Catch::Approx(1.0f).margin(1e-5));
}

TEST_CASE("A file supplying no normal properties gets generated smooth per-vertex normals",
          "[ply-import]")
{
    sandbox_test_support::SandboxFixture fixture;

    ScratchPlyFile file(BuildMeshPly(false, /*includeNormals=*/false, TwoTrianglePositions(), {},
                                      TwoTriangleFaces()));

    auto run = RunPlyImportFromRealFile(fixture.sid, file.path, /*generationId=*/20, /*maxChunkCount=*/4);
    REQUIRE(run.ready);
    REQUIRE(run.validation.ok);
    CHECK(run.validation.chunks[0].descriptor.vertexLayoutId
          == static_cast<uint32_t>(model_core::VertexLayoutId::PositionNormalUv0_F32));

    model_core::VertexPositionNormalUv0F32 vertices[6]{};
    std::memcpy(vertices, run.validation.chunks[0].payload.data(), sizeof(vertices));
    for (const auto& v : vertices) {
        CHECK(v.nx == Catch::Approx(0.0f).margin(1e-5));
        CHECK(v.ny == Catch::Approx(0.0f).margin(1e-5));
        CHECK(v.nz == Catch::Approx(1.0f).margin(1e-5));
    }
}
