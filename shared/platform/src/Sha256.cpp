#include "platform/Sha256.h"

#include <windows.h>

#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace platform {

namespace {

// bcrypt.h's handle close functions differ per handle type
// (BCryptCloseAlgorithmProvider vs. BCryptDestroyHash), so these are two
// tiny bespoke RAII wrappers rather than reusing Win32Handle (which is
// CloseHandle-specific) or a generic unique_ptr<void, Deleter> -- matches
// this repo's established small-custom-RAII-class style (Win32Handle,
// MappedView, AppContainerSid).
class AlgHandle {
public:
    AlgHandle() noexcept = default;
    AlgHandle(const AlgHandle&) = delete;
    AlgHandle& operator=(const AlgHandle&) = delete;
    ~AlgHandle()
    {
        if (handle_) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }
    BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }
    BCRYPT_ALG_HANDLE* put() noexcept { return &handle_; }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class HashHandle {
public:
    HashHandle() noexcept = default;
    HashHandle(const HashHandle&) = delete;
    HashHandle& operator=(const HashHandle&) = delete;
    ~HashHandle()
    {
        if (handle_) {
            BCryptDestroyHash(handle_);
        }
    }
    BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }
    BCRYPT_HASH_HANDLE* put() noexcept { return &handle_; }

private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

} // namespace

std::optional<std::array<std::byte, 32>> ComputeSha256(std::span<const std::byte> data)
{
    AlgHandle alg;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(alg.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return std::nullopt;
    }

    HashHandle hash;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg.get(), hash.put(), nullptr, 0, nullptr, 0, 0))) {
        return std::nullopt;
    }

    if (!data.empty()) {
        auto* input = reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data.data()));
        if (!BCRYPT_SUCCESS(BCryptHashData(hash.get(), input, static_cast<ULONG>(data.size()), 0))) {
            return std::nullopt;
        }
    }

    std::array<std::byte, 32> digest{};
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash.get(), reinterpret_cast<PUCHAR>(digest.data()),
                                          static_cast<ULONG>(digest.size()), 0))) {
        return std::nullopt;
    }
    return digest;
}

} // namespace platform
