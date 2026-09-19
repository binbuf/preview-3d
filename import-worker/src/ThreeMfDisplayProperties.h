#pragma once

#include "ThreeMfOpcPreflight.h"
#include "model_core/ImportError.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace import_worker {

enum class ThreeMfDisplayKind : uint8_t { Metallic, Specular, Unsupported };

struct ThreeMfDisplayProperty {
    float metallic = 0.0f;
    float roughness = 1.0f;
    // Materials Extension default #383838 converted from sRGB to linear.
    std::array<float, 3> specular{0.03954624f, 0.03954624f, 0.03954624f};
};

struct ThreeMfDisplayPropertySet {
    ThreeMfDisplayKind kind = ThreeMfDisplayKind::Unsupported;
    std::vector<ThreeMfDisplayProperty> properties;
};

struct ThreeMfDisplayCatalog {
    // Keys are canonical OPC part names plus a model-local resource ID.
    std::unordered_map<std::string, std::string> associations;
    std::unordered_map<std::string, ThreeMfDisplayPropertySet> groups;
};

std::string ThreeMfResourceKey(std::string_view packagePart, uint32_t localResourceId);

model_core::ImportErrorCode ScanThreeMfDisplayProperties(
    std::span<const std::byte> packageBytes,
    const ThreeMfOpcPackage& package,
    ThreeMfDisplayCatalog& catalog,
    const std::function<bool()>& isCancelled = {});

} // namespace import_worker
