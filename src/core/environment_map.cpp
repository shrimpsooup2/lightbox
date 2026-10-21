#include "core/environment_map.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <format>
#include <numbers>
#include <stdexcept>
#include <string>

#include <glm/geometric.hpp>
#include <glm/gtc/packing.hpp>

namespace lb {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr std::uint32_t kGridWidth = 512;

// A sun is a spot at least this many times brighter than the average sky.
constexpr float kSunContrast = 50.0f;
// Sun pixels are clamped to this multiple of the average sky (or to 1 % of the sun's peak).
constexpr float kSunClampContrast = 20.0f;
// The sun region never grows beyond this angle from its peak (glowing clouds stay in the image).
constexpr float kSunMaxRadius = 10.0f * kPi / 180.0f;

float luminance(const float* rgb) {
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

FloatImage halve(const FloatImage& image) {
    FloatImage half;
    half.width = std::max(image.width / 2, 1u);
    half.height = std::max(image.height / 2, 1u);
    half.pixels.resize(std::size_t{half.width} * half.height * 3);
    for (std::uint32_t y = 0; y < half.height; ++y) {
        for (std::uint32_t x = 0; x < half.width; ++x) {
            for (std::uint32_t c = 0; c < 3; ++c) {
                float sum = 0.0f;
                for (std::uint32_t dy = 0; dy < 2; ++dy) {
                    for (std::uint32_t dx = 0; dx < 2; ++dx) {
                        const std::uint32_t sx = std::min(x * 2 + dx, image.width - 1);
                        const std::uint32_t sy = std::min(y * 2 + dy, image.height - 1);
                        sum += image.pixels[(std::size_t{sy} * image.width + sx) * 3 + c];
                    }
                }
                half.pixels[(std::size_t{y} * half.width + x) * 3 + c] = sum * 0.25f;
            }
        }
    }
    return half;
}

std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // namespace

glm::vec3 environmentDirection(glm::vec2 uv) {
    const float bearing = (uv.x - 0.5f) * 2.0f * kPi;
    const float theta = uv.y * kPi;
    const float s = std::sin(theta);
    return {s * std::sin(bearing), s * std::cos(bearing), std::cos(theta)};
}

glm::vec2 environmentUv(glm::vec3 direction) {
    const float bearing = std::atan2(direction.x, direction.y);
    float u = 0.5f + bearing / (2.0f * kPi);
    u -= std::floor(u);
    return {u, std::acos(std::clamp(direction.z, -1.0f, 1.0f)) / kPi};
}

glm::vec3 environmentToWorld(glm::vec3 direction, float rotationDegrees) {
    // Turning clockwise adds to the bearing (measured clockwise from +Y).
    const float r = rotationDegrees * kPi / 180.0f;
    const float c = std::cos(r);
    const float s = std::sin(r);
    return {direction.x * c + direction.y * s, -direction.x * s + direction.y * c, direction.z};
}

EnvironmentMap makeEnvironmentMap(FloatImage image) {
    if (image.empty() || image.pixels.size() != std::size_t{image.width} * image.height * 3) {
        throw std::invalid_argument("makeEnvironmentMap: needs RGB pixels matching its size");
    }
    while (image.width > kMaxEnvironmentWidth) {
        image = halve(image);
    }
    const std::uint32_t w = image.width;
    const std::uint32_t h = image.height;
    std::vector<float>& px = image.pixels;
    for (float& value : px) {
        value = std::isfinite(value) ? std::max(value, 0.0f) : 0.0f; // broken files: no NaN sky
    }
    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        return px.data() + (std::size_t{y} * w + x) * 3;
    };
    std::vector<float> rowSin(h);
    for (std::uint32_t y = 0; y < h; ++y) {
        rowSin[y] = std::sin((static_cast<float>(y) + 0.5f) / static_cast<float>(h) * kPi);
    }
    // Solid angle of one pixel in row y.
    const float pixelArea = (2.0f * kPi / static_cast<float>(w)) * (kPi / static_cast<float>(h));
    const auto direction = [&](std::uint32_t x, std::uint32_t y) {
        return environmentDirection({(static_cast<float>(x) + 0.5f) / static_cast<float>(w),
                                     (static_cast<float>(y) + 0.5f) / static_cast<float>(h)});
    };

    EnvironmentMap map;
    map.width = w;
    map.height = h;

    // Typical sky brightness (median of the upper half: a bright sun does not move it) and the
    // brightest pixel near or above the horizon.
    std::vector<float> skySamples;
    const std::size_t upperPixels = std::size_t{w} * std::max(h / 2, 1u);
    const std::size_t stride = std::max<std::size_t>(upperPixels / 65536, 1);
    skySamples.reserve(upperPixels / stride + 1);
    float peak = 0.0f;
    std::uint32_t peakX = 0;
    std::uint32_t peakY = 0;
    const std::uint32_t searchRows = std::min(h, h / 2 + std::max(h / 36, 1u)); // to 5° below
    std::size_t index = 0;
    for (std::uint32_t y = 0; y < searchRows; ++y) {
        for (std::uint32_t x = 0; x < w; ++x, ++index) {
            const float l = luminance(at(x, y));
            if ((y < h / 2 || h == 1) && index % stride == 0) {
                skySamples.push_back(l);
            }
            if (l > peak) {
                peak = l;
                peakX = x;
                peakY = y;
            }
        }
    }
    const auto middle = skySamples.begin() + static_cast<std::ptrdiff_t>(skySamples.size() / 2);
    std::nth_element(skySamples.begin(), middle, skySamples.end());
    const float skyLevel = skySamples.empty() ? 0.0f : *middle;
    map.sunDirection = direction(peakX, peakY);
    map.hasSun = peak > 0.0f && peak > kSunContrast * skyLevel;

    if (map.hasSun) {
        // Flood the sun region from the peak (wrapping around horizontally) and move its light
        // above the clamp level into the analytic sun.
        const float clampLevel = std::max(kSunClampContrast * skyLevel, peak * 0.01f);
        const float cosMaxRadius = std::cos(kSunMaxRadius);
        const glm::vec3 peakDirection = map.sunDirection;
        std::vector<std::uint8_t> visited(std::size_t{w} * h, 0);
        std::deque<std::pair<std::uint32_t, std::uint32_t>> queue{{peakX, peakY}};
        visited[std::size_t{peakY} * w + peakX] = 1;
        glm::dvec3 irradiance{0.0};
        glm::dvec3 weightedDirection{0.0};
        while (!queue.empty()) {
            const auto [x, y] = queue.front();
            queue.pop_front();
            float* rgb = at(x, y);
            const float l = luminance(rgb);
            if (l <= clampLevel || glm::dot(direction(x, y), peakDirection) < cosMaxRadius) {
                continue;
            }
            const float keep = clampLevel / l;
            const double area = double{pixelArea} * rowSin[y];
            const glm::dvec3 removed{rgb[0] * (1.0f - keep), rgb[1] * (1.0f - keep),
                                     rgb[2] * (1.0f - keep)};
            irradiance += removed * area;
            weightedDirection += glm::dvec3(direction(x, y)) * (double{l - clampLevel} * area);
            for (std::uint32_t c = 0; c < 3; ++c) {
                rgb[c] *= keep;
            }
            const std::uint32_t left = x == 0 ? w - 1 : x - 1;
            const std::uint32_t right = x + 1 == w ? 0 : x + 1;
            const std::pair<std::uint32_t, std::uint32_t> neighbors[] = {
                {left, y}, {right, y}, {x, y == 0 ? y : y - 1}, {x, y + 1 == h ? y : y + 1}};
            for (const auto& [nx, ny] : neighbors) {
                std::uint8_t& seen = visited[std::size_t{ny} * w + nx];
                if (!seen) {
                    seen = 1;
                    queue.emplace_back(nx, ny);
                }
            }
        }
        map.sunIrradiance = glm::vec3(irradiance);
        if (glm::length(weightedDirection) > 0.0) {
            map.sunDirection = glm::vec3(glm::normalize(weightedDirection));
        }
    }

    // Sky light of the raster preview: band averages by solid angle.
    glm::dvec3 bands[3]{};
    double bandWeights[3]{};
    for (std::uint32_t y = 0; y < h; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
        const int band = v < 1.0f / 3.0f ? 0 : (v < 0.5f ? 1 : 2);
        for (std::uint32_t x = 0; x < w; ++x) {
            const float* rgb = at(x, y);
            bands[band] += glm::dvec3(rgb[0], rgb[1], rgb[2]) * double{rowSin[y]};
            bandWeights[band] += rowSin[y];
        }
    }
    const auto average = [&](int band) {
        return bandWeights[band] > 0.0 ? glm::vec3(bands[band] / bandWeights[band]) : glm::vec3(0.0f);
    };
    map.zenithRadiance = average(0);
    map.horizonRadiance = average(1);
    map.groundRadiance = average(2);

    // Importance sampling grid: average luminance per cell × solid angle of the cell.
    const std::uint32_t gw = std::min(w, kGridWidth);
    const std::uint32_t gh = std::clamp(gw / 2, 1u, h);
    map.gridWidth = gw;
    map.gridHeight = gh;
    map.cellProbability.assign(std::size_t{gw} * gh, 0.0f);
    double total = 0.0;
    for (std::uint32_t cy = 0; cy < gh; ++cy) {
        const std::uint32_t y0 = cy * h / gh;
        const std::uint32_t y1 = std::max((cy + 1) * h / gh, y0 + 1);
        const float cellSin =
            std::sin((static_cast<float>(cy) + 0.5f) / static_cast<float>(gh) * kPi);
        for (std::uint32_t cx = 0; cx < gw; ++cx) {
            const std::uint32_t x0 = cx * w / gw;
            const std::uint32_t x1 = std::max((cx + 1) * w / gw, x0 + 1);
            double sum = 0.0;
            for (std::uint32_t y = y0; y < y1; ++y) {
                for (std::uint32_t x = x0; x < x1; ++x) {
                    sum += luminance(at(x, y));
                }
            }
            const double weight = sum / double((y1 - y0) * (x1 - x0)) * cellSin;
            map.cellProbability[std::size_t{cy} * gw + cx] = static_cast<float>(weight);
            total += weight;
        }
    }
    if (total <= 0.0) { // black image: sample uniformly
        std::fill(map.cellProbability.begin(), map.cellProbability.end(), 1.0f);
        total = double(gw) * gh;
    }
    map.rowCdf.resize(gh);
    map.columnCdf.resize(std::size_t{gw} * gh);
    double rowRunning = 0.0;
    for (std::uint32_t cy = 0; cy < gh; ++cy) {
        float* row = map.cellProbability.data() + std::size_t{cy} * gw;
        float* cdf = map.columnCdf.data() + std::size_t{cy} * gw;
        double rowSum = 0.0;
        for (std::uint32_t cx = 0; cx < gw; ++cx) {
            row[cx] = static_cast<float>(row[cx] / total);
            rowSum += row[cx];
        }
        double running = 0.0;
        for (std::uint32_t cx = 0; cx < gw; ++cx) {
            running += row[cx];
            cdf[cx] = rowSum > 0.0 ? static_cast<float>(running / rowSum)
                                   : static_cast<float>(cx + 1) / static_cast<float>(gw);
        }
        cdf[gw - 1] = 1.0f;
        rowRunning += rowSum;
        map.rowCdf[cy] = static_cast<float>(rowRunning);
    }
    for (float& value : map.rowCdf) {
        value = static_cast<float>(value / rowRunning);
    }
    map.rowCdf[gh - 1] = 1.0f;

    // Half floats for the GPU (65 000 is just under the largest finite half).
    map.pixels.resize(std::size_t{w} * h * 4);
    for (std::size_t i = 0, n = std::size_t{w} * h; i < n; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            map.pixels[i * 4 + c] = glm::packHalf1x16(std::min(px[i * 3 + c], 65000.0f));
        }
        map.pixels[i * 4 + 3] = glm::packHalf1x16(1.0f);
    }
    return map;
}

EnvironmentMap loadEnvironmentMap(const std::filesystem::path& path) {
    if (!isEnvironmentMapFile(path)) {
        throw std::runtime_error(std::format(
            "{} is not a sky image. Use a Radiance .hdr panorama.", path.filename().string()));
    }
    return makeEnvironmentMap(loadFloatImage(path));
}

bool isEnvironmentMapFile(const std::filesystem::path& path) {
    return lowerExtension(path) == ".hdr";
}

} // namespace lb
