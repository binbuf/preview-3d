#include "../../interactive-viewer/src/app/D3D12ImportBridge.h"
#include "import_broker/SharedSection.h"
#include "SandboxTestSupport.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace {
struct ScratchSource {
    std::filesystem::path path;
    explicit ScratchSource(const std::string& bytes, const wchar_t* extension) {
        path = std::filesystem::temp_directory_path() / (L"Preview3D-private-source-" + std::to_wstring(GetCurrentProcessId()) + extension);
        std::ofstream output(path, std::ios::binary); output.write(bytes.data(), bytes.size());
        REQUIRE(output.good());
    }
    ~ScratchSource() { std::error_code error; std::filesystem::remove(path,error); }
};
import_broker::ImportSessionRequest Request(const std::wstring& path) {
    import_broker::ImportSessionRequest request;
    request.workerExePath = sandbox_test_support::WorkerExePath(); request.sourcePath = path;
    request.generationId = 204; request.sectionByteCapacity = import_broker::kImportSectionBytes;
    request.maxChunkCount = 1024; request.maxChunkBatchesPerGeneration = 8;
    request.maxSidecarRequestsPerGeneration = 64; request.maxSidecarFileBytes = 256ull*1024*1024;
    return request;
}
std::wstring Triangle() { return std::wstring(PREVIEW3D_TEST_ASSETS_DIR) + L"tri_tight.glb"; }
}

TEST_CASE("Scope encoding and empty geometry failures remain typed and recoverable", "[recovery]") {
    struct Fixture { std::string bytes; const wchar_t* extension; import_broker::ImportFormat format; model_core::ImportErrorCode error; };
    const Fixture fixtures[] = {
        {"solid triangle\nendsolid triangle\n",L".stl",import_broker::ImportFormat::Stl,model_core::ImportErrorCode::UnsupportedEncoding},
        {"ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n0 0 0\n",L".ply",import_broker::ImportFormat::Ply,model_core::ImportErrorCode::UnsupportedEncoding},
        {std::string(84,'\0'),L".stl",import_broker::ImportFormat::Stl,model_core::ImportErrorCode::EmptyGeometry},
        {"ply\nformat binary_little_endian 1.0\nelement vertex 0\nproperty float x\nproperty float y\nproperty float z\nend_header\n",L".ply",import_broker::ImportFormat::Ply,model_core::ImportErrorCode::EmptyGeometry},
        {R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[]}]})",L".gltf",import_broker::ImportFormat::Gltf,model_core::ImportErrorCode::EmptyGeometry},
        {R"({"asset":{"version":"2.0"},"extensionsRequired":["UNSUPPORTED_private"]})",L".gltf",import_broker::ImportFormat::Gltf,model_core::ImportErrorCode::UnsupportedRequiredFeature},
        {"not a glTF",L".glb",import_broker::ImportFormat::Gltf,model_core::ImportErrorCode::MalformedData}
    };
    for (const auto& fixture : fixtures) {
        CAPTURE(fixture.extension,fixture.error);
        ScratchSource source(fixture.bytes,fixture.extension); auto request=Request(source.path.wstring()); request.format=fixture.format;
        auto result=import_broker::RunImportSession(request);
        REQUIRE_FALSE(result.ok); CHECK(result.stage==import_broker::ImportStage::WorkerReportedError); CHECK(result.errorCode==fixture.error);
        CHECK(result.errorPhase==model_core::ImportFailurePhase::Geometry);
        std::wstring summary,details; d3d12_import_bridge::DescribeSessionFailure(result,summary,details);
        CHECK_FALSE(summary.empty()); CHECK_FALSE(details.empty());
        CHECK(import_broker::RunImportSession(Request(Triangle())).ok);
    }
}

TEST_CASE("Worker error notices reject stale IDs unknown codes phases and lengths", "[recovery][hostile-worker]") {
    for (auto mode : {L"--error-stale",L"--error-unknown",L"--error-none",L"--error-phase",L"--error-short"}) {
        CAPTURE(mode); auto request=Request(Triangle()); request.workerExePath=sandbox_test_support::HostileWorkerExePath(); request.workerArgumentsOverride=mode;
        auto result=import_broker::RunImportSession(request);
        CHECK_FALSE(result.ok); CHECK(result.stage==import_broker::ImportStage::UnexpectedReply); CHECK(result.errorCode==model_core::ImportErrorCode::ImportProtocolViolation);
    }
    auto request=Request(Triangle()); request.workerExePath=sandbox_test_support::HostileWorkerExePath(); request.workerArgumentsOverride=L"--error-cancel";
    auto result=import_broker::RunImportSession(request);
    CHECK(result.stage==import_broker::ImportStage::Cancelled); CHECK(result.errorCode==model_core::ImportErrorCode::Cancelled);
}

TEST_CASE("Status facts reject unknown flags excessive counts reserved values and duplicates", "[recovery][hostile-worker]") {
    for (auto mode : {L"--status-flags",L"--status-count",L"--status-reserved",L"--status-duplicate"}) {
        CAPTURE(mode); auto request=Request(Triangle()); request.workerExePath=sandbox_test_support::HostileWorkerExePath(); request.workerArgumentsOverride=mode;
        auto result=import_broker::RunImportSession(request);
        CHECK_FALSE(result.ok); CHECK(result.stage==import_broker::ImportStage::ValidateSection); CHECK(result.errorCode==model_core::ImportErrorCode::ImportProtocolViolation);
    }
}

