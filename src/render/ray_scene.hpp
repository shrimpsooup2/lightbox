#pragma once

#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "core/bvh.hpp"

namespace lb::render {

// World-space geometry for the path tracer, laid out exactly as its storage buffers (std430).
// Built by SceneSync from the document (gathered on the UI thread, BVH built on a worker thread)
// and uploaded by the presenter. Immutable once built, so it can be shared across threads.
struct RayVertex {
    glm::vec3 position;    // world space
    float u;
    glm::vec3 normal;      // world space, unit length
    float v;
    glm::vec3 boxPosition; // object space scaled to meters (box mapping)
    std::uint32_t instance;
};
static_assert(sizeof(RayVertex) == 48);

// Per mesh instance: its world axes (unit length), for box mapping orientation.
struct RayInstance {
    glm::vec4 axisX;
    glm::vec4 axisY;
    glm::vec4 axisZ;
};
static_assert(sizeof(RayInstance) == 48);

// BVH node for stackless traversal: nodes are stored depth first, so an interior node's first child
// follows it directly, and every node knows where to continue when the ray misses it (the node after
// its subtree). Small GPUs (Intel Iris) run much faster without a traversal stack in registers.
struct RayNode {
    glm::vec3 boundsMin;
    std::uint32_t miss;  // next node when the ray misses this one (or after a leaf); count = end
    glm::vec3 boundsMax;
    std::uint32_t leaf;  // 0 for interior nodes, else (first triangle << 3) | triangle count
};
static_assert(sizeof(RayNode) == 32);

// Depth-first relayout of a BVH with miss links (leaves hold at most 7 triangles).
std::vector<RayNode> threadBvh(const std::vector<BvhNode>& nodes);

struct RayScene {
    std::vector<RayVertex> vertices;
    std::vector<glm::uvec4> triangles; // vertex indices + material index, in BVH leaf order
    std::vector<RayInstance> instances;
    std::vector<RayNode> nodes;        // depth first; leaves index `triangles` directly
    std::uint64_t geometryVersion = 0; // SceneSync geometry version it was built from
};

} // namespace lb::render
