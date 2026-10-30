#include "core/terrain_edit.hpp"

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include "core/terrain.hpp"

namespace lb {
namespace {

constexpr float kRaiseRate = 6.0f;   // meters per second at strength 1
constexpr float kSmoothRate = 8.0f;  // share of the way to the local average, per second
constexpr float kFlattenRate = 10.0f;
constexpr float kRoughenRate = 4.0f;
constexpr float kGridTolerance = 1e-3f; // relative to the cell size

float falloff(float distance, float radius) {
    const float t = 1.0f - std::clamp(distance / radius, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

int clampIndex(int i, int cells) {
    return std::clamp(i, 0, cells);
}

} // namespace

float HeightGrid::heightAt(glm::vec2 position) const {
    if (cells <= 0) {
        return 0.0f;
    }
    const glm::vec2 local = (position - origin) / cellSize;
    const float x = std::clamp(local.x, 0.0f, static_cast<float>(cells));
    const float y = std::clamp(local.y, 0.0f, static_cast<float>(cells));
    const int i = std::min(static_cast<int>(x), cells - 1);
    const int j = std::min(static_cast<int>(y), cells - 1);
    const float fx = x - static_cast<float>(i);
    const float fy = y - static_cast<float>(j);
    return glm::mix(glm::mix(at(i, j), at(i + 1, j), fx), glm::mix(at(i, j + 1), at(i + 1, j + 1), fx),
                    fy);
}

std::optional<glm::vec3> HeightGrid::intersect(const glm::vec3& rayOrigin,
                                               const glm::vec3& direction) const {
    if (cells <= 0 || glm::dot(direction, direction) < 1e-12f) {
        return std::nullopt;
    }
    const glm::vec3 dir = glm::normalize(direction);
    const auto [lowest, highest] = std::minmax_element(heights.begin(), heights.end());
    // Padded a little above and below: a ray that meets the lowest or highest point exactly must
    // still be inside the box (rounding), and perfectly flat ground has no thickness otherwise.
    const float pad = cellSize * 0.01f;
    const glm::vec3 boxMin(origin, *lowest - pad);
    const glm::vec3 boxMax(maxCorner(), *highest + pad);
    // Slab test against the grid's box: where the ray is over the ground at all.
    float tEnter = 0.0f;
    float tExit = 1e30f;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(dir[axis]) < 1e-9f) {
            if (rayOrigin[axis] < boxMin[axis] || rayOrigin[axis] > boxMax[axis]) {
                return std::nullopt;
            }
            continue;
        }
        float t0 = (boxMin[axis] - rayOrigin[axis]) / dir[axis];
        float t1 = (boxMax[axis] - rayOrigin[axis]) / dir[axis];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
    }
    if (tEnter > tExit) {
        return std::nullopt;
    }
    const auto above = [&](float t) {
        const glm::vec3 p = rayOrigin + dir * t;
        return p.z - heightAt(glm::vec2(p));
    };
    if (above(tEnter) < 0.0f) {
        return std::nullopt; // starts under the ground
    }
    const float step = cellSize * 0.5f;
    float previous = tEnter;
    for (float t = tEnter + step; t <= tExit + step; t += step) {
        const float current = std::min(t, tExit);
        if (above(current) <= 0.0f) {
            float lo = previous;
            float hi = current;
            for (int i = 0; i < 24; ++i) {
                const float mid = 0.5f * (lo + hi);
                (above(mid) > 0.0f ? lo : hi) = mid;
            }
            const glm::vec3 p = rayOrigin + dir * (0.5f * (lo + hi));
            return glm::vec3(p.x, p.y, heightAt(glm::vec2(p)));
        }
        previous = current;
        if (current >= tExit) {
            break;
        }
    }
    return std::nullopt;
}

std::optional<HeightGrid> heightGridFromMesh(const MeshData& mesh) {
    const std::size_t count = mesh.positions.size();
    const auto side = static_cast<std::size_t>(std::lround(std::sqrt(static_cast<double>(count))));
    if (side < 3 || side * side != count) {
        return std::nullopt;
    }
    HeightGrid grid;
    grid.cells = static_cast<int>(side) - 1;
    grid.origin = glm::vec2(mesh.positions[0]);
    grid.cellSize = mesh.positions[1].x - mesh.positions[0].x;
    if (!(grid.cellSize > 0.0f)) {
        return std::nullopt;
    }
    // Rows of x, rows advancing in y, evenly spaced: check the corners and the second row.
    const float tolerance = grid.cellSize * kGridTolerance;
    const glm::vec2 farCorner = grid.origin + glm::vec2(static_cast<float>(grid.cells) * grid.cellSize);
    const auto near = [tolerance](glm::vec2 a, glm::vec2 b) {
        return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance;
    };
    if (!near(glm::vec2(mesh.positions[side - 1]),
              glm::vec2(farCorner.x, grid.origin.y)) ||
        !near(glm::vec2(mesh.positions[side]), glm::vec2(grid.origin.x, grid.origin.y + grid.cellSize)) ||
        !near(glm::vec2(mesh.positions.back()), farCorner)) {
        return std::nullopt;
    }
    grid.heights.reserve(count);
    for (const glm::vec3& p : mesh.positions) {
        grid.heights.push_back(p.z);
    }
    return grid;
}

GridRegion wholeGrid(const HeightGrid& grid) {
    return {0, 0, grid.cells, grid.cells};
}

GridRegion applyBrush(HeightGrid& grid, const Brush& brush, glm::vec2 center, float seconds,
                      float flattenHeight, std::uint32_t seed) {
    if (grid.cells <= 0 || brush.radius <= 0.0f || seconds <= 0.0f) {
        return {};
    }
    const glm::vec2 lo = (center - glm::vec2(brush.radius) - grid.origin) / grid.cellSize;
    const glm::vec2 hi = (center + glm::vec2(brush.radius) - grid.origin) / grid.cellSize;
    GridRegion region{std::max(static_cast<int>(std::floor(lo.x)), 0),
                      std::max(static_cast<int>(std::floor(lo.y)), 0),
                      std::min(static_cast<int>(std::ceil(hi.x)), grid.cells),
                      std::min(static_cast<int>(std::ceil(hi.y)), grid.cells)};
    if (region.empty()) {
        return {};
    }
    const float strength = std::clamp(brush.strength, 0.0f, 1.0f);
    // Smoothing reads the heights as they were before this application, not half-updated ones.
    const std::vector<float> before =
        brush.mode == BrushMode::Smooth ? grid.heights : std::vector<float>();
    const float noiseScale = std::max(brush.radius * 0.3f, grid.cellSize * 2.0f);

    for (int j = region.j0; j <= region.j1; ++j) {
        for (int i = region.i0; i <= region.i1; ++i) {
            const glm::vec2 position = grid.origin + glm::vec2(static_cast<float>(i), static_cast<float>(j)) *
                                                          grid.cellSize;
            const float weight = falloff(glm::length(position - center), brush.radius);
            if (weight <= 0.0f) {
                continue;
            }
            float& height = grid.at(i, j);
            switch (brush.mode) {
            case BrushMode::Raise:
                height += kRaiseRate * strength * weight * seconds;
                break;
            case BrushMode::Lower:
                height -= kRaiseRate * strength * weight * seconds;
                break;
            case BrushMode::Smooth: {
                float sum = 0.0f;
                for (int dj = -1; dj <= 1; ++dj) {
                    for (int di = -1; di <= 1; ++di) {
                        sum += before[static_cast<std::size_t>(clampIndex(j + dj, grid.cells) * grid.side() +
                                                               clampIndex(i + di, grid.cells))];
                    }
                }
                const float t = std::clamp(kSmoothRate * strength * weight * seconds, 0.0f, 1.0f);
                height += (sum / 9.0f - height) * t;
                break;
            }
            case BrushMode::Flatten: {
                const float t = std::clamp(kFlattenRate * strength * weight * seconds, 0.0f, 1.0f);
                height += (flattenHeight - height) * t;
                break;
            }
            case BrushMode::Roughen:
                height += (terrainNoise(position / noiseScale, seed) * 2.0f - 1.0f) * kRoughenRate *
                          strength * weight * seconds;
                break;
            }
        }
    }
    return region;
}

void writeHeights(MeshData& mesh, const HeightGrid& grid, GridRegion region) {
    const auto side = static_cast<std::size_t>(grid.side());
    if (region.empty() || mesh.positions.size() != side * side || mesh.normals.size() != side * side) {
        return;
    }
    region.i0 = clampIndex(region.i0, grid.cells);
    region.j0 = clampIndex(region.j0, grid.cells);
    region.i1 = clampIndex(region.i1, grid.cells);
    region.j1 = clampIndex(region.j1, grid.cells);
    for (int j = region.j0; j <= region.j1; ++j) {
        for (int i = region.i0; i <= region.i1; ++i) {
            mesh.positions[static_cast<std::size_t>(j) * side + static_cast<std::size_t>(i)].z = grid.at(i, j);
        }
    }
    // Normals reach one vertex further than the heights that changed.
    for (int j = clampIndex(region.j0 - 1, grid.cells); j <= clampIndex(region.j1 + 1, grid.cells); ++j) {
        for (int i = clampIndex(region.i0 - 1, grid.cells); i <= clampIndex(region.i1 + 1, grid.cells); ++i) {
            const int left = clampIndex(i - 1, grid.cells);
            const int right = clampIndex(i + 1, grid.cells);
            const int down = clampIndex(j - 1, grid.cells);
            const int up = clampIndex(j + 1, grid.cells);
            const float slopeX = (grid.at(right, j) - grid.at(left, j)) /
                                 (static_cast<float>(right - left) * grid.cellSize);
            const float slopeY = (grid.at(i, up) - grid.at(i, down)) /
                                 (static_cast<float>(up - down) * grid.cellSize);
            mesh.normals[static_cast<std::size_t>(j) * side + static_cast<std::size_t>(i)] =
                glm::normalize(glm::vec3(-slopeX, -slopeY, 1.0f));
        }
    }
}

} // namespace lb
