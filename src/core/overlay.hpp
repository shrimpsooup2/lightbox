#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace lb {

// A line drawn on top of the scene (gizmos): world-space ends, width in pixels at each end (a width
// tapering to 0 draws an arrowhead), color with alpha. Never hidden by geometry.
struct OverlayLine {
    glm::vec3 a{0.0f};
    float widthA = 2.0f;
    glm::vec3 b{0.0f};
    float widthB = 2.0f;
    glm::vec4 color{1.0f};
};

} // namespace lb
