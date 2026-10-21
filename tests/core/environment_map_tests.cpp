#include <cmath>
#include <numeric>
#include <vector>

#include <doctest/doctest.h>
#include <glm/geometric.hpp>
#include <glm/gtc/packing.hpp>

#include "core/environment_map.hpp"

using namespace lb;

namespace {

constexpr float kPi = 3.14159265f;

// A uniform grey sky (radiance `sky`) with an optional bright square around `sunUv`.
FloatImage skyImage(std::uint32_t width, float sky, glm::vec2 sunUv = {-1.0f, -1.0f},
                    float sunRadiance = 0.0f, int sunHalfSize = 1) {
    FloatImage image;
    image.width = width;
    image.height = width / 2;
    image.pixels.assign(std::size_t{image.width} * image.height * 3, sky);
    if (sunUv.x >= 0.0f) {
        const int cx = static_cast<int>(sunUv.x * static_cast<float>(image.width));
        const int cy = static_cast<int>(sunUv.y * static_cast<float>(image.height));
        for (int y = cy - sunHalfSize; y <= cy + sunHalfSize; ++y) {
            for (int x = cx - sunHalfSize; x <= cx + sunHalfSize; ++x) {
                for (int c = 0; c < 3; ++c) {
                    image.pixels[(static_cast<std::size_t>(y) * image.width +
                                  static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)] =
                        sunRadiance;
                }
            }
        }
    }
    return image;
}

bool near(glm::vec3 a, glm::vec3 b, float tolerance) {
    return glm::length(a - b) < tolerance;
}

} // namespace

TEST_CASE("Sky image directions: center faces north, columns turn clockwise, row 0 is the zenith") {
    CHECK(near(environmentDirection({0.5f, 0.5f}), {0.0f, 1.0f, 0.0f}, 1e-5f));  // north
    CHECK(near(environmentDirection({0.75f, 0.5f}), {1.0f, 0.0f, 0.0f}, 1e-5f)); // east
    CHECK(near(environmentDirection({0.3f, 0.0f}), {0.0f, 0.0f, 1.0f}, 1e-5f));  // zenith
    for (const glm::vec2 uv : {glm::vec2(0.1f, 0.2f), glm::vec2(0.6f, 0.45f), glm::vec2(0.9f, 0.8f)}) {
        const glm::vec2 back = environmentUv(environmentDirection(uv));
        CHECK(back.x == doctest::Approx(uv.x).epsilon(1e-4));
        CHECK(back.y == doctest::Approx(uv.y).epsilon(1e-4));
    }
    // Turning the sky 90° clockwise moves what faced north to the east.
    CHECK(near(environmentToWorld({0.0f, 1.0f, 0.0f}, 90.0f), {1.0f, 0.0f, 0.0f}, 1e-5f));
}

TEST_CASE("A uniform sky has no sun, even band averages and uniform sampling by solid angle") {
    const EnvironmentMap map = makeEnvironmentMap(skyImage(64, 0.5f));
    CHECK_FALSE(map.hasSun);
    CHECK(map.sunIrradiance == glm::vec3(0.0f));
    CHECK(near(map.zenithRadiance, glm::vec3(0.5f), 1e-4f));
    CHECK(near(map.horizonRadiance, glm::vec3(0.5f), 1e-4f));
    CHECK(near(map.groundRadiance, glm::vec3(0.5f), 1e-4f));

    REQUIRE(map.gridWidth == 64);
    REQUIRE(map.gridHeight == 32);
    const float total =
        std::accumulate(map.cellProbability.begin(), map.cellProbability.end(), 0.0f);
    CHECK(total == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(map.rowCdf.back() == 1.0f);
    // Uniform radiance: the pdf over solid angle is the same everywhere, 1 / 4π.
    for (std::uint32_t row : {1u, 8u, 16u, 30u}) {
        const float sinTheta =
            std::sin((static_cast<float>(row) + 0.5f) / static_cast<float>(map.gridHeight) * kPi);
        const float pdf = map.cellProbability[std::size_t{row} * map.gridWidth + 5] *
                          static_cast<float>(map.gridWidth * map.gridHeight) /
                          (2.0f * kPi * kPi * sinTheta);
        CHECK(pdf == doctest::Approx(1.0f / (4.0f * kPi)).epsilon(0.01));
    }
}

TEST_CASE("The sun is taken out of a sky image and its light kept") {
    const std::uint32_t width = 256;
    const float sky = 0.2f;
    const float sunRadiance = 5000.0f;
    // Sun at bearing 90° (east), 30° above the horizon.
    const glm::vec2 sunUv{0.75f, 1.0f / 3.0f};
    const FloatImage image = skyImage(width, sky, sunUv, sunRadiance);
    const EnvironmentMap map = makeEnvironmentMap(image);

    REQUIRE(map.hasSun);
    const glm::vec3 expected = environmentDirection(sunUv);
    CHECK(glm::dot(map.sunDirection, expected) > std::cos(2.0f * kPi / 180.0f));

    // All the light above the clamp level moved into the sun: 9 pixels at ~sin(60°) solid angle.
    const float pixelArea = (2.0f * kPi / static_cast<float>(width)) *
                            (kPi / static_cast<float>(width / 2)) * std::sin(kPi / 3.0f);
    const float clampLevel = std::max(20.0f * sky, sunRadiance * 0.01f);
    const float expectedIrradiance = 9.0f * (sunRadiance - clampLevel) * pixelArea;
    CHECK(map.sunIrradiance.x == doctest::Approx(expectedIrradiance).epsilon(0.05));
    CHECK(map.sunIrradiance.x == doctest::Approx(map.sunIrradiance.z));

    // The image keeps no pixel brighter than the clamp level.
    for (std::size_t i = 0; i < map.pixels.size(); i += 4) {
        CHECK(glm::unpackHalf1x16(map.pixels[i]) <= clampLevel * 1.001f);
    }
}

TEST_CASE("Sampling favors the bright part of the sky") {
    // Upper half bright, lower half dark.
    FloatImage image = skyImage(64, 0.01f);
    for (std::uint32_t y = 0; y < image.height / 2; ++y) {
        for (std::uint32_t x = 0; x < image.width * 3; ++x) {
            image.pixels[std::size_t{y} * image.width * 3 + x] = 1.0f;
        }
    }
    const EnvironmentMap map = makeEnvironmentMap(image);
    CHECK_FALSE(map.hasSun); // bright, but not a small spot
    // The upper rows hold 100 / 101 of the probability.
    CHECK(map.rowCdf[map.gridHeight / 2 - 1] == doctest::Approx(100.0f / 101.0f).epsilon(1e-3));
}

TEST_CASE("Broken sky image pixels (NaN, negative) become black") {
    FloatImage image = skyImage(16, 1.0f);
    image.pixels[0] = std::nanf("");
    image.pixels[4] = -3.0f;
    const EnvironmentMap map = makeEnvironmentMap(std::move(image));
    CHECK(glm::unpackHalf1x16(map.pixels[0]) == 0.0f); // pixel 0, red
    CHECK(glm::unpackHalf1x16(map.pixels[5]) == 0.0f); // pixel 1, green
    CHECK(glm::unpackHalf1x16(map.pixels[4]) == 1.0f); // pixel 1, red untouched
}
