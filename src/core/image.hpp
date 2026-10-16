#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace lb {

// Decoded 8-bit image: RGBA, rows top to bottom, tightly packed (width * 4 bytes per row).
// Color space is decided by how the image is used (base color = sRGB, data maps = linear).
struct ImageData {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;

    bool empty() const { return width == 0 || height == 0; }
    bool operator==(const ImageData&) const = default;
};

// Decodes PNG, JPEG, TGA, BMP, PSD (composited), GIF (first frame) and Radiance HDR (tone-mapped to
// 8 bits) into RGBA8. 16-bit images are reduced to 8 bits. Thread-safe.
// Throws std::runtime_error with a user-presentable message (naming `nameForErrors`) on failure.
ImageData decodeImage(std::span<const std::uint8_t> encoded, std::string_view nameForErrors);
ImageData loadImage(const std::filesystem::path& path);

// File extensions decodeImage() understands, lower case with the dot (".png", ".jpg", …).
std::span<const std::string_view> imageFileExtensions();
bool isImageFile(const std::filesystem::path& path);

// Halves the image (2×2 box filter, odd edges clamped) until both sides are <= maxSize. Used to keep
// textures within GPU/memory budgets and to make thumbnails. Returns the input when it already fits.
ImageData downscaleToFit(ImageData image, std::uint32_t maxSize);

} // namespace lb
