#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "core/mesh.hpp"

namespace lb {

// Sculpting the ground (ROADMAP 5.2). The ground mesh from makeTerrainMesh is a regular grid, so its
// vertices ARE the height field: a HeightGrid is read from the mesh, brushes change the heights, and
// writeHeights puts them (and the normals) back. Nothing else is stored, so sculpting saves with the
// project and survives reopening.
struct HeightGrid {
    int cells = 0;            // quads per side; the grid has (cells + 1)² heights
    glm::vec2 origin{0.0f};   // world xy of height (0, 0)
    float cellSize = 1.0f;    // meters
    std::vector<float> heights; // row-major, rows going +y

    int side() const { return cells + 1; }
    float at(int i, int j) const { return heights[static_cast<std::size_t>(j * side() + i)]; }
    float& at(int i, int j) { return heights[static_cast<std::size_t>(j * side() + i)]; }
    glm::vec2 maxCorner() const { return origin + glm::vec2(static_cast<float>(cells) * cellSize); }
    // Bilinear height at a world position (clamped to the grid's edge).
    float heightAt(glm::vec2 position) const;
    // First point where the ray meets the ground from above; nullopt when it misses or starts
    // below the surface.
    std::optional<glm::vec3> intersect(const glm::vec3& origin, const glm::vec3& direction) const;
};

// The grid behind a terrain mesh; nullopt when the mesh is not a regular square grid.
std::optional<HeightGrid> heightGridFromMesh(const MeshData& mesh);

enum class BrushMode {
    Raise,   // push the ground up
    Lower,   // push it down
    Smooth,  // even out bumps
    Flatten, // level toward a height (the one under the cursor when the stroke began)
    Roughen, // add small bumps
};

struct Brush {
    BrushMode mode = BrushMode::Raise;
    float radius = 10.0f;   // meters; the effect fades out smoothly toward the edge
    float strength = 0.5f;  // 0…1: Raise/Lower move up to 6 m per second at 1
};

// Vertices touched by a brush (inclusive index range); empty when nothing was.
struct GridRegion {
    int i0 = 0;
    int j0 = 0;
    int i1 = -1;
    int j1 = -1;
    bool empty() const { return i1 < i0 || j1 < j0; }
};

// Applies the brush for `seconds` at `center` (world xy). `flattenHeight` is the level Flatten
// aims for; `seed` makes a stroke's Roughen pattern its own. Returns the region that may have
// changed.
GridRegion applyBrush(HeightGrid& grid, const Brush& brush, glm::vec2 center, float seconds,
                      float flattenHeight, std::uint32_t seed);

// Writes the heights of `region` into the mesh's positions and refreshes the normals of the region
// and its ring of neighbors. The mesh must be the one the grid was read from.
void writeHeights(MeshData& mesh, const HeightGrid& grid, GridRegion region);
GridRegion wholeGrid(const HeightGrid& grid);

} // namespace lb
