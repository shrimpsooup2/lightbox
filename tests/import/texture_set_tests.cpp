#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <miniz.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "core/scene/scene.hpp"
#include "import/importer.hpp"

using namespace lb;
using import::MapKind;
namespace fs = std::filesystem;

namespace {

// A 2×2 uncompressed 32-bit TGA, mid grey.
std::vector<std::uint8_t> tgaBytes() {
    std::vector<std::uint8_t> tga(18, 0);
    tga[2] = 2;
    tga[12] = 2;
    tga[14] = 2;
    tga[16] = 32;
    tga[17] = 0x28;
    tga.resize(18 + 2 * 2 * 4, 128);
    return tga;
}

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

    const fs::path& path() const { return path_; }

    std::vector<fs::path> write(const std::vector<std::string>& names) const {
        const std::vector<std::uint8_t> tga = tgaBytes();
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
        // Poliigon, older names: the trailing workflow marker is not a metalness map.
        {"BricksRuralWorn001_COL_8K_METALNESS.jpg", MapKind::BaseColor},
        {"BricksRuralWorn001_NRM_8K_METALNESS.png", MapKind::Normal},
        {"BricksRuralWorn001_NRM16_8K_METALNESS.tif", MapKind::Normal},
        {"BricksRuralWorn001_METALNESS_8K_METALNESS.jpg", MapKind::Metalness},
        {"WoodFine001_GLOSS_3K_SPECULAR.jpg", MapKind::Gloss},
        {"WoodFine001_REFL_3K_SPECULAR.jpg", MapKind::Specular},
        {"BricksRuralWorn001_DISP16_8K_METALNESS.tif", MapKind::Height},
        {"WoodFine001_COL_VAR1_3K.jpg", MapKind::BaseColor},
        // Poliigon, current names.
        {"Poliigon_BrickReclaimedCommon_8442_BaseColor.jpg", MapKind::BaseColor},
        {"Poliigon_BrickReclaimedCommon_8442_ORM.jpg", MapKind::PackedOcclusionRoughnessMetalness},
        {"Poliigon_SheerFabricDrapery_6565_BaseColorOpacity.png", MapKind::BaseColor},
        {"Poliigon_SheerFabricDrapery_6565_SheenColor.jpg", MapKind::Other},
        {"Poliigon_SheerFabricDrapery_6565_Opacity.jpg", MapKind::Opacity},
        // ambientCG puts the map name after the resolution, even for metals.
        {"Metal032_1K-JPG_Metalness.jpg", MapKind::Metalness},
        {"Metal032_1K-JPG_Color.jpg", MapKind::BaseColor},
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
    CHECK(material.material.textureScale == glm::vec2(import::kDefaultTextureTileMeters));
    CHECK(textureName(model, material.baseColor.texture) == "Bricks076C_1K-PNG_Color.tga");
    CHECK(textureName(model, material.normal.texture) == "Bricks076C_1K-PNG_NormalGL.tga");
    CHECK_FALSE(material.material.normalFlipY);
    CHECK(textureName(model, material.roughness.texture) == "Bricks076C_1K-PNG_Roughness.tga");
    CHECK(material.roughness.channel == 0);
    CHECK(textureName(model, material.occlusion.texture) ==
          "Bricks076C_1K-PNG_AmbientOcclusion.tga");
    CHECK(material.metalness.texture == -1);
    CHECK(model.textures.size() == 5); // DX normal and preview are not loaded
    CHECK(model.warnings.size() == 1); // preview
    CHECK(textureName(model, material.height.texture) == "Bricks076C_1K-PNG_Displacement.tga");
    CHECK(material.material.heightDepth > 0.0f);

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
        CHECK(model.textures.size() == 4); // diff, DX normal, ARM, displacement
        CHECK(material.height.texture >= 0);

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

TEST_CASE("importTextureSet reads downloaded packs: zip archives and folders") {
    const TextureDir dir;
    const std::vector<std::uint8_t> tga = tgaBytes();

    SUBCASE("Poliigon zip with a subfolder, extra files and a preview") {
        const fs::path zip = dir.path() / "BricksRuralWorn001.zip";
        const auto add = [&](const char* name, const void* data, std::size_t size) {
            REQUIRE(mz_zip_add_mem_to_archive_file_in_place(zip.string().c_str(), name, data, size,
                                                            nullptr, 0, MZ_BEST_SPEED));
        };
        for (const char* name : {"BricksRuralWorn001/BricksRuralWorn001_COL_2K_METALNESS.tga",
                                 "BricksRuralWorn001/BricksRuralWorn001_NRM_2K_METALNESS.tga",
                                 "BricksRuralWorn001/BricksRuralWorn001_ROUGHNESS_2K_METALNESS.tga",
                                 "BricksRuralWorn001/BricksRuralWorn001_DISP16_2K_METALNESS.tga",
                                 "BricksRuralWorn001/BricksRuralWorn001_Sphere.tga"}) {
            add(name, tga.data(), tga.size());
        }
        const std::string readme = "Thanks for downloading";
        add("BricksRuralWorn001/readme.txt", readme.data(), readme.size());

        CHECK(import::isTextureSource(zip));
        const import::ImportedModel model = import::importTextureSet(std::vector<fs::path>{zip});
        REQUIRE(model.materials.size() == 1);
        const import::ImportedMaterial& material = model.materials[0];
        CHECK(model.name == "BricksRuralWorn001");
        CHECK(textureName(model, material.baseColor.texture) ==
              "BricksRuralWorn001_COL_2K_METALNESS.tga");
        CHECK(textureName(model, material.normal.texture) ==
              "BricksRuralWorn001_NRM_2K_METALNESS.tga");
        CHECK_FALSE(material.material.normalFlipY); // Poliigon normals are OpenGL
        CHECK(textureName(model, material.roughness.texture) ==
              "BricksRuralWorn001_ROUGHNESS_2K_METALNESS.tga");
        CHECK(material.metalness.texture == -1); // METALNESS is only the workflow marker here
        CHECK(model.textures.size() == 4); // DISP16 is the height map
        CHECK(model.warnings.size() == 1); // preview; readme.txt silently skipped

        Scene scene;
        const ResourceId id = import::addMaterialToScene(scene, model);
        REQUIRE(scene.material(id) != nullptr);
        CHECK(scene.texture(scene.material(id)->material.baseColorMap.texture)->image.width == 2);
    }

    SUBCASE("folder with nested images") {
        const fs::path folder = dir.path() / "Wood049_2K-JPG";
        fs::create_directories(folder / "maps");
        for (const char* name : {"maps/Wood049_2K-JPG_Color.tga", "maps/Wood049_2K-JPG_NormalGL.tga",
                                 "Wood049_2K-JPG_Roughness.tga"}) {
            std::ofstream(folder / name, std::ios::binary)
                .write(reinterpret_cast<const char*>(tga.data()),
                       static_cast<std::streamsize>(tga.size()));
        }
        std::ofstream(folder / "Wood049.mtlx") << "<materialx/>";

        CHECK(import::isTextureSource(folder));
        const import::ImportedModel model =
            import::importTextureSet(std::vector<fs::path>{folder});
        REQUIRE(model.materials.size() == 1);
        CHECK(model.name == "Wood049");
        CHECK(model.materials[0].baseColor.texture >= 0);
        CHECK(model.materials[0].normal.texture >= 0);
        CHECK(model.materials[0].roughness.texture >= 0);
        CHECK(model.warnings.empty());
    }

    SUBCASE("a damaged zip is reported") {
        const fs::path zip = dir.path() / "broken.zip";
        std::ofstream(zip, std::ios::binary) << "not a zip at all";
        CHECK_THROWS_AS((void)import::importTextureSet(std::vector<fs::path>{zip}),
                        import::ImportError);
    }
}
