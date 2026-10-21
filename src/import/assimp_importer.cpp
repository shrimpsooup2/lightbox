// Collada (.dae: SketchUp's free export, 3D Warehouse), KMZ (zipped Collada from Google Earth and
// 3D Warehouse), 3DS, STL and PLY via Assimp (only these importers are compiled in, D-025).
//
// Axes: Assimp turns Collada (by its <up_axis>) and 3DS (Z-up by convention) into Y-up on the root
// node, so those get the Y-up → Z-up turn on top. STL and PLY carry no axis information: they are
// taken as Z-up (CAD and most scanners) and marked so the app can check them against the geometry.
// Units: Collada's <unit> is applied by Assimp (the result is meters); the others are guessed from
// the model size. Meshes keep their instancing through the node hierarchy.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include <glm/gtc/type_ptr.hpp>
#include <glm/matrix.hpp>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <miniz.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "import/formats.hpp"
#include "import/mesh_processing.hpp"

namespace lb::import::detail {
namespace {

enum class Kind { Collada, ThreeDs, Stl, Ply };

glm::mat4 toMat4(const aiMatrix4x4& m) {
    return glm::transpose(glm::make_mat4(&m.a1)); // Assimp is row-major
}

std::string toString(const aiString& text) {
    return {text.C_Str(), text.length};
}

// Unzips a KMZ into a fresh temporary folder and returns its main .dae (the one under models/, or
// the first). The caller removes the folder. Entries that would land outside it are skipped.
std::filesystem::path unpackKmz(const std::filesystem::path& kmz,
                                const std::filesystem::path& folder) {
    mz_zip_archive zip{};
    const std::string kmzPath = pathToUtf8(kmz);
    if (!mz_zip_reader_init_file(&zip, kmzPath.c_str(), 0)) {
        throw ImportError(std::format("{} is not a valid KMZ (zip) file.", displayName(kmz)));
    }
    std::filesystem::path model;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory) {
            continue;
        }
        const std::string name = stat.m_filename;
        if (name.find("..") != std::string::npos || name.starts_with('/') ||
            name.find(':') != std::string::npos) {
            continue;
        }
        const std::filesystem::path target = folder / pathFromUtf8(name);
        std::error_code error;
        std::filesystem::create_directories(target.parent_path(), error);
        if (!mz_zip_reader_extract_to_file(&zip, i, pathToUtf8(target).c_str(), 0)) {
            continue;
        }
        if (toLowerAscii(pathToUtf8(target.extension())) == ".dae" &&
            (model.empty() || toLowerAscii(name).starts_with("models/"))) {
            model = target;
        }
    }
    mz_zip_reader_end(&zip);
    if (model.empty()) {
        throw ImportError(
            std::format("{} contains no 3D model (no .dae file inside).", displayName(kmz)));
    }
    return model;
}

class AssimpReader {
public:
    AssimpReader(const std::filesystem::path& path, Kind kind, ImportedModel& model, const Job& job)
        : path_(path), kind_(kind), model_(model), job_(job), textures_(model, job) {}

    void read() {
        Assimp::Importer importer;
        // Points and lines are dropped; LightBox shows surfaces only.
        importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
                                    aiPrimitiveType_POINT | aiPrimitiveType_LINE);
        const unsigned flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                               aiProcess_SortByPType | aiProcess_FlipUVs;
        scene_ = importer.ReadFile(pathToUtf8(path_), flags);
        if (scene_ == nullptr || scene_->mRootNode == nullptr) {
            throw ImportError(std::format("{} could not be read: {}", displayName(path_),
                                          importer.GetErrorString()));
        }
        job_.report(0.4f, "Loading textures");
        convertMaterials();
        job_.report(0.7f, "Converting geometry");
        convertMeshes();
        convertNodes();
        setUnitsAndAxes();
    }

