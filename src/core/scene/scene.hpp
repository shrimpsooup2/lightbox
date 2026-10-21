#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>

#include "core/image.hpp"
#include "core/mesh.hpp"
#include "core/scene/environment.hpp"
#include "core/scene/ids.hpp"
#include "core/scene/light.hpp"
#include "core/scene/look.hpp"
#include "core/scene/material.hpp"
#include "core/scene/transform.hpp"

namespace lb {

// Places a mesh resource in the world with a material.
struct MeshInstance {
    ResourceId mesh;
    ResourceId material; // none = default clay material

    bool operator==(const MeshInstance&) const = default;
};

struct Entity {
    EntityId id;
    std::string name;
    EntityId parent;                // none for root entities
    std::vector<EntityId> children; // display order
    Transform transform;            // relative to the parent
    std::optional<MeshInstance> meshInstance;
    std::optional<Light> light; // an artificial light at this entity (shines along its −Z)
};

struct MeshResource {
    ResourceId id;
    std::string name;
    MeshData data;
    Bounds bounds; // local-space bounds of `data`
};

struct MaterialResource {
    ResourceId id;
    std::string name;
    Material material;
};

// A decoded image used by materials. Kept in memory at import resolution (capped, see
// import::kMaxTextureSize); the project file (0.6) stores or links the source.
struct TextureResource {
    ResourceId id;
    std::string name;
    std::string sourcePath; // UTF-8 path of the original file; empty when embedded in a model
    ImageData image;
};

struct SceneChange {
    enum class Kind {
        EntityAdded,
        EntityRemoved,
        EntityRenamed,
        EntityReparented,
        TransformChanged,
        MeshInstanceChanged,
        LightChanged,
        ResourceAdded,
        ResourceChanged,
        ResourceRemoved,
        EnvironmentChanged, // sun / sky / site settings
        LookChanged,        // exposure, tone mapping, color, clay
    };
    Kind kind;
    EntityId entity;     // set for entity changes
    ResourceId resource; // set for resource changes
};

// Complete document contents, used by project files. IDs are kept as given. `Entity::children` is the
// display order of each group, `roots` the display order at the top level; mesh bounds are recomputed
// when the snapshot is installed.
struct SceneSnapshot {
    std::vector<Entity> entities;
    std::vector<EntityId> roots;
    std::vector<MeshResource> meshes;
    std::vector<MaterialResource> materials;
    std::vector<TextureResource> textures;
    Environment environment;
    Look look;
};

// The LightBox document: entities in a hierarchy plus shared resources (meshes, materials,
// textures). Not thread-safe; owned and mutated on the UI thread. Every mutation notifies listeners
// synchronously. Listeners must not mutate the scene from inside the callback.
// Passing an unknown ID to a mutator is a programming error and throws std::invalid_argument.
class Scene {
public:
    using Listener = std::function<void(const SceneChange&)>;
    using ListenerId = std::uint64_t;
    using Visitor = std::function<void(const Entity&, const glm::mat4& world)>;

    Scene();
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // --- Resources -------------------------------------------------------------------------------
    // Throws std::invalid_argument for meshes without vertices, indices or matching normals, or with
    // texture coordinates that don't match the vertex count.
    ResourceId addMesh(std::string name, MeshData data);
    // Materials may only reference textures that exist in this scene (std::invalid_argument).
    ResourceId addMaterial(std::string name, const Material& material);
    void setMaterial(ResourceId id, const Material& material);
    // Throws std::invalid_argument for an empty image or one whose pixel count doesn't match.
    ResourceId addTexture(std::string name, ImageData image, std::string sourcePath = {});
    void setResourceName(ResourceId id, std::string name);
    // Throws std::logic_error while any entity (meshes, materials) or material (textures) still
    // references the resource.
    void removeResource(ResourceId id);
    const MeshResource* mesh(ResourceId id) const;
    const MaterialResource* material(ResourceId id) const;
    const TextureResource* texture(ResourceId id) const;
    // Visit every resource of a kind (unspecified order; sort for display).
    void forEachMesh(const std::function<void(const MeshResource&)>& visit) const;
    void forEachMaterial(const std::function<void(const MaterialResource&)>& visit) const;
    void forEachTexture(const std::function<void(const TextureResource&)>& visit) const;
    // Meshes and materials: number of mesh instances using them. Textures: number of materials.
    std::size_t userCount(ResourceId id) const;

