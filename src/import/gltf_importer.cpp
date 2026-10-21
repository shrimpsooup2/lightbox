// glTF 2.0 (.gltf with external or data-URI buffers, .glb) via cgltf.
//
// Axes: glTF is Y-up, LightBox is Z-up. Vertices are rotated into Z-up (x, y, z) → (x, −z, y), and
// every node matrix M is conjugated to C·M·C⁻¹ (C = that rotation), so node translations and
// rotations are expressed in Z-up too and the hierarchy composes exactly as in the file. Units are
// meters by specification.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/gtc/type_ptr.hpp>
#include <glm/matrix.hpp>
#include <glm/trigonometric.hpp>

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
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#pragma GCC diagnostic ignored "-Wcast-align"
#endif
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace lb::import::detail {
namespace {

// cgltf opens files with fopen(), which can't open non-ASCII paths on Windows. These callbacks get
// the UTF-8 path cgltf built and open it through std::filesystem (wide API on Windows).
cgltf_result readFile(const cgltf_memory_options*, const cgltf_file_options*, const char* path,
                      cgltf_size* size, void** data) {
    std::ifstream file;
    try {
        file.open(pathFromUtf8(path), std::ios::binary | std::ios::ate);
    } catch (const std::exception&) {
        return cgltf_result_out_of_memory; // must not throw through cgltf
    }
    if (!file) {
        return cgltf_result_file_not_found;
    }
    const std::streamoff length = file.tellg();
    if (length < 0) {
        return cgltf_result_io_error;
    }
    const auto bytes = static_cast<std::size_t>(length);
    if (size != nullptr && *size != 0 && *size > bytes) {
        return cgltf_result_data_too_short;
    }
    void* buffer = std::malloc(bytes == 0 ? 1 : bytes);
    if (buffer == nullptr) {
        return cgltf_result_out_of_memory;
    }
    file.seekg(0);
    if (!file.read(static_cast<char*>(buffer), static_cast<std::streamsize>(length))) {
        std::free(buffer);
        return cgltf_result_io_error;
    }
    if (size != nullptr) {
        *size = bytes;
    }
    *data = buffer;
    return cgltf_result_success;
}

void releaseFile(const cgltf_memory_options*, const cgltf_file_options*, void* data) {
    std::free(data);
}

struct DataDeleter {
    void operator()(cgltf_data* data) const { cgltf_free(data); }
};
using DataPtr = std::unique_ptr<cgltf_data, DataDeleter>;

std::string resultText(cgltf_result result) {
    switch (result) {
    case cgltf_result_data_too_short:
        return "the file is truncated";
    case cgltf_result_unknown_format:
        return "it is not a glTF file, or it references data LightBox can't load";
    case cgltf_result_invalid_json:
        return "the JSON is invalid";
    case cgltf_result_invalid_gltf:
        return "the glTF structure is invalid";
    case cgltf_result_file_not_found:
        return "a referenced file was not found";
    case cgltf_result_io_error:
        return "a file could not be read";
    case cgltf_result_out_of_memory:
        return "not enough memory";
    case cgltf_result_legacy_gltf:
        return "it is a glTF 1.0 file (only glTF 2.0 is supported)";
    default:
        return "unknown error";
    }
}

glm::vec3 vec3From(const float* v) { return {v[0], v[1], v[2]}; }

std::vector<float> readFloats(const cgltf_accessor* accessor, cgltf_size components) {
    if (accessor == nullptr || cgltf_num_components(accessor->type) != components) {
        return {};
    }
    std::vector<float> values(accessor->count * components);
    if (cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) != values.size()) {
        return {};
    }
    return values;
}

const cgltf_accessor* findAttribute(const cgltf_primitive& primitive, cgltf_attribute_type type) {
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
        if (primitive.attributes[i].type == type && primitive.attributes[i].index == 0) {
            return primitive.attributes[i].data;
        }
    }
    return nullptr;
}

std::string nameOr(const char* name, std::string fallback) {
    return name != nullptr && name[0] != '\0' ? std::string(name) : std::move(fallback);
}

class GltfReader {
public:
    GltfReader(const std::filesystem::path& path, ImportedModel& model, const Job& job)
        : path_(path), model_(model), job_(job), textures_(model, job) {}

