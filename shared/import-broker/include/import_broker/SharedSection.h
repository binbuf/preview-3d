#pragma once

#include "platform/Win32Handle.h"

#include <windows.h>

#include <cstdint>

namespace import_broker {

// Generously larger than the ~1.5 KiB synthetic cube+point-cluster payload;
// bounded and arbitrary but documented. This is the *synthetic generator's*
// size and stays that way -- the real Tier A budgets the "revisit in Gate 3+"
// note asked for are kImportSectionBytes/kImportMaxChunkCount below, which
// the product path now uses instead.
constexpr SIZE_T kSyntheticSectionBytes = 1ULL * 1024 * 1024;

// One normalized detail chunk's maximum GPU payload, per
// .docs/design/03-file-formats-and-ingestion.md:140: "Target detail chunks
// are 4-16 MiB of GPU payload and at most 262,144 triangles or 1,048,576
// points."
constexpr uint64_t kMaxDetailChunkPayloadBytes = 16ULL * 1024 * 1024;

// The output section the product hands a worker.
//
// It is a bounded *transfer window*, not a buffer sized to hold a whole
// model. It must hold at least one maximum-size detail chunk or an import
// can never make forward progress, so:
//
//     one maximum detail chunk                16 MiB   (03-...:140)
//   x 4, so one batch carries several chunks  64 MiB
//
// 64 MiB sits far below both the 1 GiB Tier A "parser/normalizer live
// scratch" budget (03-...:171) and the 4 GiB Job Object commit ceiling
// (kImportWorkerCommitLimitBytes), so the window is never the binding
// constraint on either side of the boundary.
//
// This replaces kSyntheticSectionBytes on the product path, which at 1 MiB
// could not hold even the A-small perf fixture (100k triangles, ~4.4 MiB
// normalized) and so would have measured the section rather than the parser.
//
// Still one terminal write per generation: A-medium and larger exceed this
// window and need the non-terminal progressive delivery it is sized for.
// That is a separate protocol change; until it lands, a model too big for
// the window fails cleanly with ResourceLimit, which is what every adapter's
// `sectionLength > destination.size()` check already does.
constexpr uint64_t kImportSectionBytes = 4 * kMaxDetailChunkPayloadBytes;

// Bounds the descriptor table the host allocates from a worker-declared
// chunkCount, checked before any payload is trusted
// (SharedSectionValidator.cpp:102).
//
// At model_core::kChunkDescriptorSize (92 bytes) 1024 entries is 92 KiB --
// 0.14% of the window, so the byte budget stays the binding limit rather
// than the count. The previous 64 was chosen for a ~1.5 KiB synthetic
// fixture: A-medium's 2,000 nodes alone can produce more chunks than that
// once logical primitives are split "while preserving material and instance
// identity" (03-...:140), so the count would have bound long before the
// bytes did.
constexpr uint32_t kImportMaxChunkCount = 1024;

// Creates a pagefile-backed, PAGE_READWRITE shared section of exactly
// sizeBytes (hFile = INVALID_HANDLE_VALUE -- system paging file, per
// .docs/design/03-file-formats-and-ingestion.md: "bounded scratch section,
// not a real file"). The returned handle has
// SECURITY_ATTRIBUTES.bInheritHandle = TRUE baked in at creation, so it is
// eligible for PROC_THREAD_ATTRIBUTE_HANDLE_LIST inheritance the same way
// the launch spike's pipes already are. No explicit AppContainer ACE is set
// on the security descriptor -- Windows' access check happens at handle
// open/duplicate time, not on every use of an already-inherited handle, the
// same reason the existing pipes already work with a null security
// descriptor. (Flagged as a risk to confirm empirically the first time a
// worker actually maps an inherited section under a real AppContainer
// token.)
platform::Win32Handle CreateSharedSection(SIZE_T sizeBytes);

} // namespace import_broker
