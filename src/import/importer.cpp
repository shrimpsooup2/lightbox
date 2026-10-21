#include "import/importer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <new>
#include <string>
#include <system_error>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "import/formats.hpp"

namespace lb::import {
namespace detail {

void Job::checkCancelled() const {
    if (cancelled()) {
        throw ImportCancelled();
    }
}

void Job::report(float fraction, std::string_view stage) const {
    checkCancelled();
    if (progress_) {
        progress_(std::clamp(fraction, 0.0f, 1.0f), stage);
    }
}

std::filesystem::path pathFromUtf8(std::string_view utf8) {
    // char8_t keeps Windows from interpreting the bytes in the ANSI code page.
    std::u8string text(utf8.size(), u8'\0');
    std::transform(utf8.begin(), utf8.end(), text.begin(),
                   [](char c) { return static_cast<char8_t>(c); });
    return std::filesystem::path(text);
}

std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    std::string result(text.size(), '\0');
    std::transform(text.begin(), text.end(), result.begin(),
                   [](char8_t c) { return static_cast<char>(c); });
    return result;
}

std::string displayName(const std::filesystem::path& path) {
    return pathToUtf8(path.filename());
}

std::string toLowerAscii(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

glm::vec3 srgbToLinear(const glm::vec3& color) {
    const auto channel = [](float c) {
        c = std::clamp(c, 0.0f, 1.0f);
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return {channel(color.r), channel(color.g), channel(color.b)};
}

glm::mat4 yUpToZUp() {
    return glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
}

bool decodeBase64(std::string_view text, std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    bool padding = false;
    for (const char c : text) {
        int value = -1;
        if (c >= 'A' && c <= 'Z') {
            value = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            value = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            value = c - '0' + 52;
        } else if (c == '+' || c == '-') {
            value = 62;
        } else if (c == '/' || c == '_') {
            value = 63;
        } else if (c == '=') {
            padding = true;
            continue;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        } else {
            return false;
        }
        if (padding) {
            return false; // data after '='
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFFu));
        }
    }
    return true;
}

std::string decodePercent(std::string_view uri) {
    const auto hex = [](char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::string result;
    result.reserve(uri.size());
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size() && hex(uri[i + 1]) >= 0 && hex(uri[i + 2]) >= 0) {
            result.push_back(static_cast<char>(hex(uri[i + 1]) * 16 + hex(uri[i + 2])));
            i += 2;
        } else {
            result.push_back(uri[i]);
        }
    }
    return result;
}

namespace {

bool isFile(const std::filesystem::path& path) {
    std::error_code error;
    return !path.empty() && std::filesystem::is_regular_file(path, error);
}

} // namespace

std::filesystem::path resolveTexturePath(std::string_view reference,
                                         const std::filesystem::path& baseDirectory) {
    std::string text(reference);
    // Trim whitespace and quotes that some exporters leave around file names.
    const auto isJunk = [](char c) {
        return c == '"' || c == '\'' || std::isspace(static_cast<unsigned char>(c));
    };
    while (!text.empty() && isJunk(text.back())) {
        text.pop_back();
    }
    const auto firstKept = std::find_if_not(text.begin(), text.end(), isJunk);
    text.erase(text.begin(), firstKept);
    if (text.empty()) {
        return {};
    }
    // Paths written on Windows use backslashes; '/' works on every platform.
    std::replace(text.begin(), text.end(), '\\', '/');
    const std::filesystem::path written = pathFromUtf8(text);

    if (written.is_absolute() && isFile(written)) {
        return written;
    }
    if (!written.is_absolute() && isFile(baseDirectory / written)) {
        return baseDirectory / written;
    }
    // Fall back to the file name next to the model or in one of its subfolders.
    const std::filesystem::path fileName = written.filename();
    if (fileName.empty()) {
        return {};
    }
    if (isFile(baseDirectory / fileName)) {
        return baseDirectory / fileName;
    }
    std::error_code error;
    for (std::filesystem::directory_iterator it(baseDirectory, error), end; !error && it != end;
         it.increment(error)) {
        std::error_code typeError;
        if (it->is_directory(typeError) && isFile(it->path() / fileName)) {
            return it->path() / fileName;
        }
    }
    return {};
}

int TextureCache::fromReference(std::string_view reference,
                                const std::filesystem::path& baseDirectory) {
    if (reference.empty()) {
        return -1;
    }
    const std::filesystem::path resolved = resolveTexturePath(reference, baseDirectory);
    if (resolved.empty()) {
        std::string name(reference);
        std::replace(name.begin(), name.end(), '\\', '/');
        name = displayName(pathFromUtf8(name));
        return fail("missing:" + std::string(reference),
                    std::format("Texture {} was not found next to the model; the material is "
                                "shown without it.",
                                name.empty() ? std::string(reference) : name));
    }
    return fromFile(resolved);
}

