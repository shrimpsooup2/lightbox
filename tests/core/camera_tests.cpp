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
