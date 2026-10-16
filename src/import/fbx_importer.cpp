// Autodesk FBX (binary and ASCII) via ufbx (compiled separately as lightbox_ufbx).
//
// Axes: ufbx converts to right-handed Z-up with UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS, which puts
// the rotation into the top-level node transforms and leaves vertices and units untouched. Units:
// the file's own (scene settings unit_meters, usually centimeters), applied later on the model root.
// Geometric (pivot) transforms are baked into the geometry by ufbx, or moved to helper nodes when a
// mesh is shared, so meshes stay instanced.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/common.hpp>

#include "import/formats.hpp"
#include "import/mesh_processing.hpp"

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
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <ufbx.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace lb::import::detail {
namespace {

struct SceneDeleter {
    void operator()(ufbx_scene* scene) const { ufbx_free_scene(scene); }
};
using ScenePtr = std::unique_ptr<ufbx_scene, SceneDeleter>;

std::string toString(ufbx_string text) { return {text.data, text.length}; }

glm::vec3 toVec3(const ufbx_vec3& v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

glm::mat4 toMat4(const ufbx_matrix& m) {
    glm::mat4 result(1.0f);
    for (int c = 0; c < 4; ++c) {
        result[c] = glm::vec4(toVec3(m.cols[c]), c == 3 ? 1.0f : 0.0f);
    }
    return result;
}

bool isIdentity(const glm::mat4& m) {
    const glm::mat4 identity(1.0f);
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (std::abs(m[c][r] - identity[c][r]) > 1e-6f) {
                return false;
            }
        }
    }
    return true;
}

// Called from inside ufbx (C code): exceptions must not unwind through it, so a cancellation (or a
// throwing progress callback) is turned into UFBX_PROGRESS_CANCEL and rethrown after loading.
ufbx_progress_result reportProgress(void* user, const ufbx_progress* progress) {
    const auto* job = static_cast<const Job*>(user);
    try {
        if (progress->bytes_total > 0) {
            const double fraction = static_cast<double>(progress->bytes_read) /
                                    static_cast<double>(progress->bytes_total);
            job->report(static_cast<float>(0.4 * fraction), "Reading file");
        } else {
            job->checkCancelled();
        }
    } catch (...) {
        return UFBX_PROGRESS_CANCEL;
    }
    return UFBX_PROGRESS_CONTINUE;
}

const ufbx_texture* mapTexture(const ufbx_material_map& map) {
    return map.texture != nullptr && map.texture_enabled ? map.texture : nullptr;
}

class FbxReader {
public:
    FbxReader(const std::filesystem::path& path, ImportedModel& model, const Job& job)
        : path_(path), model_(model), job_(job), textures_(model, job) {}

    void read();

private:
    void load();
    void convertMaterials();
    int loadTexture(const ufbx_texture* texture);
    ImportedTextureSlot slot(const ufbx_texture* texture, int channel = 0, bool invert = false);
    void convertNodes();
    std::vector<int> meshesFor(const ufbx_node& node);
    std::vector<int> buildMesh(const ufbx_mesh& mesh, const glm::mat4& geometryToNode);
    int materialFor(const ufbx_node& node, const ufbx_mesh& mesh, std::uint32_t slotIndex) const;

    const std::filesystem::path& path_;
    ImportedModel& model_;
    const Job& job_;
    TextureCache textures_;
    ScenePtr scene_;
    std::string fileName_;

    // Imported meshes (one per material part, -1 = part without triangles) of each FBX mesh and
    // per-instance material assignment; the first variant owns freshly built geometry.
    struct Variant {
        std::vector<int> materials;
        std::vector<int> meshes;
    };
    struct MeshKey {
        const ufbx_mesh* mesh;
        const ufbx_node* bakedFor; // only set when a geometric transform had to be baked per node
        bool operator==(const MeshKey&) const = default;
    };
    struct MeshKeyHash {
        std::size_t operator()(const MeshKey& k) const noexcept {
            return std::hash<const void*>{}(k.mesh) * 31u ^ std::hash<const void*>{}(k.bakedFor);
        }
    };
    std::unordered_map<MeshKey, std::vector<Variant>, MeshKeyHash> meshes_;
};

void FbxReader::read() {
    fileName_ = displayName(path_);
    load();
    job_.report(0.4f, "Loading textures");
    convertMaterials();
    job_.report(0.6f, "Reading geometry");
    convertNodes();

    const double unit = scene_->settings.unit_meters;
    model_.metersPerUnit = std::isfinite(unit) && unit > 0.0 ? unit : 0.01; // FBX default: cm
    model_.unitsFromFile = true;
}

void FbxReader::load() {
    ufbx_load_opts options{};
    options.target_axes = ufbx_axes_right_handed_z_up;
    options.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
    options.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    options.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
    options.ignore_animation = true;
    options.progress_cb.fn = &reportProgress;
    options.progress_cb.user = const_cast<Job*>(&job_); // only read back as const
    options.progress_interval_hint = 1u << 20;

    const std::string pathUtf8 = pathToUtf8(path_); // ufbx opens UTF-8 paths with _wfopen on Windows
    ufbx_error error{};
    scene_.reset(ufbx_load_file(pathUtf8.c_str(), &options, &error));
    if (scene_) {
        return;
    }
    switch (error.type) {
    case UFBX_ERROR_CANCELLED:
        throw ImportCancelled();
    case UFBX_ERROR_FILE_NOT_FOUND:
        throw ImportError(std::format("{} could not be opened.", fileName_));
    case UFBX_ERROR_OUT_OF_MEMORY:
    case UFBX_ERROR_MEMORY_LIMIT:
    case UFBX_ERROR_ALLOCATION_LIMIT:
        throw ImportError(std::format("Not enough memory to import {}.", fileName_));
    case UFBX_ERROR_UNRECOGNIZED_FILE_FORMAT:
        throw ImportError(std::format("{} is not a valid FBX file.", fileName_));
    default:
        break;
    }
    const std::string description = toString(error.description);
    throw ImportError(std::format("{} could not be read as FBX: {}.", fileName_,
                                  description.empty() ? "the file is damaged" : description));
}

int FbxReader::loadTexture(const ufbx_texture* texture) {
    if (texture == nullptr) {
        return -1;
    }
    if (texture->type != UFBX_TEXTURE_FILE) {
        if (texture->file_textures.count == 0) {
            warnOnce(model_, std::format("Procedural textures in {} are not supported and were "
                                         "skipped.",
                                         fileName_));
            return -1;
        }
        if (texture->file_textures.count > 1) {
            warnOnce(model_, std::format("Layered textures in {} use only their first layer.",
                                         fileName_));
        }
        texture = texture->file_textures.data[0];
    }
    const std::string fileName = toString(texture->filename);
    std::string name = displayName(pathFromUtf8(fileName));
    if (name.empty()) {
        name = toString(texture->name);
    }
    if (texture->content.size > 0) {
        const std::string key =
            texture->has_file ? std::format("fbx-file:{}", texture->file_index)
                              : std::format("fbx-texture:{}", texture->element_id);
        const auto* bytes = static_cast<const std::uint8_t*>(texture->content.data);
        return textures_.fromMemory(key, std::span<const std::uint8_t>(bytes, texture->content.size),
                                    name.empty() ? "embedded texture" : name);
    }
    const std::filesystem::path directory = path_.parent_path();
    for (const ufbx_string& reference :
         {texture->filename, texture->absolute_filename, texture->relative_filename}) {
        const std::filesystem::path resolved = resolveTexturePath(toString(reference), directory);
        if (!resolved.empty()) {
            return textures_.fromFile(resolved);
        }
    }
    if (fileName.empty() && texture->relative_filename.length == 0) {
        return -1;
    }
    return textures_.fail("missing:" + fileName,
                          std::format("Texture {} was not found next to the model; the material "
                                      "is shown without it.",
                                      name));
}

ImportedTextureSlot FbxReader::slot(const ufbx_texture* texture, int channel, bool invert) {
    ImportedTextureSlot result;
    result.channel = channel;
    result.invert = invert;
    result.texture = loadTexture(texture);
    return result;
}

void FbxReader::convertMaterials() {
    const ufbx_material_list& materials = scene_->materials;
    model_.materials.reserve(materials.count);
    for (std::size_t i = 0; i < materials.count; ++i) {
        job_.report(0.4f + 0.2f * static_cast<float>(i) / static_cast<float>(materials.count),
                    "Loading textures");
        const ufbx_material& source = *materials.data[i];
        ImportedMaterial imported;
        imported.name = toString(source.name);
        if (imported.name.empty()) {
            imported.name = std::format("Material {}", i + 1);
        }
        Material& material = imported.material;
        const ufbx_material_pbr_maps& pbr = source.pbr;
        const ufbx_material_fbx_maps& fbx = source.fbx;

        // FBX colors are stored linear (what Maya/3ds Max/Blender write and read back).
        const glm::vec3 color = toVec3(pbr.base_color.has_value ? pbr.base_color.value_vec3
                                                                : fbx.diffuse_color.value_vec3);
        const float factor =
            pbr.base_factor.has_value ? static_cast<float>(pbr.base_factor.value_real) : 1.0f;
        material.baseColor = glm::clamp(color * factor, glm::vec3(0.0f), glm::vec3(1.0f));
        const ufbx_texture* colorMap = mapTexture(pbr.base_color);
        imported.baseColor = slot(colorMap != nullptr ? colorMap : mapTexture(fbx.diffuse_color));
        if (imported.baseColor.texture >= 0) {
            // A connected texture replaces the color in DCC tools; keep only the factor.
            material.baseColor = glm::vec3(std::clamp(factor, 0.0f, 1.0f));
        }

        if (pbr.roughness.has_value) {
            material.roughness =
                std::clamp(static_cast<float>(pbr.roughness.value_real), 0.0f, 1.0f);
        } else if (pbr.glossiness.has_value) {
            material.roughness =
                std::clamp(1.0f - static_cast<float>(pbr.glossiness.value_real), 0.0f, 1.0f);
        }
        if (const ufbx_texture* map = mapTexture(pbr.roughness)) {
            imported.roughness = slot(map);
        } else if (const ufbx_texture* gloss = mapTexture(pbr.glossiness)) {
            imported.roughness = slot(gloss, 0, true);
        }
        if (imported.roughness.texture >= 0) {
            material.roughness = 1.0f;
        }

        material.metalness =
            pbr.metalness.has_value
                ? std::clamp(static_cast<float>(pbr.metalness.value_real), 0.0f, 1.0f)
                : 0.0f;
        imported.metalness = slot(mapTexture(pbr.metalness));
        if (imported.metalness.texture >= 0) {
            material.metalness = 1.0f;
        }

        const ufbx_texture* normalMap = mapTexture(pbr.normal_map);
        if (normalMap == nullptr) {
            normalMap = mapTexture(fbx.normal_map);
        }
        if (normalMap == nullptr) {
            if (const ufbx_texture* bump = mapTexture(fbx.bump)) {
                const MapKind kind = classifyTextureFile(pathFromUtf8(toString(bump->filename)));
                if (kind == MapKind::Normal || kind == MapKind::NormalDirectX) {
                    normalMap = bump; // many exporters put normal maps in the bump slot
                    material.normalFlipY = kind == MapKind::NormalDirectX;
                } else {
                    warnOnce(model_, std::format("Bump (height) maps are not supported yet; the "
                                                 "bump map of material {} was skipped.",
                                                 imported.name));
                }
            }
        }
        imported.normal = slot(normalMap);
        imported.occlusion = slot(mapTexture(pbr.ambient_occlusion));
        model_.materials.push_back(std::move(imported));
    }
}

int FbxReader::materialFor(const ufbx_node& node, const ufbx_mesh& mesh,
                           std::uint32_t slotIndex) const {
    const ufbx_material* material = nullptr;
    if (slotIndex < node.materials.count) {
        material = node.materials.data[slotIndex];
    } else if (slotIndex < mesh.materials.count) {
        material = mesh.materials.data[slotIndex];
    }
    return material != nullptr ? static_cast<int>(material->typed_id) : -1;
}

std::vector<int> FbxReader::buildMesh(const ufbx_mesh& mesh, const glm::mat4& geometryToNode) {
    std::vector<int> result;
    std::vector<std::uint32_t> triangle(mesh.max_face_triangles * 3);
    const bool hasNormals = mesh.vertex_normal.exists;
    const bool hasUvs = mesh.vertex_uv.exists;
    const std::string name = toString(mesh.name);

    // Faces per material slot. Every FBX mesh has at least one part; guard anyway.
    struct Part {
        std::uint32_t slot;
        const std::uint32_t* faces;
        std::size_t count;
    };
    std::vector<Part> parts;
    std::vector<std::uint32_t> allFaces;
    if (mesh.material_parts.count > 0) {
        for (std::size_t p = 0; p < mesh.material_parts.count; ++p) {
            const ufbx_mesh_part& part = mesh.material_parts.data[p];
            parts.push_back({part.index, part.face_indices.data, part.face_indices.count});
        }
    } else {
        allFaces.resize(mesh.faces.count);
        for (std::size_t f = 0; f < allFaces.size(); ++f) {
            allFaces[f] = static_cast<std::uint32_t>(f);
        }
        parts.push_back({0, allFaces.data(), allFaces.size()});
    }

    struct Corner {
        std::uint32_t position, normal, uv;
        bool operator==(const Corner&) const = default;
    };
    struct CornerHash {
        std::size_t operator()(const Corner& c) const noexcept {
            std::size_t h = c.position;
            h = h * 0x9E3779B97F4A7C15ull ^ c.normal;
            h = h * 0x9E3779B97F4A7C15ull ^ c.uv;
            return h ^ (h >> 29);
        }
    };

    for (const Part& part : parts) {
        job_.checkCancelled();
        MeshData data;
        std::unordered_map<Corner, std::uint32_t, CornerHash> corners;
        for (std::size_t f = 0; f < part.count; ++f) {
            if (part.faces[f] >= mesh.faces.count) {
                continue;
            }
            const ufbx_face face = mesh.faces.data[part.faces[f]];
            if (face.num_indices < 3) {
                continue; // points and lines
            }
            const std::uint32_t triangles =
                ufbx_triangulate_face(triangle.data(), triangle.size(), &mesh, face);
            for (std::uint32_t t = 0; t < triangles * 3; ++t) {
                const std::uint32_t index = triangle[t];
                Corner corner{mesh.vertex_position.indices.data[index],
                              hasNormals ? mesh.vertex_normal.indices.data[index] : 0u,
                              hasUvs ? mesh.vertex_uv.indices.data[index] : 0u};
                const auto [it, inserted] =
                    corners.try_emplace(corner, static_cast<std::uint32_t>(data.positions.size()));
                if (inserted) {
                    data.positions.push_back(
                        corner.position < mesh.vertex_position.values.count
                            ? toVec3(mesh.vertex_position.values.data[corner.position])
                            : glm::vec3(std::nanf("")));
                    if (hasNormals) {
                        data.normals.push_back(
                            corner.normal < mesh.vertex_normal.values.count
                                ? toVec3(mesh.vertex_normal.values.data[corner.normal])
                                : glm::vec3(0.0f));
                    }
                    if (hasUvs) {
                        glm::vec2 uv(0.0f);
                        if (corner.uv < mesh.vertex_uv.values.count) {
                            const ufbx_vec2& value = mesh.vertex_uv.values.data[corner.uv];
                            uv = {static_cast<float>(value.x), 1.0f - static_cast<float>(value.y)};
                        }
                        data.uvs.push_back(uv);
                    }
                }
                data.indices.push_back(it->second);
            }
        }
        if (!isIdentity(geometryToNode)) {
            transformMesh(data, geometryToNode);
        }
        if (!finalizeMesh(data)) {
            result.push_back(-1);
            continue;
        }
        result.push_back(static_cast<int>(model_.meshes.size()));
        model_.meshes.push_back({name, std::move(data), -1});
    }
    return result;
}

std::vector<int> FbxReader::meshesFor(const ufbx_node& node) {
    const ufbx_mesh& mesh = *node.mesh;
    if (mesh.skin_deformers.count > 0 || mesh.blend_deformers.count > 0) {
        warnOnce(model_, std::format("Rigged or morphing objects in {} are imported in their rest "
                                     "pose.",
                                     fileName_));
    }
    const glm::mat4 geometryToNode = toMat4(node.geometry_to_node);
    const MeshKey key{&mesh, isIdentity(geometryToNode) ? nullptr : &node};

    std::vector<std::uint32_t> slots;
    if (mesh.material_parts.count > 0) {
        for (std::size_t p = 0; p < mesh.material_parts.count; ++p) {
            slots.push_back(mesh.material_parts.data[p].index);
        }
    } else {
        slots.push_back(0);
    }
    std::vector<int> materials;
    for (const std::uint32_t slotIndex : slots) {
        materials.push_back(materialFor(node, mesh, slotIndex));
    }

    std::vector<Variant>& variants = meshes_[key];
    for (const Variant& variant : variants) {
        if (variant.materials == materials) {
            return variant.meshes;
        }
    }
    Variant variant;
    variant.materials = materials;
    if (variants.empty()) {
        variant.meshes = buildMesh(mesh, geometryToNode);
    } else {
        // Same geometry with other materials on this instance: copy the first variant's data.
        for (const int source : variants.front().meshes) {
            if (source < 0) {
                variant.meshes.push_back(-1);
                continue;
            }
            variant.meshes.push_back(static_cast<int>(model_.meshes.size()));
            ImportedMesh copy = model_.meshes[static_cast<std::size_t>(source)];
            model_.meshes.push_back(std::move(copy));
        }
    }
    for (std::size_t p = 0; p < variant.meshes.size() && p < materials.size(); ++p) {
        if (variant.meshes[p] >= 0) {
            model_.meshes[static_cast<std::size_t>(variant.meshes[p])].material = materials[p];
        }
    }
    variants.push_back(std::move(variant));
    return variants.back().meshes;
}

void FbxReader::convertNodes() {
    struct Pending {
        const ufbx_node* node;
        int parent;
    };
    std::vector<Pending> stack;
    const ufbx_node* root = scene_->root_node;
    if (root == nullptr) {
        return;
    }
    for (std::size_t c = root->children.count; c > 0; --c) {
        stack.push_back({root->children.data[c - 1], -1});
    }
    const std::size_t total = std::max<std::size_t>(scene_->nodes.count, 1);
    std::size_t done = 0;
    while (!stack.empty()) {
        const Pending pending = stack.back();
        stack.pop_back();
        const ufbx_node& node = *pending.node;
        job_.report(0.6f + 0.4f * static_cast<float>(done++) / static_cast<float>(total),
                    "Reading geometry");

        ImportedNode imported;
        imported.name = toString(node.name);
        if (imported.name.empty() && node.mesh != nullptr) {
            imported.name = toString(node.mesh->name);
        }
        if (imported.name.empty()) {
            imported.name = node.mesh != nullptr ? std::format("Object {}", node.typed_id + 1)
                                                 : std::format("Group {}", node.typed_id + 1);
        }
        imported.parent = pending.parent;
        imported.transform = toMat4(node.node_to_parent);
        if (node.mesh != nullptr) {
            for (const int mesh : meshesFor(node)) {
                if (mesh >= 0) {
                    imported.meshes.push_back(mesh);
                }
            }
        }
        const int self = static_cast<int>(model_.nodes.size());
        model_.nodes.push_back(std::move(imported));
        for (std::size_t c = node.children.count; c > 0; --c) {
            stack.push_back({node.children.data[c - 1], self});
        }
    }
}

} // namespace

void importFbx(const std::filesystem::path& path, ImportedModel& model, const Job& job) {
    FbxReader(path, model, job).read();
}

} // namespace lb::import::detail
