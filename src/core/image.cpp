#include "core/image.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_FAILURE_USERMSG
#include <stb_image.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace lb {
namespace {

constexpr std::array<std::string_view, 9> kExtensions = {".png", ".jpg", ".jpeg", ".tga", ".bmp",
                                                          ".psd", ".gif", ".hdr", ".jpe"};

std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // namespace

ImageData decodeImage(std::span<const std::uint8_t> encoded, std::string_view nameForErrors) {
    if (encoded.empty()) {
        throw std::runtime_error(std::format("{} is empty.", nameForErrors));
    }
    if (encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(std::format("{} is too large to decode.", nameForErrors));
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width,
                                             &height, &channels, STBI_rgb_alpha);
    if (decoded == nullptr) {
        throw std::runtime_error(std::format("Could not read the image {} ({}).", nameForErrors,
                                             stbi_failure_reason()));
    }
    ImageData image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    const std::size_t bytes = std::size_t{image.width} * image.height * 4;
    image.pixels.assign(decoded, decoded + bytes);
    stbi_image_free(decoded);
    return image;
}

ImageData loadImage(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error(std::format("Could not open the image {}.", path.string()));
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error(std::format("The image {} is empty.", name));
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error(std::format("Could not read the image {}.", path.string()));
    }
    return decodeImage(bytes, name);
}

std::span<const std::string_view> imageFileExtensions() {
    return kExtensions;
}

bool isImageFile(const std::filesystem::path& path) {
    const std::string ext = lowerExtension(path);
    return std::find(kExtensions.begin(), kExtensions.end(), ext) != kExtensions.end();
}

ImageData downscaleToFit(ImageData image, std::uint32_t maxSize) {
    maxSize = std::max<std::uint32_t>(maxSize, 1);
    while (image.width > maxSize || image.height > maxSize) {
        const std::uint32_t w = std::max<std::uint32_t>(image.width / 2, 1);
        const std::uint32_t h = std::max<std::uint32_t>(image.height / 2, 1);
        ImageData half;
        half.width = w;
        half.height = h;
        half.pixels.resize(std::size_t{w} * h * 4);
        for (std::uint32_t y = 0; y < h; ++y) {
            const std::uint32_t y0 = std::min(y * 2, image.height - 1);
            const std::uint32_t y1 = std::min(y * 2 + 1, image.height - 1);
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::uint32_t x0 = std::min(x * 2, image.width - 1);
                const std::uint32_t x1 = std::min(x * 2 + 1, image.width - 1);
                for (std::uint32_t c = 0; c < 4; ++c) {
                    const auto at = [&](std::uint32_t px, std::uint32_t py) {
                        return static_cast<std::uint32_t>(
                            image.pixels[(std::size_t{py} * image.width + px) * 4 + c]);
                    };
                    const std::uint32_t sum = at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1);
                    half.pixels[(std::size_t{y} * w + x) * 4 + c] =
                        static_cast<std::uint8_t>((sum + 2) / 4);
                }
            }
        }
        image = std::move(half);
    }
    return image;
}

} // namespace lb