    void read();

private:
    void load();
    void convertMeshes();
    std::vector<int> convertMesh(const cgltf_mesh& mesh);
    bool convertPrimitive(const cgltf_mesh& mesh, const cgltf_primitive& primitive, MeshData& out);
    void convertMaterials();
    ImportedTextureSlot textureSlot(const cgltf_texture_view& view, int channel);
    int loadImage(const cgltf_image& image);
    void convertNodes();

    const std::filesystem::path& path_;
    ImportedModel& model_;
    const Job& job_;
    TextureCache textures_;
    DataPtr data_;
    std::string fileName_;
    // Imported meshes per glTF mesh, filled on first use so instances share them.
    std::unordered_map<const cgltf_mesh*, std::vector<int>> meshes_;
};

void GltfReader::read() {
    fileName_ = displayName(path_);
    load();
    job_.report(0.15f, "Reading geometry");
    convertMeshes();
    job_.report(0.6f, "Loading textures");
    convertMaterials();
    job_.report(0.95f, "Building hierarchy");
    convertNodes();
    model_.metersPerUnit = 1.0; // glTF is always in meters
    model_.unitsFromFile = true;
}

void GltfReader::load() {
    cgltf_options options{};
    options.file.read = &readFile;
    options.file.release = &releaseFile;
    const std::string pathUtf8 = pathToUtf8(path_);

    cgltf_data* raw = nullptr;
    cgltf_result result = cgltf_parse_file(&options, pathUtf8.c_str(), &raw);
    data_.reset(raw);
    if (result != cgltf_result_success) {
        throw ImportError(
            std::format("{} could not be read as glTF: {}.", fileName_, resultText(result)));
    }
    for (cgltf_size i = 0; i < data_->extensions_required_count; ++i) {
        const std::string_view extension = data_->extensions_required[i];
        if (extension == "KHR_draco_mesh_compression" || extension == "EXT_meshopt_compression" ||
            extension == "KHR_mesh_quantization") {
            throw ImportError(std::format("{} uses {} compressed geometry, which LightBox can't "
                                          "read yet. Export it again without compression.",
                                          fileName_, extension));
        }
    }
    job_.report(0.05f, "Loading buffers");
    result = cgltf_load_buffers(&options, data_.get(), pathUtf8.c_str());
    if (result != cgltf_result_success) {
        throw ImportError(std::format("{} could not be loaded: {} (are its .bin files next to "
                                      "it?).",
                                      fileName_, resultText(result)));
    }
    result = cgltf_validate(data_.get());
    if (result != cgltf_result_success) {
        throw ImportError(std::format("{} is damaged: {}.", fileName_, resultText(result)));
    }
}

void GltfReader::convertMeshes() {
    const cgltf_size count = data_->meshes_count;
    for (cgltf_size i = 0; i < count; ++i) {
        job_.report(0.15f + 0.45f * static_cast<float>(i) / static_cast<float>(count),
                    "Reading geometry");
        meshes_[&data_->meshes[i]] = convertMesh(data_->meshes[i]);
    }
}

std::vector<int> GltfReader::convertMesh(const cgltf_mesh& mesh) {
    std::vector<int> result;
    const std::string meshName =
        nameOr(mesh.name, std::format("Mesh {}", cgltf_mesh_index(data_.get(), &mesh) + 1));
    for (cgltf_size p = 0; p < mesh.primitives_count; ++p) {
        job_.checkCancelled();
        const cgltf_primitive& primitive = mesh.primitives[p];
        ImportedMesh imported;
        imported.name = meshName;
        if (!convertPrimitive(mesh, primitive, imported.data)) {
            continue;
        }
        imported.material = primitive.material != nullptr
                                 ? static_cast<int>(cgltf_material_index(data_.get(),
                                                                         primitive.material))
                                 : -1;
        result.push_back(static_cast<int>(model_.meshes.size()));
        model_.meshes.push_back(std::move(imported));
    }
    return result;
}

