#pragma once

#include <limits>
#include <optional>

#include <glm/vec3.hpp>

#include "core/scene/ids.hpp"

namespace lb {

class Scene;

struct RayHit {
    EntityId entity;    // the mesh entity that was hit
    glm::vec3 point{};  // world space
    glm::vec3 normal{}; // world space, unit length, facing the ray origin (geometry is two-sided)
    float distance = 0.0f;
};

// Nearest mesh surface along a world-space ray (`direction` need not be normalized; `distance` is
// in world units along the normalized direction). Tests every mesh instance: world bounds first,
// then triangles. Fine for clicks on architectural models; not meant for per-frame use.
std::optional<RayHit> raycast(const Scene& scene, const glm::vec3& origin,
                              const glm::vec3& direction,
                              float maxDistance = std::numeric_limits<float>::infinity());

} // namespace lb
