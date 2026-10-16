#include <doctest/doctest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "core/image.hpp"
#include "core/primitives.hpp"
#include "core/scene/scene.hpp"

using namespace lb;

namespace {

// Uncompressed 32-bit TGA, top-left origin: the smallest format to build by hand.
std::vector<std::uint8_t> makeTga(std::uint16_t width, std::uint16_t height,
                                  const std::vector<std::uint8_t>& rgba) {
    std::vector<std::uint8_t> file(18, 0);
    file[2] = 2; // uncompressed true color
    file[12] = static_cast<std::uint8_t>(width & 0xFF);
    file[13] = static_cast<std::uint8_t>(width >> 8);
    file[14] = static_cast<std::uint8_t>(height & 0xFF);
    file[15] = static_cast<std::uint8_t>(height >> 8);
    file[16] = 32;
    file[17] = 0x28; // 8 alpha bits, top-left origin
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        file.insert(file.end(), {rgba[i + 2], rgba[i + 1], rgba[i], rgba[i + 3]}); // BGRA
    }
    return file;
}

ImageData solid(std::uint32_t width, std::uint32_t height, std::uint8_t value) {
    ImageData image;
    image.width = width;
    image.height = height;
    image.pixels.assign(std::size_t{width} * height * 4, value);
    return image;
}

} // namespace

TEST_CASE("decodeImage returns RGBA rows top to bottom") {
    const std::vector<std::uint8_t> rgba = {
        255, 0,   0,   255, /**/ 0,   255, 0,   128, // top row: red, half-transparent green
        0,   0,   255, 255, /**/ 10,  20,  30,  40,  // bottom row
    };
    const ImageData image = decodeImage(makeTga(2, 2, rgba), "test.tga");
    CHECK(image.width == 2);
    CHECK(image.height == 2);
    CHECK(image.pixels == rgba);
}

TEST_CASE("decodeImage reports unreadable data with the file name") {
    const std::vector<std::uint8_t> garbage = {1, 2, 3, 4, 5, 6, 7, 8};
    try {
        (void)decodeImage(garbage, "broken.png");
        FAIL("expected an exception");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("broken.png") != std::string::npos);
    }
    CHECK_THROWS_AS((void)decodeImage({}, "empty.png"), std::runtime_error);
}

TEST_CASE("downscaleToFit halves with a box filter until it fits") {
    ImageData image = solid(5, 3, 0);
    // Left column bright, so the averaged left texel is half of it.
    for (std::uint32_t y = 0; y < 3; ++y) {
        image.pixels[(std::size_t{y} * 5) * 4] = 200;
    }
    const ImageData half = downscaleToFit(image, 4);
    CHECK(half.width == 2);
    CHECK(half.height == 1);
    CHECK(half.pixels.size() == 2u * 1u * 4u);
    CHECK(half.pixels[0] == 100); // (200 + 0 + 200 + 0) / 4
    CHECK(half.pixels[4] == 0);

    const ImageData unchanged = downscaleToFit(image, 8);
    CHECK(unchanged == image);
    CHECK(downscaleToFit(solid(4096, 16, 7), 1024).width == 1024);
    CHECK(downscaleToFit(solid(4096, 16, 7), 1024).height == 4);
}

TEST_CASE("image file extensions are matched case-insensitively") {
    CHECK(isImageFile("wall/Brick_Albedo.PNG"));
    CHECK(isImageFile("a.jpeg"));
    CHECK_FALSE(isImageFile("model.obj"));
    CHECK_FALSE(isImageFile("noextension"));
}

TEST_CASE("textures are scene resources referenced by materials") {
    Scene scene;
    const ResourceId texture = scene.addTexture("Brick", solid(2, 2, 255), "C:/tex/brick.png");
    REQUIRE(scene.texture(texture) != nullptr);
    CHECK(scene.texture(texture)->sourcePath == "C:/tex/brick.png");
    CHECK(scene.userCount(texture) == 0);

    Material brick;
    brick.baseColorMap.texture = texture;
    brick.roughnessMap = {texture, 1, true};
    const ResourceId material = scene.addMaterial("Brick", brick);
    CHECK(scene.userCount(texture) == 1); // one material, even with two slots
    CHECK_THROWS_AS(scene.removeResource(texture), std::logic_error);

    scene.setMaterial(material, Material{});
    CHECK(scene.userCount(texture) == 0);
    scene.removeResource(texture);
    CHECK(scene.texture(texture) == nullptr);
}

TEST_CASE("materials cannot reference unknown textures") {
    Scene scene;
    Material bad;
    bad.normalMap.texture = ResourceId{12345};
    CHECK_THROWS_AS(scene.addMaterial("bad", bad), std::invalid_argument);
    const ResourceId good = scene.addMaterial("good", Material{});
    CHECK_THROWS_AS(scene.setMaterial(good, bad), std::invalid_argument);
    CHECK_THROWS_AS(scene.addTexture("empty", ImageData{}), std::invalid_argument);
}

TEST_CASE("meshes need one texture coordinate per vertex when they have any") {
    Scene scene;
    MeshData box = makeBox({0, 0, 0}, {2, 1, 3});
    CHECK(box.uvs.size() == box.positions.size());
    CHECK_NOTHROW(scene.addMesh("box", box));
    box.uvs.pop_back();
    CHECK_THROWS_AS(scene.addMesh("bad uvs", box), std::invalid_argument);
    box.uvs.clear();
    CHECK_NOTHROW(scene.addMesh("no uvs", box));
}

TEST_CASE("box texture coordinates are in meters") {
    const MeshData box = makeBox({0, 0, 0}, {2, 1, 3});
    // +X face: U runs along +Y (1 m), V up the 3 m height, negative (V = 0 is the image top).
    CHECK(box.uvs[2].x == doctest::Approx(1.0f));
    CHECK(box.uvs[2].y == doctest::Approx(-3.0f));
}
