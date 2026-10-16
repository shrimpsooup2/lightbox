// Turns an ImportedModel into scene resources and entities (UI thread).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include "core/scene/scene.hpp"
#include "import/formats.hpp"
#include "import/importer.hpp"

namespace lb::import {
namespace {

// Further than this from the origin (meters), float positions get visibly imprecise (~1 mm steps at
// 10 km), so the model is moved next to the origin.
constexpr double kFarAwayMeters = 10'000.0;

// Splits a node matrix into the editable Transform (scale, then rotation, then translation).
// Returns false when the matrix has shear, which a Transform can't express (approximated).
bool decompose(const glm::mat4& matrix, Transform& transform) {
    transform.position = glm::vec3(matrix[3]);
    std::array<glm::vec3, 3> axes = {glm::vec3(matrix[0]), glm::vec3(matrix[1]),
                                     glm::vec3(matrix[2])};
    glm::vec3 scale(glm::length(axes[0]), glm::length(axes[1]), glm::length(axes[2]));
    if (!(scale.x > 1e-12f && scale.y > 1e-12f && scale.z > 1e-12f) ||
        !std::isfinite(scale.x + scale.y + scale.z)) {
        // Flattened to a plane/line/point: keep what scale there is, no rotation.
        transform.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        transform.scale = glm::vec3(std::isfinite(scale.x) ? scale.x : 1.0f,
                                    std::isfinite(scale.y) ? scale.y : 1.0f,
                                    std::isfinite(scale.z) ? scale.z : 1.0f);
        return true;
    }
    for (int i = 0; i < 3; ++i) {
        axes[static_cast<std::size_t>(i)] /= scale[i];
    }
    if (glm::determinant(glm::mat3(axes[0], axes[1], axes[2])) < 0.0f) {
        scale.x = -scale.x; // mirrored: one negative scale plus a proper rotation
        axes[0] = -axes[0];
    }
    const bool sheared = std::abs(glm::dot(axes[0], axes[1])) > 1e-3f ||
                         std::abs(glm::dot(axes[0], axes[2])) > 1e-3f ||
                         std::abs(glm::dot(axes[1], axes[2])) > 1e-3f;
    // Gram-Schmidt keeps the rotation valid even for slightly sheared or imprecise matrices.
    axes[1] = glm::normalize(axes[1] - glm::dot(axes[1], axes[0]) * axes[0]);
    axes[2] = glm::cross(axes[0], axes[1]);
    transform.rotation = glm::normalize(glm::quat_cast(glm::mat3(axes[0], axes[1], axes[2])));
    transform.scale = scale;
    return !sheared;
}

Bounds transformBounds(const Bounds& bounds, const glm::mat4& matrix) {
    Bounds result{glm::vec3(std::numeric_limits<float>::max()),
                  glm::vec3(std::numeric_limits<float>::lowest())};
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 p((corner & 1) != 0 ? bounds.max.x : bounds.min.x,
                          (corner & 2) != 0 ? bounds.max.y : bounds.min.y,
                          (corner & 4) != 0 ? bounds.max.z : bounds.min.z);
        const glm::vec3 world(matrix * glm::vec4(p, 1.0f));
        result.min = glm::min(result.min, world);
        result.max = glm::max(result.max, world);
    }
    return result;
}

Bounds positionBounds(const MeshData& mesh) {
    Bounds bounds{mesh.positions.front(), mesh.positions.front()};
    for (const glm::vec3& p : mesh.positions) {
        bounds.min = glm::min(bounds.min, p);
        bounds.max = glm::max(bounds.max, p);
    }
    return bounds;
}

bool validIndex(int index, std::size_t size) {
    return index >= 0 && static_cast<std::size_t>(index) < size;
}

TextureSlot sceneSlot(const ImportedTextureSlot& slot, const std::vector<ResourceId>& textures) {
    TextureSlot result;
    if (validIndex(slot.texture, textures.size())) {
        result.texture = textures[static_cast<std::size_t>(slot.texture)];
        result.channel = std::clamp(slot.channel, 0, 3);
        result.invert = slot.invert;
    }
    return result;
}

} // namespace

