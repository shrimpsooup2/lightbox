#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "core/scene/scene.hpp"
#include "import/importer.hpp"

using namespace lb;
using import::MapKind;
namespace fs = std::filesystem;

namespace {

// A unique scratch folder under the system temp directory with 2×2 TGA files, removed afterwards.
class TextureDir {
public:
    TextureDir() {
        std::random_device random;
        path_ = fs::temp_directory_path() /
                ("lightbox-texture-test-" + std::to_string(random()) + std::to_string(random()));
        fs::create_directories(path_);
    }
    ~TextureDir() {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    TextureDir(const TextureDir&) = delete;
    TextureDir& operator=(const TextureDir&) = delete;

    std::vector<fs::path> write(const std::vector<std::string>& names) const {
        std::vector<std::uint8_t> tga(18, 0);
        tga[2] = 2;
        tga[12] = 2;
        tga[14] = 2;
        tga[16] = 32;
        tga[17] = 0x28;
        tga.resize(18 + 2 * 2 * 4, 128);
        std::vector<fs::path> files;
        for (const std::string& name : names) {
            files.push_back(path_ / name);
            std::ofstream(files.back(), std::ios::binary)
                .write(reinterpret_cast<const char*>(tga.data()),
                       static_cast<std::streamsize>(tga.size()));
        }
        return files;
    }

private:
    fs::path path_;
};

std::string textureName(const import::ImportedModel& model, int index) {
    REQUIRE(index >= 0);
    REQUIRE(static_cast<std::size_t>(index) < model.textures.size());
    return model.textures[static_cast<std::size_t>(index)].name;
}

} // namespace

TEST_CASE("classifyTextureFile recognizes map names") {
    const struct {
        const char* file;
        MapKind kind;
    } cases[] = {
        {"Bricks076C_1K-PNG_Color.png", MapKind::BaseColor},
        {"Bricks076C_1K-PNG_AmbientOcclusion.png", MapKind::Occlusion},
        {"Bricks076C_1K-PNG_Displacement.png", MapKind::Height},
        {"Bricks076C_1K-PNG_NormalDX.png", MapKind::NormalDirectX},
        {"Bricks076C_1K-PNG_NormalGL.png", MapKind::Normal},
        {"Bricks076C_1K-PNG_Roughness.png", MapKind::Roughness},
        {"Bricks076C.png", MapKind::Unknown},
        {"brick_wall_001_diff_1k.png", MapKind::BaseColor},
        {"brick_wall_001_nor_gl_1k.png", MapKind::Normal},
        {"brick_wall_001_nor_dx_1k.png", MapKind::NormalDirectX},
        {"brick_wall_001_rough_1k.png", MapKind::Roughness},
        {"brick_wall_001_arm_1k.png", MapKind::PackedOcclusionRoughnessMetalness},
        {"brick_wall_001_disp_1k.png", MapKind::Height},
        {"Wood_BaseColor.jpg", MapKind::BaseColor},
        {"metal_plate_diff_1k.jpg", MapKind::BaseColor},
        {"metal_plate_metal_1k.jpg", MapKind::Metalness},
        {"Tiles_Gloss.png", MapKind::Gloss},
        {"Tiles_ORM.png", MapKind::PackedOcclusionRoughnessMetalness},
        {"albedo.png", MapKind::BaseColor},
        {"Concrete AO.png", MapKind::Occlusion},
        {"rock-normal-directx.png", MapKind::NormalDirectX},
        {"MyTexture.png", MapKind::Unknown},
    };
    for (const auto& c : cases) {
        INFO(c.file);
        CHECK(import::classifyTextureFile(c.file) == c.kind);
    }
}

TEST_CASE("textureSetName finds the shared name") {
    const std::vector<fs::path> ambientCg = {
        "Bricks076C_1K-PNG_Color.png",  "Bricks076C_1K-PNG_AmbientOcclusion.png",
        "Bricks076C_1K-PNG_NormalDX.png", "Bricks076C_1K-PNG_NormalGL.png",
        "Bricks076C_1K-PNG_Roughness.png", "Bricks076C.png"};
    CHECK(import::textureSetName(ambientCg) == "Bricks076C");

    const std::vector<fs::path> polyHaven = {
        "brick_wall_001_diff_1k.png", "brick_wall_001_nor_gl_1k.png", "brick_wall_001_rough_1k.png",
        "brick_wall_001_arm_1k.png", "brick_wall_001_disp_1k.png"};
    CHECK(import::textureSetName(polyHaven) == "brick_wall_001");

    const std::vector<fs::path> single = {"Wood_BaseColor.jpg"};
    CHECK(import::textureSetName(single) == "Wood");
    const std::vector<fs::path> midToken = {"wood_diff.png", "wood_disp.png"};
    CHECK(import::textureSetName(midToken) == "wood");
    const std::vector<fs::path> camel = {"OakColor.png", "OakRoughness.png"};
    CHECK(import::textureSetName(camel) == "Oak");
}

TEST_CASE("importTextureSet builds one box-mapped material (ambientCG layout)") {
    const TextureDir dir;
    const std::vector<fs::path> files = dir.write(
        {"Bricks076C_1K-PNG_Color.tga", "Bricks076C_1K-PNG_AmbientOcclusion.tga",
         "Bricks076C_1K-PNG_Displacement.tga", "Bricks076C_1K-PNG_NormalDX.tga",
         "Bricks076C_1K-PNG_NormalGL.tga", "Bricks076C_1K-PNG_Roughness.tga", "Bricks076C.tga"});
    const import::ImportedModel model = import::importTextureSet(files);

    CHECK(model.name == "Bricks076C");
    CHECK(model.meshes.empty());
    REQUIRE(model.materials.size() == 1);
    const import::ImportedMaterial& material = model.materials[0];
    CHECK(material.name == "Bricks076C");
    CHECK(material.material.mapping == TextureMapping::Box);
    CHECK(material.material.textureScale == glm::vec2(1.0f));
    CHECK(textureName(model, material.baseColor.texture) == "Bricks076C_1K-PNG_Color.tga");
    CHECK(textureName(model, material.normal.texture) == "Bricks076C_1K-PNG_NormalGL.tga");
    CHECK_FALSE(material.material.normalFlipY);
    CHECK(textureName(model, material.roughness.texture) == "Bricks076C_1K-PNG_Roughness.tga");
    CHECK(material.roughness.channel == 0);
    CHECK(textureName(model, material.occlusion.texture) ==
          "Bricks076C_1K-PNG_AmbientOcclusion.tga");
    CHECK(material.metalness.texture == -1);
    CHECK(model.textures.size() == 4); // DX normal, height map and preview are not loaded
    CHECK(model.warnings.size() == 2); // preview + displacement

    Scene scene;
    const ResourceId id = import::addMaterialToScene(scene, model);
    const MaterialResource* resource = scene.material(id);
    REQUIRE(resource != nullptr);
    CHECK(resource->name == "Bricks076C");
    CHECK(resource->material.mapping == TextureMapping::Box);
    REQUIRE(resource->material.baseColorMap);
    CHECK(scene.texture(resource->material.baseColorMap.texture) != nullptr);
    CHECK(scene.userCount(resource->material.normalMap.texture) == 1);
}

TEST_CASE("importTextureSet uses packed ARM maps, DirectX normals and gloss") {
    const TextureDir dir;

    SUBCASE("Poly Haven ARM + DirectX normal") {
        const import::ImportedModel model = import::importTextureSet(
            dir.write({"brick_wall_001_diff_1k.tga", "brick_wall_001_nor_dx_1k.tga",
                       "brick_wall_001_arm_1k.tga", "brick_wall_001_disp_1k.tga"}));
        REQUIRE(model.materials.size() == 1);
        const import::ImportedMaterial& material = model.materials[0];
        CHECK(model.name == "brick_wall_001");
        CHECK(material.material.normalFlipY);
        const int arm = material.occlusion.texture;
        CHECK(textureName(model, arm) == "brick_wall_001_arm_1k.tga");
        CHECK(material.occlusion.channel == 0);
        CHECK(material.roughness.texture == arm);
        CHECK(material.roughness.channel == 1);
        CHECK(material.metalness.texture == arm);
        CHECK(material.metalness.channel == 2);
        CHECK(material.material.roughness == doctest::Approx(1.0f));
        CHECK(material.material.metalness == doctest::Approx(1.0f));
        CHECK(model.textures.size() == 3);

        Scene scene;
        const ResourceId id = import::addMaterialToScene(scene, model);
        REQUIRE(scene.material(id) != nullptr);
        CHECK(scene.material(id)->material.roughnessMap.channel == 1);
        CHECK(scene.material(id)->material.roughnessMap.texture ==
              scene.material(id)->material.metalnessMap.texture);
    }

    SUBCASE("gloss drives roughness inverted") {
        const import::ImportedModel model =
            import::importTextureSet(dir.write({"Tiles_Color.tga", "Tiles_Gloss.tga"}));
        REQUIRE(model.materials.size() == 1);
        CHECK(textureName(model, model.materials[0].roughness.texture) == "Tiles_Gloss.tga");
        CHECK(model.materials[0].roughness.invert);
    }
}

TEST_CASE("importTextureSet reports unusable sets and cancellation") {
    const TextureDir dir;
    CHECK_THROWS_AS((void)import::importTextureSet(std::vector<fs::path>{}), import::ImportError);
    CHECK_THROWS_AS((void)import::importTextureSet(dir.write({"Preview.tga"})),
                    import::ImportError);
    std::atomic<bool> cancel{true};
    CHECK_THROWS_AS((void)import::importTextureSet(dir.write({"Wood_Color.tga"}), {}, &cancel),
                    import::ImportCancelled);
}
