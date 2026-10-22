#include "render/scene_sync.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <glm/trigonometric.hpp>

#include "core/log.hpp"
#include "core/solar.hpp"
#include "render/renderer.hpp"

namespace lb::render {

SceneSync::SceneSync(Scene& scene, Renderer& renderer)
    : SceneSync(
          scene, [&renderer](const MeshData& mesh) { return renderer.uploadMesh(mesh); },
          [&renderer](MeshHandle handle) { renderer.releaseMesh(handle); },
          [&renderer](const ImageData& image, bool color) {
              return renderer.uploadTexture(image, color);
          },
          [&renderer](TextureHandle handle) { renderer.releaseTexture(handle); }) {}

SceneSync::SceneSync(Scene& scene, MeshUploader uploadMesh, MeshReleaser releaseMesh,
                     TextureUploader uploadTexture, TextureReleaser releaseTexture)
    : scene_(scene), uploadMesh_(std::move(uploadMesh)), releaseMesh_(std::move(releaseMesh)),
      uploadTexture_(std::move(uploadTexture)), releaseTexture_(std::move(releaseTexture)) {
    listener_ = scene_.addListener([this](const SceneChange& change) { onChange(change); });
}

SceneSync::~SceneSync() {
    scene_.removeListener(listener_);
    for (const auto& [id, handle] : meshes_) {
        releaseMesh_(handle);
    }
    for (const auto& [id, handle] : textures_) {
        releaseTexture_(handle);
    }
}

void SceneSync::onChange(const SceneChange& change) {
    switch (change.kind) {
    case SceneChange::Kind::ResourceAdded:
    case SceneChange::Kind::EntityRenamed:
        // No effect: new resources are unused until an entity or material references them (which
        // notifies separately), and names are not rendered.
        return;
    case SceneChange::Kind::EnvironmentChanged:
    case SceneChange::Kind::LookChanged:
        // Read per frame; the sun and sky (and clay, a look setting) change the ray-traced image.
        ++version_;
        return;
    case SceneChange::Kind::ResourceRemoved:
        if (meshes_.contains(change.resource)) {
            removedMeshes_.push_back(change.resource);
        }
        if (textures_.contains(change.resource)) {
            removedTextures_.push_back(change.resource);
        }
        failedTextures_.erase(change.resource);
        break;
    case SceneChange::Kind::EntityAdded:
    case SceneChange::Kind::EntityRemoved:
    case SceneChange::Kind::EntityReparented:
    case SceneChange::Kind::TransformChanged:
    case SceneChange::Kind::MeshInstanceChanged:
        ++geometryVersion_;
        break;
    case SceneChange::Kind::ResourceChanged: // material edits; resource images and meshes are
                                             // immutable, so nothing is re-uploaded
    case SceneChange::Kind::LightChanged:
        break;
    }
    ++version_;
    dirty_ = true;
}

MeshHandle SceneSync::meshHandle(ResourceId id) {
    if (const auto it = meshes_.find(id); it != meshes_.end()) {
        return it->second;
    }
    const MeshResource* mesh = scene_.mesh(id);
    if (mesh == nullptr) {
        return {};
    }
    const MeshHandle handle = uploadMesh_(mesh->data);
    meshes_.emplace(id, handle);
    return handle;
}

TextureHandle SceneSync::textureHandle(ResourceId id, bool color) {
    if (!id) {
        return {};
    }
    if (const auto it = textures_.find(id); it != textures_.end()) {
        return it->second;
    }
    const TextureResource* texture = scene_.texture(id);
    if (texture == nullptr || failedTextures_.contains(id)) {
        return {};
    }
    // A material stays usable without one of its maps, so a texture that cannot be uploaded (GPU
    // texture limit, out of memory) degrades to an unused slot instead of failing the whole sync.
    try {
        const TextureHandle handle = uploadTexture_(texture->image, color);
        if (handle.valid()) {
            textures_.emplace(id, handle);
        }
        return handle;
    } catch (const std::exception& error) {
        log::warn("Texture '{}' is not shown: {}", texture->name, error.what());
        failedTextures_.insert(id);
        return {};
    }
}

MaterialParams SceneSync::toParams(const Material& material) {
    // The first use decides how the mip chain is filtered (color maps in linear light).
    const auto map = [this](const TextureSlot& slot, bool color = false) {
        MaterialTexture texture;
        texture.texture = textureHandle(slot.texture, color);
        texture.channel = static_cast<std::uint32_t>(std::clamp(slot.channel, 0, 3));
        texture.invert = slot.invert;
        return texture;
    };
    MaterialParams params;
    params.baseColor = material.baseColor;
    params.roughness = material.roughness;
    params.metalness = material.metalness;
    params.baseColorMap = map(material.baseColorMap, true);
    params.roughnessMap = map(material.roughnessMap);
    params.metalnessMap = map(material.metalnessMap);
    params.normalMap = map(material.normalMap);
    params.occlusionMap = map(material.occlusionMap);
    params.heightMap = map(material.heightMap);
    params.heightDepth = material.heightDepth;
    params.normalStrength = material.normalStrength;
    params.normalFlipY = material.normalFlipY;
    params.mapping =
        material.mapping == lb::TextureMapping::Box ? TextureMapping::Box : TextureMapping::UV;
    params.textureScale = material.textureScale;
    params.textureRotationDegrees = material.textureRotationDegrees;
    params.textureOffset = material.textureOffset;
    params.variation = material.variation;
    params.randomTileRotation = material.randomTileRotation;
    params.transmission = std::clamp(material.transmission, 0.0f, 1.0f);
    params.ior = std::clamp(material.ior, 1.0f, 3.0f);
    params.thinWalled = material.thinWalled;
    params.emission =
        material.emissionColor * std::max(material.emissionStrength, 0.0f) * kRenderUnitsPerLux;
    return params;
}

namespace {

LightParams toLightParams(const Light& light, const glm::mat4& world) {
    LightParams params;
    params.position = glm::vec3(world[3]);
    const glm::vec3 x(world[0]);
    const glm::vec3 y(world[1]);
    const glm::vec3 z(world[2]);
    params.axisX = glm::length(x) > 0.0f ? glm::normalize(x) : glm::vec3(1.0f, 0.0f, 0.0f);
    params.axisY = glm::length(y) > 0.0f ? glm::normalize(y) : glm::vec3(0.0f, 1.0f, 0.0f);
    params.direction = glm::length(z) > 0.0f ? -glm::normalize(z) : glm::vec3(0.0f, 0.0f, -1.0f);
    params.radius = std::max(light.radius, 0.005f);
    const glm::vec3 color = kelvinToLinearRgb(light.temperatureKelvin);
    const float lumens = std::max(light.lumens, 0.0f);
    constexpr float kPi = 3.14159265f;
    switch (light.type) {
    case LightType::Point:
        params.kind = LightKind::Point;
        params.intensity = color * (lumens / (4.0f * kPi)) * kRenderUnitsPerLux; // candela
        break;
    case LightType::Spot: {
        params.kind = LightKind::Spot;
        const float halfAngle =
            glm::radians(std::clamp(light.spotAngleDegrees, 1.0f, 179.0f)) * 0.5f;
        const float softness = std::clamp(light.spotSoftness, 0.0f, 1.0f);
        params.cosOuter = std::cos(halfAngle);
        params.cosInner = std::cos(halfAngle * (1.0f - softness));
        // The flux leaves through the cone (counted to the middle of the soft edge).
        const float effective = std::cos(halfAngle * (1.0f - 0.5f * softness));
        params.intensity =
            color * (lumens / (2.0f * kPi * std::max(1.0f - effective, 1e-4f))) * kRenderUnitsPerLux;
        break;
    }
    case LightType::Area: {
        params.kind = LightKind::Area;
        // Scaling the light's entity resizes the panel.
        params.halfSize = glm::max(light.areaSize * 0.5f * glm::vec2(glm::length(x), glm::length(y)),
                                   glm::vec2(0.005f));
        const float area = 4.0f * params.halfSize.x * params.halfSize.y;
        params.intensity = color * (lumens / (kPi * area)) * kRenderUnitsPerLux; // nits
        break;
    }
    }
    return params;
}

} // namespace

std::uint32_t SceneSync::materialIndex(ResourceId id) {
    if (const auto it = materialIndices_.find(id); it != materialIndices_.end()) {
        return it->second;
    }
    const MaterialResource* material = scene_.material(id);
    if (material == nullptr) {
        return 0; // no material: default clay
    }
    const auto index = static_cast<std::uint32_t>(materials_.size());
    materials_.push_back(toParams(material->material));
    materialIndices_.emplace(id, index);
    return index;
}

void SceneSync::update() {
    for (ResourceId id : removedMeshes_) {
        if (const auto it = meshes_.find(id); it != meshes_.end()) {
            releaseMesh_(it->second);
            meshes_.erase(it);
        }
    }
    removedMeshes_.clear();
    if (!removedTextures_.empty()) {
        for (ResourceId id : removedTextures_) {
            if (const auto it = textures_.find(id); it != textures_.end()) {
                releaseTexture_(it->second);
                textures_.erase(it);
            }
        }
        removedTextures_.clear();
        failedTextures_.clear(); // freed GPU slots: give earlier failures another chance
    }
    if (!dirty_) {
        return;
    }

    draws_.clear();
    materials_.clear();
    materialIndices_.clear();
    lights_.clear();
    materials_.push_back(toParams(Material{}));
    scene_.traverse([this](const Entity& entity, const glm::mat4& world) {
        if (entity.light) {
            lights_.push_back(toLightParams(*entity.light, world));
            lights_.back().entity = entity.id.value;
        }
        if (!entity.meshInstance) {
            return;
        }
        const MeshHandle handle = meshHandle(entity.meshInstance->mesh);
        if (!handle.valid()) {
            return;
        }
        DrawItem item;
        item.mesh = handle;
        item.transform = world;
        item.material = materialIndex(entity.meshInstance->material);
        item.transparent = materials_[item.material].transmission > 0.0f;
        item.entity = entity.id.value;
        draws_.push_back(item);
    });
    dirty_ = false;
}

namespace {

// UI-thread part: world-space copies of every mesh instance (meshes in the document are immutable,
// but entities can change at any time, so the worker gets its own data).
std::shared_ptr<RayScene> gatherRayGeometry(const Scene& scene,
                                            const std::function<std::uint32_t(ResourceId)>& material,
                                            std::uint64_t version) {
    auto result = std::make_shared<RayScene>();
    result->geometryVersion = version;
    scene.traverse([&](const Entity& entity, const glm::mat4& world) {
        if (!entity.meshInstance) {
            return;
        }
        const MeshResource* mesh = scene.mesh(entity.meshInstance->mesh);
        if (mesh == nullptr) {
            return;
        }
        const MeshData& data = mesh->data;
        const glm::vec3 axisScale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                                  glm::length(glm::vec3(world[2])));
        const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
        const auto instance = static_cast<std::uint32_t>(result->instances.size());
        const auto axis = [](const glm::vec3& v) {
            return glm::vec4(glm::length(v) > 0.0f ? glm::normalize(v) : v, 0.0f);
        };
        result->instances.push_back({axis(glm::vec3(world[0])), axis(glm::vec3(world[1])),
                                     axis(glm::vec3(world[2]))});
        const auto base = static_cast<std::uint32_t>(result->vertices.size());
        for (std::size_t i = 0; i < data.positions.size(); ++i) {
            RayVertex v{};
            v.position = glm::vec3(world * glm::vec4(data.positions[i], 1.0f));
            const glm::vec3 n = normalMatrix * data.normals[i];
            v.normal = glm::length(n) > 0.0f ? glm::normalize(n) : glm::vec3(0.0f, 0.0f, 1.0f);
            if (!data.uvs.empty()) {
                v.u = data.uvs[i].x;
                v.v = data.uvs[i].y;
            }
            v.boxPosition = data.positions[i] * axisScale;
            v.instance = instance;
            result->vertices.push_back(v);
        }
        const std::uint32_t materialIndex = material(entity.meshInstance->material);
        for (std::size_t i = 0; i + 2 < data.indices.size(); i += 3) {
            result->triangles.emplace_back(base + data.indices[i], base + data.indices[i + 1],
                                           base + data.indices[i + 2], materialIndex);
        }
    });
    return result;
}

// Worker-thread part: the BVH, and triangles reordered into its leaf order.
std::shared_ptr<RayScene> buildRayScene(std::shared_ptr<RayScene> scene) {
    std::vector<glm::vec3> corners;
    corners.reserve(scene->triangles.size() * 3);
    for (const glm::uvec4& t : scene->triangles) {
        corners.push_back(scene->vertices[t.x].position);
        corners.push_back(scene->vertices[t.y].position);
        corners.push_back(scene->vertices[t.z].position);
    }
    Bvh bvh = buildBvh(corners);
    std::vector<glm::uvec4> ordered;
    ordered.reserve(scene->triangles.size());
    for (const std::uint32_t index : bvh.triangles) {
        ordered.push_back(scene->triangles[index]);
    }
    scene->triangles = std::move(ordered);
    scene->nodes = threadBvh(bvh.nodes);
    return scene;
}

} // namespace