PlacedModel addToScene(Scene& scene, ImportedModel& model) {
    PlacedModel placed;
    const std::size_t nodeCount = model.nodes.size();

    // Which nodes lead to a mesh (parents come before children, so walk backwards).
    std::vector<int> parentOf(nodeCount, -1);
    std::vector<char> keep(nodeCount, 0);
    for (std::size_t i = 0; i < nodeCount; ++i) {
        const int parent = model.nodes[i].parent;
        parentOf[i] = parent >= 0 && static_cast<std::size_t>(parent) < i ? parent : -1;
    }
    std::vector<char> meshUsed(model.meshes.size(), 0);
    for (std::size_t i = nodeCount; i > 0; --i) {
        const std::size_t n = i - 1;
        for (const int mesh : model.nodes[n].meshes) {
            if (validIndex(mesh, model.meshes.size()) &&
                !model.meshes[static_cast<std::size_t>(mesh)].data.indices.empty()) {
                keep[n] = 1;
                meshUsed[static_cast<std::size_t>(mesh)] = 1;
            }
        }
        if (keep[n] && parentOf[n] >= 0) {
            keep[static_cast<std::size_t>(parentOf[n])] = 1;
        }
    }

    // World bounds in file units, to detect far-away (georeferenced) models before anything is added.
    std::optional<Bounds> modelBounds;
    {
        std::vector<Bounds> meshBounds(model.meshes.size());
        for (std::size_t m = 0; m < model.meshes.size(); ++m) {
            if (meshUsed[m]) {
                meshBounds[m] = positionBounds(model.meshes[m].data);
            }
        }
        std::vector<glm::mat4> world(nodeCount, glm::mat4(1.0f));
        for (std::size_t i = 0; i < nodeCount; ++i) {
            world[i] = parentOf[i] >= 0
                           ? world[static_cast<std::size_t>(parentOf[i])] * model.nodes[i].transform
                           : model.nodes[i].transform;
            if (!keep[i]) {
                continue;
            }
            for (const int mesh : model.nodes[i].meshes) {
                if (!validIndex(mesh, model.meshes.size()) ||
                    !meshUsed[static_cast<std::size_t>(mesh)]) {
                    continue;
                }
                const Bounds b = transformBounds(meshBounds[static_cast<std::size_t>(mesh)], world[i]);
                if (!modelBounds) {
                    modelBounds = b;
                } else {
                    modelBounds->min = glm::min(modelBounds->min, b.min);
                    modelBounds->max = glm::max(modelBounds->max, b.max);
                }
            }
        }
    }

    const double metersPerUnit =
        std::isfinite(model.metersPerUnit) && model.metersPerUnit > 0.0 ? model.metersPerUnit : 1.0;
    Transform rootTransform;
    rootTransform.scale = glm::vec3(static_cast<float>(metersPerUnit));
    if (modelBounds) {
        const glm::dvec3 center = glm::dvec3(modelBounds->center()) * metersPerUnit;
        const double distance = glm::length(center);
        if (distance > kFarAwayMeters) {
            // Center it horizontally and put its lowest point on the ground plane.
            const glm::dvec3 offset(-center.x, -center.y,
                                    -static_cast<double>(modelBounds->min.z) * metersPerUnit);
            rootTransform.position = glm::vec3(offset);
            model.warnings.push_back(std::format(
                "The model lies {:.1f} km from the origin, which is too far for precise display; "
                "LightBox moved it next to the origin (by {:.1f}, {:.1f}, {:.1f} m).",
                distance / 1000.0, offset.x, offset.y, offset.z));
        }
    }

    // Resources that some kept mesh needs.
    std::vector<char> materialUsed(model.materials.size(), 0);
    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        if (meshUsed[m] && validIndex(model.meshes[m].material, model.materials.size())) {
            materialUsed[static_cast<std::size_t>(model.meshes[m].material)] = 1;
        }
    }
    std::vector<char> textureUsed(model.textures.size(), 0);
    for (std::size_t m = 0; m < model.materials.size(); ++m) {
        if (!materialUsed[m]) {
            continue;
        }
        const ImportedMaterial& material = model.materials[m];
        for (const ImportedTextureSlot* slot : {&material.baseColor, &material.roughness,
                                                &material.metalness, &material.normal,
                                                &material.occlusion}) {
            if (validIndex(slot->texture, model.textures.size())) {
                textureUsed[static_cast<std::size_t>(slot->texture)] = 1;
            }
        }
    }

    std::vector<ResourceId> textureIds(model.textures.size());
    for (std::size_t t = 0; t < model.textures.size(); ++t) {
        ImportedTexture& texture = model.textures[t];
        if (!textureUsed[t] || texture.image.empty()) {
            continue;
        }
        const std::string name =
            texture.name.empty() ? std::format("{} texture {}", model.name, t + 1) : texture.name;
        textureIds[t] = scene.addTexture(name, std::move(texture.image), texture.sourcePath);
        texture.image = {};
        ++placed.textures;
    }

    std::vector<ResourceId> materialIds(model.materials.size());
    for (std::size_t m = 0; m < model.materials.size(); ++m) {
        if (!materialUsed[m]) {
            continue;
        }
        const ImportedMaterial& imported = model.materials[m];
        Material material = imported.material;
        material.baseColorMap = sceneSlot(imported.baseColor, textureIds);
        material.roughnessMap = sceneSlot(imported.roughness, textureIds);
        material.metalnessMap = sceneSlot(imported.metalness, textureIds);
        material.normalMap = sceneSlot(imported.normal, textureIds);
        material.occlusionMap = sceneSlot(imported.occlusion, textureIds);
        const std::string name =
            imported.name.empty() ? std::format("Material {}", m + 1) : imported.name;
        materialIds[m] = scene.addMaterial(name, material);
        ++placed.materials;
    }

    std::vector<ResourceId> meshIds(model.meshes.size());
    std::vector<std::size_t> triangles(model.meshes.size(), 0);
    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        if (!meshUsed[m]) {
            continue;
        }
        ImportedMesh& mesh = model.meshes[m];
        triangles[m] = mesh.data.indices.size() / 3;
        const std::string name = mesh.name.empty() ? std::format("Mesh {}", m + 1) : mesh.name;
        try {
            meshIds[m] = scene.addMesh(name, std::move(mesh.data));
        } catch (const std::invalid_argument&) {
            detail::warnOnce(model, std::format("Object {} has invalid geometry and was skipped.",
                                                name));
        }
        mesh.data = {};
    }

    placed.root = scene.createEntity(model.name.empty() ? "Model" : model.name, {}, rootTransform);
    const auto materialName = [&](int material) {
        if (!validIndex(material, model.materials.size())) {
            return std::string("Default");
        }
        const std::string& name = model.materials[static_cast<std::size_t>(material)].name;
        return name.empty() ? std::format("Material {}", material + 1) : name;
    };
    const auto instance = [&](int mesh) -> std::optional<MeshInstance> {
        if (!validIndex(mesh, meshIds.size()) || !meshIds[static_cast<std::size_t>(mesh)]) {
            return std::nullopt;
        }
        const int material = model.meshes[static_cast<std::size_t>(mesh)].material;
        const ResourceId materialId = validIndex(material, materialIds.size())
                                          ? materialIds[static_cast<std::size_t>(material)]
                                          : ResourceId{};
        placed.triangles += triangles[static_cast<std::size_t>(mesh)];
        ++placed.objects;
        return MeshInstance{meshIds[static_cast<std::size_t>(mesh)], materialId};
    };

    bool sheared = false;
    std::vector<EntityId> entities(nodeCount);
    for (std::size_t i = 0; i < nodeCount; ++i) {
        if (!keep[i]) {
            continue;
        }
        const ImportedNode& node = model.nodes[i];
        Transform transform;
        sheared = !decompose(node.transform, transform) || sheared;
        const EntityId parent =
            parentOf[i] >= 0 ? entities[static_cast<std::size_t>(parentOf[i])] : placed.root;
        std::vector<int> meshes;
        for (const int mesh : node.meshes) {
            if (validIndex(mesh, meshIds.size()) && meshIds[static_cast<std::size_t>(mesh)]) {
                meshes.push_back(mesh);
            }
        }
        std::string name = node.name;
        if (name.empty()) {
            name = meshes.empty() ? std::format("Group {}", i + 1) : std::format("Object {}", i + 1);
        }
        const EntityId entity = scene.createEntity(name, parent, transform);
        entities[i] = entity;
        if (meshes.size() == 1) {
            scene.setMeshInstance(entity, instance(meshes.front()));
        } else {
            for (const int mesh : meshes) {
                const EntityId part = scene.createEntity(
                    std::format("{} · {}", name,
                                materialName(model.meshes[static_cast<std::size_t>(mesh)].material)),
                    entity);
                scene.setMeshInstance(part, instance(mesh));
            }
        }
    }
    if (sheared) {
        detail::warnOnce(model, "Some objects use skewed (sheared) transforms, which LightBox "
                                "approximates; they may look slightly distorted.");
    }
    return placed;
}

