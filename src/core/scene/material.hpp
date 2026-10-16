#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "core/scene/ids.hpp"

namespace lb {

// How texture coordinates are found on a surface.
enum class TextureMapping {
    UV,  // the mesh's own texture coordinates (from the modeling app)
    Box, // projected from the three axes in object space at real-world size: no UVs needed, ideal
         // for CAD geometry
};

// One texture input of a material. Scalar maps (roughness, metalness, occlusion) read one channel, so
// packed maps (glTF metallic-roughness: G = roughness, B = metalness; ORM) work without splitting.
struct TextureSlot {
    ResourceId texture; // none = slot unused
    int channel = 0;    // 0 = R, 1 = G, 2 = B, 3 = A (scalar maps only)
    bool invert = false; // use 1 − value (e.g. a gloss map driving roughness)

    explicit operator bool() const { return static_cast<bool>(texture); }
    bool operator==(const TextureSlot&) const = default;
};

// Surface appearance (metal/roughness PBR subset). Grows into the full LightBox material model in
// Stage 3.3 (ARCHITECTURE.md §6); add parameters only together with the renderer code that uses them.
struct Material {
    glm::vec3 baseColor{0.8f}; // linear RGB (albedo, or reflectance color for metals)
    float roughness = 0.6f;    // 0 = mirror, 1 = fully matte
    float metalness = 0.0f;    // 0 = dielectric, 1 = metal

    TextureSlot baseColorMap;  // sRGB; multiplied with baseColor
    TextureSlot roughnessMap;  // linear; multiplied with roughness
    TextureSlot metalnessMap;  // linear; multiplied with metalness
    TextureSlot normalMap;     // tangent space, OpenGL convention (green = +Y)
    TextureSlot occlusionMap;  // linear ambient occlusion; darkens sky/fill light only
    float normalStrength = 1.0f;
    bool normalFlipY = false; // DirectX-convention normal maps (green = −Y)

    TextureMapping mapping = TextureMapping::UV;
    // UV mapping: repeats per UV unit. Box mapping: real-world size of one texture tile in meters.
    glm::vec2 textureScale{1.0f};
    float textureRotationDegrees = 0.0f;
    glm::vec2 textureOffset{0.0f}; // in tiles

    bool operator==(const Material&) const = default;
};

// Default material with a base color (instead of partial aggregate initialization).
inline Material materialWithColor(const glm::vec3& baseColor) {
    Material material;
    material.baseColor = baseColor;
    return material;
}

} // namespace lb
