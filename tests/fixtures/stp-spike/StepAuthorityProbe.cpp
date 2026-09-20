#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>

// STEP-001 test-only authority probe. Launched under the same zero-capability
// AppContainer as Preview3DStepSpike.exe to prove that a direct filesystem
// open, a network connect, and a child process all fail. It is never linked or
// launched by product code.

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* path = argc > 1 ? argv[1] : L"C:\\Windows\\win.ini";

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool fileDenied = file == INVALID_HANDLE_VALUE;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);

    WSADATA data{};
    bool networkDenied = true;
    if (WSAStartup(MAKEWORD(2, 2), &data) == 0) {
        SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket != INVALID_SOCKET) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(1);
            InetPtonW(AF_INET, L"127.0.0.1", &address.sin_addr);
            networkDenied = ::connect(socket, reinterpret_cast<sockaddr*>(&address),
                                      sizeof(address)) == SOCKET_ERROR;
            closesocket(socket);
        }
        WSACleanup();
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    wchar_t command[] = L"cmd.exe /c exit";
    const bool processDenied = !CreateProcessW(nullptr, command, nullptr, nullptr, FALSE,
                                               0, nullptr, nullptr, &startup, &process);
    if (!processDenied) {
        TerminateProcess(process.hProcess, 0);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }

    std::printf("file_denied=%d network_denied=%d process_denied=%d\n",
                fileDenied ? 1 : 0, networkDenied ? 1 : 0, processDenied ? 1 : 0);
    return (fileDenied && networkDenied && processDenied) ? 0 : 1;
}
