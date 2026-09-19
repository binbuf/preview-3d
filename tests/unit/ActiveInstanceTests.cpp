#include "ActiveInstance.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <array>
#include <filesystem>
#include <thread>

using namespace active_instance;

namespace {
#pragma pack(push,1)
struct RawHeader { std::uint32_t magic; std::uint16_t version; std::uint16_t type; std::uint32_t payloadBytes; GUID correlation; };
#pragma pack(pop)

bool SendRejectedRaw(const std::wstring& pipeName,RawHeader header)
{
    HANDLE pipe=INVALID_HANDLE_VALUE;
    const ULONGLONG deadline=GetTickCount64()+2000;
    while(GetTickCount64()<deadline){
        WaitNamedPipeW(pipeName.c_str(),100);
        pipe=CreateFileW(pipeName.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        if(pipe!=INVALID_HANDLE_VALUE)break;
        Sleep(10);
    }
    if(pipe==INVALID_HANDLE_VALUE)return false;
    DWORD mode=PIPE_READMODE_MESSAGE;SetNamedPipeHandleState(pipe,&mode,nullptr,nullptr);
    DWORD written=0;bool ok=WriteFile(pipe,&header,sizeof(header),&written,nullptr)&&written==sizeof(header);
    std::array<char,512> response{};DWORD read=0;
    if(ok)ok=ReadFile(pipe,response.data(),static_cast<DWORD>(response.size()),&read,nullptr)!=FALSE;
    if(ok){const char receipt=1;WriteFile(pipe,&receipt,1,&written,nullptr);}
    CloseHandle(pipe);
    return ok && std::string_view(response.data(),read).find("\"accepted\":false")!=std::string_view::npos;
}
}

TEST_CASE("Activation payload round-trips Unicode and JSON metacharacters", "[activation]")
{
    std::wstring normalized, error;
    const auto input=(std::filesystem::current_path()/L"models ☃"/L"quote\"slash\\.glb").wstring();
    REQUIRE(NormalizeForwardPath(input,normalized,error));
    Command source{CommandType::Open,normalized,true};
    std::vector<std::uint8_t> bytes;
    REQUIRE(EncodePayload(source,bytes,error));
    Command decoded;
    REQUIRE(DecodePayload(CommandType::Open,bytes.data(),bytes.size(),decoded,error));
    CHECK(decoded.path==normalized);
    CHECK(decoded.activate);
}

TEST_CASE("Activation payload validation is strict and bounded", "[activation]")
{
    Command command; std::wstring error;
    const std::string unknown=R"({"path":"C:\\model.glb","activate":true,"extra":1})";
    CHECK_FALSE(DecodePayload(CommandType::Open,reinterpret_cast<const std::uint8_t*>(unknown.data()),unknown.size(),command,error));
    const std::string reordered=R"({"activate":true,"path":"C:\\model.glb"})";
    CHECK(DecodePayload(CommandType::Open,reinterpret_cast<const std::uint8_t*>(reordered.data()),reordered.size(),command,error));
    const std::string duplicate=R"({"path":"C:\\model.glb","path":"C:\\other.glb"})";
    CHECK_FALSE(DecodePayload(CommandType::Open,reinterpret_cast<const std::uint8_t*>(duplicate.data()),duplicate.size(),command,error));
    const std::string invalidUtf8{"{\"path\":\"C:\\\\bad", 16};
    CHECK_FALSE(DecodePayload(CommandType::Open,reinterpret_cast<const std::uint8_t*>(invalidUtf8.data()),invalidUtf8.size(),command,error));
    std::vector<std::uint8_t> oversized(kMaximumPayloadBytes+1,'x');
    CHECK_FALSE(DecodePayload(CommandType::Open,oversized.data(),oversized.size(),command,error));
    const std::uint8_t junk=1;
    CHECK_FALSE(DecodePayload(CommandType::Activate,&junk,1,command,error));
    CHECK(DecodePayload(CommandType::Activate,nullptr,0,command,error));
}

TEST_CASE("Forward paths are absolute local supported-model paths", "[activation]")
{
    std::wstring path,error;
    REQUIRE(NormalizeForwardPath(L"relative model.ply",path,error));
    CHECK(std::filesystem::path(path).is_absolute());
    CHECK_FALSE(NormalizeForwardPath(L"\\\\server\\share\\model.glb",path,error));
    REQUIRE(NormalizeForwardPath(L"model.FBX",path,error));
    CHECK(std::filesystem::path(path).is_absolute());
    REQUIRE(NormalizeForwardPath(L"model.3MF",path,error));
    CHECK(std::filesystem::path(path).is_absolute());
    for (const auto* usd : {L"model.USD", L"model.usda", L"model.USDC", L"model.usdz"}) {
        REQUIRE(NormalizeForwardPath(usd,path,error));
        CHECK(std::filesystem::path(path).is_absolute());
    }
    for (const auto* step : {L"model.STEP", L"model.stp"}) {
        REQUIRE(NormalizeForwardPath(step,path,error));
        CHECK(std::filesystem::path(path).is_absolute());
    }
    CHECK_FALSE(NormalizeForwardPath(L"model.iges",path,error));
}

TEST_CASE("Session coordinator forwards and survives a close relaunch race", "[activation]")
{
    std::wstring error;
    Coordinator primary;
    REQUIRE(primary.Initialize(false,error)==Coordinator::Role::Primary);
    HWND target=CreateWindowExW(0,L"STATIC",L"activation-test",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    REQUIRE(target!=nullptr);
    REQUIRE(primary.StartListener(target,WM_APP+77,error));

    Coordinator secondary;
    Coordinator::Role secondaryRole=Coordinator::Role::Failed;
    std::thread secondaryElection([&]{ secondaryRole=secondary.Initialize(false,error); });
    secondaryElection.join();
    REQUIRE(secondaryRole==Coordinator::Role::Secondary);
    std::wstring path;
    REQUIRE(NormalizeForwardPath((std::filesystem::current_path()/L"forwarded.glb").wstring(),path,error));
    const bool forwarded=secondary.Forward(Command{CommandType::Open,path,true},error);
    std::string narrowError; for (wchar_t ch:error) narrowError.push_back(ch<128?static_cast<char>(ch):'?');
    INFO(narrowError);
    REQUIRE(forwarded);
    auto commands=primary.Drain();
    REQUIRE(commands.size()==1);
    CHECK(commands[0].path==path);
    RawHeader malformed{kActivationMagic,99,static_cast<std::uint16_t>(CommandType::Open),0,{}};
    REQUIRE(SendRejectedRaw(primary.PipeNameForTesting(),malformed));
    RawHeader oversized{kActivationMagic,kActivationVersion,static_cast<std::uint16_t>(CommandType::Open),kMaximumPayloadBytes+1,{}};
    REQUIRE(SendRejectedRaw(primary.PipeNameForTesting(),oversized));
    secondary.Stop();
    primary.Stop();
    DestroyWindow(target);

    Coordinator relaunched;
    REQUIRE(relaunched.Initialize(false,error)==Coordinator::Role::Primary);
    relaunched.Stop();
}
