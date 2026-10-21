#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>

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
namespace fs = std::filesystem;

namespace {

// A unique scratch folder under the system temp directory, removed with its contents.
class TempDir {
public:
    TempDir() {
        std::random_device random;
        path_ = fs::temp_directory_path() /
                ("lightbox-import-test-" + std::to_string(random()) + std::to_string(random()));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    fs::path write(const std::string& name, std::string_view content) const {
        const fs::path file = path_ / name;
        std::ofstream(file, std::ios::binary).write(content.data(),
                                                    static_cast<std::streamsize>(content.size()));
        return file;
    }
    fs::path write(const std::string& name, const std::vector<std::uint8_t>& bytes) const {
        return write(name, std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                            bytes.size()));
    }

private:
    fs::path path_;
};

// Uncompressed 32-bit TGA, top-left origin (see tests/core/image_tests.cpp).
std::vector<std::uint8_t> makeTga(std::uint16_t width, std::uint16_t height) {
    std::vector<std::uint8_t> file(18, 0);
    file[2] = 2;
    file[12] = static_cast<std::uint8_t>(width & 0xFF);
    file[13] = static_cast<std::uint8_t>(width >> 8);
    file[14] = static_cast<std::uint8_t>(height & 0xFF);
    file[15] = static_cast<std::uint8_t>(height >> 8);
    file[16] = 32;
    file[17] = 0x28;
    file.resize(file.size() + std::size_t{width} * height * 4, 200);
    return file;
}

std::string base64(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t b0 = bytes[i];
        const std::uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0u;
        const std::uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0u;
        const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kAlphabet[(triple >> 18) & 63u]);
        out.push_back(kAlphabet[(triple >> 12) & 63u]);
        out.push_back(i + 1 < bytes.size() ? kAlphabet[(triple >> 6) & 63u] : '=');
        out.push_back(i + 2 < bytes.size() ? kAlphabet[triple & 63u] : '=');
    }
    return out;
}

bool near(float a, float b, float epsilon = 1e-4f) { return std::abs(a - b) <= epsilon; }
bool near(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-4f) {
    return glm::length(a - b) <= epsilon;
}

const import::ImportedMaterial* findMaterial(const import::ImportedModel& model,
                                             std::string_view name) {
    for (const auto& material : model.materials) {
        if (material.name == name) {
            return &material;
        }
    }
    return nullptr;
}

// Two quads side by side (Y-up, centimeters), one per material; the first is textured.
constexpr std::string_view kObj = R"(mtllib room.mtl
o Floor
v 0 0 0
v 1000 0 0
v 1000 0 -500
v 0 0 -500
v 2000 0 0
v 2000 0 -500
vt 0 0
vt 1 0
vt 1 1
vt 0 1
usemtl Wood
f 1/1 2/2 3/3 4/4
usemtl Paint
f 2/1 5/2 6/3 3/4
)";

constexpr std::string_view kMtl = R"(newmtl Wood
Kd 0.8 0.8 0.8
map_Kd wood.tga
newmtl Paint
Kd 0.5 0.5 0.5
Ns 10
)";

fs::path writeObjFixture(const TempDir& dir) {
    dir.write("room.mtl", kMtl);
    dir.write("wood.tga", makeTga(2, 2));
    return dir.write("room.obj", kObj);
}

// A 1 m quad facing +Y (glTF up) with an embedded data-URI buffer, placed twice. The quad uses the
// first of `materials` (a JSON array).
constexpr std::string_view kBlueMaterial =
    R"([{"name":"Blue","pbrMetallicRoughness":{"baseColorFactor":[0.5,0.25,1,1],)"
    R"("metallicFactor":0.3,"roughnessFactor":0.7}}])";

