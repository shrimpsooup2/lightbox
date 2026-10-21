#pragma once

#include <glm/vec3.hpp>
#include <string>


namespace lb {

// Where the project is on Earth. Drives the sun in SunMode::Location and later terrain/GIS (5.3).
struct SiteLocation {
    double latitudeDegrees = 51.5074; // London as a neutral default
    double longitudeDegrees = -0.1278;
    float utcOffsetHours = 0.0f;    // site clock offset from UTC on the chosen date
    float northAngleDegrees = 0.0f; // true north, measured clockwise from project north (+Y)

    bool operator==(const SiteLocation&) const = default;
};

struct LocalDateTime {
    int year = 2026;
    int month = 6;
    int day = 21;
    float hours = 15.0f; // site clock time, 0–24

    bool operator==(const LocalDateTime&) const = default;
};

enum class SunMode {
    Manual,   // direction set by hand (azimuth/elevation)
    Location, // computed from site location, date and time
};

// Direction angles in project coordinates: azimuth is the bearing clockwise from project north (+Y)
// toward east (+X); elevation is the angle above the horizon.
struct SunAngles {
    float azimuthDegrees = 0.0f;
    float elevationDegrees = 0.0f;
};

struct Sun {
    SunMode mode = SunMode::Manual;
    float azimuthDegrees = 225.0f; // manual mode
    float elevationDegrees = 40.0f;
    SiteLocation location; // location mode
    LocalDateTime dateTime;
    float intensity = 1.0f;            // multiplier on standard daylight
    float temperatureKelvin = 5600.0f; // color of the sunlight

    bool operator==(const Sun&) const = default;
};

struct Sky {
    float intensity = 1.0f; // brightness of the sky dome and its fill light
    // Sky image (HDRI): absolute path of an equirectangular .hdr panorama; empty = procedural sky.
    // With an image, the sun comes from it (direction, color, strength; see EnvironmentMap).
    std::string image;
    float imageRotationDegrees = 0.0f; // turns the image (and its sun) clockwise seen from above

    bool operator==(const Sky&) const = default;
};

struct Shadows {
    bool enabled = true;
    float softness = 1.0f; // 0 = razor sharp, 1 = natural, up to 4 = very soft

    bool operator==(const Shadows&) const = default;
};

// Document-level lighting and site settings. Grows with Stage 3.4 (physical sky, HDRI, lamps) and
// Stage 5 (weather, terrain defaults).
struct Environment {
    Sun sun;
    Sky sky;
    Shadows shadows;

    // Resolved sun angles in project coordinates (computed from the site in location mode).
    SunAngles sunAngles() const;
    // Unit vector from the scene toward the sun (world space, Z-up).
    glm::vec3 sunDirection() const;
    bool operator==(const Environment&) const = default;
};

// Unit vector toward the sun for project-space angles.
glm::vec3 sunDirectionFromAngles(const SunAngles& angles);

} // namespace lb
