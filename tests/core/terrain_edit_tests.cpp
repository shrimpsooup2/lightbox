#include <algorithm>
#include <cmath>
#include <numeric>

#include <doctest/doctest.h>
#include <glm/geometric.hpp>

#include "core/terrain.hpp"
#include "core/terrain_edit.hpp"

using namespace lb;

namespace {

TerrainSettings flatSettings() {
    TerrainSettings s;
    s.extent = 40.0f;
    s.cells = 40; // 1 m cells, centered on the origin: x, y from -20 to 20
    s.hillHeight = 0.0f;
    return s;
}

HeightGrid flatGrid() {
    return *heightGridFromMesh(makeTerrainMesh(flatSettings()));
}

float variation(const HeightGrid& g) { // how bumpy: mean absolute difference of neighbors
    float sum = 0.0f;
    int count = 0;
    for (int j = 0; j < g.cells; ++j) {
        for (int i = 0; i < g.cells; ++i) {
            sum += std::abs(g.at(i + 1, j) - g.at(i, j)) + std::abs(g.at(i, j + 1) - g.at(i, j));
            count += 2;
        }
    }
    return sum / static_cast<float>(count);
}

} // namespace

TEST_CASE("a terrain mesh is read back as the grid it came from, anything else is refused") {
    TerrainSettings s = flatSettings();
    s.hillHeight = 3.0f;
    s.hillSize = 8.0f;
    const MeshData mesh = makeTerrainMesh(s);
    const std::optional<HeightGrid> grid = heightGridFromMesh(mesh);
    REQUIRE(grid);
    CHECK(grid->cells == 40);
    CHECK(grid->origin.x == doctest::Approx(-20.0f));
    CHECK(grid->cellSize == doctest::Approx(1.0f));
    CHECK(grid->heights.size() == 41u * 41u);
    CHECK(grid->heightAt({3.25f, -4.5f}) == doctest::Approx(terrainHeightAt(s, {3.25f, -4.5f})).epsilon(0.05));

    MeshData notAGrid = mesh;
    notAGrid.positions.pop_back();
    CHECK_FALSE(heightGridFromMesh(notAGrid));
    MeshData shuffled = mesh;
    std::swap(shuffled.positions[1], shuffled.positions[5]);
    CHECK_FALSE(heightGridFromMesh(shuffled));
    CHECK_FALSE(heightGridFromMesh(MeshData{}));
}

TEST_CASE("raise and lower change heights inside the brush only, strongest at the center") {
    HeightGrid grid = flatGrid();
    const Brush raise{BrushMode::Raise, 5.0f, 0.5f};
    const GridRegion region = applyBrush(grid, raise, {0.0f, 0.0f}, 1.0f, 0.0f, 1);
    CHECK_FALSE(region.empty());
    const float center = grid.heightAt({0.0f, 0.0f});
    CHECK(center == doctest::Approx(3.0f)); // 6 m/s × 0.5 × 1 s at full weight
    CHECK(grid.heightAt({2.0f, 0.0f}) < center);
    CHECK(grid.heightAt({2.0f, 0.0f}) > grid.heightAt({4.0f, 0.0f}));
    CHECK(grid.heightAt({8.0f, 0.0f}) == doctest::Approx(0.0f));
    CHECK(grid.heightAt({0.0f, -15.0f}) == doctest::Approx(0.0f));
    // The region covers every changed vertex and not the whole grid.
    for (int j = 0; j <= grid.cells; ++j) {
        for (int i = 0; i <= grid.cells; ++i) {
            if (grid.at(i, j) != 0.0f) {
                CHECK(i >= region.i0);
                CHECK(i <= region.i1);
                CHECK(j >= region.j0);
                CHECK(j <= region.j1);
            }
        }
    }
    CHECK(region.i1 - region.i0 < 14);

    applyBrush(grid, Brush{BrushMode::Lower, 5.0f, 0.5f}, {0.0f, 0.0f}, 1.0f, 0.0f, 1);
    CHECK(grid.heightAt({0.0f, 0.0f}) == doctest::Approx(0.0f).epsilon(1e-4));
}

TEST_CASE("strokes near the border and outside the ground are clipped, not wrapped") {
    HeightGrid grid = flatGrid();
    const GridRegion edge = applyBrush(grid, Brush{BrushMode::Raise, 6.0f, 1.0f}, {-20.0f, -20.0f}, 0.1f, 0.0f, 1);
    CHECK(edge.i0 == 0);
    CHECK(edge.j0 == 0);
    CHECK(grid.heightAt({20.0f, 20.0f}) == doctest::Approx(0.0f));
    CHECK(applyBrush(grid, Brush{BrushMode::Raise, 3.0f, 1.0f}, {100.0f, 100.0f}, 0.1f, 0.0f, 1).empty());
}

