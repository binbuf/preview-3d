#pragma once

#include <cstdint>

namespace import_worker {

// The subset of a resolved 3MF property that selects a material chunk. Colour,
// metallic and roughness can vary per corner without changing the material;
// texture identity, sampler state, texture-layer and mix flags cannot.
struct ThreeMfCornerGrouping {
    uint32_t textureId = 0;
    uint32_t samplerFlags = 0;
    bool textureLayer = false;
    bool textureMix = false;

    bool operator==(const ThreeMfCornerGrouping&) const = default;
};

enum class ThreeMfTriangleMaterialChoice : uint8_t {
    // All corners share one material grouping.
    Uniform = 0,
    // The triangle is fanned from its centroid into one sub-triangle per
    // corner, so each sub-triangle keeps a single material grouping.
    Split = 1,
    // Splitting was not possible (budget or material cap), so the dominant
    // corner grouping is used for the whole triangle.
    Dominant = 2,
};

struct ThreeMfTriangleMaterialPlan {
    ThreeMfTriangleMaterialChoice choice = ThreeMfTriangleMaterialChoice::Uniform;
    // Number of distinct corner group latitudes (1..3).
    uint32_t distinctGroupings = 1;
    // First corner carrying the most frequent grouping; ties keep the lowest
    // corner index so material ordering stays deterministic.
    uint32_t dominantCorner = 0;
};

// Pure, deterministic planner shared by the adapter and its tests.
// `splitWithinBudget` is false when emitting a per-corner fan would exceed the
// bounded triangle ceiling for this generation.
inline ThreeMfTriangleMaterialPlan PlanThreeMfTriangleMaterials(
    const ThreeMfCornerGrouping corners[3], bool splitWithinBudget)
{
    ThreeMfTriangleMaterialPlan plan;
    plan.distinctGroupings = 0;
    uint32_t occurrences[3]{1, 1, 1};
    for (uint32_t corner = 0; corner < 3; ++corner) {
        for (uint32_t earlier = 0; earlier < corner; ++earlier)
            if (corners[earlier] == corners[corner]) {
                ++occurrences[earlier];
                break;
            }
    }
    for (uint32_t corner = 0; corner < 3; ++corner) {
        bool first = true;
        for (uint32_t earlier = 0; earlier < corner; ++earlier)
            if (corners[earlier] == corners[corner]) { first = false; break; }
        if (first) ++plan.distinctGroupings;
    }
    uint32_t best = 0;
    for (uint32_t corner = 1; corner < 3; ++corner)
        if (occurrences[corner] > occurrences[best]) best = corner;
    plan.dominantCorner = best;
    if (plan.distinctGroupings <= 1) {
        plan.choice = ThreeMfTriangleMaterialChoice::Uniform;
    } else if (splitWithinBudget) {
        plan.choice = ThreeMfTriangleMaterialChoice::Split;
    } else {
        plan.choice = ThreeMfTriangleMaterialChoice::Dominant;
    }
    return plan;
}

} // namespace import_worker
