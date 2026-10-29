#include <algorithm>
#include <cmath>

#include <doctest/doctest.h>
#include <glm/geometric.hpp>

#include "core/primitives.hpp"
#include "core/terrain.hpp"

using namespace lb;

namespace {

TerrainSettings smallTerrain() {
    TerrainSettings s;
    s.extent = 60.0f;
    s.cells = 48;
    s.hillHeight = 3.0f;
    s.hillSize = 15.0f;
    return s;
}

} // namespace

TEST_CASE("terrain mesh is a closed grid with unit upward normals and meter UVs") {
    const TerrainSettings s = smallTerrain();
    const MeshData mesh = makeTerrainMesh(s);
    const std::size_t vertices = 49 * 49;
    CHECK(mesh.positions.size() == vertices);
    CHECK(mesh.normals.size() == vertices);
    CHECK(mesh.uvs.size() == vertices);
    CHECK(mesh.indices.size() == 48u * 48u * 6u);
    CHECK(*std::max_element(mesh.indices.begin(), mesh.indices.end()) == vertices - 1);
    for (const glm::vec3& n : mesh.normals) {
        CHECK(glm::length(n) == doctest::Approx(1.0f).epsilon(1e-4));
        CHECK(n.z > 0.0f);
    }
    // Covers the extent around the center, heights within [base, base + hillHeight].
    const auto [lo, hi] = std::minmax_element(
        mesh.positions.begin(), mesh.positions.end(),
        [](const glm::vec3& a, const glm::vec3& b) { return a.z < b.z; });
    CHECK(lo->z >= s.baseHeight - 1e-4f);
    CHECK(hi->z <= s.baseHeight + s.hillHeight + 1e-4f);
    CHECK(mesh.positions.front().x == doctest::Approx(-30.0f));
    CHECK(mesh.positions.back().y == doctest::Approx(30.0f));
    // UVs advance by one per meter along the first row.
    CHECK(mesh.uvs[1].x - mesh.uvs[0].x == doctest::Approx(60.0f / 48.0f));
    // Triangles wind counter-clockwise seen from above (the surface faces up).
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const glm::vec3 a = mesh.positions[mesh.indices[i]];
        const glm::vec3 b = mesh.positions[mesh.indices[i + 1]];
        const glm::vec3 c = mesh.positions[mesh.indices[i + 2]];
        CHECK(glm::cross(b - a, c - a).z > 0.0f);
    }
}

TEST_CASE("the seed decides the landscape, and the same seed gives the same one") {
    TerrainSettings a = smallTerrain();
    TerrainSettings b = a;
    CHECK(makeTerrainMesh(a).positions == makeTerrainMesh(b).positions);
    b.seed = 2;
    CHECK(makeTerrainMesh(a).positions != makeTerrainMesh(b).positions);
    CHECK(terrainHeightAt(a, {5.0f, 7.0f}) == terrainHeightAt(a, {5.0f, 7.0f}));
}

TEST_CASE("the pad is flat at the base height and rises smoothly into the hills") {
    TerrainSettings s = smallTerrain();
    s.pad = true;
    s.padMin = {-5.0f, -4.0f};
    s.padMax = {6.0f, 3.0f};
    s.padBlend = 10.0f;
    s.baseHeight = 0.2f;
    // Flat everywhere on and inside the pad.
    for (const glm::vec2 p : {glm::vec2(0.0f), glm::vec2(-5.0f, -4.0f), glm::vec2(6.0f, 3.0f),
                              glm::vec2(2.0f, -3.0f)}) {
        CHECK(terrainHeightAt(s, p) == doctest::Approx(0.2f).epsilon(1e-6));
    }
    // Beyond the blend distance the pad no longer matters.
    TerrainSettings free = s;
    free.pad = false;
    const glm::vec2 far(6.0f + 10.0f, 0.0f);
    CHECK(terrainHeightAt(s, far) == doctest::Approx(terrainHeightAt(free, far)));
    // Across the blend, the height changes gradually: no steps (a slope under 1:1 per 0.1 m step).
    float previous = terrainHeightAt(s, {6.0f, 0.0f});
    for (float x = 6.1f; x <= 16.0f; x += 0.1f) {
        const float height = terrainHeightAt(s, {x, 0.0f});
        CHECK(std::abs(height - previous) < 0.1f);
        previous = height;
    }
}

TEST_CASE("terrain around content is centered on it and flat under it, ignoring the old ground") {
    Scene scene;
    const ResourceId box = scene.addMesh("Box", makeBox({0, 0, 0}, {1, 1, 1}));
    Transform t;
    t.position = {20.0f, -10.0f, 0.0f};
    t.scale = {12.0f, 8.0f, 6.0f};
    const EntityId house = scene.createEntity("House", {}, t);
    scene.setMeshInstance(house, MeshInstance{box, {}});

    const TerrainSettings first = terrainAroundContent(scene);
    CHECK(first.center.x == doctest::Approx(26.0f));
    CHECK(first.center.y == doctest::Approx(-6.0f));
    CHECK(first.pad);
    CHECK(first.padMin.x <= 20.0f);
    CHECK(first.padMax.y >= -2.0f);
    CHECK(first.extent >= 36.0f);
    // The ground just under the content's footprint: a hair above its lowest point.
    CHECK(terrainHeightAt(first, {26.0f, -6.0f}) == doctest::Approx(0.005f));

    // Making the ground adds it once; the next one replaces it (and its mesh), and then the content
    // bounds and settings stay the same.
    const EntityId ground = setTerrain(scene, first, {});
    CHECK(findTerrain(scene) == ground);
    CHECK(scene.entityCount() == 2);
    const EntityId again = setTerrain(scene, first, {});
    CHECK(findTerrain(scene) == again);
    CHECK(scene.entityCount() == 2);
    int meshes = 0;
    scene.forEachMesh([&](const MeshResource&) { ++meshes; });
    CHECK(meshes == 2); // the box and one ground mesh
    CHECK(terrainAroundContent(scene) == first);

    CHECK(removeTerrain(scene));
    CHECK_FALSE(findTerrain(scene));
    CHECK_FALSE(removeTerrain(scene));
    meshes = 0;
    scene.forEachMesh([&](const MeshResource&) { ++meshes; });
    CHECK(meshes == 1);
}
