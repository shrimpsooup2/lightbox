#include <doctest/doctest.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include "core/placement.hpp"
#include "core/primitives.hpp"
#include "core/scene/scene.hpp"

using namespace lb;

namespace {

// Gable roof over x −5…5, y −4…4: eaves at z = 3.3, ridge along X at z = 5.
MeshData gableRoof() {
    MeshData roof;
    roof.positions = {{-5, -4, 3.3f}, {5, -4, 3.3f}, {5, 0, 5}, {-5, 0, 5},
                      {-5, 4, 3.3f},  {5, 4, 3.3f},  {5, 0, 5}, {-5, 0, 5}};
    roof.normals = {{0, -0.39f, 0.92f}, {0, -0.39f, 0.92f}, {0, -0.39f, 0.92f}, {0, -0.39f, 0.92f},
                    {0, 0.39f, 0.92f},  {0, 0.39f, 0.92f},  {0, 0.39f, 0.92f},  {0, 0.39f, 0.92f}};
    roof.indices = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6};
    return roof;
}

// A house (site slab, walls, gable roof) under a root entity turned by `rotation`.
EntityId addHouse(Scene& scene, const glm::quat& rotation) {
    Transform turned;
    turned.rotation = rotation;
    const EntityId root = scene.createEntity("house", {}, turned);
    const auto part = [&](const char* name, MeshData data) {
        const EntityId id = scene.createEntity(name, root);
        scene.setMeshInstance(id, MeshInstance{scene.addMesh(name, std::move(data)), {}});
    };
    part("slab", makeBox({-6, -5, 0}, {6, 5, 0.3f}));
    part("walls", makeBox({-5, -4, 0.3f}, {5, 4, 3.3f}));
    part("roof", gableRoof());
    return root;
}

} // namespace

TEST_CASE("a house lying on its side is stood upright; an upright one is left alone") {
    Scene scene;
    const EntityId upright = addHouse(scene, glm::quat(glm::vec3(0.0f)));
    CHECK_FALSE(uprightCorrection(scene, upright));

    // Read with the wrong up axis: the house lies on its back, roof toward −Y.
    const EntityId lying =
        addHouse(scene, glm::angleAxis(glm::half_pi<float>(), glm::vec3(1, 0, 0)));
    const std::optional<glm::quat> fix = uprightCorrection(scene, lying);
    REQUIRE(fix);
    Transform transform = scene.entity(lying)->transform;
    transform.rotation = *fix * transform.rotation;
    scene.setTransform(lying, transform);
    const Bounds bounds = *worldBoundsOf(scene, {&lying, 1});
    CHECK(bounds.size().z == doctest::Approx(5.0f)); // ridge height again
    CHECK(bounds.size().x == doctest::Approx(12.0f));
    CHECK_FALSE(uprightCorrection(scene, lying)); // settled: no further turning

    // Upside down is turned over too.
    const EntityId flipped = addHouse(scene, glm::angleAxis(glm::pi<float>(), glm::vec3(1, 0, 0)));
    REQUIRE(uprightCorrection(scene, flipped));
}

TEST_CASE("models without a clear base keep their orientation") {
    Scene scene;
    const EntityId root = scene.createEntity("crate");
    const EntityId box = scene.createEntity("box", root);
    scene.setMeshInstance(box, MeshInstance{scene.addMesh("box", makeBox({0, 0, 0}, {1, 3, 2})), {}});
    CHECK_FALSE(uprightCorrection(scene, root)); // both ends of every axis look alike
    const EntityId empty = scene.createEntity("empty");
    CHECK_FALSE(uprightCorrection(scene, empty));
}

TEST_CASE("placing on the ground centers the footprint and rests the lowest point on z = 0") {
    Scene scene;
    Transform far;
    far.position = {120, -40, 7};
    const EntityId root = scene.createEntity("model", {}, far);
    const EntityId box = scene.createEntity("box", root);
    scene.setMeshInstance(box,
                          MeshInstance{scene.addMesh("box", makeBox({-1, -2, -3}, {3, 2, 1})), {}});
    placeOnGround(scene, root, {5, 6});
    const Bounds bounds = *worldBoundsOf(scene, {&root, 1});
    CHECK(bounds.center().x == doctest::Approx(5.0f));
    CHECK(bounds.center().y == doctest::Approx(6.0f));
    CHECK(bounds.min.z == doctest::Approx(0.0f));
}
