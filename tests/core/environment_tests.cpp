#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <glm/geometric.hpp>

#include "core/primitives.hpp"
#include "core/scene/environment.hpp"
#include "core/scene/scene.hpp"

using namespace lb;

namespace {

bool near(glm::vec3 a, glm::vec3 b) {
    return glm::length(a - b) < 1e-5f;
}

glm::vec3 sunAt(float azimuth, float elevation) {
    return sunDirectionFromAngles(SunAngles{azimuth, elevation});
}

} // namespace

TEST_CASE("sun azimuth is a compass bearing from project north, clockwise") {
    CHECK(near(sunAt(0.0f, 0.0f), {0.0f, 1.0f, 0.0f}));   // north = +Y
    CHECK(near(sunAt(90.0f, 0.0f), {1.0f, 0.0f, 0.0f}));  // east = +X
    CHECK(near(sunAt(180.0f, 0.0f), {0.0f, -1.0f, 0.0f})); // south
    CHECK(near(sunAt(270.0f, 0.0f), {-1.0f, 0.0f, 0.0f})); // west
    CHECK(near(sunAt(123.0f, 90.0f), {0.0f, 0.0f, 1.0f})); // zenith, any azimuth

    const glm::vec3 afternoon = sunAt(225.0f, 40.0f);
    CHECK(glm::length(afternoon) == doctest::Approx(1.0f));
    CHECK(afternoon.x < 0.0f);
    CHECK(afternoon.y < 0.0f); // south-west
    CHECK(afternoon.z == doctest::Approx(0.642788f));
}

TEST_CASE("location mode derives the sun from the site and rotates by the north angle") {
    Environment environment;
    environment.sun.mode = SunMode::Location;
    environment.sun.location = SiteLocation{51.5074, -0.1278, 1.0f, 0.0f};
    environment.sun.dateTime = LocalDateTime{2024, 6, 21, 13.0f};
    const SunAngles trueNorth = environment.sunAngles();
    CHECK(trueNorth.azimuthDegrees == doctest::Approx(178.8f).epsilon(0.002));
    CHECK(trueNorth.elevationDegrees == doctest::Approx(61.9f).epsilon(0.002));

    // Project north rotated: true north lies 30° clockwise from +Y, so bearings shift by +30°.
    environment.sun.location.northAngleDegrees = 30.0f;
    const SunAngles rotated = environment.sunAngles();
    CHECK(rotated.azimuthDegrees == doctest::Approx(trueNorth.azimuthDegrees + 30.0f));
    CHECK(rotated.elevationDegrees == doctest::Approx(trueNorth.elevationDegrees));

    environment.sun.mode = SunMode::Manual; // manual angles are used as-is
    CHECK(environment.sunAngles().azimuthDegrees == doctest::Approx(environment.sun.azimuthDegrees));
}

TEST_CASE("environment edits notify once and ignore no-ops") {
    Scene scene;
    int changes = 0;
    scene.addListener([&](const SceneChange& change) {
        changes += change.kind == SceneChange::Kind::EnvironmentChanged ? 1 : 0;
    });
    Environment environment = scene.environment();
    scene.setEnvironment(environment); // unchanged
    environment.sun.elevationDegrees = 12.0f;
    scene.setEnvironment(environment);
    CHECK(changes == 1);
    CHECK(scene.environment().sun.elevationDegrees == doctest::Approx(12.0f));
}

TEST_CASE("resource names and usage counts") {
    Scene scene;
    const ResourceId cube = scene.addMesh("cube", makeBox({0, 0, 0}, {1, 1, 1}));
    const ResourceId clay = scene.addMaterial("Clay", Material{});
    const ResourceId glass = scene.addMaterial("Glass", Material{});
    for (int i = 0; i < 3; ++i) {
        const EntityId e = scene.createEntity("box");
        scene.setMeshInstance(e, MeshInstance{cube, i < 2 ? clay : glass});
    }
    CHECK(scene.userCount(clay) == 2);
    CHECK(scene.userCount(glass) == 1);
    CHECK(scene.userCount(cube) == 3);

    scene.setResourceName(clay, "White clay");
    CHECK(scene.material(clay)->name == "White clay");
    CHECK_THROWS_AS(scene.setResourceName(ResourceId{99}, "x"), std::invalid_argument);

    std::vector<std::string> names;
    scene.forEachMaterial([&](const MaterialResource& m) { names.push_back(m.name); });
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"Glass", "White clay"});
}
