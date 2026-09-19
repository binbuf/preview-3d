#include "ActiveInstance.h"

#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <memory>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

namespace active_instance
{
namespace
{
#pragma pack(push, 1)
struct FrameHeader
{
    std::uint32_t magic = kActivationMagic;
    std::uint16_t version = kActivationVersion;
    std::uint16_t type = 0;
    std::uint32_t payloadBytes = 0;
    GUID correlation{};
};
#pragma pack(pop)
static_assert(sizeof(FrameHeader) == 28);

constexpr DWORD kActivationTimeoutMs = 1000;
constexpr std::size_t kMaximumQueuedCommands = 16;

struct LocalFreeDeleter { void operator()(void* value) const noexcept { if (value) LocalFree(value); } };
using LocalMemory = std::unique_ptr<void, LocalFreeDeleter>;

bool CurrentIdentity(std::vector<std::uint8_t>& sid, DWORD& sessionId, std::wstring& error)
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        error = L"Windows could not identify the current user."; return false;
    }
    std::unique_ptr<void, decltype(&CloseHandle)> token(rawToken, CloseHandle);
    DWORD bytes = 0;
    GetTokenInformation(rawToken, TokenUser, nullptr, 0, &bytes);
    sid.resize(bytes);
    if (!bytes || !GetTokenInformation(rawToken, TokenUser, sid.data(), bytes, &bytes)) {
        error = L"Windows could not read the current user identity."; return false;
    }
    DWORD sessionBytes = sizeof(sessionId);
    if (!GetTokenInformation(rawToken, TokenSessionId, &sessionId, sessionBytes, &sessionBytes)) {
        error = L"Windows could not read the current session identity."; return false;
    }
    return true;
}

bool Sha256(const void* bytes, ULONG byteCount, std::array<std::uint8_t, 32>& digest)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    const auto closeAlgorithm = [&] { BCryptCloseAlgorithmProvider(algorithm, 0); };
    DWORD objectBytes = 0, resultBytes = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
        sizeof(objectBytes), &resultBytes, 0) < 0) { closeAlgorithm(); return false; }
    std::vector<std::uint8_t> object(objectBytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0) {
        closeAlgorithm(); return false;
    }
    const bool ok = BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(bytes)), byteCount, 0) >= 0
        && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    BCryptDestroyHash(hash);
    closeAlgorithm();
    return ok;
}

std::wstring IdentitySuffix(const std::vector<std::uint8_t>& tokenUser, DWORD sessionId)
{
    const auto* user = reinterpret_cast<const TOKEN_USER*>(tokenUser.data());
    const DWORD sidBytes = GetLengthSid(user->User.Sid);
    std::array<std::uint8_t, 32> digest{};
    if (!Sha256(user->User.Sid, sidBytes, digest)) return {};
    static constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring hash;
    for (std::size_t i = 0; i < 12; ++i) {
        hash.push_back(hex[digest[i] >> 4]); hash.push_back(hex[digest[i] & 15]);
    }
    return std::to_wstring(sessionId) + L"." + hash;
}

bool SecurityForCurrentUser(const std::vector<std::uint8_t>& tokenUser, SECURITY_ATTRIBUTES& attributes,
    LocalMemory& descriptor, std::wstring& error)
{
    const auto* user = reinterpret_cast<const TOKEN_USER*>(tokenUser.data());
    LPWSTR sidString = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sidString)) {
        error = L"Windows could not secure the activation channel."; return false;
    }
    LocalMemory sidHolder(sidString);
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sidString) + L")";
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw, nullptr)) {
        error = L"Windows could not secure the activation channel."; return false;
    }
    descriptor.reset(raw);
    attributes = { sizeof(attributes), raw, FALSE };
    return true;
}

bool Utf8FromWide(std::wstring_view text, std::string& utf8)
{
    if (text.size() > INT_MAX) return false;
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return text.empty();
    utf8.resize(bytes);
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        utf8.data(), bytes, nullptr, nullptr) == bytes;
}

bool WideFromUtf8(std::string_view utf8, std::wstring& text)
{
    if (utf8.size() > INT_MAX) return false;
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (chars <= 0) return utf8.empty();
    text.resize(chars);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), text.data(), chars) == chars;
}

