#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ThreeMfDisplayProperties.h"
#include "ThreeMfOpcPreflight.h"

#include <objbase.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace {
constexpr uint32_t kMagic = 0x5a464d33; // 3MFZ
constexpr size_t kMaxInput = 1024 * 1024;
constexpr uint64_t kMaxExpanded = 4 * 1024 * 1024;

struct Envelope {
    uint32_t magic;
    uint8_t mode;
    uint8_t cancelAfter;
    uint16_t reserved;
};
static_assert(sizeof(Envelope) == 8);
}

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > kMaxInput || !data) return 0;
    uint8_t mode = 0;
    uint8_t cancelAfter = 0;
    if (size >= sizeof(Envelope)) {
        Envelope envelope{};
        std::memcpy(&envelope, data, sizeof(envelope));
        if (envelope.magic == kMagic) {
            mode = envelope.mode;
            cancelAfter = envelope.cancelAfter;
            data += sizeof(envelope);
            size -= sizeof(envelope);
        }
    }
    const auto bytes = std::as_bytes(std::span(data, size));
    import_worker::ThreeMfOpcLimits limits{};
    limits.maxEntries = 128;
    limits.maxRelationships = 256;
    limits.maxModelParts = 32;
    limits.maxEntryBytes = kMaxExpanded;
    limits.maxExpandedBytes = kMaxExpanded;
    limits.maxExpansionRatio = 32;
    import_worker::ThreeMfOpcPackage package;
    unsigned checks = 0;
    const auto cancelled = [&] {
        return (mode & 1u) && ++checks > cancelAfter;
    };
    if (import_worker::InspectThreeMfOpc(bytes, &package, limits, cancelled)
        != import_worker::ThreeMfOpcError::None) return 0;
    for (const auto& part : package.parts) {
        if (part.expandedBytes > kMaxExpanded) continue;
        std::vector<std::byte> expanded;
        (void)import_worker::ExtractThreeMfOpcPart(bytes, part, expanded,
                                                     kMaxExpanded, cancelled);
    }
    if (!(mode & 2u)) {
        import_worker::ThreeMfDisplayCatalog catalog;
        (void)import_worker::ScanThreeMfDisplayProperties(bytes, package,
                                                            catalog, cancelled);
    }
    return 0;
}
