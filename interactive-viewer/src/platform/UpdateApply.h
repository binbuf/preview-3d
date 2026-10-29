#pragma once

#include <string>

// Update handoff: verifies that this process is the per-machine installed copy
// and starts the non-elevated helper that waits for this process to exit, runs
// the verified installer silently, and reopens the app with the same model.
namespace preview3d_update
{
// True when the running executable's directory is the install location
// recorded by the per-machine installer. Portable copies return false and keep
// the existing "open the releases page" behavior.
bool IsInstalledCopy(std::wstring& installDirectory, std::wstring& error);

// Starts Apply-Preview3DUpdate.ps1 beside the executable. `expectedSha256` is
// re-checked by the helper immediately before it elevates setup. `openPath`
// may be empty; when it still names a file it is reopened after the update.
// Returns once the helper process is created; the caller then closes the app.
bool StartInstallerHandoff(const std::wstring& installerPath,
    const std::wstring& expectedSha256, const std::wstring& openPath,
    std::wstring& error);

// Exposed for tests: quotes one argument for a Windows command line.
std::wstring QuoteWindowsCommandLineArgument(const std::wstring& argument);
}
