#include <cmath>

#include <doctest/doctest.h>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/vec4.hpp>

#include "core/camera.hpp"
#include "core/gizmo.hpp"
#include "core/primitives.hpp"
#include "core/scene/scene.hpp"

using namespace lb;

namespace {

// A camera looking down at the origin from the south-east, 1200 × 800 window.
struct TestView {
    OrbitCamera camera;
    GizmoView view;

    TestView() {
        camera.setState(CameraState{{0, 0, 0}, 20.0f, -0.8f, 0.6f, 35.0f, false});
        view.viewProj = camera.projectionMatrix(1.5f) * camera.viewMatrix();
        view.cameraPosition = camera.position();
        view.verticalFov = camera.verticalFov();
        view.viewportSize = {1200.0f, 800.0f};
    }

    glm::vec2 screen(const glm::vec3& world) const {
        const glm::vec4 clip = view.viewProj * glm::vec4(world, 1.0f);
        return (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * view.viewportSize;
    }

    // World-space ray through a window position (reverse-Z: depth 1 is the near plane).
    std::pair<glm::vec3, glm::vec3> ray(const glm::vec2& pixel) const {
        const glm::mat4 inverse = glm::inverse(view.viewProj);
        const glm::vec2 ndc = pixel / view.viewportSize * 2.0f - 1.0f;
        const glm::vec4 a = inverse * glm::vec4(ndc, 1.0f, 1.0f);
        const glm::vec4 b = inverse * glm::vec4(ndc, 0.5f, 1.0f);
        const glm::vec3 origin = glm::vec3(a) / a.w;
        return {origin, glm::normalize(glm::vec3(b) / b.w - origin)};
    }
};

} // namespace

TEST_CASE("gizmo keeps its on-screen size and picks the handle under the cursor") {
    const TestView t;
    const Gizmo near({0, 0, 0}, t.view);
    // Length on screen of a gizmo-sized segment across the view (not foreshortened).
    const glm::mat4 view = t.camera.viewMatrix();
    const glm::vec3 right(view[0][0], view[1][0], view[2][0]);
    const float arrowPixels = glm::length(t.screen(right * near.size()) - t.screen({0, 0, 0}));
    CHECK(arrowPixels == doctest::Approx(Gizmo::kArrowPixels).epsilon(0.03));

    // Farther from the camera (which is south-east of the origin), a bigger gizmo in meters.
    const Gizmo far({-8, 8, 0}, t.view);
    CHECK(far.size() > near.size());

    const auto tipOf = [&](const glm::vec3& axis) { return t.screen(axis * (0.9f * near.size())); };
    CHECK(near.pick(tipOf({1, 0, 0})) == GizmoHandle::MoveX);
    CHECK(near.pick(tipOf({0, 1, 0})) == GizmoHandle::MoveY);
    CHECK(near.pick(tipOf({0, 0, 1})) == GizmoHandle::MoveZ);
    CHECK(near.pick(t.screen(glm::vec3(0.32f, 0.32f, 0.0f) * near.size())) ==
          GizmoHandle::MovePlane);
    CHECK(near.pick(t.screen(glm::vec3(0, 0, 1.25f) * near.size())) != GizmoHandle::MoveZ);
    CHECK(near.pick(t.screen(glm::vec3(-1.25f, 0, 0) * near.size())) != GizmoHandle::None);
    CHECK(near.pick({5.0f, 5.0f}) == GizmoHandle::None);
}

TEST_CASE("dragging an arrow moves the selection along that axis only") {
    const TestView t;
    Scene scene;
    const ResourceId cube = scene.addMesh("cube", makeBox({0, 0, 0}, {1, 1, 1}));
    Transform parentTransform;
    parentTransform.position = {10, 0, 0};
    parentTransform.scale = glm::vec3(2.0f); // children move in world meters all the same
    const EntityId group = scene.createEntity("group", {}, parentTransform);
    const EntityId box = scene.createEntity("box", group);
    scene.setMeshInstance(box, MeshInstance{cube, {}});
    const EntityId ids[] = {group, box}; // the box is inside the group: moved once, not twice

    const Gizmo gizmo({0, 0, 0}, t.view);
    const glm::vec2 grab = t.screen(glm::vec3(0.6f * gizmo.size(), 0, 0));
    const glm::vec2 to = t.screen(glm::vec3(0.6f * gizmo.size() + 3.0f, 0, 0));
    GizmoDrag drag;
    const auto [o0, d0] = t.ray(grab);
    REQUIRE(drag.begin(scene, ids, GizmoHandle::MoveX, gizmo, o0, d0));
    const auto [o1, d1] = t.ray(to + glm::vec2(0.0f, 25.0f)); // sideways mouse drift is ignored
    drag.update(o1, d1, false);
    CHECK(drag.moved().x > 2.0f);
    CHECK(drag.moved().y == doctest::Approx(0.0f));
    CHECK(drag.moved().z == doctest::Approx(0.0f));
    CHECK(scene.entity(group)->transform.position.x == doctest::Approx(10.0f + drag.moved().x));
    CHECK(scene.entity(box)->transform.position.x == doctest::Approx(0.0f));

    drag.update(o1, d1, true); // snapping: whole 0.1 m steps
    const float steps = drag.moved().x / 0.1f;
    CHECK(steps == doctest::Approx(std::round(steps)).epsilon(1e-4));
}

TEST_CASE("dragging a ring turns the selection about the gizmo origin") {
    const TestView t;
    Scene scene;
    Transform transform;
    transform.position = {3, 0, 0};
    const EntityId id = scene.createEntity("object", {}, transform);
    const EntityId ids[] = {id};

    const Gizmo gizmo({0, 0, 0}, t.view);
    const float radius = 1.25f * gizmo.size();
    const auto ringPixel = [&](float angle) {
        return t.screen(glm::vec3(std::cos(angle), std::sin(angle), 0.0f) * radius);
    };
    GizmoDrag drag;
    const auto [o0, d0] = t.ray(ringPixel(0.0f));
    REQUIRE(drag.begin(scene, ids, GizmoHandle::RotateZ, gizmo, o0, d0));
    // Go most of the way around in steps (past ±180°) to check the angle keeps counting.
    for (int i = 1; i <= 8; ++i) {
        const auto [o, d] = t.ray(ringPixel(glm::radians(25.0f * static_cast<float>(i))));
        drag.update(o, d, false);
    }
    CHECK(drag.turned() == doctest::Approx(glm::radians(200.0f)).epsilon(1e-3));
    const glm::vec3 position = scene.entity(id)->transform.position;
    CHECK(position.x == doctest::Approx(3.0f * std::cos(glm::radians(200.0f))).epsilon(1e-3));
    CHECK(position.y == doctest::Approx(3.0f * std::sin(glm::radians(200.0f))).epsilon(1e-3));
    const glm::vec3 facing = scene.entity(id)->transform.rotation * glm::vec3(1, 0, 0);
    CHECK(std::atan2(facing.y, facing.x) ==
          doctest::Approx(glm::radians(200.0f) - glm::two_pi<float>()).epsilon(1e-3));
}

TEST_CASE("scale handles stretch along one axis or evenly, about the gizmo origin") {
    const TestView t;
    Scene scene;
    Transform transform;
    transform.position = {2, 1, 0};
    transform.scale = {1, 2, 3};
    // Turned a quarter about Z: its own Y axis now lies along world X.
    transform.rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 0, 1));
    const EntityId id = scene.createEntity("object", {}, transform);
    const EntityId ids[] = {id};
    const Gizmo gizmo({0, 0, 0}, t.view);

    SUBCASE("X square: dragged out to twice its distance doubles X") {
        const glm::vec3 handle(1.5f * gizmo.size(), 0, 0);
        GizmoDrag drag;
        const auto [o0, d0] = t.ray(t.screen(handle));
        REQUIRE(gizmo.pick(t.screen(handle)) == GizmoHandle::ScaleX);
        REQUIRE(drag.begin(scene, ids, GizmoHandle::ScaleX, gizmo, o0, d0));
        const auto [o1, d1] = t.ray(t.screen(handle * 2.0f));
        drag.update(o1, d1, false);
        CHECK(drag.scaled().x == doctest::Approx(2.0f).epsilon(1e-3));
        CHECK(drag.scaled().y == doctest::Approx(1.0f));
        const Transform& now = scene.entity(id)->transform;
        CHECK(now.position.x == doctest::Approx(4.0f).epsilon(1e-3)); // spread from the origin
        CHECK(now.position.y == doctest::Approx(1.0f));
        CHECK(now.scale.x == doctest::Approx(1.0f)); // own X runs along world Y: unchanged
        CHECK(now.scale.y == doctest::Approx(4.0f).epsilon(1e-3)); // own Y runs along world X
        CHECK(now.scale.z == doctest::Approx(3.0f));

        drag.update(o1, d1, true);
        CHECK(drag.scaled().x == doctest::Approx(2.0f)); // already a 10 % step
    }

    SUBCASE("white square: dragging right enlarges evenly, left shrinks") {
        REQUIRE(gizmo.pick(t.screen({0, 0, 0})) == GizmoHandle::ScaleUniform);
        GizmoDrag drag;
        const auto [o0, d0] = t.ray(t.screen({0, 0, 0}));
        REQUIRE(drag.begin(scene, ids, GizmoHandle::ScaleUniform, gizmo, o0, d0));
        const auto [o1, d1] = t.ray(t.screen({0, 0, 0}) + glm::vec2(Gizmo::kArrowPixels, 0.0f));
        drag.update(o1, d1, false);
        const float grown = drag.scaled().x;
        CHECK(grown == doctest::Approx(2.0f).epsilon(0.05)); // one gizmo length right: doubled
        CHECK(drag.scaled().y == doctest::Approx(grown));
        CHECK(scene.entity(id)->transform.scale.z == doctest::Approx(3.0f * grown));
        CHECK(scene.entity(id)->transform.position.x == doctest::Approx(2.0f * grown));

        const auto [o2, d2] = t.ray(t.screen({0, 0, 0}) - glm::vec2(Gizmo::kArrowPixels, 0.0f));
        drag.update(o2, d2, false);
        CHECK(drag.scaled().x < 0.55f);
        CHECK(drag.scaled().x > 0.45f);
    }
}
