#include "core/scene/scene.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

#include <glm/common.hpp>
#include <glm/vec4.hpp>

namespace lb {
namespace {

Bounds computeBounds(const std::vector<glm::vec3>& positions) {
    Bounds bounds{positions.front(), positions.front()};
    for (const glm::vec3& p : positions) {
        bounds.min = glm::min(bounds.min, p);
        bounds.max = glm::max(bounds.max, p);
    }
    return bounds;
}

Bounds transformBounds(const Bounds& local, const glm::mat4& world) {
    Bounds result{glm::vec3(std::numeric_limits<float>::max()),
                  glm::vec3(std::numeric_limits<float>::lowest())};
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 p{(corner & 1) ? local.max.x : local.min.x,
                          (corner & 2) ? local.max.y : local.min.y,
                          (corner & 4) ? local.max.z : local.min.z};
        const glm::vec3 w = glm::vec3(world * glm::vec4(p, 1.0f));
        result.min = glm::min(result.min, w);
        result.max = glm::max(result.max, w);
    }
    return result;
}

[[noreturn]] void unknownId(const char* what, std::uint64_t value) {
    throw std::invalid_argument(std::format("Scene: unknown {} id {:#018x}", what, value));
}

// The rules of addMesh/addTexture, shared with snapshot validation. Return a reason or nullptr.
const char* meshProblem(const MeshData& data) {
    if (data.positions.empty() || data.indices.empty() ||
        data.normals.size() != data.positions.size()) {
        return "mesh needs vertices, matching normals and indices";
    }
    if (!data.uvs.empty() && data.uvs.size() != data.positions.size()) {
        return "texture coordinates must match the vertices";
    }
    return nullptr;
}

const char* imageProblem(const ImageData& image) {
    if (image.empty() || image.pixels.size() != std::size_t{image.width} * image.height * 4) {
        return "image is empty or its pixel data does not match its size";
    }
    return nullptr;
}

template <class HasTexture>
void checkTextureReferences(const Material& material, const HasTexture& hasTexture) {
    for (const TextureSlot* slot : {&material.baseColorMap, &material.roughnessMap,
                                    &material.metalnessMap, &material.normalMap,
                                    &material.occlusionMap, &material.heightMap}) {
        if (slot->texture && !hasTexture(slot->texture)) {
            unknownId("texture", slot->texture.value);
        }
    }
}

[[noreturn]] void invalidSnapshot(std::string_view problem) {
    throw std::invalid_argument(std::format("Scene::replaceContents: {}", problem));
}

// The subtree of `root`, children before parents, so listeners never see a child whose parent is
// gone.
std::vector<EntityId> subtreeChildrenFirst(const std::unordered_map<EntityId, Entity>& entities,
                                           EntityId root) {
    std::vector<EntityId> order;
    std::vector<std::pair<EntityId, bool>> stack{{root, false}};
    while (!stack.empty()) {
        auto [current, expanded] = stack.back();
        stack.pop_back();
        if (expanded) {
            order.push_back(current);
            continue;
        }
        stack.emplace_back(current, true);
        for (EntityId child : entities.at(current).children) {
            stack.emplace_back(child, false);
        }
    }
    return order;
}

} // namespace

Scene::Scene() : random_(std::random_device{}()) {}

template <class IdType, class Map>
IdType Scene::newId(const Map& existing) {
    IdType id;
    do {
        id.value = random_();
    } while (id.value == 0 || existing.contains(id));
    return id;
}

Entity& Scene::require(EntityId id) {
    const auto it = entities_.find(id);
    if (it == entities_.end()) {
        unknownId("entity", id.value);
    }
    return it->second;
}

const Entity& Scene::require(EntityId id) const {
    const auto it = entities_.find(id);
    if (it == entities_.end()) {
        unknownId("entity", id.value);
    }
    return it->second;
}

std::vector<EntityId>& Scene::siblingsOf(const Entity& entity) {
    return entity.parent ? require(entity.parent).children : roots_;
}

void Scene::notify(SceneChange::Kind kind, EntityId entity, ResourceId resource) {
    const SceneChange change{kind, entity, resource};
    // Copy so a listener may unsubscribe itself while being notified.
    const auto listeners = listeners_;
    for (const auto& [id, listener] : listeners) {
        listener(change);
    }
}

// --- Resources -----------------------------------------------------------------------------------