fs::path writeGltfFixture(const TempDir& dir, std::string_view materials = kBlueMaterial) {
    const float positions[12] = {0, 0, 0, 1, 0, 0, 1, 0, -1, 0, 0, -1};
    const std::uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    std::vector<std::uint8_t> buffer(sizeof(positions) + sizeof(indices));
    std::memcpy(buffer.data(), positions, sizeof(positions));
    std::memcpy(buffer.data() + sizeof(positions), indices, sizeof(indices));
    const std::string json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],)"
        R"("nodes":[{"name":"Quad","mesh":0,"translation":[0,2,0]},)"
        R"({"name":"Copy","mesh":0,"translation":[5,0,0]}],)"
        R"("meshes":[{"name":"QuadMesh","primitives":[{"attributes":{"POSITION":0},"indices":1,)"
        R"("material":0}]}],)"
        R"("materials":)" + std::string(materials) + R"(,)"
        R"("buffers":[{"byteLength":60,"uri":"data:application/octet-stream;base64,)" +
        base64(buffer) +
        R"("}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48,"target":34962},)"
        R"({"buffer":0,"byteOffset":48,"byteLength":12,"target":34963}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3",)"
        R"("min":[0,0,-1],"max":[1,0,0]},)"
        R"({"bufferView":1,"componentType":5123,"count":6,"type":"SCALAR"}]})";
    return dir.write("quad.gltf", json);
}

// Trimmed from ufbx's test data (data/maya_cube_7500_ascii.fbx, public domain / MIT): a 1-unit
// cube in a Y-up centimeter scene, moved 3 units up.
constexpr std::string_view kFbx = R"(; FBX 7.5.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7500
	Creator: "FBX SDK/FBX Plugins version 2019.2"
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",1
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",2
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",1
	}
}
Objects:  {
	Geometry: 1908558533920, "Geometry::", "Mesh" {
		Vertices: *24 {
			a: -0.5,-0.5,0.5,0.5,-0.5,0.5,-0.5,0.5,0.5,0.5,0.5,0.5,-0.5,0.5,-0.5,0.5,0.5,-0.5,-0.5,-0.5,-0.5,0.5,-0.5,-0.5
		}
		PolygonVertexIndex: *24 {
			a: 0,1,3,-3,2,3,5,-5,4,5,7,-7,6,7,1,-1,1,7,5,-4,6,0,2,-5
		}
		GeometryVersion: 124
		LayerElementNormal: 0 {
			Version: 102
			Name: ""
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "Direct"
			Normals: *72 {
				a: 0,0,1,0,0,1,0,0,1,0,0,1,0,1,0,0,1,0,0,1,0,0,1,0,0,0,-1,0,0,-1,0,0,-1,0,0,-1,0,-1,0,0,-1,0,0,-1,0,0,-1,0,1,0,0,1,0,0,1,0,0,1,0,0,-1,0,0,-1,0,0,-1,0,0,-1,0,0
			}
		}
		LayerElementUV: 0 {
			Version: 101
			Name: "map1"
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "IndexToDirect"
			UV: *28 {
				a: 0.375,0,0.625,0,0.375,0.25,0.625,0.25,0.375,0.5,0.625,0.5,0.375,0.75,0.625,0.75,0.375,1,0.625,1,0.875,0,0.875,0.25,0.125,0,0.125,0.25
			}
			UVIndex: *24 {
				a: 0,1,3,2,2,3,5,4,4,5,7,6,6,7,9,8,1,10,11,3,12,0,2,13
			}
		}
		LayerElementMaterial: 0 {
			Version: 101
			Name: ""
			MappingInformationType: "AllSame"
			ReferenceInformationType: "IndexToDirect"
			Materials: *1 {
				a: 0
			}
		}
		Layer: 0 {
			Version: 100
			LayerElement:  {
				Type: "LayerElementNormal"
				TypedIndex: 0
			}
			LayerElement:  {
				Type: "LayerElementMaterial"
				TypedIndex: 0
			}
			LayerElement:  {
				Type: "LayerElementUV"
				TypedIndex: 0
			}
		}
	}
	Model: 1907292239120, "Model::pCube1", "Mesh" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",0,3,0
		}
		Shading: T
		Culling: "CullingOff"
	}
	Material: 1907674217712, "Material::lambert1", "" {
		Version: 102
		ShadingModel: "lambert"
		MultiLayer: 0
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",0.5,0.5,0.5
			P: "DiffuseFactor", "Number", "", "A",0.8
		}
	}
}
Connections:  {
	C: "OO",1907292239120,0
	C: "OO",1908558533920,1907292239120
	C: "OO",1907674217712,1907292239120
}
)";

} // namespace