void AppendJsonString(std::string_view value, std::string& output)
{
    output.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (ch < 0x20) {
                output += "\\u00"; output.push_back(hex[ch >> 4]); output.push_back(hex[ch & 15]);
            } else output.push_back(static_cast<char>(ch));
        }
    }
    output.push_back('"');
}

class JsonReader
{
public:
    explicit JsonReader(std::string_view input) : input_(input) {}
    void Space() { while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\t' || input_[position_] == '\r' || input_[position_] == '\n')) ++position_; }
    bool Take(char ch) { Space(); if (position_ >= input_.size() || input_[position_] != ch) return false; ++position_; return true; }
    bool End() { Space(); return position_ == input_.size(); }
    bool Literal(std::string_view value) { Space(); if (input_.substr(position_, value.size()) != value) return false; position_ += value.size(); return true; }
    bool String(std::string& output)
    {
        Space(); if (position_ >= input_.size() || input_[position_++] != '"') return false;
        while (position_ < input_.size()) {
            const unsigned char ch = input_[position_++];
            if (ch == '"') return true;
            if (ch < 0x20) return false;
            if (ch != '\\') { output.push_back(static_cast<char>(ch)); continue; }
            if (position_ >= input_.size()) return false;
            const char escape = input_[position_++];
            switch (escape) {
            case '"': case '\\': case '/': output.push_back(escape); break;
            case 'b': output.push_back('\b'); break; case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break; case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                if (position_ + 4 > input_.size()) return false;
                unsigned value = 0;
                for (int i = 0; i < 4; ++i) {
                    const char digit = input_[position_++];
                    value <<= 4;
                    if (digit >= '0' && digit <= '9') value += digit - '0';
                    else if (digit >= 'a' && digit <= 'f') value += digit - 'a' + 10;
                    else if (digit >= 'A' && digit <= 'F') value += digit - 'A' + 10;
                    else return false;
                }
                if (value >= 0xD800 && value <= 0xDFFF) return false; // Encoder never emits surrogate escapes.
                if (value < 0x80) output.push_back(static_cast<char>(value));
                else if (value < 0x800) { output.push_back(static_cast<char>(0xC0 | value >> 6)); output.push_back(static_cast<char>(0x80 | value & 63)); }
                else { output.push_back(static_cast<char>(0xE0 | value >> 12)); output.push_back(static_cast<char>(0x80 | value >> 6 & 63)); output.push_back(static_cast<char>(0x80 | value & 63)); }
                break;
            }
            default: return false;
            }
        }
        return false;
    }
private:
    std::string_view input_;
    std::size_t position_ = 0;
};

bool IsSupportedExtension(const std::wstring& path)
{
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(), std::towlower);
    return extension == L".glb" || extension == L".gltf" || extension == L".stl" ||
        extension == L".ply" || extension == L".obj" || extension == L".fbx" ||
        extension == L".3mf" || extension == L".usd" || extension == L".usda" || extension == L".usdc" ||
        extension == L".usdz" || extension == L".step" || extension == L".stp";
}

bool WaitOverlapped(HANDLE object, OVERLAPPED& operation, HANDLE stopEvent, DWORD timeout, DWORD& transferred,
    bool allowMoreData = false)
{
    const HANDLE waits[] = { operation.hEvent, stopEvent };
    const DWORD count = stopEvent ? 2 : 1;
    const DWORD wait = WaitForMultipleObjects(count, waits, FALSE, timeout);
    if (wait != WAIT_OBJECT_0) { CancelIoEx(object, &operation); WaitForSingleObject(operation.hEvent, 100); return false; }
    if (GetOverlappedResult(object, &operation, &transferred, FALSE)) return true;
    return allowMoreData && GetLastError() == ERROR_MORE_DATA;
}

