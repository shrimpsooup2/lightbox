#include "core/color.hpp"

#include <algorithm>
#include <cmath>

namespace lb {

float srgbToLinear(float encoded) {
    const float c = std::clamp(encoded, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float linear) {
    const float c = std::clamp(linear, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

glm::vec3 srgbToLinear(const glm::vec3& encoded) {
    return {srgbToLinear(encoded.r), srgbToLinear(encoded.g), srgbToLinear(encoded.b)};
}

glm::vec3 linearToSrgb(const glm::vec3& linear) {
    return {linearToSrgb(linear.r), linearToSrgb(linear.g), linearToSrgb(linear.b)};
}

} // namespace lb