TEST_CASE("OBJ import converts axes, flips V, guesses units and reads MTL materials") {
    const TempDir dir;
    const fs::path file = writeObjFixture(dir);
    std::vector<float> fractions;
    const import::ImportedModel model = import::importModel(
        file, [&](float fraction, std::string_view) { fractions.push_back(fraction); });

    CHECK(model.format == "OBJ");
    CHECK(model.name == "room");
    CHECK_FALSE(model.unitsFromFile);
    CHECK(model.metersPerUnit == doctest::Approx(0.01)); // 2000 units wide → centimeters
    CHECK_FALSE(model.warnings.empty());                 // "check the units"
    REQUIRE_FALSE(fractions.empty());
    CHECK(fractions.back() == doctest::Approx(1.0f));

    REQUIRE(model.nodes.size() == 1);
    CHECK(model.nodes[0].name == "Floor");
    REQUIRE(model.nodes[0].meshes.size() == 2); // split per material
    REQUIRE(model.meshes.size() == 2);

    const auto* wood = findMaterial(model, "Wood");
    const auto* paint = findMaterial(model, "Paint");
    REQUIRE(wood != nullptr);
    REQUIRE(paint != nullptr);
    REQUIRE(model.textures.size() == 1);
    CHECK(model.textures[0].name == "wood.tga");
    CHECK(model.textures[0].image.width == 2);
    CHECK_FALSE(model.textures[0].sourcePath.empty());
    CHECK(wood->baseColor.texture == 0);
    CHECK(near(wood->material.baseColor, glm::vec3(1.0f))); // the texture alone colors it
    CHECK(paint->baseColor.texture == -1);
    CHECK(paint->material.baseColor.r == doctest::Approx(0.21404f).epsilon(0.001)); // sRGB 0.5
    CHECK(paint->material.roughness == doctest::Approx(std::sqrt(2.0f / 12.0f)));

    for (const import::ImportedMesh& mesh : model.meshes) {
        CHECK(mesh.data.indices.size() == 6);
        REQUIRE(mesh.data.normals.size() == mesh.data.positions.size());
        for (std::size_t v = 0; v < mesh.data.positions.size(); ++v) {
            CHECK(near(mesh.data.positions[v].z, 0.0f)); // Y-up floor became Z-up floor
            CHECK(near(mesh.data.normals[v], glm::vec3(0.0f, 0.0f, 1.0f)));
        }
    }
    // OBJ (1000, 0, -500) with vt (1, 1) → LightBox (1000, 500, 0) with uv (1, 0).
    const import::ImportedMesh& woodMesh =
        model.meshes[model.meshes[0].material == static_cast<int>(wood - model.materials.data())
                         ? 0
                         : 1];
    REQUIRE(woodMesh.data.uvs.size() == woodMesh.data.positions.size());
    bool found = false;
    for (std::size_t v = 0; v < woodMesh.data.positions.size(); ++v) {
        if (near(woodMesh.data.positions[v], glm::vec3(1000.0f, 500.0f, 0.0f))) {
            found = true;
            CHECK(near(woodMesh.data.uvs[v].x, 1.0f));
            CHECK(near(woodMesh.data.uvs[v].y, 0.0f));
        }
    }
    CHECK(found);
}

