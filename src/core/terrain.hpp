#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <glm/vec2.hpp>

#include "core/mesh.hpp"
#include "core/scene/scene.hpp"

namespace lb {

// Generated ground (ROADMAP 5.1): a square height field of gently rolling hills with an optional
// flat pad under the building that blends out into them. The ground is one mesh object named
// "Terrain" at the top of the scene; making it again replaces it.
struct TerrainSettings {
    glm::vec2 center{0.0f}; // world position of the middle (meters)
    float extent = 80.0f;   // side length (meters)
    int cells = 128;        // grid cells per side (the mesh has (cells + 1)² vertices)
    float hillHeight = 1.5f; // highest hills above the base height (meters); hills never dip below it
    float hillSize = 25.0f;  // typical distance between hill tops (meters)
    std::uint32_t seed = 1;  // another seed, another landscape
    float baseHeight = 0.0f; // height of the flattest ground (meters)
    bool pad = false;        // flat ground under the building
    glm::vec2 padMin{0.0f};  // the pad's rectangle (world meters); flat at baseHeight inside
    glm::vec2 padMax{0.0f};
    float padBlend = 12.0f;  // distance over which the pad rises into the hills (meters)

    bool operator==(const TerrainSettings&) const = default;
};

constexpr int kMinTerrainCells = 16;
constexpr int kMaxTerrainCells = 256;

// Ground height at a world position (meters). Pure function of the settings.
float terrainHeightAt(const TerrainSettings& settings, glm::vec2 position);
// Cells per side that give about half-meter quads (within the limits above).
int terrainCellsFor(float extent);
// The mesh: world-space positions, smooth normals, UVs in meters (textures tile at real-world
// size). Settings are clamped to sane ranges (extent ≥ 1 m, cells within the limits).
MeshData makeTerrainMesh(const TerrainSettings& settings);

// The ground object of a scene (root entity named "Terrain" with a mesh), or none.
EntityId findTerrain(const Scene& scene);
// Smooth value noise in [0, 1] (the ground's hills and the Roughen brush use it).
float terrainNoise(glm::vec2 position, std::uint32_t seed);
// Hand-sculpted ground: its mesh is renamed so that the generator's sliders leave it alone, also
// after the project is reopened. markSculpted does nothing without a ground.
bool isSculpted(const Scene& scene);
void markSculpted(Scene& scene);
// Makes the ground (replacing the previous one and its mesh) with `material` (none = default clay);
// returns its entity. Not selected, not placed in any other way.
EntityId setTerrain(Scene& scene, const TerrainSettings& settings, ResourceId material);
// Removes the ground and its mesh. Returns whether there was one.
bool removeTerrain(Scene& scene);

// Bounds of everything except the ground (what the design is about: framing, placing imports).
std::optional<Bounds> contentBounds(const Scene& scene);
// Settings for ground under and around the content: centered on it, `margin` times the larger side
// of the content on each side (at least 30 m in total), flat under its footprint at its base. Falls
// back to the defaults around the origin for an empty scene.
TerrainSettings terrainAroundContent(const Scene& scene);

} // namespace lb
