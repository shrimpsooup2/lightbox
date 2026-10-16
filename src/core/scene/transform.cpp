#include "core/scene/transform.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace lb {

glm::mat4 Transform::matrix() const {
    const glm::mat4 translation = glm::translate(glm::mat4(1.0f), position);
    return glm::scale(translation * glm::mat4_cast(rotation), scale);
}

} // namespace lb
