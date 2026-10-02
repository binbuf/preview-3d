#include "ContainmentProbes.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace import_worker {

namespace {

void ReportLine(const char* line, size_t length)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(out, line, static_cast<DWORD>(length), &written, nullptr);
}

void Report(const char* probeName, const char* status, DWORD code)
{
    char buffer[128];
    int length = _snprintf_s(buffer, _TRUNCATE, "%s=%s code=%lu\n", probeName, status, code);
    if (length > 0) {
        ReportLine(buffer, static_cast<size_t>(length));
    }
}

} // namespace

void RunFilesystemEscapeProbe(const wchar_t* canaryPath)
{
    HANDLE file = CreateFileW(canaryPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Report("FS_PROBE", "DENIED", GetLastError());
        return;
    }
    CloseHandle(file);
    Report("FS_PROBE", "UNEXPECTED_SUCCESS", 0);
}

void RunNetworkEscapeProbe(unsigned short port)
{
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        Report("NET_PROBE", "FAIL", GetLastError());
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        Report("NET_PROBE", "DENIED", static_cast<DWORD>(WSAGetLastError()));
        WSACleanup();
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int result = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (result == SOCKET_ERROR) {
        Report("NET_PROBE", "DENIED", static_cast<DWORD>(WSAGetLastError()));
    } else {
        Report("NET_PROBE", "UNEXPECTED_SUCCESS", 0);
    }

    closesocket(sock);
    WSACleanup();
}

void RunProcessSpawnEscapeProbe()
{
    wchar_t selfPath[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        Report("SPAWN_PROBE", "FAIL", GetLastError());
        return;
    }

    std::wstring cmdLine = L"\"" + std::wstring(selfPath) + L"\" --child-noop";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL created = CreateProcessW(selfPath, cmdLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                   nullptr, &si, &pi);
    if (created) {
        Report("SPAWN_PROBE", "UNEXPECTED_SUCCESS", 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return;
    }

    Report("SPAWN_PROBE", "DENIED", GetLastError());
}

void RunOverallocateProbe()
{
    constexpr SIZE_T chunkBytes = 4ULL * 1024 * 1024; // 4 MiB
    constexpr SIZE_T safetyBackstop = 4ULL * 1024 * 1024 * 1024; // never loop past 4 GiB
    SIZE_T totalCommitted = 0;

    for (;;) {
        void* p = VirtualAlloc(nullptr, chunkBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (p == nullptr) {
            Report("COMMIT_PROBE", "DENIED", GetLastError());
            return;
        }
        totalCommitted += chunkBytes;

        if (totalCommitted > safetyBackstop) {
            Report("COMMIT_PROBE", "FAIL", 0);
            return;
        }
    }
}

void RunHangProbe()
{
    Report("HANG_PROBE", "PASS", 0);
    HANDLE neverSignaled = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    WaitForSingleObject(neverSignaled, INFINITE);
}

void RunCpuSpinProbe()
{
    Report("CPU_PROBE", "PASS", 0);
    // A volatile accumulator keeps the loop from being optimized away and
    // consumes user-mode CPU without ever yielding, so a Job CPU-time limit
    // (not a wall-clock timer) is what must terminate this process.
    volatile unsigned long long accumulator = 0;
    for (;;) {
        accumulator = accumulator * 1103515245ull + 12345ull;
    }
}

void ReportDone()
{
    ReportLine("DONE\n", 5);
}

} // namespace import_worker