bool Transfer(HANDLE pipe, bool write, void* bytes, DWORD byteCount, HANDLE stopEvent, DWORD timeout,
    DWORD& transferred, bool allowMoreData = false)
{
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return false;
    const BOOL started = write ? WriteFile(pipe, bytes, byteCount, &transferred, &operation)
                               : ReadFile(pipe, bytes, byteCount, &transferred, &operation);
    bool ok = started != FALSE;
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = WaitOverlapped(pipe, operation, stopEvent, timeout, transferred, allowMoreData);
    else if (!ok && allowMoreData && GetLastError() == ERROR_MORE_DATA) ok = true;
    CloseHandle(operation.hEvent);
    return ok;
}

bool AuthenticateClient(HANDLE pipe, const std::vector<std::uint8_t>& expectedUser, DWORD expectedSession)
{
    ULONG clientPid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &clientPid) || !clientPid) return false;
    if (!ImpersonateNamedPipeClient(pipe)) return false;
    HANDLE rawToken = nullptr;
    const bool opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &rawToken) != FALSE;
    RevertToSelf();
    if (!opened) return false;
    std::unique_ptr<void, decltype(&CloseHandle)> token(rawToken, CloseHandle);
    DWORD bytes = 0;
    GetTokenInformation(rawToken, TokenUser, nullptr, 0, &bytes);
    std::vector<std::uint8_t> actual(bytes);
    DWORD session = 0;
    if (!bytes || !GetTokenInformation(rawToken, TokenUser, actual.data(), bytes, &bytes)
        || !ProcessIdToSessionId(clientPid, &session)) return false;
    const auto* expected = reinterpret_cast<const TOKEN_USER*>(expectedUser.data());
    const auto* found = reinterpret_cast<const TOKEN_USER*>(actual.data());
    return session == expectedSession && EqualSid(expected->User.Sid, found->User.Sid);
}
}

bool EncodePayload(const Command& command, std::vector<std::uint8_t>& payload, std::wstring& error)
{
    payload.clear();
    if (command.type == CommandType::Activate) return true;
    if (command.type != CommandType::Open || command.path.empty()) { error = L"The activation command is invalid."; return false; }
    std::string path;
    if (!Utf8FromWide(command.path, path)) { error = L"The model path is not valid Unicode."; return false; }
    std::string json = "{\"path\":";
    AppendJsonString(path, json);
    json += command.activate ? ",\"activate\":true}" : ",\"activate\":false}";
    if (json.size() > kMaximumPayloadBytes) { error = L"The activation request is too large."; return false; }
    payload.assign(json.begin(), json.end());
    return true;
}

bool DecodePayload(CommandType type, const std::uint8_t* payload, std::size_t payloadBytes,
    Command& command, std::wstring& error)
{
    command = {};
    command.type = type;
    if (payloadBytes > kMaximumPayloadBytes || (payloadBytes && !payload)) { error = L"The activation request is too large."; return false; }
    if (type == CommandType::Activate) {
        if (payloadBytes != 0) { error = L"Activate does not accept a payload."; return false; }
        return true;
    }
    if (type != CommandType::Open || payloadBytes == 0
        || std::find(payload, payload + payloadBytes, std::uint8_t{0}) != payload + payloadBytes) {
        error = L"The activation request is malformed."; return false;
    }
    JsonReader reader(std::string_view(reinterpret_cast<const char*>(payload), payloadBytes));
    std::string path;
    bool havePath = false;
    bool haveActivate = false;
    bool activate = true;
    if (!reader.Take('{')) { error = L"The activation request is malformed."; return false; }
    for (int field = 0; field < 2; ++field) {
        std::string key;
        if (!reader.String(key) || !reader.Take(':')) { error = L"The activation request is malformed."; return false; }
        if (key == "path" && !havePath) {
            if (!reader.String(path)) { error = L"The activation request is malformed."; return false; }
            havePath = true;
        } else if (key == "activate" && !haveActivate) {
            if (!(reader.Literal("true") ? (activate = true) : reader.Literal("false") ? (activate = false) : false)) {
                error = L"The activation request is malformed."; return false;
            }
            haveActivate = true;
        } else {
            error = L"The activation request is malformed."; return false;
        }
        if (field == 0 && !reader.Take(',')) { error = L"The activation request is malformed."; return false; }
    }
    if (!reader.Take('}') || !reader.End() || !havePath || !haveActivate
        || !WideFromUtf8(path, command.path) || command.path.empty()) {
        error = L"The activation request is malformed."; return false;
    }
    command.activate = activate;
    std::wstring normalized;
    if (!NormalizeForwardPath(command.path, normalized, error) || normalized != command.path) return false;
    return true;
}

