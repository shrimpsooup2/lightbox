#pragma once

#include <glm/vec3.hpp>

namespace lb {

// sRGB transfer functions (IEC 61966-2-1). Scene colors are linear; color pickers and swatches show
// sRGB-encoded values, so UI code converts at the boundary.
float srgbToLinear(float encoded);
float linearToSrgb(float linear);
glm::vec3 srgbToLinear(const glm::vec3& encoded);
glm::vec3 linearToSrgb(const glm::vec3& linear);

} // namespace lb