ResourceId Scene::addMesh(std::string name, MeshData data) {
    if (const char* problem = meshProblem(data)) {
        throw std::invalid_argument(std::format("Scene::addMesh: {}", problem));
    }
    const auto id = newId<ResourceId>(meshes_);
    const Bounds bounds = computeBounds(data.positions);
    meshes_.emplace(id, MeshResource{id, std::move(name), std::move(data), bounds});
    notify(SceneChange::Kind::ResourceAdded, {}, id);
    return id;
}

void Scene::validateTextures(const Material& material) const {
    checkTextureReferences(material, [this](ResourceId id) { return textures_.contains(id); });
}

ResourceId Scene::addMaterial(std::string name, const Material& material) {
    validateTextures(material);
    const auto id = newId<ResourceId>(materials_);
    materials_.emplace(id, MaterialResource{id, std::move(name), material});
    notify(SceneChange::Kind::ResourceAdded, {}, id);
    return id;
}

void Scene::setMaterial(ResourceId id, const Material& material) {
    const auto it = materials_.find(id);
    if (it == materials_.end()) {
        unknownId("material", id.value);
    }
    if (it->second.material == material) {
        return;
    }
    validateTextures(material);
    it->second.material = material;
    notify(SceneChange::Kind::ResourceChanged, {}, id);
}

ResourceId Scene::addTexture(std::string name, ImageData image, std::string sourcePath) {
    if (const char* problem = imageProblem(image)) {
        throw std::invalid_argument(std::format("Scene::addTexture: {}", problem));
    }
    const auto id = newId<ResourceId>(textures_);
    textures_.emplace(id,
                      TextureResource{id, std::move(name), std::move(sourcePath), std::move(image)});
    notify(SceneChange::Kind::ResourceAdded, {}, id);
    return id;
}

namespace {

bool usesTexture(const Material& m, ResourceId id) {
    return m.baseColorMap.texture == id || m.roughnessMap.texture == id ||
           m.metalnessMap.texture == id || m.normalMap.texture == id ||
           m.occlusionMap.texture == id || m.heightMap.texture == id;
}

} // namespace

bool Scene::isReferenced(ResourceId id) const {
    return userCount(id) > 0;
}

void Scene::removeResource(ResourceId id) {
    if (!meshes_.contains(id) && !materials_.contains(id) && !textures_.contains(id)) {
        unknownId("resource", id.value);
    }
    if (isReferenced(id)) {
        throw std::logic_error("Scene::removeResource: resource is still in use");
    }
    meshes_.erase(id);
    materials_.erase(id);
    textures_.erase(id);
    notify(SceneChange::Kind::ResourceRemoved, {}, id);
}

const MeshResource* Scene::mesh(ResourceId id) const {
    const auto it = meshes_.find(id);
    return it == meshes_.end() ? nullptr : &it->second;
}

const MaterialResource* Scene::material(ResourceId id) const {
    const auto it = materials_.find(id);
    return it == materials_.end() ? nullptr : &it->second;
}

const TextureResource* Scene::texture(ResourceId id) const {
    const auto it = textures_.find(id);
    return it == textures_.end() ? nullptr : &it->second;
}

void Scene::forEachMesh(const std::function<void(const MeshResource&)>& visit) const {
    for (const auto& [id, mesh] : meshes_) {
        visit(mesh);
    }
}

void Scene::forEachMaterial(const std::function<void(const MaterialResource&)>& visit) const {
    for (const auto& [id, material] : materials_) {
        visit(material);
    }
}

void Scene::forEachTexture(const std::function<void(const TextureResource&)>& visit) const {
    for (const auto& [id, texture] : textures_) {
        visit(texture);
    }
}

std::size_t Scene::userCount(ResourceId id) const {
    if (textures_.contains(id)) {
        return static_cast<std::size_t>(
            std::count_if(materials_.begin(), materials_.end(), [id](const auto& entry) {
                return usesTexture(entry.second.material, id);
            }));
    }
    return static_cast<std::size_t>(
        std::count_if(entities_.begin(), entities_.end(), [id](const auto& entry) {
            const auto& instance = entry.second.meshInstance;
            return instance && (instance->mesh == id || instance->material == id);
        }));
}