bool NormalizeForwardPath(const std::wstring& input, std::wstring& absolutePath, std::wstring& error)
{
    absolutePath.clear();
    if (input.empty() || input.find(L'\0') != std::wstring::npos) { error = L"Specify one local model path."; return false; }
    const DWORD required = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (!required || required > 32768) { error = L"The model path is invalid or too long."; return false; }
    std::wstring resolved(required, L'\0');
    const DWORD length = GetFullPathNameW(input.c_str(), required, resolved.data(), nullptr);
    if (!length || length >= required) { error = L"The model path could not be resolved."; return false; }
    resolved.resize(length);
    const bool drivePath = resolved.size() >= 3 && std::iswalpha(resolved[0]) && resolved[1] == L':' && resolved[2] == L'\\';
    const bool extendedDrive = resolved.size() >= 7 && resolved.rfind(L"\\\\?\\", 0) == 0
        && std::iswalpha(resolved[4]) && resolved[5] == L':' && resolved[6] == L'\\';
    if (!drivePath && !extendedDrive) { error = L"Only local drive paths can be activated."; return false; }
    if (!IsSupportedExtension(resolved)) {
        error = L"Open a .glb, .gltf, .stl, .ply, .obj, .fbx, .3mf, .usd, .usda, .usdc, .usdz, .step, or .stp file.";
        return false;
    }
    absolutePath = std::move(resolved);
    return true;
}

Coordinator::~Coordinator() { Stop(); }

Coordinator::Role Coordinator::Initialize(bool bypass, std::wstring& error)
{
    if (bypass) return role_ = Role::Bypassed;
    if (!CurrentIdentity(userSid_, sessionId_, error)) return role_ = Role::Failed;
    const std::wstring suffix = IdentitySuffix(userSid_, sessionId_);
    if (suffix.empty()) { error = L"Windows could not derive the activation identity."; return role_ = Role::Failed; }
    mutexName_ = L"Local\\Binbuf.Preview3D." + suffix;
    readyName_ = mutexName_ + L".Ready";
    pipeName_ = L"\\\\.\\pipe\\Binbuf.Preview3D." + suffix;

    SECURITY_ATTRIBUTES attributes{}; LocalMemory descriptor;
    if (!SecurityForCurrentUser(userSid_, attributes, descriptor, error)) return role_ = Role::Failed;
    mutex_ = CreateMutexW(&attributes, TRUE, mutexName_.c_str());
    if (!mutex_) { error = L"Windows could not create the application instance lock."; return role_ = Role::Failed; }
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        readyEvent_ = CreateEventW(&attributes, TRUE, FALSE, readyName_.c_str());
        if (!readyEvent_) { error = L"Windows could not create the activation ready event."; return role_ = Role::Failed; }
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return role_ = Role::Primary;
    }
    const DWORD acquired = WaitForSingleObject(mutex_, 0);
    if (acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED) {
        readyEvent_ = CreateEventW(&attributes, TRUE, FALSE, readyName_.c_str());
        if (!readyEvent_) { error = L"Windows could not recover the activation ready event."; return role_ = Role::Failed; }
        ResetEvent(readyEvent_);
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return role_ = Role::Primary;
    }
    return role_ = Role::Secondary;
}

bool Coordinator::StartListener(HWND window, UINT message, std::wstring& error)
{
    if (role_ == Role::Bypassed) return true;
    if (role_ != Role::Primary || !window || !message || listener_.joinable()) { error = L"The activation listener could not be started."; return false; }
    window_ = window; message_ = message;
    listener_ = std::jthread([this](std::stop_token stop) { ListenerMain(stop); });
    return true;
}