    // --- Whole document ------------------------------------------------------------------------
    // Replaces everything with `snapshot`, keeping its IDs. Validates first (unique non-zero IDs; every
    // entity listed exactly once in `roots` or in its parent's children, parents consistent, no cycles;
    // mesh/material/texture references resolve; mesh and image data follow the addMesh/addTexture
    // rules) and throws std::invalid_argument without changing anything when invalid. Listeners see
    // the old entities and resources removed, then the new ones added, then EnvironmentChanged and
    // LookChanged.
    void replaceContents(SceneSnapshot snapshot);
    // The checks of replaceContents without a scene (thread-safe), so loaders can reject damaged
    // files before handing them to the UI thread. Throws std::invalid_argument.
    static void validate(const SceneSnapshot& snapshot);
    // Empty document with default environment and look (same notifications as replaceContents).
    void clear();

    // --- Environment & look ----------------------------------------------------------------------
    const Environment& environment() const { return environment_; }
    void setEnvironment(const Environment& environment);
    const Look& look() const { return look_; }
    void setLook(const Look& look);

    // --- Entities --------------------------------------------------------------------------------
    EntityId createEntity(std::string name, EntityId parent = {}, const Transform& transform = {});
    // Destroys the entity and all of its descendants.
    void destroyEntity(EntityId id);
    // Copies the entity and its descendants (sharing mesh/material resources) under the same parent,
    // inserted right after the original. Returns the copy of `id`.
    EntityId duplicate(EntityId id);
    void setName(EntityId id, std::string name);
    // Moves the entity under `parent` (none = root), appended last; keeps its local transform.
    // Throws std::invalid_argument if `parent` is the entity itself or one of its descendants.
    void setParent(EntityId id, EntityId parent);
    void setTransform(EntityId id, const Transform& transform);
    void setMeshInstance(EntityId id, std::optional<MeshInstance> instance);
    void setLight(EntityId id, std::optional<Light> light);

    const Entity* entity(EntityId id) const;
    std::span<const EntityId> roots() const { return roots_; }
    std::size_t entityCount() const { return entities_.size(); }
    glm::mat4 worldMatrix(EntityId id) const;
    // Union of the world-space bounds of every mesh instance; nullopt when there are none.
    std::optional<Bounds> worldBounds() const;
    // Visits every entity depth-first, parents before children, in display order.
    void traverse(const Visitor& visit) const;

    // --- Change notification ---------------------------------------------------------------------
    ListenerId addListener(Listener listener);
    void removeListener(ListenerId id);

private:
    template <class IdType, class Map>
    IdType newId(const Map& existing);
    Entity& require(EntityId id);
    const Entity& require(EntityId id) const;
    std::vector<EntityId>& siblingsOf(const Entity& entity);
    bool isReferenced(ResourceId id) const;
    void validateTextures(const Material& material) const;
    void notify(SceneChange::Kind kind, EntityId entity, ResourceId resource = {});

    std::unordered_map<EntityId, Entity> entities_;
    std::vector<EntityId> roots_;
    std::unordered_map<ResourceId, MeshResource> meshes_;
    std::unordered_map<ResourceId, MaterialResource> materials_;
    std::unordered_map<ResourceId, TextureResource> textures_;
    Environment environment_;
    Look look_;
    std::vector<std::pair<ListenerId, Listener>> listeners_;
    ListenerId nextListenerId_ = 1;
    std::mt19937_64 random_;
};

// Entities with a mesh among `ids` and everything inside them (groups, e.g. an imported model), each
// once, in the given order and then hierarchy order. Unknown IDs are skipped.
std::vector<EntityId> meshEntitiesIn(const Scene& scene, std::span<const EntityId> ids);
// Gives every mesh entity in or inside `ids` the material; returns how many were changed.
std::size_t applyMaterial(Scene& scene, std::span<const EntityId> ids, ResourceId material);
// World-space bounds of the meshes in or inside `ids` (corners of each mesh's box, transformed);
// nullopt when there are none.
std::optional<Bounds> worldBoundsOf(const Scene& scene, std::span<const EntityId> ids);
// Moves a (possibly parented) entity by a world-space offset.
void moveBy(Scene& scene, EntityId id, const glm::vec3& worldOffset);
// `ids` without the ones inside another listed entity, so moving each moves everything once.
std::vector<EntityId> outermostEntities(const Scene& scene, std::span<const EntityId> ids);

} // namespace lb