void Scene::setResourceName(ResourceId id, std::string name) {
    std::string* current = nullptr;
    if (const auto mesh = meshes_.find(id); mesh != meshes_.end()) {
        current = &mesh->second.name;
    } else if (const auto material = materials_.find(id); material != materials_.end()) {
        current = &material->second.name;
    } else if (const auto texture = textures_.find(id); texture != textures_.end()) {
        current = &texture->second.name;
    } else {
        unknownId("resource", id.value);
    }
    if (*current == name) {
        return;
    }
    *current = std::move(name);
    notify(SceneChange::Kind::ResourceChanged, {}, id);
}

void Scene::setEnvironment(const Environment& environment) {
    if (environment_ == environment) {
        return;
    }
    environment_ = environment;
    notify(SceneChange::Kind::EnvironmentChanged, {});
}

void Scene::setLook(const Look& look) {
    if (look_ == look) {
        return;
    }
    look_ = look;
    notify(SceneChange::Kind::LookChanged, {});
}

// --- Whole document ------------------------------------------------------------------------------

void Scene::validate(const SceneSnapshot& snapshot) {
    // Resource IDs are unique across kinds: removeResource and userCount look them up by ID alone.
    std::unordered_set<ResourceId> resourceIds;
    std::unordered_set<ResourceId> meshIds;
    std::unordered_set<ResourceId> materialIds;
    std::unordered_set<ResourceId> textureIds;
    resourceIds.reserve(snapshot.meshes.size() + snapshot.materials.size() +
                        snapshot.textures.size());
    const auto addResource = [&resourceIds](ResourceId id, std::unordered_set<ResourceId>& kind) {
        if (!id) {
            invalidSnapshot("a resource has no id");
        }
        if (!resourceIds.insert(id).second) {
            invalidSnapshot(std::format("resource id {:#018x} is used twice", id.value));
        }
        kind.insert(id);
    };
    for (const MeshResource& mesh : snapshot.meshes) {
        addResource(mesh.id, meshIds);
        if (const char* problem = meshProblem(mesh.data)) {
            invalidSnapshot(std::format("mesh {:#018x}: {}", mesh.id.value, problem));
        }
    }
    for (const TextureResource& texture : snapshot.textures) {
        addResource(texture.id, textureIds);
        if (const char* problem = imageProblem(texture.image)) {
            invalidSnapshot(std::format("texture {:#018x}: {}", texture.id.value, problem));
        }
    }
    for (const MaterialResource& material : snapshot.materials) {
        addResource(material.id, materialIds);
    }
    for (const MaterialResource& material : snapshot.materials) {
        checkTextureReferences(material.material,
                               [&textureIds](ResourceId id) { return textureIds.contains(id); });
    }

    std::unordered_map<EntityId, const Entity*> entities;
    entities.reserve(snapshot.entities.size());
    for (const Entity& entity : snapshot.entities) {
        if (!entity.id) {
            invalidSnapshot("an entity has no id");
        }
        if (!entities.emplace(entity.id, &entity).second) {
            invalidSnapshot(std::format("entity id {:#018x} is used twice", entity.id.value));
        }
        if (entity.meshInstance) {
            if (!meshIds.contains(entity.meshInstance->mesh)) {
                unknownId("mesh", entity.meshInstance->mesh.value);
            }
            const ResourceId material = entity.meshInstance->material;
            if (material && !materialIds.contains(material)) {
                unknownId("material", material.value);
            }
        }
    }
    for (const Entity& entity : snapshot.entities) {
        if (entity.parent && !entities.contains(entity.parent)) {
            unknownId("parent entity", entity.parent.value);
        }
    }

    // Every entity is listed exactly once: in `roots` without a parent, otherwise in its parent's
    // children.
    std::unordered_set<EntityId> listed;
    listed.reserve(entities.size());
    const auto list = [&entities, &listed](EntityId id, EntityId parent) {
        const auto it = entities.find(id);
        if (it == entities.end()) {
            unknownId("entity", id.value);
        }
        if (it->second->parent != parent) {
            invalidSnapshot(std::format("entity {:#018x} is listed under {:#018x} but its parent is "
                                        "{:#018x}",
                                        id.value, parent.value, it->second->parent.value));
        }
        if (!listed.insert(id).second) {
            invalidSnapshot(std::format("entity {:#018x} is listed twice", id.value));
        }
    };
    for (EntityId root : snapshot.roots) {
        list(root, {});
    }
    for (const Entity& entity : snapshot.entities) {
        for (EntityId child : entity.children) {
            list(child, entity.id);
        }
    }
    if (listed.size() != entities.size()) {
        invalidSnapshot("some entities are missing from the hierarchy");
    }

    // With every entity listed once, each is pushed at most once; entities in a parent cycle are
    // never reached from the roots.
    std::size_t reached = 0;
    std::vector<EntityId> stack(snapshot.roots.begin(), snapshot.roots.end());
    while (!stack.empty()) {
        const Entity& entity = *entities.at(stack.back());
        stack.pop_back();
        ++reached;
        stack.insert(stack.end(), entity.children.begin(), entity.children.end());
    }
    if (reached != entities.size()) {
        invalidSnapshot("the hierarchy contains a cycle");
    }
}