TEST_CASE("Worker hang and crash return typed failures and permit a valid reopen", "[recovery]") {
    auto request=Request(Triangle()); request.workerArgumentsOverride=L"--test-hang-import"; request.replyTimeoutMs=200;
    auto result=import_broker::RunImportSession(request);
    CHECK(result.stage==import_broker::ImportStage::ReplyTimedOut); CHECK(result.errorCode==model_core::ImportErrorCode::WorkerTimedOut);
    request.workerArgumentsOverride=L"--child-noop"; request.replyTimeoutMs=5000;
    result=import_broker::RunImportSession(request);
    CHECK_FALSE(result.ok); CHECK(result.errorCode==model_core::ImportErrorCode::WorkerCrashed);
    CHECK(import_broker::RunImportSession(Request(Triangle())).ok);
}

TEST_CASE("Copied diagnostics name source format and phase without source paths", "[recovery]") {
    d3d12_import_bridge::ImportResult result;
    result.errorCode=model_core::ImportErrorCode::UnsafeReference; result.errorStage=import_broker::ImportStage::WorkerReportedError;
    result.errorPhase=model_core::ImportFailurePhase::Sidecars;
    import_broker::ImportSessionResult session;session.stage=result.errorStage;session.errorCode=result.errorCode;
    d3d12_import_bridge::DescribeSessionFailure(session,result.errorSummary,result.errorDetails);
    auto text=d3d12_import_bridge::DiagnosticDetails(L"C:\\private-user\\secret-model.GlTf",result);
    CHECK(text.find(L"Format: glTF")!=std::wstring::npos); CHECK(text.find(L"Phase: resolving sidecars")!=std::wstring::npos);
    CHECK(text.find(L"private-user")==std::wstring::npos); CHECK(text.find(L"secret-model")==std::wstring::npos);
    CHECK(d3d12_import_bridge::SourceFormatLabel(L"C:\\private\\model.FBX")==L"FBX");
    CHECK(d3d12_import_bridge::SourceFormatLabel(L"C:\\private\\model.bad\npath")==L"Unknown");
}

TEST_CASE("Source write-time changes are detected through the pinned handle", "[recovery]") {
    std::ifstream input(std::filesystem::path(Triangle()),std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    ScratchSource source(bytes,L".glb");
    auto request=Request(source.path.wstring()); bool changed=false;
    request.onBatch=[&](auto) {
        platform::Win32Handle attributes(CreateFileW(source.path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        REQUIRE(attributes);
        FILETIME time{}; GetSystemTimeAsFileTime(&time); time.dwHighDateTime += 1;
        REQUIRE(SetFileTime(attributes.get(),nullptr,nullptr,&time)); changed=true;
    };
    auto result=import_broker::RunImportSession(request);
    REQUIRE(changed); CHECK_FALSE(result.ok); CHECK(result.errorCode==model_core::ImportErrorCode::FileChanged);
}

TEST_CASE("Optional feature warnings saturate at the protocol count cap", "[recovery]") {
    std::ifstream input(std::filesystem::path(Triangle()),std::ios::binary);
    std::string original((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    uint32_t jsonLength=0; std::memcpy(&jsonLength,original.data()+12,4);
    std::string json=original.substr(20,jsonLength);
    std::string extensions=",\"extensionsUsed\":[";
    for (unsigned i=0;i<100;++i) extensions+=(i ? "," : "")+std::string("\"UNSUPPORTED_optional_")+std::to_string(i)+"\"";
    extensions+="]"; json.insert(json.find_last_of('}'),extensions);
    while (json.size()%4) json+=' ';
    std::string bytes=original.substr(0,20)+json+original.substr(20+jsonLength);
    uint32_t length=uint32_t(bytes.size()), newJsonLength=uint32_t(json.size());
    std::memcpy(bytes.data()+8,&length,4);std::memcpy(bytes.data()+12,&newJsonLength,4);
    ScratchSource source(bytes,L".glb"); auto result=import_broker::RunImportSession(Request(source.path.wstring()));
    REQUIRE(result.ok); bool haveStatus=false;
    for (const auto& chunk : result.chunks) if (chunk.descriptor.topology==model_core::ChunkTopology::ImportStatus) {
        model_core::ImportStatusPayload status{};std::memcpy(&status,chunk.payload.data(),sizeof(status));
        CHECK(status.optionalFeatureWarnings==64);haveStatus=true;
    }
    CHECK(haveStatus);
}

TEST_CASE("Zero-byte Tier A sources report an empty document before worker launch", "[recovery]") {
    for (auto extension : {L".glb",L".gltf",L".stl",L".ply"}) {
        ScratchSource source("",extension);
        auto result=import_broker::RunImportSession(Request(source.path.wstring()));
        CHECK_FALSE(result.ok); CHECK(result.stage==import_broker::ImportStage::OpenSource);
        CHECK(result.errorCode==model_core::ImportErrorCode::EmptyGeometry);
    }
}
