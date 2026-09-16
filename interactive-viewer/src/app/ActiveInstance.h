#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace active_instance
{
constexpr std::uint32_t kActivationMagic = 0x49443350; // "P3DI", little endian.
constexpr std::uint16_t kActivationVersion = 1;
constexpr std::uint32_t kMaximumPayloadBytes = 256u * 1024u;

enum class CommandType : std::uint16_t
{
    Open = 1,
    Activate = 2,
};

struct Command
{
    CommandType type = CommandType::Activate;
    std::wstring path;
    bool activate = true;
};

// These pure protocol helpers are public so malformed/limit behavior can be
// covered without standing up a second GUI process.
bool EncodePayload(const Command& command, std::vector<std::uint8_t>& payload, std::wstring& error);
bool DecodePayload(CommandType type, const std::uint8_t* payload, std::size_t payloadBytes,
    Command& command, std::wstring& error);
bool NormalizeForwardPath(const std::wstring& input, std::wstring& absolutePath, std::wstring& error);

class Coordinator
{
public:
    enum class Role { Primary, Secondary, Bypassed, Failed };

    Coordinator() = default;
    ~Coordinator();
    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    Role Initialize(bool bypass, std::wstring& error);
    bool StartListener(HWND window, UINT message, std::wstring& error);
    bool Forward(const Command& command, std::wstring& error);
    std::vector<Command> Drain();
    void Stop();

    const std::wstring& PipeNameForTesting() const noexcept { return pipeName_; }

private:
    void ListenerMain(std::stop_token stop);
    bool Queue(Command command);

    Role role_ = Role::Failed;
    HANDLE mutex_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    std::wstring mutexName_;
    std::wstring readyName_;
    std::wstring pipeName_;
    std::vector<std::uint8_t> userSid_;
    DWORD sessionId_ = 0;
    HWND window_ = nullptr;
    UINT message_ = 0;
    std::jthread listener_;
    std::mutex queueMutex_;
    std::deque<Command> queue_;
};
}
