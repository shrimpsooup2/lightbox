#include <cmath>
#include <optional>
#include <random>
#include <vector>

#include <doctest/doctest.h>
#include <glm/geometric.hpp>

#include "core/bvh.hpp"
#include "render/ray_scene.hpp"

using namespace lb;
using namespace lb::render;

namespace {

// The path tracer's stackless traversal (pathtrace.comp) on the CPU: closest hit over threaded
// nodes whose leaves index triangles laid out in BVH leaf order.
std::optional<float> traceThreaded(const std::vector<RayNode>& nodes,
                                   const std::vector<glm::vec3>& ordered, const glm::vec3& origin,
                                   const glm::vec3& direction) {
    const glm::vec3 inverse = 1.0f / direction;
    float closest = 1e30f;
    bool found = false;
    std::uint32_t node = 0;
    int steps = 0;
    while (node < nodes.size()) {
        REQUIRE(++steps < 100000); // miss links must always move forward
        const RayNode& n = nodes[node];
        const glm::vec3 t0 = (n.boundsMin - origin) * inverse;
        const glm::vec3 t1 = (n.boundsMax - origin) * inverse;
        const glm::vec3 near = glm::min(t0, t1);
        const glm::vec3 far = glm::max(t0, t1);
        const float enter = std::max({near.x, near.y, near.z, 0.0f});
        const float exit = std::min({far.x, far.y, far.z, closest});
        if (enter > exit) {
            node = n.miss;
            continue;
        }
        if (n.leaf == 0) {
            ++node;
            continue;
        }
        const std::uint32_t first = n.leaf >> 3;
        const std::uint32_t count = n.leaf & 7u;
        Bvh single;
        single.nodes.push_back({glm::vec3(-1e30f), 0, glm::vec3(1e30f), count});
        for (std::uint32_t i = 0; i < count; ++i) {
            single.triangles.push_back(first + i);
        }
        if (const std::optional<BvhHit> hit =
                intersectBvh(single, ordered, origin, direction, 0.0f, closest)) {
            closest = hit->distance;
            found = true;
        }
        node = n.miss;
    }
    return found ? std::optional<float>(closest) : std::nullopt;
}

} // namespace

TEST_CASE("threaded BVH: miss links visit every triangle and match the stack traversal") {
    std::mt19937 random(5);
    std::uniform_real_distribution<float> position(-10.0f, 10.0f);
    std::uniform_real_distribution<float> offset(-0.7f, 0.7f);
    std::vector<glm::vec3> corners;
    for (int i = 0; i < 1200; ++i) {
        const glm::vec3 c(position(random), position(random), position(random));
        for (int k = 0; k < 3; ++k) {
            corners.push_back(c + glm::vec3(offset(random), offset(random), offset(random)));
        }
    }
    const Bvh bvh = buildBvh(corners);
    const std::vector<RayNode> threaded = threadBvh(bvh.nodes);
    REQUIRE(threaded.size() == bvh.nodes.size());

    // Triangles in leaf order, as the ray scene stores them.
    std::vector<glm::vec3> ordered;
    for (const std::uint32_t t : bvh.triangles) {
        ordered.insert(ordered.end(), {corners[3 * t], corners[3 * t + 1], corners[3 * t + 2]});
    }
    std::size_t leafTriangles = 0;
    for (std::size_t i = 0; i < threaded.size(); ++i) {
        CHECK(threaded[i].miss > i); // forward only: traversal always terminates
        CHECK(threaded[i].miss <= threaded.size());
        leafTriangles += threaded[i].leaf & 7u;
    }
    CHECK(leafTriangles == bvh.triangles.size());
    CHECK(threaded[0].miss == threaded.size()); // missing the root ends the traversal

    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    int hits = 0;
    for (int i = 0; i < 300; ++i) {
        const glm::vec3 origin(unit(random) * 30.0f, unit(random) * 30.0f, unit(random) * 30.0f);
        const glm::vec3 direction = glm::normalize(
            -origin + glm::vec3(unit(random), unit(random), unit(random)) * 6.0f);
        const std::optional<BvhHit> expected =
            intersectBvh(bvh, corners, origin, direction, 0.0f, 1e30f);
        const std::optional<float> actual = traceThreaded(threaded, ordered, origin, direction);
        REQUIRE(expected.has_value() == actual.has_value());
        if (expected) {
            ++hits;
            CHECK(*actual == doctest::Approx(expected->distance));
        }
    }
    CHECK(hits > 80);
    CHECK(threadBvh({}).empty());
}