int TextureCache::fromFile(const std::filesystem::path& file) {
    std::error_code error;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(file, error);
    if (error) {
        canonical = file;
    }
    const std::string key = "file:" + pathToUtf8(canonical);
    if (const auto it = byKey_.find(key); it != byKey_.end()) {
        return it->second;
    }
    const std::string name = displayName(file);
    if (!isImageFile(file)) {
        return fail(key, std::format("Texture {} has a format LightBox can't read yet ({}); the "
                                     "material is shown without it.",
                                     name, toLowerAscii(pathToUtf8(file.extension()))));
    }
    job_.checkCancelled();
    ImageData image;
    try {
        image = loadImage(file);
    } catch (const std::bad_alloc&) {
        return fail(key, std::format("Texture {} is too large to load.", name));
    } catch (const std::exception& e) {
        return fail(key, e.what());
    }
    return add(key, std::move(image), name, pathToUtf8(canonical));
}

int TextureCache::fromMemory(const std::string& key, std::span<const std::uint8_t> encoded,
                             const std::string& name) {
    if (const auto it = byKey_.find(key); it != byKey_.end()) {
        return it->second;
    }
    job_.checkCancelled();
    ImageData image;
    try {
        image = decodeImage(encoded, name);
    } catch (const std::bad_alloc&) {
        return fail(key, std::format("Texture {} is too large to load.", name));
    } catch (const std::exception& e) {
        return fail(key, e.what());
    }
    return add(key, std::move(image), name, {});
}

int TextureCache::fail(const std::string& key, std::string warning) {
    if (byKey_.emplace(key, -1).second) {
        warnOnce(model_, std::move(warning));
    }
    return -1;
}

int TextureCache::add(const std::string& key, ImageData image, std::string name,
                      std::string sourcePath) {
    const int index = static_cast<int>(model_.textures.size());
    model_.textures.push_back(
        {std::move(name), std::move(sourcePath), downscaleToFit(std::move(image), kMaxTextureSize)});
    byKey_.emplace(key, index);
    return index;
}

void warnOnce(ImportedModel& model, std::string warning) {
    if (std::find(model.warnings.begin(), model.warnings.end(), warning) == model.warnings.end()) {
        model.warnings.push_back(std::move(warning));
    }
}

UnitGuess guessUnits(float largestExtent) {
    if (largestExtent > 5000.0f) {
        return {0.001, "millimeters"};
    }
    if (largestExtent > 500.0f) {
        return {0.01, "centimeters"};
    }
    return {1.0, "meters"};
}

} // namespace detail

namespace {

constexpr std::array<std::string_view, 9> kModelExtensions = {
    ".gltf", ".glb", ".obj", ".fbx", ".dae", ".kmz", ".3ds", ".stl", ".ply"};

constexpr std::array<UnitChoice, 5> kUnitChoices = {{
    {"Meters", 1.0},
    {"Centimeters", 0.01},
    {"Millimeters", 0.001},
    {"Inches", 0.0254},
    {"Feet", 0.3048},
}};

} // namespace

std::span<const std::string_view> modelFileExtensions() { return kModelExtensions; }

bool isModelFile(const std::filesystem::path& path) {
    const std::string ext = detail::toLowerAscii(detail::pathToUtf8(path.extension()));
    return std::find(kModelExtensions.begin(), kModelExtensions.end(), ext) !=
           kModelExtensions.end();
}

std::span<const UnitChoice> unitChoices() { return kUnitChoices; }

ImportedModel importModel(const std::filesystem::path& path, const Progress& progress,
                          const std::atomic<bool>* cancel) {
    const detail::Job job(progress, cancel);
    const std::string fileName = detail::displayName(path);
    job.report(0.0f, "Reading file");

    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        throw ImportError(std::format("The file {} does not exist or can't be opened.", fileName));
    }
    const std::string ext = detail::toLowerAscii(detail::pathToUtf8(path.extension()));

    ImportedModel model;
    model.name = detail::pathToUtf8(path.stem());
    if (model.name.empty()) {
        model.name = "Model";
    }
    model.sourcePath = detail::pathToUtf8(path);
    try {
        if (ext == ".gltf" || ext == ".glb") {
            model.format = "glTF";
            detail::importGltf(path, model, job);
        } else if (ext == ".obj") {
            model.format = "OBJ";
            detail::importObj(path, model, job);
        } else if (ext == ".fbx") {
            model.format = "FBX";
            detail::importFbx(path, model, job);
        } else if (ext == ".dae" || ext == ".kmz") {
            model.format = ext == ".kmz" ? "KMZ (Collada)" : "Collada";
            detail::importAssimp(path, model, job);
        } else if (ext == ".3ds" || ext == ".stl" || ext == ".ply") {
            model.format = ext == ".3ds" ? "3DS" : ext == ".stl" ? "STL" : "PLY";
            detail::importAssimp(path, model, job);
        } else {
            throw ImportError(std::format("{} is not a model format LightBox can import (use glTF, "
                                          "GLB, OBJ, FBX, Collada, KMZ, 3DS, STL or PLY).",
                                          fileName));
        }
    } catch (const ImportError&) {
        throw;
    } catch (const ImportCancelled&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw ImportError(std::format("Not enough memory to import {}.", fileName));
    } catch (const std::exception& e) {
        throw ImportError(std::format("Could not import {}: {}", fileName, e.what()));
    }

    if (model.meshes.empty()) {
        throw ImportError(std::format("{} contains no surfaces LightBox can show (only points, "
                                      "lines or empty objects).",
                                      fileName));
    }
    job.report(1.0f, "Done");
    return model;
}

} // namespace lb::import
