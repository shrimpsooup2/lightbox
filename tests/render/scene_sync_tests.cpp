#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include <doctest/doctest.h>
#include <glm/geometric.hpp>
#include <glm/vec4.hpp>

#include "core/image.hpp"
#include "core/primitives.hpp"
#include "core/scene/scene.hpp"
#include "render/scene_sync.hpp"

using namespace lb;

namespace {

// Stands in for the GPU: hands out mesh/texture handles and records uploads/releases.
struct FakeGpu {
    std::uint32_t next = 0;
    std::vector<std::uint32_t> uploaded;
    std::vector<std::uint32_t> released;
    std::uint32_t nextTexture = 0;
    std::vector<std::uint32_t> texturesUploaded;
    std::vector<std::uint32_t> texturesReleased;
    bool failTextureUploads = false;

    render::SceneSync::MeshUploader uploader() {
        return [this](const MeshData&) {
            uploaded.push_back(next);
            return render::MeshHandle{next++};
        };
    }
    render::SceneSync::MeshReleaser releaser() {
        return [this](render::MeshHandle handle) { released.push_back(handle.index); };
    }
    render::SceneSync::TextureUploader textureUploader() {
        return [this](const ImageData&) {
            if (failTextureUploads) {
                throw std::runtime_error("texture limit reached");
            }
            texturesUploaded.push_back(nextTexture);
            return render::TextureHandle{nextTexture++};
        };
    }
    render::SceneSync::TextureReleaser textureReleaser() {
        return [this](render::TextureHandle handle) { texturesReleased.push_back(handle.index); };
    }
};

render::SceneSync makeSync(Scene& scene, FakeGpu& gpu) {
    return render::SceneSync(scene, gpu.uploader(), gpu.releaser(), gpu.textureUploader(),
                             gpu.textureReleaser());
}

Transform at(glm::vec3 position) {
    Transform t;
    t.position = position;
    return t;
}

glm::vec3 translation(const render::DrawItem& item) {
    return glm::vec3(item.transform * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
}

bool near(glm::vec3 a, glm::vec3 b) {
    return glm::length(a - b) < 1e-4f;
}

ImageData pixel() {
    ImageData image;
    image.width = 1;
    image.height = 1;
    image.pixels = {255, 128, 0, 255};
    return image;
}

const render::MaterialParams& materialOf(const render::SceneSync& sync, std::size_t draw) {
    return sync.materials()[sync.draws()[draw].material];
}

} // namespace

TEST_CASE("draw list mirrors mesh instances with world transforms and materials") {
    Scene scene;
    FakeGpu gpu;
    const ResourceId cube = scene.addMesh("cube", makeBox({0, 0, 0}, {1, 1, 1}));
    const ResourceId red = scene.addMaterial("red", materialWithColor({1.0f, 0.0f, 0.0f}));
    const EntityId group = scene.createEntity("group", {}, at({10.0f, 0.0f, 0.0f}));
    const EntityId first = scene.createEntity("first", group);
    const EntityId second = scene.createEntity("second", group, at({0.0f, 2.0f, 0.0f}));
    scene.createEntity("empty", group); // no mesh instance: not drawn
    scene.setMeshInstance(first, MeshInstance{cube, red});
    scene.setMeshInstance(second, MeshInstance{cube, {}});

    render::SceneSync sync = makeSync(scene, gpu);
    sync.update();

    REQUIRE(sync.draws().size() == 2);
    REQUIRE(sync.materials().size() == 2); // default clay + red
    CHECK(gpu.uploaded.size() == 1);        // shared mesh uploaded once
    CHECK(sync.draws()[0].material != 0);
    CHECK(materialOf(sync, 0).baseColor == glm::vec3(1.0f, 0.0f, 0.0f));
    CHECK(sync.draws()[1].material == 0); // no material: default clay
    CHECK(materialOf(sync, 1).baseColor == Material{}.baseColor);
    CHECK(near(translation(sync.draws()[1]), {10.0f, 2.0f, 0.0f}));

    scene.setMaterial(red, materialWithColor({0.0f, 0.0f, 1.0f}));
    sync.update();
    CHECK(materialOf(sync, 0).baseColor == glm::vec3(0.0f, 0.0f, 1.0f));
}

TEST_CASE("materials map every parameter and texture slot") {
    Scene scene;
    FakeGpu gpu;
    const ResourceId cube = scene.addMesh("cube", makeBox({0, 0, 0}, {1, 1, 1}));
    const ResourceId color = scene.addTexture("color", pixel());
    const ResourceId packed = scene.addTexture("packed", pixel());
    Material material = materialWithColor({0.5f, 0.25f, 1.0f});
    material.roughness = 0.3f;
    material.metalness = 0.7f;
    material.baseColorMap.texture = color;
    material.roughnessMap = TextureSlot{packed, 1, true};
    material.metalnessMap = TextureSlot{packed, 2, false};
    material.normalStrength = 0.5f;
    material.normalFlipY = true;
    material.mapping = TextureMapping::Box;
    material.textureScale = {2.0f, 3.0f};
    material.textureRotationDegrees = 45.0f;
    material.textureOffset = {0.25f, 0.5f};
    const ResourceId brick = scene.addMaterial("brick", material);
    const EntityId wall = scene.createEntity("wall");
    scene.setMeshInstance(wall, MeshInstance{cube, brick});

    render::SceneSync sync = makeSync(scene, gpu);
    sync.update();

    REQUIRE(sync.draws().size() == 1);
    const render::MaterialParams& p = materialOf(sync, 0);
    CHECK(p.baseColor == glm::vec3(0.5f, 0.25f, 1.0f));
    CHECK(p.roughness == 0.3f);
    CHECK(p.metalness == 0.7f);
    CHECK(gpu.texturesUploaded.size() == 2); // the packed map is uploaded once for both slots
    CHECK(p.baseColorMap.texture.valid());
    CHECK(p.roughnessMap.texture.index == p.metalnessMap.texture.index);
    CHECK(p.roughnessMap.texture.index != p.baseColorMap.texture.index);
    CHECK(p.roughnessMap.channel == 1);
    CHECK(p.roughnessMap.invert);
    CHECK(p.metalnessMap.channel == 2);
    CHECK_FALSE(p.metalnessMap.invert);
    CHECK_FALSE(p.normalMap.texture.valid()); // unused slots stay empty
    CHECK_FALSE(p.occlusionMap.texture.valid());
    CHECK(p.normalStrength == 0.5f);
    CHECK(p.normalFlipY);
    CHECK(p.mapping == render::TextureMapping::Box);
    CHECK(p.textureScale == glm::vec2(2.0f, 3.0f));
    CHECK(p.textureRotationDegrees == 45.0f);
    CHECK(p.textureOffset == glm::vec2(0.25f, 0.5f));
}

TEST_CASE("a texture shared by two materials is uploaded once") {
    Scene scene;
    FakeGpu gpu;
    const ResourceId cube = scene.addMesh("cube", makeBox({0, 0, 0}, {1, 1, 1}));
    const ResourceId wood = scene.addTexture("wood", pixel());
    Material light = materialWithColor({1.0f, 1.0f, 1.0f});
    light.baseColorMap.texture = wood;
    Material dark = materialWithColor({0.2f, 0.2f, 0.2f});
    dark.baseColorMap.texture = wood;
    const ResourceId lightId = scene.addMaterial("light", light);
    const ResourceId darkId = scene.addMaterial("dark", dark);
    scene.setMeshInstance(scene.createEntity("a"), MeshInstance{cube, lightId});
    scene.setMeshInstance(scene.createEntity("b"), MeshInstance{cube, darkId});
    scene.setMeshInstance(scene.createEntity("c"), MeshInstance{cube, darkId});

    render::SceneSync sync = makeSync(scene, gpu);
    sync.update();

    REQUIRE(sync.draws().size() == 3);
    CHECK(sync.materials().size() == 3); // clay + light + dark: shared materials appear once
    CHECK(gpu.texturesUploaded.size() == 1);
    for (std::size_t i = 0; i < sync.draws().size(); ++i) {
        CHECK(materialOf(sync, i).baseColorMap.texture.index == gpu.texturesUploaded.front());
    }

    // Edits rebuild the materials without re-uploading textures.
    dark.roughness = 0.9f;
    scene.setMaterial(darkId, dark);
    sync.update();
    CHECK(gpu.texturesUploaded.size() == 1);
}

TEST_CASE("removing a texture resource releases its GPU texture") {
    Scene scene;
    FakeGpu gpu;
    const ResourceId cube = scene.addMesh("cube", makeBox({0, 0, 0}, {1, 1, 1}));
    const ResourceId tiles = scene.addTexture("tiles", pixel());
    Material material;
    material.baseColorMap.texture = tiles;
    const ResourceId floor = scene.addMaterial("floor", material);
    scene.setMeshInstance(scene.createEntity("floor"), MeshInstance{cube, floor});
    render::SceneSync sync = makeSync(scene, gpu);
    sync.update();
    REQUIRE(gpu.texturesUploaded.size() == 1);

    scene.setMaterial(floor, Material{});
    scene.removeResource(tiles);
    sync.update();
    CHECK(gpu.texturesReleased == gpu.texturesUploaded);
    REQUIRE(sync.draws().size() == 1);
    CHECK_FALSE(materialOf(sync, 0).baseColorMap.texture.valid());
}

TEST_CASE("textures that fail to upload or were never used don't break the sync") {
    Scene scene;
    FakeGpu gpu;
    gpu.failTextureUploads = true;
    const ResourceId cube = scene.addMesh("cube", makeBox({0, 0, 0}, {1, 1, 1}));
    const ResourceId unused = scene.addTexture("unused", pixel());
    const ResourceId broken = scene.addTexture("broken", pixel());
    Material material = materialWithColor({0.1f, 0.2f, 0.3f});
    material.baseColorMap.texture = broken;
    material.normalMap.texture = broken;
    const ResourceId glass = scene.addMaterial("glass", material);
    scene.setMeshInstance(scene.createEntity("pane"), MeshInstance{cube, glass});

    render::SceneSync sync = makeSync(scene, gpu);
    sync.update();
    REQUIRE(sync.draws().size() == 1);
    CHECK(materialOf(sync, 0).baseColor == glm::vec3(0.1f, 0.2f, 0.3f)); // material still applies
    CHECK_FALSE(materialOf(sync, 0).baseColorMap.texture.valid());
    CHECK_FALSE(materialOf(sync, 0).normalMap.texture.valid());
    CHECK(gpu.texturesUploaded.empty());

    // Removing a texture the sync never uploaded releases nothing.
    scene.removeResource(unused);
    sync.update();
    CHECK(gpu.texturesReleased.empty());

    // Once uploads work again (a freed slot), the texture appears.
    gpu.failTextureUploads = false;
    const ResourceId spare = scene.addTexture("spare", pixel());
    Material spareMaterial;
    spareMaterial.baseColorMap.texture = spare;
    const ResourceId spareId = scene.addMaterial("spare", spareMaterial);
    const EntityId other = scene.createEntity("other");
    scene.setMeshInstance(other, MeshInstance{cube, spareId});
    sync.update();
    scene.setMeshInstance(other, std::nullopt);
    scene.removeResource(spareId);
    scene.removeResource(spare);
    sync.update();
    REQUIRE(gpu.texturesReleased.size() == 1);
    CHECK(materialOf(sync, 0).baseColorMap.texture.valid());
}

TEST_CASE("moving, adding and removing entities updates the draw list") {
    Scene scene;
    FakeGpu gpu;
    const ResourceId cube = scene.addMesh("cube", makeBox({0, 0, 0}, {1, 1, 1}));
    const EntityId box = scene.createEntity("box");
    scene.setMeshInstance(box, MeshInstance{cube, {}});
    render::SceneSync sync = makeSync(scene, gpu);
    sync.update();
    REQUIRE(sync.draws().size() == 1);

    scene.setTransform(box, at({3.0f, 4.0f, 0.0f}));
    sync.update();
    CHECK(near(translation(sync.draws()[0]), {3.0f, 4.0f, 0.0f}));

    const EntityId another = scene.createEntity("another");
    scene.setMeshInstance(another, MeshInstance{cube, {}});
    sync.update();
    CHECK(sync.draws().size() == 2);

    scene.destroyEntity(box);
    sync.update();
    REQUIRE(sync.draws().size() == 1);
    CHECK(near(translation(sync.draws()[0]), {0.0f, 0.0f, 0.0f}));
    CHECK(gpu.uploaded.size() == 1); // edits never re-upload geometry
}

TEST_CASE("removing a mesh resource releases its GPU mesh") {
    Scene scene;
    FakeGpu gpu;
    const ResourceId cube = scene.addMesh("cube", makeBox({0, 0, 0}, {1, 1, 1}));
    const EntityId box = scene.createEntity("box");
    scene.setMeshInstance(box, MeshInstance{cube, {}});
    render::SceneSync sync = makeSync(scene, gpu);
    sync.update();
    REQUIRE(gpu.uploaded.size() == 1);

    scene.setMeshInstance(box, std::nullopt);
    scene.removeResource(cube);
    sync.update();
    CHECK(sync.draws().empty());
    CHECK(gpu.released == gpu.uploaded);
}

TEST_CASE("destroying the sync releases every uploaded mesh and texture") {
    Scene scene;
    FakeGpu gpu;
    Material textured;
    textured.baseColorMap.texture = scene.addTexture("texture", pixel());
    const ResourceId material = scene.addMaterial("textured", textured);
    for (int i = 0; i < 3; ++i) {
        const ResourceId mesh = scene.addMesh("mesh", makeBox({0, 0, 0}, {1, 1, 1}));
        const EntityId e = scene.createEntity("e");
        scene.setMeshInstance(e, MeshInstance{mesh, material});
    }
    {
        render::SceneSync sync = makeSync(scene, gpu);
        sync.update();
        CHECK(gpu.uploaded.size() == 3);
        CHECK(gpu.texturesUploaded.size() == 1);
    }
    CHECK(gpu.released.size() == 3);
    CHECK(gpu.texturesReleased == gpu.texturesUploaded);
    // The scene no longer notifies a destroyed sync.
    scene.createEntity("after");
}
