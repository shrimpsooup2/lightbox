// Wavefront OBJ/MTL via tinyobjloader.
//
// OBJ has no axis or unit metadata. Axes: Y-up is assumed (what nearly every exporter writes), so
// vertices are rotated into Z-up. Units: guessed from the largest model extent, see guessUnits().
// Texture V is flipped (OBJ's origin is bottom-left, LightBox's top-left).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
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
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#pragma GCC diagnostic ignored "-Wsuggest-override"
#endif
#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace lb::import::detail {
namespace {

// Opens .mtl files through std::filesystem, so non-ASCII paths work on Windows (tinyobjloader's own
// reader uses narrow fopen-style paths).
class MtlReader : public tinyobj::MaterialReader {
public:
    explicit MtlReader(std::filesystem::path directory) : directory_(std::move(directory)) {}

    bool operator()(const std::string& matId, std::vector<tinyobj::material_t>* materials,
                    std::map<std::string, int>* matMap, std::string* warn,
                    std::string* err) override {
        std::string reference = matId;
        std::replace(reference.begin(), reference.end(), '\\', '/');
        std::filesystem::path file = directory_ / pathFromUtf8(reference);
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error)) {
            // Exporters sometimes write absolute paths from the author's machine.
            file = directory_ / pathFromUtf8(reference).filename();
        }
        std::ifstream stream(file);
        if (!stream) {
            missing.push_back(displayName(pathFromUtf8(reference)));
            return false;
        }
        tinyobj::LoadMtl(matMap, materials, &stream, warn, err);
        return true;
    }

    std::vector<std::string> missing;

private:
    std::filesystem::path directory_;
};

struct CornerKey {
    int position, normal, uv;
    bool operator==(const CornerKey&) const = default;
};

struct CornerKeyHash {
    std::size_t operator()(const CornerKey& k) const noexcept {
        std::size_t h = static_cast<std::uint32_t>(k.position);
        h = h * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint32_t>(k.normal);
        h = h * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint32_t>(k.uv);
        return h ^ (h >> 29);
    }
};

// OBJ stores no units. Architecture exports are usually meters, centimeters or millimeters; a
// building or room in meters rarely exceeds 500 m, in centimeters 5000 units (50 m) — so the largest
// extent picks the unit: > 5000 → millimeters, > 500 → centimeters, otherwise meters.
struct UnitGuess {
    double metersPerUnit;
    std::string_view name;
};

UnitGuess guessUnits(float largestExtent) {
    if (largestExtent > 5000.0f) {
        return {0.001, "millimeters"};
    }
    if (largestExtent > 500.0f) {
        return {0.01, "centimeters"};
    }
    return {1.0, "meters"};
}

class ObjReader {
public:
    ObjReader(const std::filesystem::path& path, ImportedModel& model, const Job& job)
        : path_(path), model_(model), job_(job), textures_(model, job) {}

    void read();

private:
    void convertMaterials();
    void convertShapes();
    void guessModelUnits();
    ImportedTextureSlot slotFor(const std::string& reference, int channel = 0);

    const std::filesystem::path& path_;
    ImportedModel& model_;
    const Job& job_;
    TextureCache textures_;
    std::string fileName_;
    tinyobj::attrib_t attrib_;
    std::vector<tinyobj::shape_t> shapes_;
    std::vector<tinyobj::material_t> materials_;
};

void ObjReader::read() {
    fileName_ = displayName(path_);
    std::ifstream stream(path_);
    if (!stream) {
        throw ImportError(std::format("{} could not be opened.", fileName_));
    }
    MtlReader mtlReader(path_.parent_path());
    std::string warn;
    std::string err;
    const bool ok = tinyobj::LoadObj(&attrib_, &shapes_, &materials_, &warn, &err, &stream,
                                     &mtlReader, true, false);
    if (!ok) {
        const std::string detail = err.substr(0, err.find('\n'));
        throw ImportError(std::format("{} could not be read as OBJ{}{}.", fileName_,
                                      detail.empty() ? "" : ": ", detail));
    }
    for (const std::string& missing : mtlReader.missing) {
        warnOnce(model_, std::format("Material library {} was not found next to {}; default "
                                     "materials are used.",
                                     missing, fileName_));
    }
    job_.report(0.4f, "Loading textures");
    convertMaterials();
    job_.report(0.7f, "Reading geometry");
    convertShapes();
    guessModelUnits();
}

