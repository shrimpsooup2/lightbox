#include <stdexcept>
#include <unordered_set>
#include <vector>

#include <doctest/doctest.h>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/vec4.hpp>

#include "core/primitives.hpp"
#include "core/scene/scene.hpp"

using namespace lb;

namespace {

glm::vec3 worldPosition(const Scene& scene, EntityId id) {
    return glm::vec3(scene.worldMatrix(id) * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
}

bool near(glm::vec3 a, glm::vec3 b) {
    return glm::length(a - b) < 1e-4f;
}

Transform at(glm::vec3 position) {
    Transform t;
    t.position = position;
    return t;
}

} // namespace

TEST_CASE("world matrix composes parent and child transforms") {
    Scene scene;
    Transform parentTransform = at({10.0f, 0.0f, 0.0f});
    parentTransform.rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
    const EntityId parent = scene.createEntity("parent", {}, parentTransform);
    const EntityId child = scene.createEntity("child", parent, at({5.0f, 0.0f, 0.0f}));

    // The parent's 90° turn about Z maps the child's local +X onto world +Y.
    CHECK(near(worldPosition(scene, child), {10.0f, 5.0f, 0.0f}));

    scene.setTransform(parent, at({0.0f, 0.0f, 3.0f}));
    CHECK(near(worldPosition(scene, child), {5.0f, 0.0f, 3.0f}));
}

TEST_CASE("destroying an entity removes its subtree, children first") {
    Scene scene;
    const EntityId root = scene.createEntity("root");
    const EntityId child = scene.createEntity("child", root);
    const EntityId grandchild = scene.createEntity("grandchild", child);
    const EntityId sibling = scene.createEntity("sibling", root);

    std::vector<EntityId> removed;
    scene.addListener([&](const SceneChange& change) {
        if (change.kind == SceneChange::Kind::EntityRemoved) {
            removed.push_back(change.entity);
        }
    });
    scene.destroyEntity(child);

    CHECK(removed == std::vector<EntityId>{grandchild, child});
    CHECK(scene.entity(child) == nullptr);
    CHECK(scene.entity(grandchild) == nullptr);
    CHECK(scene.entity(root)->children == std::vector<EntityId>{sibling});
    CHECK(scene.entityCount() == 2);
}

TEST_CASE("reparenting moves the entity and rejects cycles") {
    Scene scene;
    const EntityId a = scene.createEntity("a");
    const EntityId b = scene.createEntity("b", a);
    const EntityId c = scene.createEntity("c");

    scene.setParent(b, c);
    CHECK(scene.entity(a)->children.empty());
    CHECK(scene.entity(c)->children == std::vector<EntityId>{b});
    CHECK(scene.entity(b)->parent == c);

    CHECK_THROWS_AS(scene.setParent(c, b), std::invalid_argument); // c is b's ancestor
    CHECK_THROWS_AS(scene.setParent(c, c), std::invalid_argument);
    CHECK(scene.entity(c)->parent == EntityId{});

    scene.setParent(b, {});
    CHECK(std::vector<EntityId>(scene.roots().begin(), scene.roots().end()) ==
          std::vector<EntityId>{a, c, b});
}

TEST_CASE("resources cannot be removed while an entity uses them") {
    Scene scene;
    const ResourceId mesh = scene.addMesh("box", makeBox({0, 0, 0}, {1, 1, 1}));
    const ResourceId material = scene.addMaterial("red", materialWithColor({1.0f, 0.0f, 0.0f}));
    const EntityId box = scene.createEntity("box");
    scene.setMeshInstance(box, MeshInstance{mesh, material});

    CHECK_THROWS_AS(scene.removeResource(mesh), std::logic_error);
    CHECK_THROWS_AS(scene.removeResource(material), std::logic_error);

    scene.setMeshInstance(box, std::nullopt);
    scene.removeResource(mesh);
    scene.removeResource(material);
    CHECK(scene.mesh(mesh) == nullptr);
    CHECK(scene.material(material) == nullptr);
}

TEST_CASE("mesh instances must reference existing resources") {
    Scene scene;
    const ResourceId mesh = scene.addMesh("box", makeBox({0, 0, 0}, {1, 1, 1}));
    const EntityId box = scene.createEntity("box");
    CHECK_THROWS_AS(scene.setMeshInstance(box, MeshInstance{ResourceId{12345}, {}}),
                    std::invalid_argument);
    CHECK_THROWS_AS(scene.setMeshInstance(box, MeshInstance{mesh, ResourceId{12345}}),
                    std::invalid_argument);
    CHECK_FALSE(scene.entity(box)->meshInstance.has_value());
}

TEST_CASE("invalid meshes are rejected") {
    Scene scene;
    MeshData noNormals = makeBox({0, 0, 0}, {1, 1, 1});
    noNormals.normals.clear();
    CHECK_THROWS_AS(scene.addMesh("broken", noNormals), std::invalid_argument);
    CHECK_THROWS_AS(scene.addMesh("empty", MeshData{}), std::invalid_argument);
}

TEST_CASE("world bounds follow transforms of every instance") {
    Scene scene;
    CHECK_FALSE(scene.worldBounds().has_value());

    const ResourceId cube = scene.addMesh("cube", makeBox({0, 0, 0}, {1, 1, 1}));
    Transform turned;
    turned.scale = {4.0f, 1.0f, 1.0f};
    turned.rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
    const EntityId beam = scene.createEntity("beam", {}, turned);
    scene.setMeshInstance(beam, MeshInstance{cube, {}});

    // A 4 m beam along X, turned 90° about Z, now runs along +Y and sits at x in [-1, 0].
    std::optional<Bounds> bounds = scene.worldBounds();
    REQUIRE(bounds.has_value());
    CHECK(near(bounds->min, {-1.0f, 0.0f, 0.0f}));
    CHECK(near(bounds->max, {0.0f, 4.0f, 1.0f}));

    const EntityId far = scene.createEntity("far", {}, at({20.0f, 0.0f, 0.0f}));
    scene.setMeshInstance(far, MeshInstance{cube, {}});
    bounds = scene.worldBounds();
    CHECK(near(bounds->max, {21.0f, 4.0f, 1.0f}));
}

TEST_CASE("listeners hear real changes only, until removed") {
    Scene scene;
    const EntityId e = scene.createEntity("e");
    std::vector<SceneChange::Kind> kinds;
    const Scene::ListenerId listener =
        scene.addListener([&](const SceneChange& change) { kinds.push_back(change.kind); });

    scene.setTransform(e, Transform{}); // unchanged: silent
    scene.setName(e, "e");              // unchanged: silent
    scene.setTransform(e, at({1.0f, 0.0f, 0.0f}));
    scene.setName(e, "renamed");
    CHECK(kinds == std::vector<SceneChange::Kind>{SceneChange::Kind::TransformChanged,
                                                  SceneChange::Kind::EntityRenamed});

    scene.removeListener(listener);
    scene.setTransform(e, at({2.0f, 0.0f, 0.0f}));
    CHECK(kinds.size() == 2);
}

TEST_CASE("entity ids are non-zero and unique") {
    Scene scene;
    std::unordered_set<std::uint64_t> seen;
    for (int i = 0; i < 10000; ++i) {
        const EntityId id = scene.createEntity("e");
        CHECK_FALSE(id.value == 0);
        seen.insert(id.value);
    }
    CHECK(seen.size() == 10000);
}

namespace {

constexpr EntityId kHouse{0x1000};
constexpr EntityId kWall{0x1001};
constexpr EntityId kDoor{0x1002};
constexpr EntityId kTree{0x1003};
constexpr ResourceId kBoxMesh{0x2000};
constexpr ResourceId kBrick{0x2001};
constexpr ResourceId kBrickTexture{0x2002};

Entity makeEntity(EntityId id, std::string name, EntityId parent, std::vector<EntityId> children) {
    Entity entity;
    entity.id = id;
    entity.name = std::move(name);
    entity.parent = parent;
    entity.children = std::move(children);
    return entity;
}

// House { Wall { Door }, }, Tree, with one mesh, material and texture.
SceneSnapshot validSnapshot() {
    SceneSnapshot snapshot;
    snapshot.entities.push_back(makeEntity(kHouse, "House", {}, {kWall}));
    snapshot.entities.push_back(makeEntity(kWall, "Wall", kHouse, {kDoor}));
    snapshot.entities.push_back(makeEntity(kDoor, "Door", kWall, {}));
    snapshot.entities.push_back(makeEntity(kTree, "Tree", {}, {}));
    snapshot.entities[1].meshInstance = MeshInstance{kBoxMesh, kBrick};
    snapshot.entities[1].transform.position = {1.0f, 2.0f, 3.0f};
    snapshot.roots = {kTree, kHouse};

    MeshResource mesh;
    mesh.id = kBoxMesh;
    mesh.name = "box";
    mesh.data = makeBox({0, 0, 0}, {2, 1, 1});
    mesh.bounds = Bounds{{9.0f, 9.0f, 9.0f}, {9.0f, 9.0f, 9.0f}}; // wrong on purpose: recomputed
    snapshot.meshes.push_back(std::move(mesh));

    TextureResource texture;
    texture.id = kBrickTexture;
    texture.name = "brick";
    texture.image = ImageData{1, 1, {200, 80, 60, 255}};
    snapshot.textures.push_back(std::move(texture));

    MaterialResource material;
    material.id = kBrick;
    material.name = "Brick";
    material.material.baseColorMap.texture = kBrickTexture;
    snapshot.materials.push_back(material);

    snapshot.environment.sun.mode = SunMode::Location;
    snapshot.look.clay = true;
    return snapshot;
}

Entity& entityIn(SceneSnapshot& snapshot, EntityId id) {
    return *std::find_if(snapshot.entities.begin(), snapshot.entities.end(),
                         [id](const Entity& entity) { return entity.id == id; });
}

std::vector<EntityId> rootsOf(const Scene& scene) {
    return {scene.roots().begin(), scene.roots().end()};
}

} // namespace

TEST_CASE("replaceContents installs a snapshot with its ids") {
    Scene scene;
    const EntityId old = scene.createEntity("old");
    scene.replaceContents(validSnapshot());

    CHECK(scene.entity(old) == nullptr);
    CHECK(scene.entityCount() == 4);
    CHECK(rootsOf(scene) == std::vector<EntityId>{kTree, kHouse});
    REQUIRE(scene.entity(kWall) != nullptr);
    CHECK(scene.entity(kWall)->parent == kHouse);
    CHECK(scene.entity(kWall)->children == std::vector<EntityId>{kDoor});
    CHECK(scene.entity(kWall)->meshInstance == MeshInstance{kBoxMesh, kBrick});
    CHECK(near(worldPosition(scene, kDoor), {1.0f, 2.0f, 3.0f}));
    REQUIRE(scene.mesh(kBoxMesh) != nullptr);
    CHECK(near(scene.mesh(kBoxMesh)->bounds.max, {2.0f, 1.0f, 1.0f}));
    CHECK(scene.material(kBrick)->material.baseColorMap.texture == kBrickTexture);
    CHECK(scene.texture(kBrickTexture)->image.pixels.size() == 4);
    CHECK(scene.userCount(kBrickTexture) == 1);
    CHECK(scene.environment().sun.mode == SunMode::Location);
    CHECK(scene.look().clay);

    // New content still gets fresh ids that don't collide with the installed ones.
    const EntityId added = scene.createEntity("new", kHouse);
    CHECK(added != kHouse);
    CHECK(scene.entity(kHouse)->children == std::vector<EntityId>{kWall, added});
}

TEST_CASE("replaceContents rejects invalid snapshots without changing the scene") {
    Scene scene;
    const EntityId keep = scene.createEntity("keep");
    const ResourceId keepMesh = scene.addMesh("keep", makeBox({0, 0, 0}, {1, 1, 1}));
    int notifications = 0;
    scene.addListener([&notifications](const SceneChange&) { ++notifications; });

    const auto rejected = [&scene](SceneSnapshot snapshot) {
        CHECK_THROWS_AS(scene.replaceContents(std::move(snapshot)), std::invalid_argument);
    };
    {
        SceneSnapshot s = validSnapshot(); // dangling parent
        entityIn(s, kTree).parent = EntityId{0x9999};
        rejected(s);
    }
    {
        SceneSnapshot s = validSnapshot(); // child listed twice
        entityIn(s, kWall).children.push_back(kDoor);
        rejected(s);
    }
    {
        SceneSnapshot s = validSnapshot(); // listed under the wrong parent
        entityIn(s, kHouse).children.push_back(kDoor);
        rejected(s);
    }
    {
        SceneSnapshot s = validSnapshot(); // missing from the hierarchy
        s.roots = {kHouse};
        rejected(s);
    }
    {
        SceneSnapshot s = validSnapshot(); // cycle: House and Tree parent each other, no roots
        entityIn(s, kHouse).parent = kTree;
        entityIn(s, kTree).parent = kHouse;
        entityIn(s, kTree).children = {kHouse};
        entityIn(s, kHouse).children = {kWall, kTree};
        s.roots.clear();
        rejected(s);
    }
    {
        SceneSnapshot s = validSnapshot(); // duplicate entity id
        s.entities.push_back(makeEntity(kTree, "Tree 2", {}, {}));
        rejected(s);
    }
    {
        SceneSnapshot s = validSnapshot(); // unknown material
        entityIn(s, kWall).meshInstance->material = ResourceId{0x7777};
        rejected(s);
    }
    {
        SceneSnapshot s = validSnapshot(); // unknown mesh
        entityIn(s, kWall).meshInstance->mesh = ResourceId{0x7777};
        rejected(s);
    }
    {
        SceneSnapshot s = validSnapshot(); // unknown texture
        s.materials[0].material.normalMap.texture = ResourceId{0x7777};
        rejected(s);
    }
    {
        SceneSnapshot s = validSnapshot(); // resource ids are unique across kinds
        s.textures[0].id = kBrick;
        s.materials[0].material.baseColorMap.texture = kBrick;
        rejected(s);
    }
    {
        SceneSnapshot s = validSnapshot(); // uvs don't match the vertices
        s.meshes[0].data.uvs.resize(1);
        rejected(s);
    }
    {
        SceneSnapshot s = validSnapshot(); // pixel data doesn't match the size
        s.textures[0].image.width = 2;
        rejected(s);
    }

    CHECK(notifications == 0);
    CHECK(rootsOf(scene) == std::vector<EntityId>{keep});
    CHECK(scene.entityCount() == 1);
    CHECK(scene.mesh(keepMesh) != nullptr);
    CHECK(scene.entity(kHouse) == nullptr);
}

TEST_CASE("replaceContents notifies removals, then additions, then environment and look") {
    Scene scene;
    const EntityId oldParent = scene.createEntity("old parent");
    const EntityId oldChild = scene.createEntity("old child", oldParent);
    const ResourceId oldMesh = scene.addMesh("old", makeBox({0, 0, 0}, {1, 1, 1}));
    scene.setMeshInstance(oldChild, MeshInstance{oldMesh, {}});

    std::vector<SceneChange> changes;
    scene.addListener([&](const SceneChange& change) {
        // Every notification sees a consistent document.
        if (change.kind == SceneChange::Kind::EntityRemoved) {
            CHECK(scene.entity(change.entity) == nullptr);
        }
        if (change.kind == SceneChange::Kind::EntityAdded) {
            const Entity* added = scene.entity(change.entity);
            REQUIRE(added != nullptr);
            if (added->meshInstance) {
                CHECK(scene.mesh(added->meshInstance->mesh) != nullptr);
            }
        }
        changes.push_back(change);
    });
    scene.replaceContents(validSnapshot());

    using Kind = SceneChange::Kind;
    std::vector<EntityId> removed;
    std::vector<EntityId> added;
    std::vector<ResourceId> removedResources;
    std::vector<ResourceId> addedResources;
    for (const SceneChange& change : changes) {
        if (change.kind == Kind::EntityRemoved) {
            removed.push_back(change.entity);
        } else if (change.kind == Kind::EntityAdded) {
            added.push_back(change.entity);
        } else if (change.kind == Kind::ResourceRemoved) {
            removedResources.push_back(change.resource);
        } else if (change.kind == Kind::ResourceAdded) {
            addedResources.push_back(change.resource);
        }
    }
    CHECK(removed == std::vector<EntityId>{oldChild, oldParent});
    CHECK(removedResources == std::vector<ResourceId>{oldMesh});
    CHECK(added == std::vector<EntityId>{kTree, kHouse, kWall, kDoor}); // parents first, display order
    CHECK(addedResources == std::vector<ResourceId>{kBrickTexture, kBrick, kBoxMesh});

    const auto firstAddition = std::find_if(changes.begin(), changes.end(), [](const SceneChange& c) {
        return c.kind == Kind::EntityAdded || c.kind == Kind::ResourceAdded;
    });
    CHECK(std::none_of(firstAddition, changes.end(), [](const SceneChange& c) {
        return c.kind == Kind::EntityRemoved || c.kind == Kind::ResourceRemoved;
    }));
    REQUIRE(changes.size() >= 2);
    CHECK(changes[changes.size() - 2].kind == Kind::EnvironmentChanged);
    CHECK(changes.back().kind == Kind::LookChanged);
}

TEST_CASE("clear empties the document and resets environment and look") {
    Scene scene;
    scene.replaceContents(validSnapshot());
    std::vector<SceneChange::Kind> kinds;
    scene.addListener([&kinds](const SceneChange& change) { kinds.push_back(change.kind); });
    scene.clear();

    CHECK(scene.entityCount() == 0);
    CHECK(scene.roots().empty());
    CHECK(scene.mesh(kBoxMesh) == nullptr);
    CHECK(scene.material(kBrick) == nullptr);
    CHECK(scene.texture(kBrickTexture) == nullptr);
    CHECK(scene.environment() == Environment{});
    CHECK(scene.look() == Look{});
    CHECK_FALSE(scene.worldBounds().has_value());
    REQUIRE(kinds.size() == 4 + 3 + 2);
    CHECK(kinds.back() == SceneChange::Kind::LookChanged);
}