bool Coordinator::Queue(Command command)
{
    std::scoped_lock lock(queueMutex_);
    if (command.type == CommandType::Open) {
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const Command& item) { return item.type == CommandType::Open; }), queue_.end());
    } else if (std::any_of(queue_.begin(), queue_.end(), [](const Command& item) { return item.type == CommandType::Activate; })) return true;
    if (queue_.size() >= kMaximumQueuedCommands) queue_.pop_front();
    queue_.push_back(std::move(command));
    return PostMessageW(window_, message_, 0, 0) != FALSE;
}

void Coordinator::ListenerMain(std::stop_token stop)
{
    SECURITY_ATTRIBUTES attributes{}; LocalMemory descriptor; std::wstring ignored;
    if (!SecurityForCurrentUser(userSid_, attributes, descriptor, ignored)) return;
    bool published = false;
    while (!stop.stop_requested() && WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
        HANDLE pipe = CreateNamedPipeW(pipeName_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, kMaximumPayloadBytes + sizeof(FrameHeader), kActivationTimeoutMs, &attributes);
        if (pipe == INVALID_HANDLE_VALUE) return;
        if (!published) { SetEvent(readyEvent_); published = true; }

        OVERLAPPED connection{}; connection.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ConnectNamedPipe(pipe, &connection);
        bool accepted = connected != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!accepted && GetLastError() == ERROR_IO_PENDING) {
            DWORD transferred = 0; accepted = WaitOverlapped(pipe, connection, stopEvent_, INFINITE, transferred);
        }
        CloseHandle(connection.hEvent);
        if (!accepted) { CloseHandle(pipe); continue; }

        std::vector<std::uint8_t> frame(sizeof(FrameHeader) + kMaximumPayloadBytes);
        DWORD read = 0;
        const char* rejection = "read";
        bool valid = Transfer(pipe, false, frame.data(), static_cast<DWORD>(frame.size()), stopEvent_, kActivationTimeoutMs, read, true)
            && read >= sizeof(FrameHeader);
        // Named-pipe impersonation is defined against the client's last
        // completed write, so receive the bounded opaque frame before
        // authenticating, but do not inspect or parse it until this succeeds.
        if (valid) { rejection = "authentication"; valid = AuthenticateClient(pipe, userSid_, sessionId_); }
        FrameHeader header{};
        Command command;
        if (valid) {
            std::memcpy(&header, frame.data(), sizeof(header));
            rejection = "frame";
            valid = header.magic == kActivationMagic && header.version == kActivationVersion
                && header.payloadBytes <= kMaximumPayloadBytes && read == sizeof(header) + header.payloadBytes;
        }
        if (valid) { rejection = "payload"; valid = DecodePayload(static_cast<CommandType>(header.type), frame.data() + sizeof(header), header.payloadBytes, command, ignored); }
        if (valid) { rejection = "queue"; valid = Queue(std::move(command)); }

        FrameHeader response{}; response.type = static_cast<std::uint16_t>(header.type | 0x8000u); response.correlation = header.correlation;
        const std::string rejectedBody = std::string("{\"accepted\":false,\"status\":\"rejected-") + rejection + "\"}";
        const char* body = valid ? "{\"accepted\":true,\"status\":\"queued\"}" : rejectedBody.c_str();
        response.payloadBytes = static_cast<std::uint32_t>(std::strlen(body));
        std::vector<std::uint8_t> reply(sizeof(response) + response.payloadBytes);
        std::memcpy(reply.data(), &response, sizeof(response)); std::memcpy(reply.data() + sizeof(response), body, response.payloadBytes);
        DWORD written = 0;
        if (Transfer(pipe, true, reply.data(), static_cast<DWORD>(reply.size()), stopEvent_, kActivationTimeoutMs, written)) {
            // A one-byte receipt keeps DisconnectNamedPipe from discarding a
            // response that the client has not consumed yet. It is bounded
            // like every other operation, unlike FlushFileBuffers on a pipe.
            std::uint8_t receipt = 0; DWORD receiptBytes = 0;
            Transfer(pipe, false, &receipt, 1, stopEvent_, kActivationTimeoutMs, receiptBytes);
        }
        DisconnectNamedPipe(pipe); CloseHandle(pipe);
    }
}