ImportedTextureSlot ObjReader::slotFor(const std::string& reference, int channel) {
    ImportedTextureSlot slot;
    slot.channel = channel;
    if (!reference.empty()) {
        slot.texture = textures_.fromReference(reference, path_.parent_path());
    }
    return slot;
}

void ObjReader::convertMaterials() {
    model_.materials.reserve(materials_.size());
    for (std::size_t i = 0; i < materials_.size(); ++i) {
        job_.report(0.4f + 0.3f * static_cast<float>(i) / static_cast<float>(materials_.size()),
                    "Loading textures");
        const tinyobj::material_t& source = materials_[i];
        ImportedMaterial imported;
        imported.name = source.name.empty() ? std::format("Material {}", i + 1) : source.name;
        Material& material = imported.material;

        // Kd is a display color picked in sRGB.
        material.baseColor =
            srgbToLinear({source.diffuse[0], source.diffuse[1], source.diffuse[2]});
        imported.baseColor = slotFor(source.diffuse_texname);
        if (imported.baseColor.texture >= 0) {
            // With a texture, Kd is the fallback color for viewers without textures (often 0.64 or
            // 0.8 grey), not a tint; multiplying it in would darken every textured material.
            material.baseColor = glm::vec3(1.0f);
            const auto& scale = source.diffuse_texopt.scale;
            if (scale[0] > 0.0f && scale[1] > 0.0f) {
                material.textureScale = {scale[0], scale[1]};
            }
        }

        // PBR extension (Pr/Pm/map_Pr/map_Pm). tinyobjloader reports absent values as 0, so a
        // material counts as PBR when any of them is set.
        const bool pbr = source.roughness > 0.0f || source.metallic > 0.0f ||
                         !source.roughness_texname.empty() || !source.metallic_texname.empty();
        if (pbr) {
            material.roughness = std::clamp(source.roughness, 0.0f, 1.0f);
            material.metalness = std::clamp(source.metallic, 0.0f, 1.0f);
            imported.roughness = slotFor(source.roughness_texname);
            if (imported.roughness.texture >= 0 && source.roughness <= 0.0f) {
                material.roughness = 1.0f; // the map alone defines roughness
            }
            imported.metalness = slotFor(source.metallic_texname);
            if (imported.metalness.texture >= 0 && source.metallic <= 0.0f) {
                material.metalness = 1.0f;
            }
        } else {
            // Phong exponent to roughness (Blinn-Phong ↔ GGX approximation), clamped so very high
            // exponents don't turn into perfect mirrors.
            const float exponent = std::max(source.shininess, 0.0f);
            material.roughness = std::clamp(std::sqrt(2.0f / (exponent + 2.0f)), 0.05f, 1.0f);
            material.metalness = 0.0f;
        }

        // Normal maps: "norm", or a bump map whose file name says it is a normal map.
        if (!source.normal_texname.empty()) {
            imported.normal = slotFor(source.normal_texname);
            if (source.normal_texopt.bump_multiplier > 0.0f) {
                material.normalStrength = source.normal_texopt.bump_multiplier;
            }
        } else if (!source.bump_texname.empty()) {
            const MapKind kind = classifyTextureFile(pathFromUtf8(source.bump_texname));
            if (kind == MapKind::Normal || kind == MapKind::NormalDirectX) {
                imported.normal = slotFor(source.bump_texname);
                material.normalFlipY = kind == MapKind::NormalDirectX;
                if (source.bump_texopt.bump_multiplier > 0.0f) {
                    material.normalStrength = source.bump_texopt.bump_multiplier;
                }
            } else {
                warnOnce(model_, std::format("Bump (height) maps are not supported yet; the bump "
                                             "map of material {} was skipped.",
                                             imported.name));
            }
        }
        model_.materials.push_back(std::move(imported));
    }
}

