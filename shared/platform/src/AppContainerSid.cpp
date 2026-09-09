#include "platform/AppContainerSid.h"

#include <aclapi.h>
#include <userenv.h>

#include <stdexcept>
#include <utility>

#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

namespace platform {

namespace {

bool ApplyDirectoryAce(const std::wstring& directory, PSID sid, ACCESS_MODE mode)
{
    if (sid == nullptr || directory.empty()) {
        return false;
    }

    // SetNamedSecurityInfoW takes LPWSTR (not LPCWSTR) even though it does
    // not modify the path, so the name needs a writable buffer.
    std::wstring path = directory;

    PACL existingDacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                               &existingDacl, nullptr, &descriptor)
        != ERROR_SUCCESS) {
        return false;
    }

    EXPLICIT_ACCESS_W access{};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    access.grfAccessMode = mode;
    access.grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(sid);

    PACL updatedDacl = nullptr;
    if (SetEntriesInAclW(1, &access, existingDacl, &updatedDacl) != ERROR_SUCCESS) {
        LocalFree(descriptor);
        return false;
    }

    DWORD status = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                          updatedDacl, nullptr);
    LocalFree(updatedDacl);
    LocalFree(descriptor);
    return status == ERROR_SUCCESS;
}

} // namespace

AppContainerSid::AppContainerSid(AppContainerSid&& other) noexcept
    : sid_(std::exchange(other.sid_, nullptr))
{
}

AppContainerSid& AppContainerSid::operator=(AppContainerSid&& other) noexcept
{
    if (this != &other) {
        if (sid_ != nullptr) {
            FreeSid(sid_);
        }
        sid_ = std::exchange(other.sid_, nullptr);
    }
    return *this;
}

AppContainerSid::~AppContainerSid()
{
    if (sid_ != nullptr) {
        FreeSid(sid_);
    }
}

AppContainerSid AppContainerSid::CreateOrOpen(const std::wstring& containerName,
                                               const std::wstring& displayName,
                                               const std::wstring& description)
{
    PSID sid = nullptr;
    HRESULT hr = CreateAppContainerProfile(containerName.c_str(), displayName.c_str(),
                                            description.c_str(), nullptr, 0, &sid);

    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        hr = DeriveAppContainerSidFromAppContainerName(containerName.c_str(), &sid);
    }

    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create or derive AppContainer SID");
    }

    return AppContainerSid(sid);
}

void AppContainerSid::Delete(const std::wstring& containerName)
{
    DeleteAppContainerProfile(containerName.c_str());
}

bool GrantDirectoryReadExecute(const std::wstring& directory, PSID sid)
{
    return ApplyDirectoryAce(directory, sid, GRANT_ACCESS);
}

bool RevokeDirectoryAccess(const std::wstring& directory, PSID sid)
{
    return ApplyDirectoryAce(directory, sid, REVOKE_ACCESS);
}

} // namespace platform
