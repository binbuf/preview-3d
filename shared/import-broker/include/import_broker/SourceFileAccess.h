#pragma once

// The trusted-process-side half of wiring a real on-disk source file into
// the sandboxed import pipeline, per .docs/design/03-file-formats-and-
// ingestion.md's "Input boundary" ("Preview3D.exe canonicalizes a path
// with GetFinalPathNameByHandleW after opening it, holds the primary
// handle for the generation's lifetime with FILE_SHARE_READ only") and
// "Mapped-file abstraction" ("the trusted process duplicates a read-only
// handle across the process boundary"). Deliberately minimal: no
// regular-file/size/identity validation here (model_core::MappedFile::
// FromHandle does that worker-side once the handle crosses over, so it
// isn't duplicated on this side too) and no reparse-point/sidecar-
// directory containment policy (a broader Input-boundary concern, still a
// later slice).

#include "platform/Win32Handle.h"

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

namespace import_broker {

struct OpenSourceFileResult {
    platform::Win32Handle file;
    std::wstring canonicalPath; // set only when `file` is valid
    std::wstring error;         // set only when `file` is empty
};

// CreateFileW(GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING) -- deliberately
// NOT opened inheritable; see DuplicateInheritableHandle for why that's a
// separate, explicit step. Then GetFinalPathNameByHandleW to canonicalize.
OpenSourceFileResult OpenAndCanonicalizeSourceFile(const std::wstring& path);

// Produces a fresh, inheritable duplicate of `source` (DuplicateHandle with
// bInheritHandle=TRUE, DUPLICATE_SAME_ACCESS), suitable for
// SandboxLauncher's PROC_THREAD_ATTRIBUTE_HANDLE_LIST inheritance -- the
// same mechanism CreateSharedSection's already-inheritable pagefile
// sections use, generalized to an arbitrary handle the caller doesn't want
// broadly inheritable at its point of creation. `source` itself is
// untouched (not closed, not made inheritable).
std::optional<platform::Win32Handle> DuplicateInheritableHandle(HANDLE source);

// Duplicates `source` directly into an already-running target process
// (DuplicateHandle with an explicit hTargetProcessHandle, bInheritHandle=
// FALSE -- inheritance is irrelevant once a process already exists).
// Distinct from DuplicateInheritableHandle, which duplicates for a *future*
// CreateProcessW's inheritance list; this one is for a worker the broker
// already launched, mirroring WorkerPool.cpp's existing
// DuplicateSectionIntoWorker for output sections, generalized to any
// handle. Returns the duplicate's numeric value as it exists in
// `targetProcess`'s own handle table -- meaningless in the caller's own
// process.
std::optional<uint64_t> DuplicateHandleIntoProcess(HANDLE source, HANDLE targetProcess);

} // namespace import_broker