void Scene::replaceContents(SceneSnapshot snapshot) {
    validate(snapshot);

    // Build the new maps before touching the document, so running out of memory changes nothing.
    std::unordered_map<EntityId, Entity> entities;
    entities.reserve(snapshot.entities.size());
    for (Entity& entity : snapshot.entities) {
        const EntityId id = entity.id;
        entities.emplace(id, std::move(entity));
    }
    std::vector<ResourceId> added; // dependency order: textures, materials, meshes
    added.reserve(snapshot.textures.size() + snapshot.materials.size() + snapshot.meshes.size());
    std::unordered_map<ResourceId, TextureResource> textures;
    textures.reserve(snapshot.textures.size());
    for (TextureResource& texture : snapshot.textures) {
        added.push_back(texture.id);
        textures.emplace(texture.id, std::move(texture));
    }
    std::unordered_map<ResourceId, MaterialResource> materials;
    materials.reserve(snapshot.materials.size());
    for (MaterialResource& material : snapshot.materials) {
        added.push_back(material.id);
        materials.emplace(material.id, std::move(material));
    }
    std::unordered_map<ResourceId, MeshResource> meshes;
    meshes.reserve(snapshot.meshes.size());
    for (MeshResource& mesh : snapshot.meshes) {
        added.push_back(mesh.id);
        mesh.bounds = computeBounds(mesh.data.positions);
        meshes.emplace(mesh.id, std::move(mesh));
    }

    // Remove the old document the way destroyEntity/removeResource do: subtrees children first,
    // then resources once nothing references them (materials before the textures they use).
    while (!roots_.empty()) {
        const EntityId root = roots_.back();
        roots_.pop_back();
        for (EntityId removed : subtreeChildrenFirst(entities_, root)) {
            entities_.erase(removed);
            notify(SceneChange::Kind::EntityRemoved, removed);
        }
    }
    const auto removeAll = [this](auto& resources) {
        while (!resources.empty()) {
            const ResourceId id = resources.begin()->first;
            resources.erase(resources.begin());
            notify(SceneChange::Kind::ResourceRemoved, {}, id);
        }
    };
    removeAll(meshes_);
    removeAll(materials_);
    removeAll(textures_);

    entities_ = std::move(entities);
    roots_ = std::move(snapshot.roots);
    meshes_ = std::move(meshes);
    materials_ = std::move(materials);
    textures_ = std::move(textures);
    environment_ = snapshot.environment;
    look_ = snapshot.look;

    for (ResourceId id : added) {
        notify(SceneChange::Kind::ResourceAdded, {}, id);
    }
    std::vector<EntityId> pending(roots_.rbegin(), roots_.rend());
    while (!pending.empty()) {
        const EntityId id = pending.back();
        pending.pop_back();
        notify(SceneChange::Kind::EntityAdded, id);
        const std::vector<EntityId>& children = entities_.at(id).children;
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    notify(SceneChange::Kind::EnvironmentChanged, {});
    notify(SceneChange::Kind::LookChanged, {});
}

void Scene::clear() {
    replaceContents(SceneSnapshot{});
}

// --- Entities ------------------------------------------------------------------------------------

EntityId Scene::createEntity(std::string name, EntityId parent, const Transform& transform) {
    if (parent) {
        require(parent);
    }
    const auto id = newId<EntityId>(entities_);
    Entity entity;
    entity.id = id;
    entity.name = std::move(name);
    entity.parent = parent;
    entity.transform = transform;
    entities_.emplace(id, std::move(entity));
    (parent ? require(parent).children : roots_).push_back(id);
    notify(SceneChange::Kind::EntityAdded, id);
    return id;
}

void Scene::destroyEntity(EntityId id) {
    const Entity& root = require(id);
    const std::vector<EntityId> doomed = subtreeChildrenFirst(entities_, id);
    std::vector<EntityId>& siblings = siblingsOf(root);
    siblings.erase(std::remove(siblings.begin(), siblings.end(), id), siblings.end());
    for (EntityId removed : doomed) {
        entities_.erase(removed);
        notify(SceneChange::Kind::EntityRemoved, removed);
    }
}

EntityId Scene::duplicate(EntityId id) {
    const Entity& original = require(id);
    const EntityId parent = original.parent;

    // Copy the subtree breadth-first: each copied entity is created under its copied parent.
    std::vector<std::pair<EntityId, EntityId>> queue{{id, parent}}; // (source, destination parent)
    EntityId rootCopy;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const auto [sourceId, destinationParent] = queue[i];
        const Entity source = require(sourceId); // copy: createEntity may rehash the map
        const EntityId copy = createEntity(source.name, destinationParent, source.transform);
        if (source.meshInstance) {
            setMeshInstance(copy, source.meshInstance);
        }
        if (source.light) {
            setLight(copy, source.light);
        }
        if (!rootCopy) {
            rootCopy = copy;
        }
        for (EntityId child : source.children) {
            queue.emplace_back(child, copy);
        }
    }

    // createEntity appended the copy last; move it right after the original.
    std::vector<EntityId>& siblings = parent ? require(parent).children : roots_;
    siblings.erase(std::remove(siblings.begin(), siblings.end(), rootCopy), siblings.end());
    siblings.insert(std::find(siblings.begin(), siblings.end(), id) + 1, rootCopy);
    notify(SceneChange::Kind::EntityReparented, rootCopy);
    return rootCopy;
}