void ObjReader::convertShapes() {
    const auto vertexCount = static_cast<int>(attrib_.vertices.size() / 3);
    const auto normalCount = static_cast<int>(attrib_.normals.size() / 3);
    const auto uvCount = static_cast<int>(attrib_.texcoords.size() / 2);
    bool skippedLines = false;

    for (std::size_t s = 0; s < shapes_.size(); ++s) {
        job_.report(0.7f + 0.3f * static_cast<float>(s) / static_cast<float>(shapes_.size()),
                    "Reading geometry");
        const tinyobj::shape_t& shape = shapes_[s];
        skippedLines = skippedLines || !shape.lines.indices.empty() ||
                       !shape.points.indices.empty();

        // Split the shape per material, in order of first use.
        struct Part {
            MeshData data;
            std::unordered_map<CornerKey, std::uint32_t, CornerKeyHash> corners;
            bool allNormals = true;
            bool anyUvs = false;
        };
        std::vector<int> partMaterial;
        std::vector<Part> parts;

        const tinyobj::mesh_t& mesh = shape.mesh;
        std::size_t offset = 0;
        for (std::size_t f = 0; f < mesh.num_face_vertices.size(); ++f) {
            const std::size_t count = mesh.num_face_vertices[f];
            const std::size_t first = offset;
            offset += count;
            if (count != 3) {
                continue; // triangulated by the loader; anything else is a point/line "face"
            }
            int material = f < mesh.material_ids.size() ? mesh.material_ids[f] : -1;
            if (material < 0 || material >= static_cast<int>(model_.materials.size())) {
                material = -1;
            }
            const auto found = std::find(partMaterial.begin(), partMaterial.end(), material);
            const std::size_t partIndex = static_cast<std::size_t>(found - partMaterial.begin());
            if (found == partMaterial.end()) {
                partMaterial.push_back(material);
                parts.emplace_back();
            }
            Part& part = parts[partIndex];
            for (std::size_t c = first; c < first + 3; ++c) {
                const tinyobj::index_t& index = mesh.indices[c];
                CornerKey key{index.vertex_index, index.normal_index, index.texcoord_index};
                if (key.position < 0 || key.position >= vertexCount) {
                    key.position = -1;
                }
                if (key.normal >= normalCount) {
                    key.normal = -1;
                }
                if (key.uv >= uvCount) {
                    key.uv = -1;
                }
                const auto [it, inserted] = part.corners.try_emplace(
                    key, static_cast<std::uint32_t>(part.data.positions.size()));
                if (inserted) {
                    if (key.position < 0) {
                        // Invalid reference: a NaN vertex makes the triangle degenerate (removed).
                        part.data.positions.push_back(glm::vec3(std::nanf("")));
                    } else {
                        const auto p = static_cast<std::size_t>(key.position) * 3;
                        part.data.positions.push_back(yUpToZUp(glm::vec3(
                            attrib_.vertices[p], attrib_.vertices[p + 1], attrib_.vertices[p + 2])));
                    }
                    if (key.normal >= 0) {
                        const auto n = static_cast<std::size_t>(key.normal) * 3;
                        part.data.normals.push_back(yUpToZUp(glm::vec3(
                            attrib_.normals[n], attrib_.normals[n + 1], attrib_.normals[n + 2])));
                    } else {
                        part.allNormals = false;
                        part.data.normals.emplace_back(0.0f);
                    }
                    if (key.uv >= 0) {
                        const auto t = static_cast<std::size_t>(key.uv) * 2;
                        part.data.uvs.emplace_back(attrib_.texcoords[t],
                                                   1.0f - attrib_.texcoords[t + 1]);
                        part.anyUvs = true;
                    } else {
                        part.data.uvs.emplace_back(0.0f);
                    }
                }
                part.data.indices.push_back(it->second);
            }
        }

        ImportedNode node;
        node.name = shape.name.empty()
                        ? (shapes_.size() == 1 ? model_.name : std::format("Object {}", s + 1))
                        : shape.name;
        for (std::size_t p = 0; p < parts.size(); ++p) {
            Part& part = parts[p];
            job_.checkCancelled();
            if (!part.allNormals) {
                part.data.normals.clear(); // generated by finalizeMesh()
            }
            if (!part.anyUvs) {
                part.data.uvs.clear();
            }
            if (!finalizeMesh(part.data)) {
                continue;
            }
            node.meshes.push_back(static_cast<int>(model_.meshes.size()));
            model_.meshes.push_back({node.name, std::move(part.data), partMaterial[p]});
        }
        if (!node.meshes.empty()) {
            model_.nodes.push_back(std::move(node));
        }
    }
    if (skippedLines) {
        warnOnce(model_, std::format("{} contains points or lines; they were skipped because "
                                     "LightBox shows surfaces only.",
                                     fileName_));
    }
}

void ObjReader::guessModelUnits() {
    bool any = false;
    Bounds total;
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
            "OBJ files don't store units. LightBox assumed {} from the model size ({:.3g} m); "
            "check the units in the Import panel if the model looks too big or too small.",
            guess.name, static_cast<double>(extent) * guess.metersPerUnit));
    }
}

} // namespace

void importObj(const std::filesystem::path& path, ImportedModel& model, const Job& job) {
    ObjReader(path, model, job).read();
}

} // namespace lb::import::detail
