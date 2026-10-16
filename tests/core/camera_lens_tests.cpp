#include <cmath>
#include <vector>

#include <doctest/doctest.h>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec4.hpp>

#include "core/camera.hpp"
#include "core/primitives.hpp"
#include "core/scene/scene.hpp"

using namespace lb;

namespace {

glm::vec2 project(const OrbitCamera& camera, glm::vec3 world) {
    const glm::vec4 clip = camera.projectionMatrix(1.5f) * camera.viewMatrix() * glm::vec4(world, 1.0f);
    return glm::vec2(clip) / clip.w;
}

} // namespace

TEST_CASE("focal length maps to the full-frame field of view") {
    OrbitCamera camera;
    camera.setFocalLength(50.0f);
    CHECK(glm::degrees(camera.verticalFov()) == doctest::Approx(26.99).epsilon(0.001));
    camera.setFocalLength(24.0f);
    CHECK(glm::degrees(camera.verticalFov()) == doctest::Approx(53.13).epsilon(0.001));
    camera.setFocalLength(1.0f); // clamped to the widest supported lens
    CHECK(camera.focalLength() == doctest::Approx(8.0f));
    camera.reset(); // reset keeps the lens
    CHECK(camera.focalLength() == doctest::Approx(8.0f));
}

TEST_CASE("two-point perspective keeps verticals vertical and the target centered") {
    OrbitCamera camera; // default view looks down at the target
    const glm::vec3 base{6.0f, -3.0f, 0.0f};
    const glm::vec3 top{6.0f, -3.0f, 12.0f};

    const float tiltedLean = std::abs(project(camera, base).x - project(camera, top).x);
    CHECK(tiltedLean > 0.01f); // ordinary perspective: the corner leans

    camera.setTwoPointPerspective(true);
    CHECK(std::abs(project(camera, base).x - project(camera, top).x) < 1e-4f);
    const glm::vec2 target = project(camera, camera.target());
    CHECK(std::abs(target.x) < 1e-4f);
    CHECK(std::abs(target.y) < 1e-4f);
}

TEST_CASE("eye level puts the camera at standing height, looking level") {
    OrbitCamera camera;
    camera.moveToEyeLevel(1.6f);
    CHECK(camera.position().z == doctest::Approx(1.6f));
    CHECK(camera.target().z == doctest::Approx(1.6f));
}

TEST_CASE("duplicate copies a subtree next to the original and shares resources") {
    Scene scene;
    const ResourceId cube = scene.addMesh("cube", makeBox({0, 0, 0}, {1, 1, 1}));
    const ResourceId clay = scene.addMaterial("clay", Material{});
    const EntityId first = scene.createEntity("first");
    const EntityId group = scene.createEntity("group");
    const EntityId last = scene.createEntity("last");
    Transform offset;
    offset.position = {2.0f, 0.0f, 0.0f};
    const EntityId child = scene.createEntity("child", group, offset);
    scene.setMeshInstance(child, MeshInstance{cube, clay});

    const EntityId copy = scene.duplicate(group);
    CHECK(copy != group);
    CHECK(std::vector<EntityId>(scene.roots().begin(), scene.roots().end()) ==
          std::vector<EntityId>{first, group, copy, last});
    const Entity* copied = scene.entity(copy);
    REQUIRE(copied->children.size() == 1);
    const Entity* copiedChild = scene.entity(copied->children.front());
    CHECK(copiedChild->id != child);
    CHECK(copiedChild->name == "child");
    CHECK(copiedChild->transform == offset);
    CHECK(copiedChild->meshInstance == MeshInstance{cube, clay});
    CHECK(scene.userCount(clay) == 2);
}

TEST_CASE("camera state round-trips and is clamped on restore") {
    OrbitCamera original;
    original.orbit(0.7f, -0.2f);
    original.pan(40.0f, -15.0f, 800.0f);
    original.dolly(3.0f);
    original.setFocalLength(50.0f);
    original.setTwoPointPerspective(true);

    OrbitCamera restored;
    restored.setState(original.state());
    CHECK(restored.state() == original.state());
    CHECK(restored.viewMatrix() == original.viewMatrix());
    CHECK(restored.projectionMatrix(1.5f) == original.projectionMatrix(1.5f));

    CameraState wild;
    wild.target = {1.0f, 2.0f, 3.0f};
    wild.distance = 1.0e9f;
    wild.yawRadians = 7.0f; // more than a full turn
    wild.pitchRadians = 3.0f;
    wild.focalLength = 1.0f;
    restored.setState(wild);
    const CameraState clamped = restored.state();
    CHECK(clamped.target == wild.target);
    CHECK(clamped.distance == doctest::Approx(100000.0f));
    CHECK(clamped.yawRadians == doctest::Approx(7.0f - glm::two_pi<float>()));
    CHECK(clamped.pitchRadians < glm::half_pi<float>());
    CHECK(clamped.focalLength == doctest::Approx(8.0f));
    CHECK(restored.verticalFov() == doctest::Approx(2.0f * std::atan(12.0f / 8.0f)));

    wild.distance = 0.0f;
    wild.pitchRadians = -3.0f;
    restored.setState(wild);
    CHECK(restored.state().distance > 0.0f);
    CHECK(restored.state().pitchRadians > -glm::half_pi<float>());
}
