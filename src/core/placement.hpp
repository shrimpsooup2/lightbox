#pragma once

#include <optional>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>

#include "core/scene/scene.hpp"

namespace lb {

// For models from files that don't say which way is up (OBJ): the turn that stands the model under
// `root` upright, or nullopt when it already looks upright or it can't tell. Buildings and most
// objects have much more flat area facing down at their base (floor slab, plinth, site, feet) than
// flat area at their very top (roofs slope, objects taper); lying on a side, both ends look alike.
std::optional<glm::quat> uprightCorrection(const Scene& scene, EntityId root);

// Moves `root` so its lowest point sits on the ground (z = 0) with its footprint centered on `at`.
void placeOnGround(Scene& scene, EntityId root, const glm::vec2& at);

} // namespace lb