void Scene::setName(EntityId id, std::string name) {
    Entity& entity = require(id);
    if (entity.name == name) {
        return;
    }
    entity.name = std::move(name);
    notify(SceneChange::Kind::EntityRenamed, id);
}

void Scene::setParent(EntityId id, EntityId parent) {
    Entity& entity = require(id);
    if (entity.parent == parent) {
        return;
    }
    for (EntityId ancestor = parent; ancestor; ancestor = require(ancestor).parent) {
        if (ancestor == id) {
            throw std::invalid_argument("Scene::setParent: would create a cycle");
        }
    }
    std::vector<EntityId>& oldSiblings = siblingsOf(entity);
    oldSiblings.erase(std::remove(oldSiblings.begin(), oldSiblings.end(), id), oldSiblings.end());
    entity.parent = parent;
    siblingsOf(entity).push_back(id);
    notify(SceneChange::Kind::EntityReparented, id);
}

void Scene::setTransform(EntityId id, const Transform& transform) {
    Entity& entity = require(id);
    if (entity.transform == transform) {
        return;
    }
    entity.transform = transform;
    notify(SceneChange::Kind::TransformChanged, id);
}

void Scene::setMeshInstance(EntityId id, std::optional<MeshInstance> instance) {
    Entity& entity = require(id);
    if (instance) {
        if (!meshes_.contains(instance->mesh)) {
            unknownId("mesh", instance->mesh.value);
        }
        if (instance->material && !materials_.contains(instance->material)) {
            unknownId("material", instance->material.value);
        }
    }
    if (entity.meshInstance == instance) {
        return;
    }
    entity.meshInstance = instance;
    notify(SceneChange::Kind::MeshInstanceChanged, id);
}

void Scene::setLight(EntityId id, std::optional<Light> light) {
    Entity& entity = require(id);
    if (entity.light == light) {
        return;
    }
    entity.light = light;
    notify(SceneChange::Kind::LightChanged, id);
}

const Entity* Scene::entity(EntityId id) const {
    const auto it = entities_.find(id);
    return it == entities_.end() ? nullptr : &it->second;
}

glm::mat4 Scene::worldMatrix(EntityId id) const {
    glm::mat4 world(1.0f);
    for (EntityId current = id; current;) {
        const Entity& entity = require(current);
        world = entity.transform.matrix() * world;
        current = entity.parent;
    }
    return world;
}

std::optional<Bounds> Scene::worldBounds() const {
    std::optional<Bounds> result;
    traverse([&](const Entity& entity, const glm::mat4& world) {
        if (!entity.meshInstance) {
            return;
        }
        const Bounds b = transformBounds(meshes_.at(entity.meshInstance->mesh).bounds, world);
        if (!result) {
            result = b;
        } else {
            result->min = glm::min(result->min, b.min);
            result->max = glm::max(result->max, b.max);
        }
    });
    return result;
}

