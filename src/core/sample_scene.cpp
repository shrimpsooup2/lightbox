#include "core/sample_scene.hpp"

#include "core/primitives.hpp"

namespace lb {

EntityId addSampleMassing(Scene& scene) {
    // Every box shares one unit cube, placed by its transform (instancing).
    const ResourceId cube = scene.addMesh("Unit cube", makeBox({0, 0, 0}, {1, 1, 1}));
    const ResourceId clay = scene.addMaterial("Clay", materialWithColor({0.80f, 0.78f, 0.74f}));
    const ResourceId plinth = scene.addMaterial("Plinth", materialWithColor({0.55f, 0.55f, 0.53f}));
    const ResourceId canopy = scene.addMaterial("Canopy", materialWithColor({0.70f, 0.70f, 0.70f}));

    const auto addBox = [&](const char* name, EntityId parent, glm::vec3 min, glm::vec3 max,
                            ResourceId material) {
        Transform transform;
        transform.position = min;
        transform.scale = max - min;
        const EntityId box = scene.createEntity(name, parent, transform);
        scene.setMeshInstance(box, MeshInstance{cube, material});
        return box;
    };

    // A small two-storey house with a single-storey wing, a stair tower and a terrace canopy (meters).
    const EntityId house = scene.createEntity("Sample house");
    addBox("Site plinth", house, {-11.0f, -7.0f, 0.0f}, {13.0f, 10.0f, 0.3f}, plinth);
    addBox("Main volume", house, {-8.0f, -4.0f, 0.3f}, {4.0f, 4.0f, 6.8f}, clay);
    addBox("Wing", house, {4.0f, -4.0f, 0.3f}, {10.0f, 1.0f, 3.6f}, clay);
    addBox("Stair tower", house, {-8.0f, 4.0f, 0.3f}, {-4.5f, 7.5f, 9.3f}, clay);

    const EntityId terrace = scene.createEntity("Terrace canopy", house);
    addBox("Canopy slab", terrace, {4.0f, 1.0f, 3.6f}, {10.0f, 6.0f, 3.85f}, canopy);
    addBox("Column", terrace, {9.6f, 5.6f, 0.3f}, {9.85f, 5.85f, 3.6f}, canopy);
    addBox("Column", terrace, {4.15f, 5.6f, 0.3f}, {4.4f, 5.85f, 3.6f}, canopy);
    return house;
}

} // namespace lb
