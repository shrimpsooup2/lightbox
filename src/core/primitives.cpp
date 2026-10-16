#include "core/primitives.hpp"

#include <array>

#include <glm/geometric.hpp>

namespace lb {

MeshData makeBox(const glm::vec3& min, const glm::vec3& max) {
    struct Face {
        glm::vec3 normal;
        std::array<glm::vec3, 4> corners; // counter-clockwise seen from outside
    };
    const glm::vec3 a = min;
    const glm::vec3 b = max;
    const std::array<Face, 6> faces = {{
        {{1, 0, 0}, {{{b.x, a.y, a.z}, {b.x, b.y, a.z}, {b.x, b.y, b.z}, {b.x, a.y, b.z}}}},
        {{-1, 0, 0}, {{{a.x, b.y, a.z}, {a.x, a.y, a.z}, {a.x, a.y, b.z}, {a.x, b.y, b.z}}}},
        {{0, 1, 0}, {{{b.x, b.y, a.z}, {a.x, b.y, a.z}, {a.x, b.y, b.z}, {b.x, b.y, b.z}}}},
        {{0, -1, 0}, {{{a.x, a.y, a.z}, {b.x, a.y, a.z}, {b.x, a.y, b.z}, {a.x, a.y, b.z}}}},
        {{0, 0, 1}, {{{a.x, a.y, b.z}, {b.x, a.y, b.z}, {b.x, b.y, b.z}, {a.x, b.y, b.z}}}},
        {{0, 0, -1}, {{{a.x, b.y, a.z}, {b.x, b.y, a.z}, {b.x, a.y, a.z}, {a.x, a.y, a.z}}}},
    }};

    MeshData mesh;
    mesh.positions.reserve(24);
    mesh.normals.reserve(24);
    mesh.uvs.reserve(24);
    mesh.indices.reserve(36);
    for (const Face& face : faces) {
        const auto base = static_cast<std::uint32_t>(mesh.positions.size());
        // UVs in meters: U along the first edge, V up the second edge (negative, because V = 0 is
        // the top of the image), so textures tile at real-world size with textureScale 1.
        const glm::vec3 origin = face.corners[0];
        const auto axis = [](const glm::vec3& edge) {
            const float length = glm::length(edge);
            return length > 0.0f ? edge / length : glm::vec3(0.0f); // flat box: no extent
        };
        const glm::vec3 uAxis = axis(face.corners[1] - face.corners[0]);
        const glm::vec3 vAxis = axis(face.corners[3] - face.corners[0]);
        for (const glm::vec3& corner : face.corners) {
            mesh.positions.push_back(corner);
            mesh.normals.push_back(face.normal);
            mesh.uvs.emplace_back(glm::dot(corner - origin, uAxis), -glm::dot(corner - origin, vAxis));
        }
        for (std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
            mesh.indices.push_back(base + i);
        }
    }
    return mesh;
}

} // namespace lb
