#include <algorithm>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "core/primitives.hpp"
#include "core/scene/scene.hpp"
#include "core/scene/similar.hpp"

using namespace lb;

namespace {

std::vector<EntityId> sorted(std::vector<EntityId> ids) {
    std::sort(ids.begin(), ids.end(), [](EntityId a, EntityId b) { return a.value < b.value; });
    return ids;
}

} // namespace

TEST_CASE("Copies of a named object share a similarity key, whatever the exporter wrote") {
    for (const char* name : {"Window", "Window_01", "window.003", "Window (2)", "Window #4",
                             "Window copy", "Window copy 2", "Window_copy3", "Copy of Window",
                             "WINDOW 12", "Window01", "  window-7  "}) {
        CAPTURE(name);
        CHECK(similarityKey(name) == "window");
    }
    CHECK(similarityKey("WindowFrame_2") == "window frame");
    CHECK(similarityKey("Window Frame.001") == "window frame");
    CHECK(similarityKey("window_frame") == "window frame");
    // Revit: family, type and element id.
    CHECK(similarityKey("Basic Wall: Exterior - Brick: 345678") ==
          similarityKey("Basic Wall: Exterior - Brick: 401122"));
    // Numbers inside a name are instance details too: every floor's slab is a slab.
    CHECK(similarityKey("Level 1 Slab") == similarityKey("Level 2 Slab"));
    // Different words stay different.
    CHECK(similarityKey("Window") != similarityKey("Door"));
    CHECK(similarityKey("Tile A") != similarityKey("Tile B"));
    CHECK(similarityKey("Windowsill") != similarityKey("Window"));
    // Non-ASCII names keep their letters.
    CHECK(similarityKey("Fenêtre 2") == similarityKey("fenêtre_5"));
}

TEST_CASE("Generic names keep their number: nobody named those, the number is the identity") {
    CHECK(similarityKey("Material14") == "material 14");
    CHECK(similarityKey("Material14") != similarityKey("Material1"));
    CHECK(similarityKey("Object_12") != similarityKey("Object_13"));
    CHECK(similarityKey("Cube.034") != similarityKey("Cube.035"));
    CHECK(similarityKey("Mesh") == "mesh");
    CHECK(similarityKey("Collada visual scene group") == "collada visual scene group");
}

TEST_CASE("Imported multi-material parts match on both the object and the part name") {
    CHECK(similarityKey("Window_03 \xC2\xB7 Glass") == similarityKey("Window_1 \xC2\xB7 Glass"));
    CHECK(similarityKey("Window_03 \xC2\xB7 Glass") != similarityKey("Window_03 \xC2\xB7 Frame"));
    CHECK(similarityKey("Window_03 \xC2\xB7 Glass") != similarityKey("Door_03 \xC2\xB7 Glass"));
}

TEST_CASE("Empty or punctuation-only names have no key") {
    CHECK(similarityKey("").empty());
    CHECK(similarityKey("   ").empty());
    CHECK(similarityKey("--_.").empty());
}

TEST_CASE("withSimilarObjects finds same-named objects, copies of the geometry and nothing else") {
    Scene scene;
    // Every object has its own geometry, as in an import; only the lamps share one.
    const auto object = [&](const char* name, EntityId parent) {
        const EntityId id = scene.createEntity(name, parent);
        scene.setMeshInstance(id, MeshInstance{scene.addMesh(name, makeBox({0, 0, 0}, {1, 1, 1})), {}});
        return id;
    };
    const ResourceId lampShape = scene.addMesh("Lamp shape", makeBox({0, 0, 0}, {1, 1, 2}));

    const EntityId house = scene.createEntity("House");
    const EntityId w1 = object("Window_01", house);
    const EntityId w2 = object("Window_02", house);
    const EntityId w3 = object("Window (3)", house);
    const EntityId door = object("Door_01", house);
    const EntityId chair1 = scene.createEntity("Chair 1", house);
    const EntityId seat1 = object("Chair 1 \xC2\xB7 Seat", chair1);
    const EntityId legs1 = object("Chair 1 \xC2\xB7 Legs", chair1);
    const EntityId chair2 = scene.createEntity("Chair 2", house);
    const EntityId seat2 = object("Chair 2 \xC2\xB7 Seat", chair2);
    const EntityId legs2 = object("Chair 2 \xC2\xB7 Legs", chair2);
    const EntityId lamp = scene.createEntity("Lamp", house);
    scene.setMeshInstance(lamp, MeshInstance{lampShape, {}});
    Transform elsewhere;
    elsewhere.position = {5.0f, 0.0f, 0.0f};
    const EntityId pendant = scene.createEntity("Pendant", house, elsewhere); // a copy, other name
    scene.setMeshInstance(pendant, MeshInstance{lampShape, {}});
    const EntityId window4 = scene.createEntity("Window_04", house); // a group, not a mesh
    const EntityId window4Glass = object("Glass", window4);
    Light bulb;
    scene.setLight(scene.createEntity("Window_05"), bulb); // a light: never gets materials

    SUBCASE("same name stem, mesh objects only") {
        CHECK(sorted(withSimilarObjects(scene, std::vector{w1})) == sorted({w1, w2, w3}));
    }
    SUBCASE("parts match parts with the same part name") {
        CHECK(sorted(withSimilarObjects(scene, std::vector{seat1})) == sorted({seat1, seat2}));
    }
    SUBCASE("a selected group brings the contents of groups named like it") {
        CHECK(sorted(withSimilarObjects(scene, std::vector{chair1})) ==
              sorted({seat1, legs1, seat2, legs2}));
    }
    SUBCASE("copies of the same geometry count, whatever their name") {
        CHECK(sorted(withSimilarObjects(scene, std::vector{lamp})) == sorted({lamp, pendant}));
    }
    SUBCASE("several selected objects bring their own similar objects") {
        CHECK(sorted(withSimilarObjects(scene, std::vector{w2, door})) == sorted({w1, w2, w3, door}));
    }
    SUBCASE("a group named like a window doesn't drag its glass into the windows") {
        const auto result = withSimilarObjects(scene, std::vector{w1});
        CHECK(std::find(result.begin(), result.end(), window4Glass) == result.end());
    }
}

