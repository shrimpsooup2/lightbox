#pragma once

#include <glm/vec3.hpp>

namespace lb {

struct SolarPosition {
    double azimuthDegrees;   // compass bearing toward the sun, clockwise from true north
    double elevationDegrees; // above the horizon, including atmospheric refraction
};

// Sun position for a site and local clock time, using the NOAA solar calculator equations
// (Meeus-based, accurate to well under 0.1° for 1900–2100). `utcOffsetHours` is the site's offset
// from UTC at that date (e.g. +1 for British Summer Time).
SolarPosition solarPosition(double latitudeDegrees, double longitudeDegrees, int year, int month,
                            int day, double localHours, double utcOffsetHours);

// Color of light from a black body at `kelvin` (1000–40000 K), as linear Rec.709 RGB scaled to unit
// luminance, so changing the temperature never changes brightness.
glm::vec3 kelvinToLinearRgb(float kelvin);

} // namespace lb
