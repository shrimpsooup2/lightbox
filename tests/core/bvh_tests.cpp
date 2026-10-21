#include <algorithm>
#include <cmath>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include <doctest/doctest.h>
#include <glm/geometric.hpp>
#include <glm/vector_relational.hpp>

#include "core/bvh.hpp"

using namespace lb;

namespace {

// Random small triangles in a 20 m cube, plus a big floor (mixed sizes stress the SAH).
std::vector<glm::vec3> randomTriangles(std::size_t count, unsigned seed) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> position(-10.0f, 10.0f);
    std::uniform_real_distribution<float> offset(-0.6f, 0.6f);
    std::vector<glm::vec3> corners;
    for (std::size_t i = 0; i < count; ++i) {
        const glm::vec3 c(position(random), position(random), position(random));
        for (int k = 0; k < 3; ++k) {
            corners.push_back(c + glm::vec3(offset(random), offset(random), offset(random)));
        }
    }
    corners.insert(corners.end(), {{-50, -50, -11}, {50, -50, -11}, {0, 50, -11}});
    return corners;
}

// Brute-force closest hit: what the BVH must reproduce exactly.
std::optional<BvhHit> bruteForce(std::span<const glm::vec3> corners, const glm::vec3& origin,
                                 const glm::vec3& direction) {
    Bvh flat;
    flat.nodes.push_back({glm::vec3(-1e30f), 0, glm::vec3(1e30f),
                          static_cast<std::uint32_t>(corners.size() / 3)});
    for (std::uint32_t i = 0; i < corners.size() / 3; ++i) {
        flat.triangles.push_back(i);
    }
    return intersectBvh(flat, corners, origin, direction, 0.0f, 1e30f);
}

} // namespace

TEST_CASE("BVH covers every triangle once and nests its bounds") {
    const std::vector<glm::vec3> corners = randomTriangles(2000, 7);
    const Bvh bvh = buildBvh(corners);
    const std::size_t triangles = corners.size() / 3;
    REQUIRE(bvh.triangles.size() == triangles);

    std::vector<int> seen(triangles, 0);
    std::vector<std::uint32_t> stack{0};
    std::size_t leaves = 0;
    while (!stack.empty()) {
        const BvhNode& node = bvh.nodes[stack.back()];
        stack.pop_back();
        if (node.count == 0) {
            for (const std::uint32_t child : {node.first, node.first + 1}) {
                const BvhNode& c = bvh.nodes[child];
                CHECK(glm::all(glm::greaterThanEqual(c.boundsMin, node.boundsMin)));
                CHECK(glm::all(glm::lessThanEqual(c.boundsMax, node.boundsMax)));
                stack.push_back(child);
            }
            continue;
        }
        ++leaves;
        CHECK(node.count <= 4u);
        for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
            const std::uint32_t t = bvh.triangles[i];
            ++seen[t];
            for (int k = 0; k < 3; ++k) {
                const glm::vec3& p = corners[3 * t + static_cast<std::uint32_t>(k)];
                CHECK(glm::all(glm::greaterThanEqual(p, node.boundsMin)));
                CHECK(glm::all(glm::lessThanEqual(p, node.boundsMax)));
            }
        }
    }
    CHECK(std::all_of(seen.begin(), seen.end(), [](int n) { return n == 1; }));
    CHECK(leaves > triangles / 4); // actually split, not one big leaf
}

TEST_CASE("BVH traversal finds the same closest hits as testing every triangle") {
    const std::vector<glm::vec3> corners = randomTriangles(1500, 11);
    const Bvh bvh = buildBvh(corners);
    std::mt19937 random(3);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    int hits = 0;
    for (int i = 0; i < 400; ++i) {
        const glm::vec3 origin(unit(random) * 30.0f, unit(random) * 30.0f, unit(random) * 30.0f);
        const glm::vec3 direction = glm::normalize(-origin + glm::vec3(unit(random), unit(random),
                                                                       unit(random)) * 8.0f);
        const std::optional<BvhHit> expected = bruteForce(corners, origin, direction);
        const std::optional<BvhHit> actual =
            intersectBvh(bvh, corners, origin, direction, 0.0f, 1e30f);
        REQUIRE(expected.has_value() == actual.has_value());
        if (expected) {
            ++hits;
            CHECK(actual->triangle == expected->triangle);
            CHECK(actual->distance == doctest::Approx(expected->distance));
        }
    }
    CHECK(hits > 100);

    // The distance window limits hits (shadow rays toward the sun use it).
    const std::optional<BvhHit> near = intersectBvh(bvh, corners, {0, 0, 40}, {0, 0, -1}, 0.0f, 1e30f);
    REQUIRE(near);
    CHECK_FALSE(intersectBvh(bvh, corners, {0, 0, 40}, {0, 0, -1}, 0.0f, near->distance * 0.5f));
}

TEST_CASE("BVH of nothing, one triangle and stacked duplicates") {
    CHECK(buildBvh({}).nodes.empty());
    const std::vector<glm::vec3> one = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const Bvh single = buildBvh(one);
    REQUIRE(single.nodes.size() == 1);
    CHECK(single.nodes[0].count == 1);

    std::vector<glm::vec3> same;
    for (int i = 0; i < 64; ++i) {
        same.insert(same.end(), one.begin(), one.end());
    }
    const Bvh duplicates = buildBvh(same); // identical centroids: still small leaves
    for (const BvhNode& node : duplicates.nodes) {
        CHECK(node.count <= 4u);
    }
    CHECK(intersectBvh(duplicates, same, {0.2f, 0.2f, 1}, {0, 0, -1}, 0.0f, 10.0f));
}