void Scene::traverse(const Visitor& visit) const {
    struct Item {
        EntityId id;
        glm::mat4 parentWorld;
    };
    std::vector<Item> stack;
    for (auto it = roots_.rbegin(); it != roots_.rend(); ++it) {
        stack.push_back({*it, glm::mat4(1.0f)});
    }
    while (!stack.empty()) {
        const Item item = stack.back();
        stack.pop_back();
        const Entity& entity = require(item.id);
        const glm::mat4 world = item.parentWorld * entity.transform.matrix();
        visit(entity, world);
        for (auto it = entity.children.rbegin(); it != entity.children.rend(); ++it) {
            stack.push_back({*it, world});
        }
    }
}

// --- Listeners -----------------------------------------------------------------------------------

Scene::ListenerId Scene::addListener(Listener listener) {
    const ListenerId id = nextListenerId_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

void Scene::removeListener(ListenerId id) {
    std::erase_if(listeners_, [id](const auto& entry) { return entry.first == id; });
}

std::vector<EntityId> meshEntitiesIn(const Scene& scene, std::span<const EntityId> ids) {
    std::vector<EntityId> result;
    std::unordered_set<EntityId> seen;
    for (const EntityId root : ids) {
        std::vector<EntityId> pending{root};
        while (!pending.empty()) {
            const EntityId id = pending.back();
            pending.pop_back();
            const Entity* entity = scene.entity(id);
            if (entity == nullptr || !seen.insert(id).second) {
                continue;
            }
            if (entity->meshInstance) {
                result.push_back(id);
            }
            pending.insert(pending.end(), entity->children.rbegin(), entity->children.rend());
        }
    }
    return result;
}

std::size_t applyMaterial(Scene& scene, std::span<const EntityId> ids, ResourceId material) {
    std::size_t changed = 0;
    for (const EntityId id : meshEntitiesIn(scene, ids)) {
        const MeshInstance current = *scene.entity(id)->meshInstance;
        if (current.material != material) {
            scene.setMeshInstance(id, MeshInstance{current.mesh, material});
            ++changed;
        }
    }
    return changed;
}

std::optional<Bounds> worldBoundsOf(const Scene& scene, std::span<const EntityId> ids) {
    std::optional<Bounds> result;
    for (const EntityId id : meshEntitiesIn(scene, ids)) {
        const MeshResource* mesh = scene.mesh(scene.entity(id)->meshInstance->mesh);
        if (mesh == nullptr) {
            continue;
        }
        const glm::mat4 world = scene.worldMatrix(id);
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 local((corner & 1) != 0 ? mesh->bounds.max.x : mesh->bounds.min.x,
                                  (corner & 2) != 0 ? mesh->bounds.max.y : mesh->bounds.min.y,
                                  (corner & 4) != 0 ? mesh->bounds.max.z : mesh->bounds.min.z);
            const glm::vec3 point(world * glm::vec4(local, 1.0f));
            if (!result) {
                result = Bounds{point, point};
            } else {
                result->min = glm::min(result->min, point);
                result->max = glm::max(result->max, point);
            }
        }
    }
    return result;
}

void moveBy(Scene& scene, EntityId id, const glm::vec3& worldOffset) {
    const Entity* entity = scene.entity(id);
    if (entity == nullptr) {
        return;
    }
    const glm::mat4 parentWorld =
        entity->parent ? scene.worldMatrix(entity->parent) : glm::mat4(1.0f);
    Transform transform = entity->transform;
    transform.position += glm::vec3(glm::inverse(parentWorld) * glm::vec4(worldOffset, 0.0f));
    scene.setTransform(id, transform);
}

std::vector<EntityId> outermostEntities(const Scene& scene, std::span<const EntityId> ids) {
    const std::unordered_set<EntityId> listed(ids.begin(), ids.end());
    std::vector<EntityId> result;
    for (const EntityId id : ids) {
        const Entity* entity = scene.entity(id);
        if (entity == nullptr) {
            continue;
        }
        bool inside = false;
        for (EntityId up = entity->parent; up && !inside; up = scene.entity(up)->parent) {
            inside = listed.contains(up);
        }
        if (!inside && std::find(result.begin(), result.end(), id) == result.end()) {
            result.push_back(id);
        }
    }
    return result;
}

} // namespace lb