TEST_CASE("glTF import with a data-URI buffer converts axes, nodes and materials") {
    const TempDir dir;
    const import::ImportedModel model = import::importModel(writeGltfFixture(dir));

    CHECK(model.format == "glTF");
    CHECK(model.unitsFromFile);
    CHECK(model.metersPerUnit == doctest::Approx(1.0));
    REQUIRE(model.meshes.size() == 1); // both nodes instance the same mesh
    REQUIRE(model.nodes.size() == 2);
    CHECK(model.nodes[0].name == "Quad");
    CHECK(model.nodes[0].meshes == std::vector<int>{0});
    CHECK(model.nodes[1].meshes == std::vector<int>{0});
    // Translation (0, 2, 0) in glTF's Y-up is 2 m up in Z-up.
    CHECK(near(glm::vec3(model.nodes[0].transform[3]), glm::vec3(0.0f, 0.0f, 2.0f)));
    CHECK(near(glm::vec3(model.nodes[1].transform[3]), glm::vec3(5.0f, 0.0f, 0.0f)));

    const MeshData& mesh = model.meshes[0].data;
    CHECK(mesh.indices.size() == 6);
    CHECK(std::any_of(mesh.positions.begin(), mesh.positions.end(), [](const glm::vec3& p) {
        return near(p, glm::vec3(1.0f, 1.0f, 0.0f)); // glTF (1, 0, -1)
    }));
    for (const glm::vec3& n : mesh.normals) {
        CHECK(near(n, glm::vec3(0.0f, 0.0f, 1.0f))); // generated, facing up
    }

    REQUIRE(model.materials.size() == 1);
    CHECK(model.meshes[0].material == 0);
    const Material& material = model.materials[0].material;
    CHECK(model.materials[0].name == "Blue");
    CHECK(near(material.baseColor, glm::vec3(0.5f, 0.25f, 1.0f)));
    CHECK(material.roughness == doctest::Approx(0.7f));
    CHECK(material.metalness == doctest::Approx(0.3f));
    CHECK(model.textures.empty());
}

TEST_CASE("glTF see-through materials import as glass") {
    const TempDir dir;
    const import::ImportedModel model = import::importModel(writeGltfFixture(
        dir, R"([{"name":"Window","alphaMode":"BLEND","pbrMetallicRoughness":)"
             R"({"baseColorFactor":[0.9,0.95,1,0.3],"roughnessFactor":0.0,"metallicFactor":0}},)"
             R"({"name":"Curtain","alphaMode":"BLEND","pbrMetallicRoughness":)"
             R"({"baseColorFactor":[1,1,1,0.75],"roughnessFactor":0.6,"metallicFactor":0}},)"
             R"({"name":"Wall","alphaMode":"BLEND","pbrMetallicRoughness":)"
             R"({"baseColorFactor":[1,1,1,1],"roughnessFactor":0.6,"metallicFactor":0}},)"
             R"({"name":"Bottle","extensions":{"KHR_materials_transmission":)"
             R"({"transmissionFactor":0.8},"KHR_materials_volume":{"thicknessFactor":0.01},)"
             R"("KHR_materials_ior":{"ior":1.45}}}])"));
    REQUIRE(model.materials.size() == 4);
    const Material& window = model.materials[0].material;
    CHECK(window.transmission == doctest::Approx(1.0f)); // glossy and see-through: clear glass
    CHECK(window.thinWalled);
    const Material& curtain = model.materials[1].material;
    CHECK(curtain.transmission == doctest::Approx(0.25f)); // translucent: lets a quarter through
    CHECK(model.materials[2].material.transmission == doctest::Approx(0.0f)); // fully opaque
    const Material& bottle = model.materials[3].material;
    CHECK(bottle.transmission == doctest::Approx(0.8f));
    CHECK_FALSE(bottle.thinWalled); // has a volume: solid glass that bends light
    CHECK(bottle.ior == doctest::Approx(1.45f));
}

