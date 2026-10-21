#include <cmath>

#include <doctest/doctest.h>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "core/camera.hpp"

using namespace lb;

TEST_CASE("reverse-Z projection: near plane at depth 1, infinity toward 0") {
    const float nearPlane = 0.05f;
    const glm::mat4 projection = infiniteReverseZPerspective(glm::radians(60.0f), 1.5f, nearPlane);
    const auto depthAt = [&](float distance) {
        const glm::vec4 clip = projection * glm::vec4(0.0f, 0.0f, -distance, 1.0f);
        return clip.z / clip.w;
    };
    CHECK(depthAt(nearPlane) == doctest::Approx(1.0f));
    CHECK(depthAt(10.0f) < depthAt(1.0f));
    CHECK(depthAt(1.0e6f) == doctest::Approx(0.0f));
    CHECK(depthAt(1.0e6f) > 0.0f);
}

TEST_CASE("projection flips Y for Vulkan clip space") {
    const glm::mat4 projection = infiniteReverseZPerspective(glm::radians(45.0f), 1.0f, 0.05f);
    const glm::vec4 above = projection * glm::vec4(0.0f, 1.0f, -5.0f, 1.0f);
    CHECK(above.y / above.w < 0.0f); // up in view space is up on screen, i.e. negative NDC y
}

TEST_CASE("orbit camera stays upright at extreme pitch") {
    OrbitCamera camera;
    camera.orbit(0.0f, 10.0f); // far past straight down/up
    const glm::vec3 offset = camera.position() - camera.target();
    CHECK(offset.z > 0.0f);
    CHECK(glm::length(glm::vec2(offset.x, offset.y)) > 0.0f); // never exactly vertical
    const glm::mat4 view = camera.viewMatrix();
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            CHECK(std::isfinite(view[c][r]));
        }
    }
}

TEST_CASE("framing centers the bounds and fits them in view") {
    OrbitCamera camera;
    const Bounds bounds{{-10.0f, -5.0f, 0.0f}, {30.0f, 5.0f, 12.0f}};
    camera.frame(bounds);
    CHECK(glm::length(camera.target() - bounds.center()) < 1e-4f);
    const float radius = glm::length(bounds.size()) * 0.5f;
    CHECK(camera.distance() * std::sin(camera.verticalFov() * 0.5f) >= radius);
}

TEST_CASE("orbiting around a point keeps that point fixed on screen") {
    OrbitCamera camera;
    camera.setState(CameraState{{2.0f, 1.0f, 0.5f}, 20.0f, 0.7f, 0.4f, 35.0f, false});
    const glm::vec3 pivot{6.0f, -3.0f, 2.0f}; // off-center, not the orbit target
    const auto screen = [&] {
        const glm::vec4 clip = camera.projectionMatrix(1.5f) * camera.viewMatrix() *
                               glm::vec4(pivot, 1.0f);
        return glm::vec3(clip) / clip.w;
    };
    const glm::vec3 before = screen();
    const float reach = glm::length(camera.position() - pivot);

    camera.orbitAround(pivot, 0.9f, -0.3f);
    CHECK(camera.state().yawRadians == doctest::Approx(1.6f));
    CHECK(camera.state().pitchRadians == doctest::Approx(0.1f));
    const glm::vec3 after = screen();
    CHECK(after.x == doctest::Approx(before.x).epsilon(1e-4));
    CHECK(after.y == doctest::Approx(before.y).epsilon(1e-4));
    CHECK(after.z == doctest::Approx(before.z).epsilon(1e-4)); // same depth: same distance
    CHECK(glm::length(camera.position() - pivot) == doctest::Approx(reach));

    // Pitch stops at the limit; the pivot still stays put rather than the view jumping.
    camera.orbitAround(pivot, 0.0f, 10.0f);
    const glm::vec3 clamped = screen();
    CHECK(clamped.x == doctest::Approx(before.x).epsilon(1e-3));
    CHECK(clamped.y == doctest::Approx(before.y).epsilon(1e-3));
}