private:
    ImportedTextureSlot slot(const aiMaterial& material, aiTextureType type) {
        ImportedTextureSlot result;
        aiString reference;
        if (material.GetTexture(type, 0, &reference) != AI_SUCCESS || reference.length == 0) {
            return result;
        }
        const std::string text = toString(reference);
        if (const aiTexture* embedded = scene_->GetEmbeddedTexture(reference.C_Str())) {
            if (embedded->mHeight == 0) { // compressed image file bytes
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(embedded->pcData);
                result.texture = textures_.fromMemory(
                    "embedded:" + text, std::span<const std::uint8_t>(bytes, embedded->mWidth),
                    embedded->mFilename.length > 0 ? toString(embedded->mFilename) : text);
            } else {
                result.texture = textures_.fail("embedded:" + text,
                                                std::format("Embedded raw texture {} is not "
                                                            "supported and was skipped.",
                                                            text));
            }
            return result;
        }
        result.texture = textures_.fromReference(text, path_.parent_path());
        return result;
    }

    void convertMaterials() {
        for (unsigned i = 0; i < scene_->mNumMaterials; ++i) {
            const aiMaterial& source = *scene_->mMaterials[i];
            ImportedMaterial imported;
            aiString name;
            imported.name = source.Get(AI_MATKEY_NAME, name) == AI_SUCCESS && name.length > 0
                                ? toString(name)
                                : std::format("Material {}", i + 1);
            Material& material = imported.material;
            aiColor4D color;
            if (source.Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS ||
                source.Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
                material.baseColor = srgbToLinear(glm::vec3(color.r, color.g, color.b));
            }
            imported.baseColor = slot(source, aiTextureType_BASE_COLOR);
            if (imported.baseColor.texture < 0) {
                imported.baseColor = slot(source, aiTextureType_DIFFUSE);
            }
            if (imported.baseColor.texture >= 0) {
                // As with OBJ: the color is a stand-in for viewers without textures, not a tint.
                material.baseColor = glm::vec3(1.0f);
            }
            imported.normal = slot(source, aiTextureType_NORMALS);
            model_.materials.push_back(std::move(imported));
        }
    }

    void convertMeshes() {
        meshIndex_.assign(scene_->mNumMeshes, -1);
        for (unsigned i = 0; i < scene_->mNumMeshes; ++i) {
            job_.checkCancelled();
            const aiMesh& source = *scene_->mMeshes[i];
            if ((source.mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0) {
                continue;
            }
            ImportedMesh mesh;
            mesh.name = toString(source.mName);
            mesh.material = source.mMaterialIndex < scene_->mNumMaterials
                                ? static_cast<int>(source.mMaterialIndex)
                                : -1;
            MeshData& data = mesh.data;
            data.positions.reserve(source.mNumVertices);
            for (unsigned v = 0; v < source.mNumVertices; ++v) {
                const aiVector3D& p = source.mVertices[v];
                data.positions.emplace_back(p.x, p.y, p.z);
            }
            if (source.HasNormals()) {
                data.normals.reserve(source.mNumVertices);
                for (unsigned v = 0; v < source.mNumVertices; ++v) {
                    const aiVector3D& n = source.mNormals[v];
                    data.normals.emplace_back(n.x, n.y, n.z);
                }
            }
            if (source.HasTextureCoords(0)) {
                data.uvs.reserve(source.mNumVertices);
                for (unsigned v = 0; v < source.mNumVertices; ++v) {
                    const aiVector3D& t = source.mTextureCoords[0][v];
                    data.uvs.emplace_back(t.x, t.y);
                }
            }
            data.indices.reserve(static_cast<std::size_t>(source.mNumFaces) * 3);
            for (unsigned f = 0; f < source.mNumFaces; ++f) {
                const aiFace& face = source.mFaces[f];
                if (face.mNumIndices == 3) {
                    data.indices.insert(data.indices.end(), face.mIndices, face.mIndices + 3);
                }
            }
            if (!finalizeMesh(data)) {
                continue;
            }
            meshIndex_[i] = static_cast<int>(model_.meshes.size());
            model_.meshes.push_back(std::move(mesh));
        }
    }

    // Parents before children; the Assimp root becomes the top node and carries the axis turn.
    void convertNodes() {
        const bool yUp = kind_ == Kind::Collada || kind_ == Kind::ThreeDs;
        struct Pending {
            const aiNode* node;
            int parent;
        };
        std::vector<Pending> stack{{scene_->mRootNode, -1}};
        while (!stack.empty()) {
            const Pending current = stack.back();
            stack.pop_back();
            ImportedNode node;
            node.name = toString(current.node->mName);
            node.parent = current.parent;
            node.transform = toMat4(current.node->mTransformation);
            if (current.parent < 0 && yUp) {
                node.transform = yUpToZUp() * node.transform;
            }
            for (unsigned m = 0; m < current.node->mNumMeshes; ++m) {
                const unsigned source = current.node->mMeshes[m];
                if (source < meshIndex_.size() && meshIndex_[source] >= 0) {
                    node.meshes.push_back(meshIndex_[source]);
                }
            }
            const int index = static_cast<int>(model_.nodes.size());
            model_.nodes.push_back(std::move(node));
            for (unsigned c = current.node->mNumChildren; c-- > 0;) {
                stack.push_back({current.node->mChildren[c], index});
            }
        }
    }

    void setUnitsAndAxes() {
        model_.axesFromFile = kind_ == Kind::Collada || kind_ == Kind::ThreeDs;
        if (kind_ == Kind::Collada) {
            model_.metersPerUnit = 1.0; // Assimp applied the file's <unit>
            model_.unitsFromFile = true;
            return;
        }
        Bounds total;
        bool any = false;
        for (const ImportedMesh& mesh : model_.meshes) {
            const Bounds bounds = meshBounds(mesh.data);
            total.min = any ? glm::min(total.min, bounds.min) : bounds.min;
            total.max = any ? glm::max(total.max, bounds.max) : bounds.max;
            any = true;
        }
        const glm::vec3 size = total.size();
        const float extent = std::max({size.x, size.y, size.z});
        const UnitGuess guess = guessUnits(extent);
        model_.metersPerUnit = guess.metersPerUnit;
        model_.unitsFromFile = false;
        if (any) {
            model_.warnings.push_back(std::format(
                "This file doesn't store units. LightBox assumed {} from the model size ({:.3g} m); "
                "check the units in the Import panel if the model looks too big or too small.",
                guess.name, static_cast<double>(extent) * guess.metersPerUnit));
        }
    }

    std::filesystem::path path_;
    Kind kind_;
    ImportedModel& model_;
    const Job& job_;
    TextureCache textures_;
    const aiScene* scene_ = nullptr;
    std::vector<int> meshIndex_; // Assimp mesh → ImportedModel::meshes, -1 = dropped
};

} // namespace

void importAssimp(const std::filesystem::path& path, ImportedModel& model, const Job& job) {
    const std::string ext = toLowerAscii(pathToUtf8(path.extension()));
    if (ext == ".kmz") {
        std::mt19937_64 random(static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path folder =
            std::filesystem::temp_directory_path() /
            std::format("LightBox-kmz-{:016x}", random());
        std::filesystem::create_directories(folder);
        struct Cleanup {
            std::filesystem::path folder;
            ~Cleanup() {
                std::error_code error;
                std::filesystem::remove_all(folder, error);
            }
        } cleanup{folder};
        // Textures are read into memory during the import, so the folder can go afterwards.
        AssimpReader(unpackKmz(path, folder), Kind::Collada, model, job).read();
        return;
    }
    const Kind kind = ext == ".dae"   ? Kind::Collada
                      : ext == ".3ds" ? Kind::ThreeDs
                      : ext == ".stl" ? Kind::Stl
                                      : Kind::Ply;
    AssimpReader(path, kind, model, job).read();
}

} // namespace lb::import::detail
