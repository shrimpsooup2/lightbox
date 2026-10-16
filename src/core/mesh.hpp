#pragma once

#include <cstdint>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace lb {

// CPU-side triangle mesh in world conventions (meters, Z-up). Indices are triangle lists.
struct MeshData {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals; // one per position
    // Texture coordinates: empty, or one per position. (0, 0) is the top-left corner of the image
    // (glTF/Vulkan convention); importers of bottom-left formats (OBJ, FBX) flip V.
    std::vector<glm::vec2> uvs;
    std::vector<std::uint32_t> indices;
};

struct Bounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};

    glm::vec3 center() const { return (min + max) * 0.5f; }
    glm::vec3 size() const { return max - min; }
};

} // namespace lb
