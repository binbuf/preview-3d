#pragma once

#include <cstdint>

namespace import_worker {

// Reserved cache-key component.  The viewer's DerivedCache is not yet wired
// to parser output, but 3MF must have a deliberately changed value whenever
// preflight, lib3mf policy, or normalization semantics change.  Keeping it
// separate prevents a future cache entry from silently spanning adapters.
constexpr uint32_t kThreeMfImporterVersion = 6;

} // namespace import_worker
