#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/camera.hpp"
#include "core/scene/scene.hpp"

// LightBox project files (`.lbox`, D-022): a zip package containing
//   project.json        format version, project info, camera, environment, look, entities, materials,
//                       and metadata of meshes and textures (deterministic: sorted by ID)
//   meshes/<id>.bin     mesh data (positions, normals, uvs, indices; little-endian)
//   textures/<id>.rgba  texture pixels (RGBA8, rows top to bottom)
//   thumbnail.png       optional preview for the start screen
// Qt-free and GPU-free. loadProject/readProjectInfo touch no Scene and may run on a worker thread;
// the UI thread installs the result with Scene::replaceContents.
namespace lb::project {

// Version written by this build. Files with a higher version are refused with a clear message; lower
// versions are migrated on load.
constexpr int kFormatVersion = 1;
inline constexpr std::string_view kFileExtension = ".lbox";

// A failure the user should read (names the file, says what to do).
class ProjectError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ProjectInfo {
    std::string name;              // display name, e.g. "Villa Rosa"
    std::string appVersion;        // LightBox version that wrote the file (set by saveProject)
    std::int64_t savedAtUnix = 0;  // seconds since 1970 UTC (set by saveProject)
    std::optional<CameraState> camera; // viewport camera to restore
    std::vector<std::uint8_t> thumbnailPng; // encoded by the caller (the app uses Qt), may be empty
};

struct LoadedProject {
    SceneSnapshot scene;
    ProjectInfo info;
    int formatVersion = kFormatVersion; // version found in the file before migration
};

// Writes the whole project. Atomic: writes a temporary file next to `path` and renames it over the
// target, so a crash or error never leaves a half-written project. Throws ProjectError.
void saveProject(const Scene& scene, const ProjectInfo& info, const std::filesystem::path& path);

// Reads a whole project (no Scene access; thread-safe). Throws ProjectError for files that are not
// LightBox projects, are damaged, or were written by a newer LightBox ("…saved with a newer version
// of LightBox (format N); this version reads up to format M. Please update LightBox.").
LoadedProject loadProject(const std::filesystem::path& path);

// Reads only the project info and thumbnail (fast; used by the start screen). Throws ProjectError.
ProjectInfo readProjectInfo(const std::filesystem::path& path);

// The document part of project.json for `scene`, canonical and deterministic (sorted by ID, stable
// number formatting). Saving then loading a project reproduces this string exactly.
std::string documentJson(const Scene& scene);

} // namespace lb::project
