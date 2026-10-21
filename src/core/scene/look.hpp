#pragma once

namespace lb {

enum class ToneMapping {
    Filmic,  // ACES-like film curve: rich contrast, rolls off highlights
    Neutral, // Khronos PBR Neutral: keeps material colors true
    None,    // clamp only
    AgX,     // AgX: gentle contrast, bright colors fade to white without hue shifts
};

// Photographic and stylistic settings of the image. Document-level today; per saved View in 2.9.
struct Look {
    float exposureEv = 0.0f; // -4 … +4 stops (with auto exposure: compensation on top of it)
    ToneMapping toneMapping = ToneMapping::Filmic;
    float contrast = 1.0f;   // 0.5 … 1.5, around mid grey
    float saturation = 1.0f; // 0 … 2
    float warmth = 0.0f;     // -1 (cool) … +1 (warm) white balance shift
    float vignette = 0.15f;  // 0 … 1
    bool clay = false;       // white model: ignore materials
    // Adapt exposure to the image's brightness, like a camera: daylight, interiors and evenings
    // all come out well exposed (lamps are thousands of times dimmer than the sun).
    bool autoExposure = true;

    bool operator==(const Look&) const = default;
};

} // namespace lb
