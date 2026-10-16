#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace lb {

// Local transform relative to the parent entity, stored as editable components (gizmo friendly).
// The matrix applies scale, then rotation, then translation.
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{glm::vec3(0.0f)}; // identity (built from zero Euler angles)
    glm::vec3 scale{1.0f};

    glm::mat4 matrix() const;
    bool operator==(const Transform&) const = default;
};

} // namespace lb
