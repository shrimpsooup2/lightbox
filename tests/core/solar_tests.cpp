#include <cmath>

#include <doctest/doctest.h>
#include <glm/geometric.hpp>

#include "core/solar.hpp"

using namespace lb;

namespace {

struct Reference {
    const char* site;
    double latitude;
    double longitude;
    int year;
    int month;
    int day;
    double localHours;
    double utcOffset;
    double azimuth;   // degrees from true north, clockwise
    double elevation; // degrees, refraction included
};

// Reference values from an independent implementation (astral 3.x, refraction on).
constexpr Reference kReferences[] = {
    {"London, midsummer, BST", 51.5074, -0.1278, 2024, 6, 21, 13.0, 1.0, 178.813, 61.935},
    {"London, midwinter noon", 51.5074, -0.1278, 2024, 12, 21, 12.0, 0.0, 180.277, 15.113},
    {"Sydney, summer morning", -33.8688, 151.2093, 2025, 1, 15, 9.5, 11.0, 89.307, 41.342},
    {"Denver, equinox evening", 39.7392, -104.9903, 2025, 3, 20, 17.25, -6.0, 251.378, 21.399},
    {"Singapore, low morning sun", 1.3521, 103.8198, 2025, 9, 23, 7.5, 8.0, 90.278, 8.300},
};

} // namespace

TEST_CASE("solar position matches an independent reference within 0.15 degrees") {
    for (const Reference& r : kReferences) {
        CAPTURE(r.site);
        const SolarPosition p = solarPosition(r.latitude, r.longitude, r.year, r.month, r.day,
                                              r.localHours, r.utcOffset);
        CHECK(std::abs(p.azimuthDegrees - r.azimuth) < 0.15);
        CHECK(std::abs(p.elevationDegrees - r.elevation) < 0.15);
    }
}

TEST_CASE("sun is below the horizon at night") {
    const SolarPosition midnight = solarPosition(51.5074, -0.1278, 2024, 12, 21, 0.0, 0.0);
    CHECK(midnight.elevationDegrees < -50.0);
}

TEST_CASE("color temperature keeps luminance and shifts hue") {
    const auto luminance = [](glm::vec3 c) { return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f)); };
    for (float k : {1900.0f, 2700.0f, 5600.0f, 6500.0f, 10000.0f}) {
        CAPTURE(k);
        CHECK(luminance(kelvinToLinearRgb(k)) == doctest::Approx(1.0f).epsilon(1e-4));
    }
    const glm::vec3 warm = kelvinToLinearRgb(2700.0f);
    CHECK(warm.r > warm.g);
    CHECK(warm.g > warm.b);
    const glm::vec3 daylight = kelvinToLinearRgb(6500.0f);
    CHECK(std::abs(daylight.r - daylight.b) < 0.08f); // close to neutral white
    const glm::vec3 cool = kelvinToLinearRgb(10000.0f);
    CHECK(cool.b > cool.r);
}