TEST_CASE("addToScene builds the hierarchy under a scaled root and counts instances") {
    const TempDir dir;
    Scene scene;

    SUBCASE("glTF: shared mesh, one entity per node") {
        import::ImportedModel model = import::importModel(writeGltfFixture(dir));
        const import::PlacedModel placed = import::addToScene(scene, model);
        CHECK(placed.objects == 2);
        CHECK(placed.triangles == 4);
        CHECK(placed.materials == 1);
        CHECK(placed.textures == 0);
        const Entity* root = scene.entity(placed.root);
        REQUIRE(root != nullptr);
        CHECK(root->name == "quad");
        CHECK(root->transform.scale == glm::vec3(1.0f));
        REQUIRE(root->children.size() == 2);
        const Entity* quad = scene.entity(root->children[0]);
        const Entity* copy = scene.entity(root->children[1]);
        REQUIRE(quad != nullptr);
        REQUIRE(copy != nullptr);
        CHECK(quad->name == "Quad");
        REQUIRE(quad->meshInstance.has_value());
        REQUIRE(copy->meshInstance.has_value());
        CHECK(quad->meshInstance->mesh == copy->meshInstance->mesh);
        CHECK(quad->meshInstance->material);
        CHECK(near(quad->transform.position, glm::vec3(0.0f, 0.0f, 2.0f)));
    }

    SUBCASE("OBJ: centimeter root scale, one child per material") {
        import::ImportedModel model = import::importModel(writeObjFixture(dir));
        const import::PlacedModel placed = import::addToScene(scene, model);
        CHECK(placed.objects == 2);
        CHECK(placed.triangles == 4);
        CHECK(placed.materials == 2);
        CHECK(placed.textures == 1);
        const Entity* root = scene.entity(placed.root);
        REQUIRE(root != nullptr);
        CHECK(near(root->transform.scale, glm::vec3(0.01f)));
        REQUIRE(root->children.size() == 1);
        const Entity* floor = scene.entity(root->children[0]);
        REQUIRE(floor != nullptr);
        CHECK(floor->name == "Floor");
        CHECK_FALSE(floor->meshInstance.has_value());
        REQUIRE(floor->children.size() == 2);
        CHECK(scene.entity(floor->children[0])->name == "Floor · Wood");
        const auto bounds = scene.worldBounds();
        REQUIRE(bounds.has_value());
        CHECK(bounds->max.x == doctest::Approx(20.0f)); // 2000 cm
        CHECK(bounds->max.y == doctest::Approx(5.0f));
    }

    SUBCASE("far-away models are moved next to the origin") {
        import::ImportedModel model = import::importModel(writeGltfFixture(dir));
        model.nodes[0].transform[3] = glm::vec4(50000.0f, 0.0f, 0.0f, 1.0f);
        model.nodes[1].transform[3] = glm::vec4(50005.0f, 0.0f, 0.0f, 1.0f);
        const std::size_t warningsBefore = model.warnings.size();
        const import::PlacedModel placed = import::addToScene(scene, model);
        CHECK(model.warnings.size() == warningsBefore + 1);
        CHECK(scene.entity(placed.root)->transform.position.x == doctest::Approx(-50003.0f));
        const auto bounds = scene.worldBounds();
        REQUIRE(bounds.has_value());
        CHECK(glm::length(bounds->center()) < 10.0f);
    }
}