bool GltfReader::convertPrimitive(const cgltf_mesh& mesh, const cgltf_primitive& primitive,
                                  MeshData& out) {
    const std::string meshName = nameOr(mesh.name, "a mesh");
    switch (primitive.type) {
    case cgltf_primitive_type_triangles:
    case cgltf_primitive_type_triangle_strip:
    case cgltf_primitive_type_triangle_fan:
        break;
    default:
        warnOnce(model_, std::format("{} contains points or lines; they were skipped because "
                                     "LightBox shows surfaces only.",
                                     fileName_));
        return false;
    }
    if (primitive.has_draco_mesh_compression) {
        warnOnce(model_, std::format("Some geometry in {} is Draco-compressed and was skipped.",
                                     fileName_));
        return false;
    }

    const std::vector<float> positions =
        readFloats(findAttribute(primitive, cgltf_attribute_type_position), 3);
    if (positions.empty()) {
        warnOnce(model_, std::format("{} in {} has no readable vertex positions and was skipped.",
                                     meshName, fileName_));
        return false;
    }
    const std::size_t vertexCount = positions.size() / 3;
    out.positions.resize(vertexCount);
    for (std::size_t v = 0; v < vertexCount; ++v) {
        out.positions[v] = yUpToZUp(vec3From(&positions[v * 3]));
    }
    const std::vector<float> normals =
        readFloats(findAttribute(primitive, cgltf_attribute_type_normal), 3);
    if (normals.size() == positions.size()) {
        out.normals.resize(vertexCount);
        for (std::size_t v = 0; v < vertexCount; ++v) {
            out.normals[v] = yUpToZUp(vec3From(&normals[v * 3]));
        }
    }
    const std::vector<float> uvs =
        readFloats(findAttribute(primitive, cgltf_attribute_type_texcoord), 2);
    if (uvs.size() == vertexCount * 2) {
        out.uvs.resize(vertexCount);
        for (std::size_t v = 0; v < vertexCount; ++v) {
            out.uvs[v] = {uvs[v * 2], uvs[v * 2 + 1]}; // glTF is already top-left
        }
    }
    if (mesh.weights_count > 0 || primitive.targets_count > 0) {
        warnOnce(model_, std::format("Morph targets in {} are not supported; shapes are imported "
                                     "in their base pose.",
                                     fileName_));
    }

    // Corner list, then triangles from it.
    std::vector<std::uint32_t> corners;
    if (primitive.indices != nullptr) {
        corners.resize(primitive.indices->count);
        for (cgltf_size i = 0; i < primitive.indices->count; ++i) {
            const cgltf_size index = cgltf_accessor_read_index(primitive.indices, i);
            corners[i] = index < vertexCount ? static_cast<std::uint32_t>(index)
                                             : std::numeric_limits<std::uint32_t>::max();
        }
    } else {
        corners.resize(vertexCount);
        for (std::size_t i = 0; i < vertexCount; ++i) {
            corners[i] = static_cast<std::uint32_t>(i);
        }
    }
    switch (primitive.type) {
    case cgltf_primitive_type_triangle_strip:
        for (std::size_t i = 0; i + 2 < corners.size(); ++i) {
            // Every other strip triangle is mirrored; swapping keeps the winding consistent.
            const bool odd = i % 2 != 0;
            out.indices.insert(out.indices.end(), {corners[i], corners[odd ? i + 2 : i + 1],
                                                   corners[odd ? i + 1 : i + 2]});
        }
        break;
    case cgltf_primitive_type_triangle_fan:
        for (std::size_t i = 1; i + 1 < corners.size(); ++i) {
            out.indices.insert(out.indices.end(), {corners[0], corners[i], corners[i + 1]});
        }
        break;
    default:
        out.indices = std::move(corners);
        break;
    }
    if (!finalizeMesh(out)) {
        warnOnce(model_, std::format("{} in {} has no visible triangles and was skipped.",
                                     meshName, fileName_));
        return false;
    }
    return true;
}