TEST_CASE("smooth evens out bumps and flatten levels toward the chosen height") {
    HeightGrid grid = flatGrid();
    applyBrush(grid, Brush{BrushMode::Roughen, 10.0f, 1.0f}, {0.0f, 0.0f}, 1.0f, 0.0f, 7);
    const float rough = variation(grid);
    CHECK(rough > 0.01f);
    for (int i = 0; i < 20; ++i) {
        applyBrush(grid, Brush{BrushMode::Smooth, 12.0f, 1.0f}, {0.0f, 0.0f}, 0.1f, 0.0f, 7);
    }
    CHECK(variation(grid) < rough * 0.5f);

    HeightGrid hill = flatGrid();
    applyBrush(hill, Brush{BrushMode::Raise, 8.0f, 1.0f}, {0.0f, 0.0f}, 1.0f, 0.0f, 1);
    REQUIRE(hill.heightAt({0.0f, 0.0f}) > 5.0f);
    for (int i = 0; i < 30; ++i) {
        applyBrush(hill, Brush{BrushMode::Flatten, 8.0f, 1.0f}, {0.0f, 0.0f}, 0.1f, 1.0f, 1);
    }
    CHECK(hill.heightAt({0.0f, 0.0f}) == doctest::Approx(1.0f).epsilon(0.02));
}

TEST_CASE("roughen is deterministic per stroke seed and differs between seeds") {
    HeightGrid a = flatGrid();
    HeightGrid b = flatGrid();
    HeightGrid c = flatGrid();
    const Brush brush{BrushMode::Roughen, 8.0f, 1.0f};
    applyBrush(a, brush, {1.0f, 1.0f}, 0.5f, 0.0f, 3);
    applyBrush(b, brush, {1.0f, 1.0f}, 0.5f, 0.0f, 3);
    applyBrush(c, brush, {1.0f, 1.0f}, 0.5f, 0.0f, 4);
    CHECK(a.heights == b.heights);
    CHECK(a.heights != c.heights);
}

TEST_CASE("writing a region updates positions and normals, nothing farther away") {
    MeshData mesh = makeTerrainMesh(flatSettings());
    HeightGrid grid = *heightGridFromMesh(mesh);
    const MeshData before = mesh;
    const GridRegion region =
        applyBrush(grid, Brush{BrushMode::Raise, 4.0f, 1.0f}, {0.0f, 0.0f}, 1.0f, 0.0f, 1);
    writeHeights(mesh, grid, region);

    const auto side = static_cast<std::size_t>(grid.side());
    for (std::size_t k = 0; k < mesh.positions.size(); ++k) {
        const int i = static_cast<int>(k % side);
        const int j = static_cast<int>(k / side);
        CHECK(mesh.positions[k].z == doctest::Approx(grid.at(i, j)));
        CHECK(mesh.positions[k].x == before.positions[k].x); // never moves sideways
        CHECK(glm::length(mesh.normals[k]) == doctest::Approx(1.0f).epsilon(1e-4));
        const bool nearRegion = i >= region.i0 - 1 && i <= region.i1 + 1 && j >= region.j0 - 1 &&
                                j <= region.j1 + 1;
        if (!nearRegion) {
            CHECK(mesh.normals[k] == before.normals[k]);
        }
    }
    // The slope of the hill tilts normals away from its top: east of the center leans east.
    const std::size_t east = static_cast<std::size_t>(20 + 3) + 20 * side;
    CHECK(mesh.normals[east].x > 0.0f);
    const std::size_t top = 20 + 20 * side;
    CHECK(mesh.normals[top].z == doctest::Approx(1.0f).epsilon(1e-3));

    // Writing everything gives the same normals as rebuilding from the grid.
    MeshData whole = before;
    writeHeights(whole, grid, wholeGrid(grid));
    CHECK(whole.positions == mesh.positions);
}

TEST_CASE("a ray meets the ground where it is, and misses beyond the edge or from below") {
    HeightGrid grid = flatGrid();
    applyBrush(grid, Brush{BrushMode::Raise, 6.0f, 1.0f}, {5.0f, 0.0f}, 0.5f, 0.0f, 1); // a hill, 3 m
    const float hillTop = grid.heightAt({5.0f, 0.0f});
    REQUIRE(hillTop > 2.5f);

    const auto straightDown = grid.intersect({5.0f, 0.0f, 30.0f}, {0.0f, 0.0f, -1.0f});
    REQUIRE(straightDown);
    CHECK(straightDown->z == doctest::Approx(hillTop).epsilon(1e-3));
    const auto flatSpot = grid.intersect({-10.0f, -10.0f, 5.0f}, {0.0f, 0.0f, -1.0f});
    REQUIRE(flatSpot);
    CHECK(flatSpot->z == doctest::Approx(0.0f).epsilon(1e-4));

    // A slanted view ray over the hill lands on its side, exactly on the surface.
    const glm::vec3 from(-12.0f, 0.0f, 8.0f);
    const auto slanted = grid.intersect(from, glm::normalize(glm::vec3(5.0f, 0.0f, -4.0f)));
    REQUIRE(slanted);
    CHECK(slanted->z == doctest::Approx(grid.heightAt(glm::vec2(*slanted))).epsilon(1e-3));

    CHECK_FALSE(grid.intersect({60.0f, 0.0f, 10.0f}, {0.0f, 0.0f, -1.0f}));  // beside the ground
    CHECK_FALSE(grid.intersect({0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 1.0f}));    // looking up
    CHECK_FALSE(grid.intersect({5.0f, 0.0f, -1.0f}, {0.0f, 0.0f, -1.0f}));   // from underground
    // A ray from outside the grid's edge, skimming in, still finds the ground inside it.
    CHECK(grid.intersect({-40.0f, 0.0f, 6.0f}, glm::normalize(glm::vec3(1.0f, 0.0f, -0.2f))));
}