bool Coordinator::Forward(const Command& command, std::wstring& error)
{
    if (role_ != Role::Secondary) { error = L"There is no existing viewer instance to activate."; return false; }
    HANDLE ready = OpenEventW(SYNCHRONIZE, FALSE, readyName_.c_str());
    if (!ready || WaitForSingleObject(ready, kActivationTimeoutMs) != WAIT_OBJECT_0) {
        if (ready) CloseHandle(ready); error = L"3D Preview is not responding."; return false;
    }
    CloseHandle(ready);
    if (!WaitNamedPipeW(pipeName_.c_str(), kActivationTimeoutMs)) { error = L"3D Preview is not responding."; return false; }
    HANDLE pipe = CreateFileW(pipeName_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { error = L"3D Preview could not accept the activation request."; return false; }
    DWORD mode = PIPE_READMODE_MESSAGE; SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
    std::vector<std::uint8_t> payload;
    if (!EncodePayload(command, payload, error)) { CloseHandle(pipe); return false; }
    FrameHeader header{}; header.type = static_cast<std::uint16_t>(command.type); header.payloadBytes = static_cast<std::uint32_t>(payload.size());
    CoCreateGuid(&header.correlation);
    std::vector<std::uint8_t> frame(sizeof(header) + payload.size());
    std::memcpy(frame.data(), &header, sizeof(header));
    if (!payload.empty()) std::memcpy(frame.data() + sizeof(header), payload.data(), payload.size());
    DWORD transferred = 0;
    bool ok = Transfer(pipe, true, frame.data(), static_cast<DWORD>(frame.size()), nullptr, kActivationTimeoutMs, transferred)
        && transferred == frame.size();
    const bool writeOk=ok;
    std::array<std::uint8_t, 4096> reply{}; DWORD read = 0;
    if (ok) ok = Transfer(pipe, false, reply.data(), static_cast<DWORD>(reply.size()), nullptr, kActivationTimeoutMs, read, true);
    if (ok) { std::uint8_t receipt=1; DWORD receiptBytes=0; Transfer(pipe,true,&receipt,1,nullptr,kActivationTimeoutMs,receiptBytes); }
    CloseHandle(pipe);
    if (ok && read >= sizeof(FrameHeader)) {
        FrameHeader response{}; std::memcpy(&response, reply.data(), sizeof(response));
        const std::string_view body(reinterpret_cast<const char*>(reply.data() + sizeof(response)), read - sizeof(response));
        ok = response.magic == kActivationMagic && response.version == kActivationVersion
            && response.type == (static_cast<std::uint16_t>(command.type) | 0x8000u)
            && response.payloadBytes == body.size() && IsEqualGUID(response.correlation, header.correlation)
            && body.find("\"accepted\":true") != std::string_view::npos;
    }
    if (!ok) {
        error = L"3D Preview rejected the activation request.";
        if (!writeOk) error += L" (write)";
        else if (read < sizeof(FrameHeader)) error += L" (response)";
        if (read > sizeof(FrameHeader)) {
            const std::string_view body(reinterpret_cast<const char*>(reply.data()+sizeof(FrameHeader)),read-sizeof(FrameHeader));
            const auto marker=body.find("rejected-");
            if (marker!=std::string_view::npos) {
                const auto end=body.find('"',marker);
                std::wstring detail(body.begin()+marker+9,end==std::string_view::npos?body.end():body.begin()+end);
                error += L" ("+detail+L")";
            }
        }
    }
    return ok;
}

std::vector<Command> Coordinator::Drain()
{
    std::vector<Command> result;
    std::scoped_lock lock(queueMutex_);
    result.assign(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
    queue_.clear();
    return result;
}

void Coordinator::Stop()
{
    if (stopEvent_) SetEvent(stopEvent_);
    if (listener_.joinable()) { listener_.request_stop(); listener_.join(); }
    if (readyEvent_) { ResetEvent(readyEvent_); CloseHandle(readyEvent_); readyEvent_ = nullptr; }
    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
    if (mutex_) {
        if (role_ == Role::Primary) ReleaseMutex(mutex_);
        CloseHandle(mutex_); mutex_ = nullptr;
    }
}
}
