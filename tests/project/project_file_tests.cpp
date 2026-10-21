#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "core/primitives.hpp"
#include "core/scene/scene.hpp"
#include "project/project_file.hpp"

using namespace lb;
namespace fs = std::filesystem;

namespace {

fs::path scratchFile(const char* name) {
    std::random_device random;
    return fs::temp_directory_path() /
           (std::string("lightbox-project-test-") + std::to_string(random()) + name);
}

// A small scene using every saved feature: hierarchy, transforms, a textured glass material, an
// emissive material, all light types, environment and look settings.
void buildScene(Scene& scene) {
    ImageData image;
    image.width = 2;
    image.height = 1;
    image.pixels = {255, 0, 0, 255, 0, 255, 0, 255};
    const ResourceId texture = scene.addTexture("tint", image, "C:/textures/tint.png");

    Material glass = materialWithColor({0.9f, 0.95f, 1.0f});
    glass.transmission = 1.0f;
    glass.ior = 1.52f;
    glass.thinWalled = false;
    glass.roughness = 0.05f;
    glass.baseColorMap.texture = texture;
    const ResourceId glassMaterial = scene.addMaterial("Glass", glass);
    Material lamp = materialWithColor({1.0f, 1.0f, 1.0f});
    lamp.emissionColor = {1.0f, 0.8f, 0.6f};
    lamp.emissionStrength = 1200.0f;
    const ResourceId lampMaterial = scene.addMaterial("Lamp shade", lamp);

    const ResourceId box = scene.addMesh("box", makeBox({0, 0, 0}, {1, 2, 3}));
    Transform groupTransform;
    groupTransform.position = {4, -2, 0.5f};
    const EntityId group = scene.createEntity("House", {}, groupTransform);
    const EntityId pane = scene.createEntity("Pane", group);
    scene.setMeshInstance(pane, MeshInstance{box, glassMaterial});
    const EntityId shade = scene.createEntity("Shade", group);
    scene.setMeshInstance(shade, MeshInstance{box, lampMaterial});

    Light point;
    point.lumens = 800.0f;
    point.temperatureKelvin = 2700.0f;
    const EntityId bulb = scene.createEntity("Bulb", group);
    scene.setLight(bulb, point);
    Light spot;
    spot.type = LightType::Spot;
    spot.spotAngleDegrees = 35.0f;
    spot.spotSoftness = 0.6f;
    scene.setLight(scene.createEntity("Downlight"), spot);
    Light area;
    area.type = LightType::Area;
    area.areaSize = {1.2f, 0.3f};
    area.lumens = 4000.0f;
    scene.setLight(scene.createEntity("Panel"), area);

    Look look;
    look.autoExposure = false;
    look.toneMapping = ToneMapping::AgX;
    look.exposureEv = 1.5f;
    scene.setLook(look);

    Environment environment = scene.environment();
    environment.sky.image = "C:/Skies/Ünterberg dusk.hdr"; // non-ASCII survives as UTF-8
    environment.sky.imageRotationDegrees = 137.5f;
    scene.setEnvironment(environment);
}

} // namespace

TEST_CASE("projects round-trip lights, glass, emission, sky image and look settings exactly") {
    Scene scene;
    buildScene(scene);
    const fs::path file = scratchFile(".lbox");
    project::ProjectInfo info;
    info.name = "Round trip";
    project::saveProject(scene, info, file);

    const project::LoadedProject loaded = project::loadProject(file);
    CHECK(loaded.info.name == "Round trip");
    Scene restored;
    restored.replaceContents(loaded.scene);
    CHECK(project::documentJson(restored) == project::documentJson(scene));

    // Spot-check that the new data really arrived (not just default-equal on both sides).
    int lights = 0;
    restored.traverse([&](const Entity& entity, const glm::mat4&) {
        if (entity.light && entity.light->type == LightType::Area) {
            CHECK(entity.light->areaSize.x == doctest::Approx(1.2f));
            CHECK(entity.light->lumens == doctest::Approx(4000.0f));
        }
        lights += entity.light ? 1 : 0;
    });
    CHECK(lights == 3);
    bool sawGlass = false;
    restored.forEachMaterial([&](const MaterialResource& material) {
        if (material.name == "Glass") {
            sawGlass = true;
            CHECK(material.material.transmission == doctest::Approx(1.0f));
            CHECK(material.material.ior == doctest::Approx(1.52f));
            CHECK_FALSE(material.material.thinWalled);
        }
    });
    CHECK(sawGlass);
    CHECK(restored.look().toneMapping == ToneMapping::AgX);
    CHECK_FALSE(restored.look().autoExposure);
    CHECK(restored.environment().sky.image == "C:/Skies/Ünterberg dusk.hdr");
    CHECK(restored.environment().sky.imageRotationDegrees == doctest::Approx(137.5f));
    fs::remove(file);
}

TEST_CASE("projects refuse files that aren't LightBox projects") {
    const fs::path file = scratchFile(".lbox");
    std::ofstream(file, std::ios::binary) << "not a zip";
    CHECK_THROWS_AS((void)project::loadProject(file), project::ProjectError);
    fs::remove(file);
    CHECK_THROWS_AS((void)project::loadProject(file), project::ProjectError); // missing file
}