ResourceId addMaterialToScene(Scene& scene, const ImportedModel& model) {
    if (model.materials.empty()) {
        throw std::invalid_argument("addMaterialToScene: the texture set has no material");
    }
    const ImportedMaterial& imported = model.materials.front();
    std::vector<ResourceId> textureIds(model.textures.size());
    for (std::size_t t = 0; t < model.textures.size(); ++t) {
        const ImportedTexture& texture = model.textures[t];
        const bool used = imported.baseColor.texture == static_cast<int>(t) ||
                          imported.roughness.texture == static_cast<int>(t) ||
                          imported.metalness.texture == static_cast<int>(t) ||
                          imported.normal.texture == static_cast<int>(t) ||
                          imported.occlusion.texture == static_cast<int>(t);
        if (used && !texture.image.empty()) {
            textureIds[t] = scene.addTexture(texture.name, texture.image, texture.sourcePath);
        }
    }
    Material material = imported.material;
    material.baseColorMap = sceneSlot(imported.baseColor, textureIds);
    material.roughnessMap = sceneSlot(imported.roughness, textureIds);
    material.metalnessMap = sceneSlot(imported.metalness, textureIds);
    material.normalMap = sceneSlot(imported.normal, textureIds);
    material.occlusionMap = sceneSlot(imported.occlusion, textureIds);
    const std::string& name = imported.name.empty() ? model.name : imported.name;
    return scene.addMaterial(name.empty() ? "Material" : name, material);
}

} // namespace lb::import
