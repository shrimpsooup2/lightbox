#pragma once

#include "core/mesh.hpp"

namespace lb {

// Axis-aligned box between `min` and `max` with flat per-face normals (24 vertices, 12 triangles) and
// per-face UVs in meters (textures tile at real-world size).
MeshData makeBox(const glm::vec3& min, const glm::vec3& max);

} // namespace lb