std::vector<RayNode> threadBvh(const std::vector<BvhNode>& nodes) {
    std::vector<RayNode> result;
    if (nodes.empty()) {
        return result;
    }
    result.reserve(nodes.size());
    // Pre-order walk; an interior node's miss link is set once its whole subtree has been written.
    std::vector<std::pair<std::uint32_t, bool>> stack{{0, false}}; // (node, subtree finished)
    std::vector<std::uint32_t> written(nodes.size(), 0);
    while (!stack.empty()) {
        auto [index, done] = stack.back();
        stack.pop_back();
        const BvhNode& node = nodes[index];
        if (done) {
            // Subtree finished: the next node written is where a miss continues.
            result[written[index]].miss = static_cast<std::uint32_t>(result.size());
            continue;
        }
        written[index] = static_cast<std::uint32_t>(result.size());
        RayNode out{};
        out.boundsMin = node.boundsMin;
        out.boundsMax = node.boundsMax;
        if (node.count > 0) {
            out.leaf = (node.first << 3) | std::min<std::uint32_t>(node.count, 7);
            result.push_back(out);
            result.back().miss = static_cast<std::uint32_t>(result.size());
            continue;
        }
        out.leaf = 0;
        result.push_back(out);
        stack.emplace_back(index, true);
        stack.emplace_back(node.first + 1, false);
        stack.emplace_back(node.first, false);
    }
    return result;
}

std::shared_ptr<const RayScene> SceneSync::rayScene() {
    if (rayBuild_.valid() &&
        rayBuild_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            rayScene_ = rayBuild_.get();
        } catch (const std::exception& error) {
            log::error("Preparing the scene for ray tracing failed: {}", error.what());
            rayScene_.reset();
        }
    }
    if (rayScene_ && rayScene_->geometryVersion == geometryVersion_) {
        return rayScene_;
    }
    if (!rayBuild_.valid()) {
        std::shared_ptr<RayScene> gathered = gatherRayGeometry(
            scene_, [this](ResourceId id) { return materialIndex(id); }, geometryVersion_);
        rayBuild_ = std::async(std::launch::async, buildRayScene, std::move(gathered));
    }
    return nullptr;
}

} // namespace lb::render