int GltfReader::loadImage(const cgltf_image& image) {
    const std::string key = std::format("gltf-image:{}", cgltf_image_index(data_.get(), &image));
    const std::string mime = image.mime_type != nullptr ? toLowerAscii(image.mime_type) : "";
    std::string name = nameOr(image.name, "");

    if (image.uri != nullptr && std::strncmp(image.uri, "data:", 5) == 0) {
        const std::string_view uri = image.uri;
        const std::size_t comma = uri.find(',');
        const std::string_view header = uri.substr(0, comma);
        if (name.empty()) {
            name = std::format("{} image {}", model_.name, cgltf_image_index(data_.get(), &image) + 1);
        }
        if (header.find("ktx2") != std::string_view::npos ||
            header.find("webp") != std::string_view::npos) {
            return textures_.fail(key, std::format("Texture {} is in KTX2/WebP format, which "
                                                   "LightBox can't read yet.",
                                                   name));
        }
        std::vector<std::uint8_t> bytes;
        if (comma == std::string_view::npos || header.find(";base64") == std::string_view::npos ||
            !decodeBase64(uri.substr(comma + 1), bytes)) {
            return textures_.fail(key, std::format("Texture {} in {} is damaged.", name, fileName_));
        }
        return textures_.fromMemory(key, bytes, name);
    }
    if (image.uri != nullptr) {
        const std::string reference = decodePercent(image.uri);
        if (reference.find("://") != std::string::npos) {
            return textures_.fail(key, std::format("Texture {} is on the internet; LightBox only "
                                                   "loads local files.",
                                                   reference));
        }
        return textures_.fromReference(reference, path_.parent_path());
    }
    if (name.empty()) {
        name = std::format("{} image {}", model_.name, cgltf_image_index(data_.get(), &image) + 1);
    }
    if (mime == "image/ktx2" || mime == "image/webp") {
        return textures_.fail(key, std::format("Texture {} is in {} format, which LightBox can't "
                                               "read yet.",
                                               name, mime.substr(6)));
    }
    const cgltf_buffer_view* view = image.buffer_view;
    if (view == nullptr) {
        return textures_.fail(key, std::format("Texture {} in {} has no data.", name, fileName_));
    }
    const auto* base = static_cast<const std::uint8_t*>(view->data);
    if (base == nullptr && view->buffer != nullptr && view->buffer->data != nullptr) {
        base = static_cast<const std::uint8_t*>(view->buffer->data) + view->offset;
    }
    if (base == nullptr) {
        return textures_.fail(key, std::format("Texture {} in {} has no data.", name, fileName_));
    }
    return textures_.fromMemory(key, std::span<const std::uint8_t>(base, view->size), name);
}

ImportedTextureSlot GltfReader::textureSlot(const cgltf_texture_view& view, int channel) {
    ImportedTextureSlot slot;
    slot.channel = channel;
    const cgltf_texture* texture = view.texture;
    if (texture == nullptr) {
        return slot;
    }
    if (view.texcoord != 0 || (view.has_transform && view.transform.has_texcoord &&
                               view.transform.texcoord != 0)) {
        warnOnce(model_, std::format("Some textures in {} use a second UV set; LightBox uses the "
                                     "first one, so they may look misplaced.",
                                     fileName_));
    }
    if (texture->image != nullptr) {
        slot.texture = loadImage(*texture->image);
    } else if (texture->has_basisu || texture->has_webp) {
        const std::string name = nameOr(texture->name, "A texture");
        warnOnce(model_, std::format("{} in {} is in KTX2/WebP format, which LightBox can't read "
                                     "yet.",
                                     name, fileName_));
    }
    return slot;
}

