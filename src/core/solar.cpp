#include "core/solar.hpp"

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include "core/color.hpp"

namespace lb {
namespace {

constexpr double kPi = 3.14159265358979323846;

double radians(double degrees) {
    return degrees * kPi / 180.0;
}

double degrees(double radians) {
    return radians * 180.0 / kPi;
}

double wrap(double value, double period) {
    const double result = std::fmod(value, period);
    return result < 0.0 ? result + period : result;
}

// Julian day at 0h UT of a Gregorian calendar date (Meeus, Astronomical Algorithms, ch. 7).
double julianDay(int year, int month, int day) {
    if (month <= 2) {
        year -= 1;
        month += 12;
    }
    const int a = year / 100;
    const int b = 2 - a + a / 4;
    return std::floor(365.25 * (year + 4716)) + std::floor(30.6001 * (month + 1)) + day + b -
           1524.5;
}

} // namespace

SolarPosition solarPosition(double latitudeDegrees, double longitudeDegrees, int year, int month,
                            int day, double localHours, double utcOffsetHours) {
    const double jd = julianDay(year, month, day) + (localHours - utcOffsetHours) / 24.0;
    const double t = (jd - 2451545.0) / 36525.0; // Julian centuries since J2000

    const double meanLongitude = wrap(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
    const double meanAnomaly = 357.52911 + t * (35999.05029 - 0.0001537 * t);
    const double eccentricity = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
    const double m = radians(meanAnomaly);
    const double center = std::sin(m) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
                          std::sin(2 * m) * (0.019993 - 0.000101 * t) + std::sin(3 * m) * 0.000289;
    const double omega = radians(125.04 - 1934.136 * t);
    const double apparentLongitude = meanLongitude + center - 0.00569 - 0.00478 * std::sin(omega);
    const double meanObliquity =
        23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
    const double obliquity = radians(meanObliquity + 0.00256 * std::cos(omega));
    const double declination =
        std::asin(std::sin(obliquity) * std::sin(radians(apparentLongitude)));

    const double y = std::pow(std::tan(obliquity / 2.0), 2.0);
    const double l0 = radians(meanLongitude);
    const double equationOfTimeMinutes =
        4.0 * degrees(y * std::sin(2 * l0) - 2 * eccentricity * std::sin(m) +
                      4 * eccentricity * y * std::sin(m) * std::cos(2 * l0) -
                      0.5 * y * y * std::sin(4 * l0) -
                      1.25 * eccentricity * eccentricity * std::sin(2 * m));

    const double trueSolarMinutes = wrap(localHours * 60.0 + equationOfTimeMinutes +
                                             4.0 * longitudeDegrees - 60.0 * utcOffsetHours,
                                         1440.0);
    double hourAngle = trueSolarMinutes / 4.0 - 180.0;
    if (hourAngle < -180.0) {
        hourAngle += 360.0;
    }

    const double latitude = radians(latitudeDegrees);
    const double cosZenith =
        std::clamp(std::sin(latitude) * std::sin(declination) +
                       std::cos(latitude) * std::cos(declination) * std::cos(radians(hourAngle)),
                   -1.0, 1.0);
    const double zenith = std::acos(cosZenith);
    double elevation = 90.0 - degrees(zenith);

    // Atmospheric refraction (NOAA approximation).
    if (elevation <= 85.0) {
        const double te = std::tan(radians(elevation));
        double refractionArcSeconds = 0.0;
        if (elevation > 5.0) {
            refractionArcSeconds = 58.1 / te - 0.07 / (te * te * te) + 0.000086 / std::pow(te, 5);
        } else if (elevation > -0.575) {
            refractionArcSeconds =
                1735.0 + elevation * (-518.2 + elevation * (103.4 + elevation * (-12.79 +
                                                                                 elevation * 0.711)));
        } else {
            refractionArcSeconds = -20.772 / te;
        }
        elevation += refractionArcSeconds / 3600.0;
    }

    double azimuth = 0.0;
    const double sinZenith = std::sin(zenith);
    if (std::abs(sinZenith) < 1e-9 || std::abs(std::cos(latitude)) < 1e-9) {
        azimuth = latitudeDegrees > 0.0 ? 180.0 : 0.0; // sun overhead or observer at a pole
    } else {
        const double cosAzimuth = std::clamp(
            (std::sin(latitude) * cosZenith - std::sin(declination)) /
                (std::cos(latitude) * sinZenith),
            -1.0, 1.0);
        const double angle = degrees(std::acos(cosAzimuth));
        azimuth = hourAngle > 0.0 ? wrap(angle + 180.0, 360.0) : wrap(540.0 - angle, 360.0);
    }
    return {azimuth, elevation};
}

glm::vec3 kelvinToLinearRgb(float kelvin) {
    // Tanner Helland's fit of the black-body locus (sRGB-encoded), linearized and luminance
    // normalized.
    const double t = std::clamp(static_cast<double>(kelvin), 1000.0, 40000.0) / 100.0;
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    if (t <= 66.0) {
        r = 255.0;
        g = 99.4708025861 * std::log(t) - 161.1195681661;
        b = t <= 19.0 ? 0.0 : 138.5177312231 * std::log(t - 10.0) - 305.0447927307;
    } else {
        r = 329.698727446 * std::pow(t - 60.0, -0.1332047592);
        g = 288.1221695283 * std::pow(t - 60.0, -0.0755148492);
        b = 255.0;
    }
    const glm::vec3 srgb = glm::clamp(
        glm::vec3(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)) / 255.0f,
        glm::vec3(0.0f), glm::vec3(1.0f));
    const glm::vec3 linear = srgbToLinear(srgb);
    const float luminance = glm::dot(linear, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    return luminance > 0.0f ? linear / luminance : glm::vec3(1.0f);
}

} // namespace lb
