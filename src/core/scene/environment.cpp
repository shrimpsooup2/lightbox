#include "core/scene/environment.hpp"

#include <cmath>

#include <glm/trigonometric.hpp>

#include "core/solar.hpp"

namespace lb {

glm::vec3 sunDirectionFromAngles(const SunAngles& angles) {
    const float azimuth = glm::radians(angles.azimuthDegrees);
    const float elevation = glm::radians(angles.elevationDegrees);
    const float horizontal = std::cos(elevation);
    return {horizontal * std::sin(azimuth), horizontal * std::cos(azimuth), std::sin(elevation)};
}

SunAngles Environment::sunAngles() const {
    if (sun.mode == SunMode::Manual) {
        return {sun.azimuthDegrees, sun.elevationDegrees};
    }
    const SiteLocation& site = sun.location;
    const LocalDateTime& when = sun.dateTime;
    const SolarPosition p = solarPosition(site.latitudeDegrees, site.longitudeDegrees, when.year,
                                          when.month, when.day, when.hours, site.utcOffsetHours);
    // A true-north bearing becomes a project bearing by adding the north angle.
    const float azimuth =
        std::fmod(static_cast<float>(p.azimuthDegrees) + site.northAngleDegrees + 360.0f, 360.0f);
    return {azimuth, static_cast<float>(p.elevationDegrees)};
}

glm::vec3 Environment::sunDirection() const {
    return sunDirectionFromAngles(sunAngles());
}

} // namespace lb
