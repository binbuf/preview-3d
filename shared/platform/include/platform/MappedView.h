#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace platform {

// Move-only RAII wrapper around MapViewOfFile/UnmapViewOfFile. Plain
// MapViewOfFile, not MapViewOfFile3 -- this project has no placeholder/NUMA
// mapping need. No FlushViewOfFile: the sections this wraps are
// pagefile-backed, not file-backed, and cross-process visibility of a live
// shared section is automatic (coherent shared physical pages); ordering
// between writer and reader is guaranteed by the control-channel
// WriteFile/ReadFile syscalls themselves, not by this class.
class MappedView {
public:
    MappedView() noexcept = default;

    MappedView(const MappedView&) = delete;
    MappedView& operator=(const MappedView&) = delete;

    MappedView(MappedView&& other) noexcept;
    MappedView& operator=(MappedView&& other) noexcept;

    ~MappedView();

    // Maps MapViewOfFile(section, desiredAccess, HIDWORD(offset), LODWORD(offset),
    // sizeBytes). sizeBytes == 0 maps from `offset` to the end of the
    // section/file. `offset` defaults to 0 so every pre-existing pagefile-
    // section caller (which always maps a whole section from its start)
    // keeps compiling unchanged. A non-zero `offset` is a file-backed-mapping
    // concern (model_core::MappedFile's windowed reads) -- callers passing
    // one are responsible for `offset` already being a multiple of
    // SYSTEM_INFO::dwAllocationGranularity, since MapViewOfFile itself
    // requires that and returns a failure (empty view) otherwise. Returns an
    // empty (operator bool false) view on failure.
    static MappedView Map(HANDLE section, DWORD desiredAccess, SIZE_T sizeBytes, uint64_t offset = 0);

    std::span<std::byte> bytes() noexcept;
    std::span<const std::byte> bytes() const noexcept;

    explicit operator bool() const noexcept { return view_ != nullptr; }

private:
    MappedView(void* view, SIZE_T size) noexcept
        : view_(view)
        , size_(size)
    {
    }

    void Reset() noexcept;

    void* view_ = nullptr;
    SIZE_T size_ = 0;
};

} // namespace platform