TEST_CASE("FBX import reads units, Z-up conversion and materials") {
    const TempDir dir;
    const import::ImportedModel model = import::importModel(dir.write("cube.fbx", kFbx));
    CHECK(model.format == "FBX");
    CHECK(model.unitsFromFile);
    CHECK(model.metersPerUnit == doctest::Approx(0.01));
    REQUIRE(model.meshes.size() == 1);
    CHECK(model.meshes[0].data.indices.size() == 36);
    REQUIRE(model.nodes.size() == 1);
    CHECK(model.nodes[0].name == "pCube1");
    REQUIRE(model.materials.size() == 1);
    CHECK(model.materials[0].name == "lambert1");
    CHECK(model.meshes[0].material == 0);
    const float gray = model.materials[0].material.baseColor.r;
    CHECK(gray > 0.2f);
    CHECK(gray < 0.6f);
    // Moved 3 units along the file's Y (up) axis → 3 units up in Z.
    for (const glm::vec3& p : model.meshes[0].data.positions) {
        const glm::vec3 world(model.nodes[0].transform * glm::vec4(p, 1.0f));
        CHECK(world.z > 2.4f);
        CHECK(world.z < 3.6f);
        CHECK(std::abs(world.y) < 0.6f);
    }
}

TEST_CASE("importModel honors cancellation and reports unreadable files") {
    const TempDir dir;
    const fs::path obj = writeObjFixture(dir);
    std::atomic<bool> cancel{true};
    CHECK_THROWS_AS((void)import::importModel(obj, {}, &cancel), import::ImportCancelled);

    try {
        (void)import::importModel(dir.write("broken.gltf", "this is not glTF"));
        FAIL("expected ImportError");
    } catch (const import::ImportError& e) {
        CHECK(std::string(e.what()).find("broken.gltf") != std::string::npos);
    }
    CHECK_THROWS_AS((void)import::importModel(dir.write("broken.glb", "glTF\x02garbage")),
                    import::ImportError);
    CHECK_THROWS_AS((void)import::importModel(dir.write("broken.fbx", "garbage")),
                    import::ImportError);
    CHECK_THROWS_AS((void)import::importModel(dir.write("model.3ds", "x")), import::ImportError);
    CHECK_THROWS_AS((void)import::importModel(dir.write("empty.obj", "# nothing\n")),
                    import::ImportError);
    CHECK_THROWS_AS((void)import::importModel(fs::temp_directory_path() / "lightbox-missing.obj"),
                    import::ImportError);
}

TEST_CASE("model file extensions and unit choices") {
    CHECK(import::isModelFile("House.GLB"));
    CHECK(import::isModelFile("a/b/model.fbx"));
    CHECK(import::isModelFile("site.KMZ"));
    CHECK(import::isModelFile("export.dae"));
    CHECK(import::isModelFile("part.stl"));
    CHECK_FALSE(import::isModelFile("texture.png"));
    CHECK(import::unitChoices().size() == 5);
    CHECK(import::unitChoices()[1].metersPerUnit == doctest::Approx(0.01));
}

namespace {

// A 100 × 50 inch quad at 10 inches height in a Z-up Collada file (SketchUp's export conventions).
constexpr std::string_view kCollada = R"(<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="0.0254" name="inch"/><up_axis>Z_UP</up_axis></asset>
  <library_effects><effect id="red-fx"><profile_COMMON><technique sid="common"><lambert>
    <diffuse><color>1 0 0 1</color></diffuse></lambert></technique></profile_COMMON></effect>
  </library_effects>
  <library_materials><material id="red" name="Red"><instance_effect url="#red-fx"/></material>
  </library_materials>
  <library_geometries><geometry id="quad" name="quad"><mesh>
    <source id="quad-pos"><float_array id="quad-pos-array" count="12">0 0 10 100 0 10 100 50 10 0 50 10</float_array>
      <technique_common><accessor source="#quad-pos-array" count="4" stride="3"><param name="X" type="float"/>
        <param name="Y" type="float"/><param name="Z" type="float"/></accessor></technique_common></source>
    <vertices id="quad-vtx"><input semantic="POSITION" source="#quad-pos"/></vertices>
    <triangles material="mat" count="2"><input semantic="VERTEX" source="#quad-vtx" offset="0"/>
      <p>0 1 2 0 2 3</p></triangles>
  </mesh></geometry></library_geometries>
  <library_visual_scenes><visual_scene id="scene"><node id="n" name="Quad">
    <instance_geometry url="#quad"><bind_material><technique_common>
      <instance_material symbol="mat" target="#red"/></technique_common></bind_material></instance_geometry>
  </node></visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#scene"/></scene>
</COLLADA>
)";

