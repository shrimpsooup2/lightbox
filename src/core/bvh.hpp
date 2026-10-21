#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <glm/vec3.hpp>

#include "core/mesh.hpp"

namespace lb {

// One BVH node, 32 bytes, laid out for a GPU storage buffer (std430: two vec4s). Interior nodes
// have count 0 and their children at `first` and `first + 1`; leaves hold `count` triangles from
// Bvh::triangles[first].
struct BvhNode {
    glm::vec3 boundsMin{0.0f};
    std::uint32_t first = 0;
    glm::vec3 boundsMax{0.0f};
    std::uint32_t count = 0;
};
static_assert(sizeof(BvhNode) == 32);

struct Bvh {
    std::vector<BvhNode> nodes;          // root at 0; empty when there are no triangles
    std::vector<std::uint32_t> triangles; // triangle indices in leaf order
};

// Bounding volume hierarchy over triangles given by their three corners (`corners[3 * i + k]`).
// Binned surface area heuristic (12 bins per axis), leaves of up to 4 triangles where splitting
// doesn't pay. Deterministic; runs on any thread (no shared state).
Bvh buildBvh(std::span<const glm::vec3> corners);

struct BvhHit {
    std::uint32_t triangle = 0; // index into the input triangles
    float distance = 0.0f;
    float u = 0.0f, v = 0.0f;   // barycentrics of corners 1 and 2
};

// Closest hit along origin + t · direction, t in (minDistance, maxDistance). CPU reference of the
// GPU traversal (tests, picking).
std::optional<BvhHit> intersectBvh(const Bvh& bvh, std::span<const glm::vec3> corners,
                                   const glm::vec3& origin, const glm::vec3& direction,
                                   float minDistance, float maxDistance);

} // namespace lb
