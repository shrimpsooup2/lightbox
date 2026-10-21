#include <doctest/doctest.h>

#include "core/primitives.hpp"
#include "core/raycast.hpp"
#include "core/scene/scene.hpp"

using namespace lb;

namespace {

EntityId addBox(Scene& scene, const glm::vec3& position, const glm::vec3& size, ResourceId mesh) {
    Transform transform;
    transform.position = position;
    transform.scale = size;
    const EntityId id = scene.createEntity("box", {}, transform);
    scene.setMeshInstance(id, MeshInstance{mesh, {}});
    return id;
}

} // namespace

TEST_CASE("raycast finds the nearest surface with its normal") {
    Scene scene;
    const ResourceId cube = scene.addMesh("cube", makeBox({0, 0, 0}, {1, 1, 1}));
    const EntityId near = addBox(scene, {2, 0, 0}, {1, 1, 1}, cube); // x 2..3
    addBox(scene, {5, -1, -1}, {1, 3, 3}, cube);                      // x 5..6, behind it

    const auto hit = raycast(scene, {0, 0.5f, 0.5f}, {1, 0, 0});
    REQUIRE(hit);
    CHECK(hit->entity == near);
    CHECK(hit->point.x == doctest::Approx(2.0f));
    CHECK(hit->distance == doctest::Approx(2.0f));
    CHECK(hit->normal.x == doctest::Approx(-1.0f)); // facing back toward the ray origin

    // Unnormalized directions measure distance in world units all the same.
    const auto scaled = raycast(scene, {0, 0.5f, 0.5f}, {10, 0, 0});
    REQUIRE(scaled);
    CHECK(scaled->distance == doctest::Approx(2.0f));
}

TEST_CASE("raycast respects transforms, misses and the distance limit") {
    Scene scene;
    const ResourceId cube = scene.addMesh("cube", makeBox({0, 0, 0}, {1, 1, 1}));
    addBox(scene, {0, 0, 0}, {10, 10, 0.5f}, cube); // a 10 × 10 m slab, top at z = 0.5

    const auto down = raycast(scene, {5, 5, 10}, {0, 0, -1});
    REQUIRE(down);
    CHECK(down->point.z == doctest::Approx(0.5f));
    CHECK(down->normal.z == doctest::Approx(1.0f));

    CHECK_FALSE(raycast(scene, {5, 5, 10}, {0, 0, 1}));      // pointing away
    CHECK_FALSE(raycast(scene, {20, 5, 10}, {0, 0, -1}));    // beside the slab
    CHECK_FALSE(raycast(scene, {5, 5, 10}, {0, 0, -1}, 5.0f)); // too far
    CHECK_FALSE(raycast(scene, {5, 5, 10}, {0, 0, 0}));      // no direction
}