// World-space positions of every mesh instance (node transforms composed down the hierarchy).
std::vector<glm::vec3> worldPositions(const import::ImportedModel& model) {
    std::vector<glm::mat4> world(model.nodes.size());
    std::vector<glm::vec3> result;
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const import::ImportedNode& node = model.nodes[i];
        world[i] = node.parent >= 0 ? world[static_cast<std::size_t>(node.parent)] * node.transform
                                    : node.transform;
        for (const int mesh : node.meshes) {
            for (const glm::vec3& p : model.meshes[static_cast<std::size_t>(mesh)].data.positions) {
                result.emplace_back(world[i] * glm::vec4(p, 1.0f));
            }
        }
    }
    return result;
}

void checkColladaQuad(const import::ImportedModel& model) {
    CHECK(model.unitsFromFile);
    CHECK(model.axesFromFile);
    CHECK(model.metersPerUnit == doctest::Approx(1.0));
    const std::vector<glm::vec3> points = worldPositions(model);
    REQUIRE(points.size() >= 4);
    for (const glm::vec3& p : points) {
        CHECK(p.z == doctest::Approx(0.254f).epsilon(1e-3)); // 10 inches up, still up
        CHECK(p.x >= -1e-4f);
        CHECK(p.x <= 2.5401f);
        CHECK(p.y >= -1e-4f);
        CHECK(p.y <= 1.2701f);
    }
    REQUIRE(model.materials.size() == 1);
    CHECK(model.materials[0].material.baseColor.r > 0.9f);
    CHECK(model.materials[0].material.baseColor.g < 0.05f);
}

} // namespace

TEST_CASE("Collada import keeps the file's up axis and units") {
    const TempDir dir;
    const import::ImportedModel model = import::importModel(dir.write("quad.dae", kCollada));
    CHECK(model.format == "Collada");
    checkColladaQuad(model);
}

TEST_CASE("KMZ import unpacks the Collada model inside") {
    const TempDir dir;
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    const std::string kml = "<kml><Model><Link><href>models/quad.dae</href></Link></Model></kml>";
    REQUIRE(mz_zip_writer_add_mem(&zip, "doc.kml", kml.data(), kml.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_add_mem(&zip, "models/quad.dae", kCollada.data(), kCollada.size(),
                                  MZ_DEFAULT_COMPRESSION));
    void* bytes = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &bytes, &size));
    const fs::path kmz =
        dir.write("site.kmz", std::string_view(static_cast<const char*>(bytes), size));
    mz_free(bytes);
    mz_zip_writer_end(&zip);

    const import::ImportedModel model = import::importModel(kmz);
    CHECK(model.format == "KMZ (Collada)");
    checkColladaQuad(model);
    CHECK_THROWS_AS((void)import::importModel(dir.write("empty.kmz", "not a zip")),
                    import::ImportError);
}

TEST_CASE("STL import keeps coordinates, guesses units and flags the unknown up axis") {
    const TempDir dir;
    const import::ImportedModel model = import::importModel(dir.write("part.stl", R"(solid part
facet normal 0 0 1
 outer loop
  vertex 0 0 0
  vertex 1000 0 0
  vertex 0 2000 0
 endloop
endfacet
endsolid part
)"));
    CHECK(model.format == "STL");
    CHECK_FALSE(model.unitsFromFile);
    CHECK_FALSE(model.axesFromFile);
    CHECK(model.metersPerUnit == doctest::Approx(0.01)); // 2000 units: centimeters
    const std::vector<glm::vec3> points = worldPositions(model);
    REQUIRE(points.size() == 3);
    for (const glm::vec3& p : points) {
        CHECK(p.z == doctest::Approx(0.0f)); // taken as Z-up: no turn
    }
}