TEST_CASE("Scaled instances of one shape are not copies (the sample house's massing boxes)") {
    Scene scene;
    const ResourceId cube = scene.addMesh("Unit cube", makeBox({0, 0, 0}, {1, 1, 1}));
    const auto box = [&](const char* name, glm::vec3 size) {
        Transform transform;
        transform.scale = size;
        const EntityId id = scene.createEntity(name, {}, transform);
        scene.setMeshInstance(id, MeshInstance{cube, {}});
        return id;
    };
    const EntityId mainVolume = box("Main volume", {12.0f, 8.0f, 6.5f});
    box("Wing", {6.0f, 5.0f, 3.3f});
    const EntityId columnA = box("Column", {0.25f, 0.25f, 3.3f});
    const EntityId columnB = box("Column", {0.25f, 0.25f, 3.3f});

    CHECK(withSimilarObjects(scene, std::vector{mainVolume}) == std::vector{mainVolume});
    CHECK(sorted(withSimilarObjects(scene, std::vector{columnA})) == sorted({columnA, columnB}));
}

TEST_CASE("Objects with the same or a similarly named material are similar") {
    Scene scene;
    const auto object = [&](const char* name, EntityId parent, ResourceId material) {
        const EntityId id = scene.createEntity(name, parent);
        scene.setMeshInstance(
            id, MeshInstance{scene.addMesh(name, makeBox({0, 0, 0}, {1, 1, 1})), material});
        return id;
    };
    const ResourceId brick1 = scene.addMaterial("Brick_01", materialWithColor({0.6f, 0.3f, 0.2f}));
    const ResourceId brick2 = scene.addMaterial("brick.002", materialWithColor({0.6f, 0.3f, 0.2f}));
    const ResourceId redBrick = scene.addMaterial("Red brick", materialWithColor({0.7f, 0.2f, 0.1f}));
    const ResourceId glass = scene.addMaterial("Glass", materialWithColor({0.9f, 0.9f, 0.9f}));
    const ResourceId m14 = scene.addMaterial("Material14", materialWithColor({0.5f, 0.5f, 0.5f}));
    const ResourceId m14other = scene.addMaterial("Material14", materialWithColor({0.4f, 0.4f, 0.4f}));

    const EntityId house = scene.createEntity("House");
    const EntityId north = object("North wall", house, brick1);
    const EntityId south = object("South wall", house, brick2);    // similar material name
    const EntityId chimney = object("Chimney", house, brick1);     // same material
    object("Garden wall", house, redBrick);                       // a different material
    object("Window", house, glass);
    const EntityId plinth = object("Plinth", house, {});          // no material: default clay
    object("Path", house, {});
    const EntityId a = object("Object_1", house, m14);
    const EntityId b = object("Object_2", house, m14);
    const EntityId other = scene.createEntity("Other model");
    object("Object_3", other, m14other); // same generic material name, other model

    CHECK(sorted(withSimilarObjects(scene, std::vector{north})) == sorted({north, south, chimney}));
    // Default clay doesn't make objects similar.
    CHECK(withSimilarObjects(scene, std::vector{plinth}) == std::vector{plinth});
    // Generic material names: identical name, same model only.
    CHECK(sorted(withSimilarObjects(scene, std::vector{a})) == sorted({a, b}));
}

TEST_CASE("Generic names match identical names within the same model only") {
    Scene scene;
    const ResourceId a = scene.addMesh("A", makeBox({0, 0, 0}, {1, 1, 1}));
    const ResourceId b = scene.addMesh("B", makeBox({0, 0, 0}, {2, 1, 1}));
    const ResourceId c = scene.addMesh("C", makeBox({0, 0, 0}, {3, 1, 1}));
    const ResourceId d = scene.addMesh("D", makeBox({0, 0, 0}, {4, 1, 1}));
    const auto object = [&](const char* name, EntityId parent, ResourceId mesh) {
        const EntityId id = scene.createEntity(name, parent);
        scene.setMeshInstance(id, MeshInstance{mesh, {}});
        return id;
    };
    // A Sketchfab-style model: nodes named after their material.
    const EntityId model = scene.createEntity("Sketchfab_model");
    const EntityId m14a = object("Material14", model, a);
    const EntityId m14b = object("Material14", model, b);
    object("Material1", model, c); // a different material number: different kind
    // Another model that happens to use the same generic name.
    const EntityId other = scene.createEntity("Other model");
    object("Material14", other, d);

    CHECK(sorted(withSimilarObjects(scene, std::vector{m14a})) == sorted({m14a, m14b}));
}
