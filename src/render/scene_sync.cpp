#include "render/scene_sync.hpp"

#include <algorithm>
#include <exception>
#include <utility>

#include "core/log.hpp"
#include "render/renderer.hpp"

namespace lb::render {

SceneSync::SceneSync(Scene& scene, Renderer& renderer)
    : SceneSync(
          scene, [&renderer](const MeshData& mesh) { return renderer.uploadMesh(mesh); },
          [&renderer](MeshHandle handle) { renderer.releaseMesh(handle); },
          [&renderer](const ImageData& image) { return renderer.uploadTexture(image); },
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
    case SceneChange::Kind::EnvironmentChanged:
    case SceneChange::Kind::LookChanged:
        // No effect on the lists: new resources are unused until an entity or material references
        // them (which notifies separately), names are not rendered, lighting and look are read
        // per frame.
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
    case SceneChange::Kind::ResourceChanged: // material edits; resource images and meshes are
                                             // immutable, so nothing is re-uploaded
        break;
    }
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

TextureHandle SceneSync::textureHandle(ResourceId id) {
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
        const TextureHandle handle = uploadTexture_(texture->image);
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
    const auto map = [this](const TextureSlot& slot) {
        MaterialTexture texture;
        texture.texture = textureHandle(slot.texture);
        texture.channel = static_cast<std::uint32_t>(std::clamp(slot.channel, 0, 3));
        texture.invert = slot.invert;
        return texture;
    };
    MaterialParams params;
    params.baseColor = material.baseColor;
    params.roughness = material.roughness;
    params.metalness = material.metalness;
    params.baseColorMap = map(material.baseColorMap);
    params.roughnessMap = map(material.roughnessMap);
    params.metalnessMap = map(material.metalnessMap);
    params.normalMap = map(material.normalMap);
    params.occlusionMap = map(material.occlusionMap);
    params.normalStrength = material.normalStrength;
    params.normalFlipY = material.normalFlipY;
    params.mapping =
        material.mapping == lb::TextureMapping::Box ? TextureMapping::Box : TextureMapping::UV;
    params.textureScale = material.textureScale;
    params.textureRotationDegrees = material.textureRotationDegrees;
    params.textureOffset = material.textureOffset;
    return params;
}

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
    materials_.push_back(toParams(Material{}));
    scene_.traverse([this](const Entity& entity, const glm::mat4& world) {
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
        draws_.push_back(item);
    });
    dirty_ = false;
}

} // namespace lb::render
