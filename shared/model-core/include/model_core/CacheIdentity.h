#pragma once

// T22: one place that names the parser/normalizer identity a derived-cache
// key must carry. Bumping the wire protocol or changing the material/vertex
// contract without moving this value would let an old entry be reused under
// new rules, so the value is derived from the protocol constant rather than
// hand-maintained. The importer identity is a separate append-only counter
// bumped when an adapter changes the normalized meaning of the same wire
// version (for example the T22 material-semantic corrections).

#include "model_core/WireFormat.h"

#include <cstdint>

namespace model_core {

// Increment only for an adapter/normalizer change that keeps the same wire
// version but changes the produced meaning.
constexpr uint32_t kGltfImporterVersion = 2;
constexpr uint32_t kMaterialSemanticsVersion = 13;
constexpr uint32_t kVertexLayoutVersion = 1;

// Packed, stable identity for CacheEntryKey::parserVersion. The protocol
// version occupies the high byte so a schema change is always visible in a
// key even if an adapter forgets its own bump.
constexpr uint32_t DerivedCacheParserIdentity() noexcept
{
    return (uint32_t(kCurrentProtocolVersion & 0xFFu) << 24)
        | ((kGltfImporterVersion & 0xFFu) << 16)
        | ((kMaterialSemanticsVersion & 0xFFu) << 8)
        | (kVertexLayoutVersion & 0xFFu);
}

constexpr uint32_t kDerivedCacheParserIdentity = DerivedCacheParserIdentity();

} // namespace model_core
