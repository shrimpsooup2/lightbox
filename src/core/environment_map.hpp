#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "core/image.hpp"

namespace lb {

// A sky image (HDRI) prepared for rendering: an equirectangular linear-light panorama. Row 0 is the
// zenith; the image center faces project north (+Y) and columns turn clockwise seen from above
// (toward east), as when looking around from inside it.
//
// The sun is taken out of the image: its pixels are clamped and their light becomes sunIrradiance
// from sunDirection. Both engines then draw it as their own sharp sun with shadows, and the path
// tracer samples it directly instead of hunting for a few very bright pixels (noise).
struct EnvironmentMap {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint16_t> pixels; // RGBA half floats, rows from the zenith, sun removed

    // Importance sampling by brightness × solid angle over a coarse grid (rows from the zenith):
    // pick a row by rowCdf, a cell in it by columnCdf, then a point in the cell.
    std::uint32_t gridWidth = 0;
    std::uint32_t gridHeight = 0;
    std::vector<float> rowCdf;          // gridHeight entries, ends at 1
    std::vector<float> columnCdf;       // gridWidth per row, each row ends at 1
    std::vector<float> cellProbability; // gridWidth × gridHeight, sums to 1

    bool hasSun = false; // a distinct sun was found (clear or broken sky)
    // Unit vector toward the sun, or toward the brightest part of an overcast sky.
    glm::vec3 sunDirection{0.0f, 0.0f, 1.0f};
    glm::vec3 sunIrradiance{0.0f}; // linear RGB on a surface facing the sun (0 without a sun)

    // Average radiance (sun removed) of the sky above 30° elevation, the band below it down to the
    // horizon, and everything under the horizon: the sky light of the raster preview.
    glm::vec3 zenithRadiance{0.0f};
    glm::vec3 horizonRadiance{0.0f};
    glm::vec3 groundRadiance{0.0f};
};

// Prepares a panorama (any size; wider than kMaxEnvironmentWidth is halved until it fits).
constexpr std::uint32_t kMaxEnvironmentWidth = 4096;
EnvironmentMap makeEnvironmentMap(FloatImage image);
// Loads a Radiance .hdr panorama. Slow (seconds for 4K): call it off the UI thread. Throws
// std::runtime_error with a user-presentable message.
EnvironmentMap loadEnvironmentMap(const std::filesystem::path& path);
bool isEnvironmentMapFile(const std::filesystem::path& path);

// Panorama coordinates (u right, v down, 0..1) ↔ unit direction in the map's own frame.
glm::vec3 environmentDirection(glm::vec2 uv);
glm::vec2 environmentUv(glm::vec3 direction);
// A direction in the map's frame turned by the sky rotation (clockwise seen from above, degrees)
// into project space.
glm::vec3 environmentToWorld(glm::vec3 direction, float rotationDegrees);

} // namespace lb
