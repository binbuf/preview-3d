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

enum class ThreeMfLatticeCapMode : uint8_t { Sphere, Hemisphere, Butt };
enum class ThreeMfLatticeClipMode : uint8_t { None, Inside, Outside };
enum class ThreeMfLatticeBallMode : uint8_t { None, Mixed, All };

struct ThreeMfLatticePropertyRef {
    uint32_t resourceId = 0;
    uint32_t propertyIndex = 0;
    bool hasResource = false;
    bool hasProperty = false;
};

struct ThreeMfLatticeBeam {
    uint32_t vertex[2]{};
    double radius[2]{};
    bool hasRadius[2]{};
    ThreeMfLatticeCapMode cap[2]{ThreeMfLatticeCapMode::Sphere,
                                 ThreeMfLatticeCapMode::Sphere};
    ThreeMfLatticePropertyRef property[2];
};

struct ThreeMfLatticeBall {
    uint32_t vertex = 0;
    double radius = 0.0;
    bool hasRadius = false;
    ThreeMfLatticePropertyRef property;
};

struct ThreeMfLatticeDefinition {
    std::string packagePart;
    uint32_t objectId = 0;
    double minimumLength = 0.0;
    double defaultRadius = 0.0;
    double defaultBallRadius = 0.0;
    bool hasDefaultBallRadius = false;
    ThreeMfLatticeCapMode defaultCap = ThreeMfLatticeCapMode::Sphere;
    ThreeMfLatticeClipMode clipMode = ThreeMfLatticeClipMode::None;
    ThreeMfLatticeBallMode ballMode = ThreeMfLatticeBallMode::None;
    uint32_t clippingMeshId = 0;
    uint32_t representationMeshId = 0;
    ThreeMfLatticePropertyRef defaultProperty;
    std::vector<ThreeMfLatticeBeam> beams;
    std::vector<ThreeMfLatticeBall> balls;
    uint32_t beamSetCount = 0;
};

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
    // lib3mf 2.5 does not expose Beam Lattice property references and its
    // compatible reader accepts several malformed optional attributes.  The
    // product-owned XML boundary retains the validated source semantics here.
    std::unordered_map<std::string, ThreeMfLatticeDefinition> lattices;
};

std::string ThreeMfResourceKey(std::string_view packagePart, uint32_t localResourceId);

model_core::ImportErrorCode ScanThreeMfDisplayProperties(
    std::span<const std::byte> packageBytes,
    const ThreeMfOpcPackage& package,
    ThreeMfDisplayCatalog& catalog,
    const std::function<bool()>& isCancelled = {});

} // namespace import_worker