void GltfReader::convertMaterials() {
    const cgltf_size count = data_->materials_count;
    model_.materials.reserve(count);
    for (cgltf_size i = 0; i < count; ++i) {
        job_.report(0.6f + 0.35f * static_cast<float>(i) / static_cast<float>(count),
                    "Loading textures");
        const cgltf_material& source = data_->materials[i];
        ImportedMaterial imported;
        imported.name = nameOr(source.name, std::format("Material {}", i + 1));
        Material& material = imported.material;

        const cgltf_texture_view* colorView = nullptr;
        if (source.has_pbr_metallic_roughness || !source.has_pbr_specular_glossiness) {
            // cgltf fills the spec defaults (white, metallic 1, roughness 1) when the block is absent.
            const cgltf_pbr_metallic_roughness& pbr = source.pbr_metallic_roughness;
            material.baseColor = vec3From(pbr.base_color_factor);
            material.roughness = pbr.roughness_factor;
            material.metalness = pbr.metallic_factor;
            colorView = &pbr.base_color_texture;
            imported.baseColor = textureSlot(pbr.base_color_texture, 0);
            imported.roughness = textureSlot(pbr.metallic_roughness_texture, 1);
            imported.metalness = textureSlot(pbr.metallic_roughness_texture, 2);
        } else {
            // KHR_materials_pbrSpecularGlossiness (older exporters): diffuse + inverted glossiness.
            const cgltf_pbr_specular_glossiness& sg = source.pbr_specular_glossiness;
            material.baseColor = vec3From(sg.diffuse_factor);
            material.roughness = 1.0f - sg.glossiness_factor;
            material.metalness = 0.0f;
            colorView = &sg.diffuse_texture;
            imported.baseColor = textureSlot(sg.diffuse_texture, 0);
        }
        imported.normal = textureSlot(source.normal_texture, 0);
        if (imported.normal.texture >= 0) {
            material.normalStrength = source.normal_texture.scale;
        }
        imported.occlusion = textureSlot(source.occlusion_texture, 0);

        if (colorView != nullptr && colorView->texture != nullptr && colorView->has_transform) {
            // KHR_texture_transform of the base color map stands for the whole material.
            const cgltf_texture_transform& transform = colorView->transform;
            material.textureScale = {transform.scale[0], transform.scale[1]};
            material.textureOffset = {transform.offset[0], transform.offset[1]};
            material.textureRotationDegrees = glm::degrees(transform.rotation);
        }
        // See-through materials become glass: KHR_materials_transmission as authored, and
        // alpha-blended materials (how most exporters write windows) by their opacity.
        const float alpha = source.has_pbr_metallic_roughness || !source.has_pbr_specular_glossiness
                                ? source.pbr_metallic_roughness.base_color_factor[3]
                                : source.pbr_specular_glossiness.diffuse_factor[3];
        if (source.has_transmission) {
            material.transmission = std::clamp(source.transmission.transmission_factor, 0.0f, 1.0f);
            material.thinWalled = !source.has_volume || source.volume.thickness_factor <= 0.0f;
        } else if (source.alpha_mode == cgltf_alpha_mode_blend && alpha < 0.95f) {
            // Glossy and see-through: window glass. Otherwise translucent (curtains, frosted
            // panels): part of the light passes.
            material.transmission = material.roughness < 0.3f ? 1.0f : 1.0f - alpha;
            material.thinWalled = true;
        }
        if (source.has_ior) {
            material.ior = std::clamp(source.ior.ior, 1.0f, 3.0f);
        }
        if (source.alpha_mode == cgltf_alpha_mode_mask) {
            warnOnce(model_, std::format("Cut-out transparency (leaves, fences) in {} is not "
                                         "supported yet; those surfaces are shown solid.",
                                         fileName_));
        }
        model_.materials.push_back(std::move(imported));
    }
}

void GltfReader::convertNodes() {
    std::vector<const cgltf_node*> roots;
    const cgltf_scene* scene =
        data_->scene != nullptr ? data_->scene
                                : (data_->scenes_count > 0 ? &data_->scenes[0] : nullptr);
    if (scene != nullptr) {
        roots.assign(scene->nodes, scene->nodes + scene->nodes_count);
    } else {
        for (cgltf_size i = 0; i < data_->nodes_count; ++i) {
            if (data_->nodes[i].parent == nullptr) {
                roots.push_back(&data_->nodes[i]);
            }
        }
    }

    const glm::mat4 toZUp = yUpToZUp();
    const glm::mat4 fromZUp = glm::inverse(toZUp);
    struct Pending {
        const cgltf_node* node;
        int parent;
    };
    std::vector<Pending> stack;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        stack.push_back({*it, -1});
    }
    std::unordered_set<const cgltf_node*> visited;
    bool skinned = false;
    while (!stack.empty()) {
        const Pending pending = stack.back();
        stack.pop_back();
        const cgltf_node& node = *pending.node;
        if (!visited.insert(&node).second) {
            continue; // a node listed twice would otherwise be duplicated
        }
        ImportedNode imported;
        const cgltf_size index = cgltf_node_index(data_.get(), &node);
        imported.name = nameOr(node.name, node.mesh != nullptr
                                              ? nameOr(node.mesh->name, std::format("Object {}",
                                                                                    index + 1))
                                              : std::format("Group {}", index + 1));
        imported.parent = pending.parent;
        float local[16];
        cgltf_node_transform_local(&node, local);
        imported.transform = toZUp * glm::make_mat4(local) * fromZUp;
        if (node.mesh != nullptr) {
            if (const auto it = meshes_.find(node.mesh); it != meshes_.end()) {
                imported.meshes = it->second;
            }
            skinned = skinned || node.skin != nullptr;
        }
        const int self = static_cast<int>(model_.nodes.size());
        model_.nodes.push_back(std::move(imported));
        for (cgltf_size c = node.children_count; c > 0; --c) {
            stack.push_back({node.children[c - 1], self});
        }
    }
    if (skinned) {
        warnOnce(model_, std::format("Skinned (rigged) objects in {} are imported in their rest "
                                     "pose.",
                                     fileName_));
    }
}

} // namespace

void importGltf(const std::filesystem::path& path, ImportedModel& model, const Job& job) {
    GltfReader(path, model, job).read();
}

} // namespace lb::import::detail
