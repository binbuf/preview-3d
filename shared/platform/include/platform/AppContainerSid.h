#pragma once

#include <windows.h>

#include <string>

namespace platform {

// Move-only RAII owner of an AppContainer profile SID.
class AppContainerSid {
public:
    AppContainerSid() noexcept = default;

    AppContainerSid(const AppContainerSid&) = delete;
    AppContainerSid& operator=(const AppContainerSid&) = delete;

    AppContainerSid(AppContainerSid&& other) noexcept;
    AppContainerSid& operator=(AppContainerSid&& other) noexcept;

    ~AppContainerSid();

    // Creates the named AppContainer profile and returns its SID. Process
    // creation under an AppContainer token expects the profile to exist on
    // disk/in the registry, which is why this calls CreateAppContainerProfile
    // rather than the side-effect-free DeriveAppContainerSidFromAppContainerName
    // -- that call is used here only as the ERROR_ALREADY_EXISTS fallback, to
    // recover the same SID when a prior run's profile was not cleaned up.
    static AppContainerSid CreateOrOpen(const std::wstring& containerName,
                                         const std::wstring& displayName,
                                         const std::wstring& description);

    // Removes the profile created by CreateOrOpen. Call in test teardown so
    // repeated runs don't accumulate entries under %LOCALAPPDATA%\Packages.
    static void Delete(const std::wstring& containerName);

    PSID get() const noexcept { return sid_; }
    explicit operator bool() const noexcept { return sid_ != nullptr; }

private:
    explicit AppContainerSid(PSID sid) noexcept
        : sid_(sid)
    {
    }

    PSID sid_ = nullptr;
};

// Adds (or removes) a read+execute ACE for `sid` on `directory`, with object
// and container inheritance so files inside it are covered too. Additive:
// every other ACE on the directory is preserved.
//
// This is a launch prerequisite, not a refinement: a zero-capability
// AppContainer process cannot load its own .exe unless its SID has
// read+execute on the directory holding it, so without the grant
// CreateProcessW fails at loader level with ERROR_ACCESS_DENIED before any
// product code runs.
//
// Grants exactly the one SID passed in -- never the machine-wide
// S-1-15-2-1 / S-1-15-2-2 ("ALL APPLICATION PACKAGES") groups, which would
// widen access to every AppContainer on the machine. An installed build
// provisions this per profile at install time instead; see
// .docs/design/08-installation-and-registration.md.
bool GrantDirectoryReadExecute(const std::wstring& directory, PSID sid);
bool RevokeDirectoryAccess(const std::wstring& directory, PSID sid);

} // namespace platform
