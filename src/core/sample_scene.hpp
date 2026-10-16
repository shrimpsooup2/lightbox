#pragma once

#include "core/scene/scene.hpp"

namespace lb {

// Adds the clay house massing LightBox shows on launch until the start screen with templates and
// sample projects exists (roadmap 2.10). Returns the root group entity.
EntityId addSampleMassing(Scene& scene);

} // namespace lb
