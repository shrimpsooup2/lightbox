#pragma once

#include <glm/vec2.hpp>

namespace lb {

enum class LightType {
    Point, // a bulb shining in every direction
    Spot,  // a cone along the entity's −Z axis (downlights, spotlights)
    Area,  // a glowing rectangle in the entity's XY plane, shining toward −Z (panels, skylights)
};

// An artificial light on an entity. Position and orientation come from the entity's transform.
// Photometric units (AGENTS.md world conventions): luminous flux in lumens, color as a temperature.
struct Light {
    LightType type = LightType::Point;
    float lumens = 1500.0f;             // total output, like a bulb's box (an 100 W bulb ≈ 1500 lm)
    float temperatureKelvin = 3000.0f;  // 2700 warm white … 6500 daylight
    float radius = 0.05f;               // point and spot: size of the bulb, meters (softens shadows)
    glm::vec2 areaSize{0.6f, 0.6f};     // area: width (local X) and height (local Y), meters
    float spotAngleDegrees = 60.0f;     // spot: full cone angle
    float spotSoftness = 0.25f;         // spot: share of the cone that fades out at its edge (0…1)

    bool operator==(const Light&) const = default;
};

} // namespace lb
