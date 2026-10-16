#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>

#include "core/image.hpp"
#include "core/mesh.hpp"
#include "core/scene/ids.hpp"
#include "core/scene/material.hpp"

namespace lb {
class Scene;
}

// Model and texture import (Stage 1). Qt-free; parsing runs on a worker thread, producing an
// ImportedModel that is then added to the Scene on the UI thread with addToScene().
namespace lb::import {

// Textures larger than this (either side) are downscaled on import to keep memory in budget.
constexpr std::uint32_t kMaxTextureSize = 4096;

// A failure the user should read ("The file is not a valid glTF file: …"). Thrown by importModel().
class ImportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thrown by importModel() when `cancel` was set.
class ImportCancelled : public std::exception {
public:
    const char* what() const noexcept override { return "Import cancelled"; }
};

struct ImportedTexture {
    std::string name;       // file name or embedded image name
    std::string sourcePath; // UTF-8 path, empty for images embedded in the model file
    ImageData image;        // already capped to kMaxTextureSize
};

// Texture slot by index into ImportedModel::textures (-1 = unused).
struct ImportedTextureSlot {
    int texture = -1;
    int channel = 0;
    bool invert = false;
};

struct ImportedMaterial {
    std::string name;
    Material material; // scalar/color values and mapping; its TextureSlots are ignored
    ImportedTextureSlot baseColor;
    ImportedTextureSlot roughness;
    ImportedTextureSlot metalness;
    ImportedTextureSlot normal;
    ImportedTextureSlot occlusion;
};

// One material per mesh: multi-material source meshes are split per material.
struct ImportedMesh {
    std::string name;
    MeshData data; // normals always present (generated if the file has none)
    int material = -1; // index into ImportedModel::materials, -1 = default clay
};

struct ImportedNode {
    std::string name;
    int parent = -1;              // index of an earlier node, -1 = child of the model root
    glm::mat4 transform{1.0f};    // relative to the parent
    std::vector<int> meshes;      // instances of ImportedModel::meshes placed at this node
};

// Geometry is already converted to LightBox axes (right-handed, Z up) but kept in file units;
// addToScene() applies `metersPerUnit` on the model's root entity, so units stay changeable.
struct ImportedModel {
    std::string name; // file name without extension
    std::string sourcePath;
    std::string format; // "glTF", "OBJ", "FBX"
    std::vector<ImportedNode> nodes; // parents before children
    std::vector<ImportedMesh> meshes;
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedTexture> textures;
    double metersPerUnit = 1.0;
    bool unitsFromFile = false; // false = guessed from the model size
    std::vector<std::string> warnings; // user-presentable, e.g. "Texture wood.jpg not found"
};

// Progress callback: fraction 0..1 and a short stage text ("Reading file", "Loading textures").
// Called on the importing thread.
using Progress = std::function<void(float fraction, std::string_view stage)>;

// Model formats importModel() accepts, lower case with the dot (".gltf", ".glb", ".obj", ".fbx").
std::span<const std::string_view> modelFileExtensions();
bool isModelFile(const std::filesystem::path& path);

// Parses a model file and everything it references (buffers, textures). Thread-safe, no Scene access.
// Throws ImportError (readable message) or ImportCancelled.
ImportedModel importModel(const std::filesystem::path& path, const Progress& progress = {},
                          const std::atomic<bool>* cancel = nullptr);

// Common length units offered for changing a model's scale after import.
struct UnitChoice {
    std::string_view name; // "Meters", "Centimeters", "Millimeters", "Inches", "Feet"
    double metersPerUnit;
};
std::span<const UnitChoice> unitChoices();

struct PlacedModel {
    EntityId root; // group named after the file; its transform scales by metersPerUnit
    std::size_t objects = 0;   // entities with a mesh
    std::size_t triangles = 0; // over all instances
    std::size_t materials = 0;
    std::size_t textures = 0;
};

// Adds textures, materials, meshes and the node hierarchy under a new root entity (UI thread).
// Models whose bounds lie more than 10 km from the origin are shifted next to it (warning added to
// `model.warnings`), because far-away coordinates break float precision.
// Mesh and image data are moved into the scene (no multi-hundred-MB copies on the UI thread), so
// `model` keeps its names, structure and warnings but no geometry or pixels afterwards. Materials,
// textures and node branches that no mesh uses are not added.
PlacedModel addToScene(Scene& scene, ImportedModel& model);

// --- Texture sets (Poly Haven, ambientCG, Quixel style PBR folders) ---------------------------------

enum class MapKind { BaseColor, Normal, NormalDirectX, Roughness, Gloss, Metalness, Occlusion, Height,
                     PackedOcclusionRoughnessMetalness, Unknown };

// Classifies an image file by name tokens: "albedo|basecolor|diffuse|color|col|diff" → BaseColor,
// "normal|nor|nrm|norm" (+ "dx|directx" → NormalDirectX), "roughness|rough|rgh", "gloss|glossiness",
// "metal|metallic|metalness", "ao|occlusion|ambientocclusion", "height|disp|displacement|bump",
// "arm|orm" → PackedOcclusionRoughnessMetalness (R = occlusion, G = roughness, B = metalness).
// The last recognized token wins ("metal_plate_diff_1k" is a base color map).
MapKind classifyTextureFile(const std::filesystem::path& path);

// Material name shared by a set: the common file-name prefix without trailing separators/tokens
// ("Bricks076C_1K-PNG_Color.png" + "…_Roughness.png" → "Bricks076C").
std::string textureSetName(std::span<const std::filesystem::path> files);

// Loads a set of image files as one material. Files without a recognized map token (previews) and
// height maps are ignored with a warning; with both OpenGL and DirectX normal maps the OpenGL one is
// used; a gloss map is used (inverted) only without a roughness map; a packed ARM/ORM map fills the
// occlusion/roughness/metalness slots that no separate map provides.
// Thread-safe (worker thread). The result has no meshes or nodes: add it with addMaterialToScene().
// Throws ImportError if no file is a usable image.
ImportedModel importTextureSet(std::span<const std::filesystem::path> files,
                               const Progress& progress = {},
                               const std::atomic<bool>* cancel = nullptr);

// Adds the textures and the first material of `model` (from importTextureSet) to the scene.
// Box mapping with a 1 m tile is used, because a texture set has no UVs of its own.
ResourceId addMaterialToScene(Scene& scene, const ImportedModel& model);

} // namespace lb::import
