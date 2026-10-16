#pragma once

// Internal to lb_import: helpers shared by the format readers (implemented in importer.cpp) and the
// per-format entry points that importModel() dispatches to.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "import/importer.hpp"

namespace lb::import::detail {

// Cancellation and progress for one import. Every report() also checks for cancellation, so readers
// call it between stages and per mesh/texture.
class Job {
public:
    Job(const Progress& progress, const std::atomic<bool>* cancel)
        : progress_(progress), cancel_(cancel) {}

    // Throws ImportCancelled when the user cancelled.
    void checkCancelled() const;
    // `fraction` is clamped to 0..1.
    void report(float fraction, std::string_view stage) const;
    bool cancelled() const { return cancel_ != nullptr && cancel_->load(std::memory_order_relaxed); }

private:
    const Progress& progress_;
    const std::atomic<bool>* cancel_;
};

// Per-format readers. They fill geometry, nodes, materials, textures, units and warnings; importModel()
// sets name/sourcePath/format and turns unexpected exceptions into ImportError.
void importGltf(const std::filesystem::path& path, ImportedModel& model, const Job& job);
void importObj(const std::filesystem::path& path, ImportedModel& model, const Job& job);
void importFbx(const std::filesystem::path& path, ImportedModel& model, const Job& job);

// UTF-8 <-> std::filesystem::path without going through the Windows ANSI code page.
std::filesystem::path pathFromUtf8(std::string_view utf8);
std::string pathToUtf8(const std::filesystem::path& path);
// File name (with extension) as UTF-8, for messages.
std::string displayName(const std::filesystem::path& path);

std::string toLowerAscii(std::string_view text);

// sRGB-encoded display color (color pickers, MTL Kd) to linear RGB.
glm::vec3 srgbToLinear(const glm::vec3& color);

// Rotation taking Y-up data to LightBox's Z-up (+90° about X): (x, y, z) → (x, −z, y).
glm::mat4 yUpToZUp();
inline glm::vec3 yUpToZUp(const glm::vec3& v) { return {v.x, -v.z, v.y}; }

// Standard (RFC 4648) base64 decoding; whitespace is skipped. Returns false for malformed input.
bool decodeBase64(std::string_view text, std::vector<std::uint8_t>& out);
// Decodes %XX escapes in a URI.
std::string decodePercent(std::string_view uri);

// Finds a texture referenced by a model file: as written (absolute or relative to `baseDirectory`),
// then by file name in `baseDirectory` and its immediate subfolders ("textures", "maps", …), because
// models often carry absolute paths from the author's machine. Returns an empty path if not found.
std::filesystem::path resolveTexturePath(std::string_view reference,
                                         const std::filesystem::path& baseDirectory);

// Loads each texture of a model once (deduplicated by resolved path or embedded key) into
// ImportedModel::textures. Failures add a readable warning and return -1, never throw (except
// ImportCancelled), because a missing texture must not fail the whole import.
class TextureCache {
public:
    TextureCache(ImportedModel& model, const Job& job) : model_(model), job_(job) {}

    // A texture path as written in the model file, resolved with resolveTexturePath().
    int fromReference(std::string_view reference, const std::filesystem::path& baseDirectory);
    // An image file that is known to exist.
    int fromFile(const std::filesystem::path& file);
    // Encoded image bytes embedded in the model file. `key` must be unique per image.
    int fromMemory(const std::string& key, std::span<const std::uint8_t> encoded,
                   const std::string& name);
    // Records an unusable texture under `key` with a warning (only once per key).
    int fail(const std::string& key, std::string warning);

private:
    int add(const std::string& key, ImageData image, std::string name, std::string sourcePath);

    ImportedModel& model_;
    const Job& job_;
    std::unordered_map<std::string, int> byKey_;
};

// Adds `warning` to the model unless the same text is already there (per-mesh issues repeat).
void warnOnce(ImportedModel& model, std::string warning);

} // namespace lb::import::detail
