#pragma once

// Model Core's read-only MappedFile/MappingLease abstraction --
// .docs/design/03-file-formats-and-ingestion.md's "Mapped-file
// abstraction". Opens a local file and maps aligned windows of it on
// demand rather than eagerly copying or mapping the whole thing, per that
// section's numbered steps (1-6). Deliberately scoped to the mapping
// primitive itself: path canonicalization/reparse-point rejection (the
// trusted process's job, per that document's "Input boundary" section)
// and cross-process handle duplication (the broker's job) are both a
// later slice -- this type just opens a path with CreateFileW and maps
// windows of the resulting handle. Re-verifying "did the file change
// since open" against a fresh identity query is likewise left to whatever
// future generation-lifecycle caller needs it; this type only exposes
// Identity(), it does not re-check it itself.

#include <platform/MappedView.h>
#include <platform/Win32Handle.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace model_core {

enum class MappedFileAccessHint {
    Unknown,    // no hint -- access pattern not yet known; never a trust decision
    Sequential, // STL/PLY/packaged sequential scans
    Random,     // glTF/USD offset graphs
};

struct FileIdentity {
    uint64_t volumeSerialNumber = 0;
    std::array<std::byte, 16> fileId128{}; // FILE_ID_INFO::FileId, from GetFileInformationByHandleEx
    uint64_t sizeBytes = 0;

    bool operator==(const FileIdentity& other) const noexcept
    {
        return volumeSerialNumber == other.volumeSerialNumber && fileId128 == other.fileId128
            && sizeBytes == other.sizeBytes;
    }
    bool operator!=(const FileIdentity& other) const noexcept { return !(*this == other); }
};

// A bounded-lifetime view over part of a MappedFile. Owns the underlying
// OS mapping via platform::MappedView -- no parser-owned raw pointer can
// outlive it, by construction rather than convention. Move-only.
class MappingLease {
public:
    MappingLease() noexcept = default;
    MappingLease(const MappingLease&) = delete;
    MappingLease& operator=(const MappingLease&) = delete;
    MappingLease(MappingLease&&) noexcept = default;
    MappingLease& operator=(MappingLease&&) noexcept = default;

    // Exactly the caller's originally requested [offset, offset+length)
    // range -- the allocation-granularity alignment padding MapWindow
    // applied internally is never visible here.
    std::span<const std::byte> Bytes() const noexcept { return view_.bytes().subspan(offsetWithinView_, length_); }

    explicit operator bool() const noexcept { return static_cast<bool>(view_); }

private:
    friend class MappedFile;
    MappingLease(platform::MappedView view, uint64_t offsetWithinView, uint64_t length) noexcept
        : view_(std::move(view))
        , offsetWithinView_(offsetWithinView)
        , length_(length)
    {
    }

    platform::MappedView view_;
    uint64_t offsetWithinView_ = 0;
    uint64_t length_ = 0;
};

class MappedFile;

// Deliberately NOT nested inside MappedFile: a std::optional<MappedFile>
// member requires MappedFile to be a complete type when its special
// members are instantiated, which a nested struct's inline member would
// need while MappedFile itself is still being defined (self-referential
// incompleteness) -- MSVC rejects this with C2139. Declared here (forward
// reference only) so MappedFile::Open can name it as a return type, and
// defined below once MappedFile itself is complete.
struct MappedFileOpenResult;

class MappedFile {
public:
    static MappedFileOpenResult Open(const std::wstring& path,
                                      MappedFileAccessHint hint = MappedFileAccessHint::Unknown);

    // For a caller that already holds an open, readable file handle --
    // e.g. the sandboxed import worker, receiving a handle the broker
    // duplicated in, per .docs/design/03-file-formats-and-ingestion.md's
    // "Mapped-file abstraction" step 1: "...or, for a duplicated handle
    // received from the broker, reopen a mapping directly from that
    // handle without a fresh CreateFileW call." Takes ownership of
    // `file`. Shares every check/step after the open with Open() itself
    // (identity/size/regular-file, then the PAGE_READONLY mapping).
    static MappedFileOpenResult FromHandle(platform::Win32Handle file);

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&&) noexcept = default;
    MappedFile& operator=(MappedFile&&) noexcept = default;

    const FileIdentity& Identity() const noexcept { return identity_; }
    uint64_t SizeBytes() const noexcept { return identity_.sizeBytes; }

    // Maps [offset, offset + length), rounding the mapping's base down to
    // the system allocation granularity internally -- the returned
    // lease's Bytes() hides that padding and returns exactly the
    // requested range. Fails (empty, operator-bool-false lease, `error`
    // set) if the range is out of bounds or MapViewOfFile itself fails.
    // Multiple leases from the same MappedFile may be alive at once.
    MappingLease MapWindow(uint64_t offset, uint64_t length, std::wstring& error) const;

    // Convenience for a compact source: MapWindow(0, SizeBytes(), error).
    MappingLease MapWhole(std::wstring& error) const;

private:
    // The shared tail of Open()/FromHandle(): given an already-open file
    // handle (however it was obtained), validate it and build the
    // PAGE_READONLY mapping. Does not close/duplicate `file` -- ownership
    // transfers into the returned MappedFile on success, or `file` is
    // simply dropped (closed) on failure.
    static MappedFileOpenResult BuildFromOpenFile(platform::Win32Handle file);

    MappedFile(platform::Win32Handle file, platform::Win32Handle mapping, FileIdentity identity) noexcept
        : file_(std::move(file))
        , mapping_(std::move(mapping))
        , identity_(identity)
    {
    }

    platform::Win32Handle file_;
    platform::Win32Handle mapping_;
    FileIdentity identity_;
};

struct MappedFileOpenResult {
    std::optional<MappedFile> file;
    std::wstring error; // set only when `file` is empty
};

} // namespace model_core
