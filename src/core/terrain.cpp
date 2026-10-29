#include "core/terrain.hpp"

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace lb {
namespace {

constexpr const char* kTerrainName = "Terrain";
constexpr float kMinExtent = 1.0f;

float hash01(std::int32_t x, std::int32_t y, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8DA6B343u ^
                      static_cast<std::uint32_t>(y) * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// Smooth value noise in [0, 1].
float valueNoise(glm::vec2 p, std::uint32_t seed) {
    const glm::vec2 cell = glm::floor(p);
    const glm::vec2 f = p - cell;
    const auto ix = static_cast<std::int32_t>(cell.x);
    const auto iy = static_cast<std::int32_t>(cell.y);
    const float u = fade(f.x);
    const float v = fade(f.y);
    const float a = hash01(ix, iy, seed);
    const float b = hash01(ix + 1, iy, seed);
    const float c = hash01(ix, iy + 1, seed);
    const float d = hash01(ix + 1, iy + 1, seed);
    return glm::mix(glm::mix(a, b, u), glm::mix(c, d, u), v);
}

// Four octaves in [0, 1]; the large shapes dominate.
float hills(glm::vec2 p, std::uint32_t seed) {
    float sum = 0.0f;
    float amplitude = 1.0f;
    float total = 0.0f;
    for (int octave = 0; octave < 4; ++octave) {
        sum += amplitude * valueNoise(p, seed + static_cast<std::uint32_t>(octave) * 101u);
        total += amplitude;
        amplitude *= 0.5f;
        p *= 2.0f;
    }
    return sum / total;
}

float smoothstep01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

TerrainSettings clamped(TerrainSettings s) {
    s.extent = std::max(s.extent, kMinExtent);
    s.cells = std::clamp(s.cells, kMinTerrainCells, kMaxTerrainCells);
    s.hillHeight = std::max(s.hillHeight, 0.0f);
    s.hillSize = std::max(s.hillSize, 1.0f);
    s.padBlend = std::max(s.padBlend, 0.1f);
    return s;
}

} // namespace

float terrainHeightAt(const TerrainSettings& settings, glm::vec2 position) {
    const TerrainSettings s = clamped(settings);
    float height = hills(position / s.hillSize, s.seed) * s.hillHeight;
    if (s.pad) {
        // Distance from the pad's rectangle (0 inside): the hills fade in over padBlend meters.
        const glm::vec2 outside =
            glm::max(glm::max(s.padMin - position, position - s.padMax), glm::vec2(0.0f));
        height *= smoothstep01(glm::length(outside) / s.padBlend);
    }
    return s.baseHeight + height;
}

int terrainCellsFor(float extent) {
    return std::clamp(static_cast<int>(std::lround(extent / 0.5f)), 64, kMaxTerrainCells);
}

MeshData makeTerrainMesh(const TerrainSettings& settings) {
    const TerrainSettings s = clamped(settings);
    const auto side = static_cast<std::size_t>(s.cells) + 1;
    const float cell = s.extent / static_cast<float>(s.cells);
    const glm::vec2 origin = s.center - glm::vec2(s.extent * 0.5f);

    MeshData mesh;
    mesh.positions.reserve(side * side);
    mesh.normals.reserve(side * side);
    mesh.uvs.reserve(side * side);
    for (std::size_t j = 0; j < side; ++j) {
        for (std::size_t i = 0; i < side; ++i) {
            const glm::vec2 xy = origin + glm::vec2(static_cast<float>(i), static_cast<float>(j)) * cell;
            mesh.positions.emplace_back(xy, terrainHeightAt(s, xy));
            // Slope from central differences of the height function (smooth, also at the borders).
            const float dx = terrainHeightAt(s, xy + glm::vec2(cell, 0.0f)) -
                             terrainHeightAt(s, xy - glm::vec2(cell, 0.0f));
            const float dy = terrainHeightAt(s, xy + glm::vec2(0.0f, cell)) -
                             terrainHeightAt(s, xy - glm::vec2(0.0f, cell));
            mesh.normals.push_back(glm::normalize(glm::vec3(-dx, -dy, 2.0f * cell)));
            // Meters, V up the image like makeBox, so textures tile at their real size.
            mesh.uvs.emplace_back(xy.x - origin.x, -(xy.y - origin.y));
        }
    }
    mesh.indices.reserve(static_cast<std::size_t>(s.cells) * static_cast<std::size_t>(s.cells) * 6);
    for (std::size_t j = 0; j < static_cast<std::size_t>(s.cells); ++j) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(s.cells); ++i) {
            const auto a = static_cast<std::uint32_t>(j * side + i);
            const auto b = a + 1;
            const auto c = static_cast<std::uint32_t>((j + 1) * side + i + 1);
            const auto d = static_cast<std::uint32_t>((j + 1) * side + i);
            for (const std::uint32_t index : {a, b, c, a, c, d}) { // counter-clockwise from above
                mesh.indices.push_back(index);
            }
        }
    }
    return mesh;
}

EntityId findTerrain(const Scene& scene) {
    for (const EntityId root : scene.roots()) {
        const Entity* entity = scene.entity(root);
        if (entity != nullptr && entity->name == kTerrainName && entity->meshInstance) {
            return root;
        }
    }
    return {};
}

bool removeTerrain(Scene& scene) {
    const EntityId old = findTerrain(scene);
    if (!old) {
        return false;
    }
    const ResourceId mesh = scene.entity(old)->meshInstance->mesh;
    scene.destroyEntity(old);
    if (mesh && scene.userCount(mesh) == 0) {
        scene.removeResource(mesh);
    }
    return true;
}

EntityId setTerrain(Scene& scene, const TerrainSettings& settings, ResourceId material) {
    removeTerrain(scene);
    const ResourceId mesh = scene.addMesh(kTerrainName, makeTerrainMesh(settings));
    const EntityId id = scene.createEntity(kTerrainName);
    scene.setMeshInstance(id, MeshInstance{mesh, material});
    return id;
}

std::optional<Bounds> contentBounds(const Scene& scene) {
    const EntityId terrain = findTerrain(scene);
    std::vector<EntityId> others;
    for (const EntityId root : scene.roots()) {
        if (root != terrain) {
            others.push_back(root);
        }
    }
    return worldBoundsOf(scene, others);
}

TerrainSettings terrainAroundContent(const Scene& scene) {
    TerrainSettings settings;
    const std::optional<Bounds> bounds = contentBounds(scene);
    if (!bounds) {
        return settings;
    }
    const glm::vec3 size = bounds->size();
    const float larger = std::max(size.x, size.y);
    settings.center = glm::vec2(bounds->center());
    settings.extent = std::clamp(std::max(larger * 3.0f, 30.0f), 30.0f, 1000.0f);
    settings.cells = terrainCellsFor(settings.extent);
    settings.hillSize = std::max(settings.extent * 0.3f, 10.0f);
    settings.hillHeight = std::clamp(settings.extent * 0.02f, 0.5f, 6.0f);
    settings.padBlend = std::max(larger * 0.6f, 6.0f);
    // A hair above the content's lowest point (and so above the z = 0 grid and the ray tracer's
    // infinite ground plane, which would otherwise cover the flat part): the content's own bottom
    // faces stay hidden under it.
    settings.baseHeight = bounds->min.z + 0.005f;
    settings.pad = true;
    const glm::vec2 margin(1.0f);
    settings.padMin = glm::vec2(bounds->min) - margin;
    settings.padMax = glm::vec2(bounds->max) + margin;
    return settings;
}

} // namespace lb
