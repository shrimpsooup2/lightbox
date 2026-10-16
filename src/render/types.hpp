#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

// Public, Vulkan-free types of the Live renderer API.
namespace lb::render {

struct MeshHandle {
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    bool valid() const { return index != std::numeric_limits<std::uint32_t>::max(); }
};

struct TextureHandle {
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    bool valid() const { return index != std::numeric_limits<std::uint32_t>::max(); }
};

enum class TextureMapping { UV, Box };

// A texture input of a material (mirror of lb::TextureSlot with a GPU handle).
struct MaterialTexture {
    TextureHandle texture; // invalid = unused
    std::uint32_t channel = 0; // scalar maps: 0 = R … 3 = A
    bool invert = false;
};

// Everything the shaders need to shade one material (mirror of lb::Material).
struct MaterialParams {
    glm::vec3 baseColor{0.8f}; // linear
    float roughness = 0.6f;
    float metalness = 0.0f;
    MaterialTexture baseColorMap; // sampled as sRGB
    MaterialTexture roughnessMap;
    MaterialTexture metalnessMap;
    MaterialTexture normalMap;
    MaterialTexture occlusionMap;
    float normalStrength = 1.0f;
    bool normalFlipY = false;
    TextureMapping mapping = TextureMapping::UV;
    glm::vec2 textureScale{1.0f}; // UV: repeats per UV unit; Box: tile size in meters
    float textureRotationDegrees = 0.0f;
    glm::vec2 textureOffset{0.0f};
};

struct DrawItem {
    MeshHandle mesh;
    glm::mat4 transform{1.0f};
    std::uint32_t material = 0; // index into FrameInput::materials
};

struct Lighting {
    glm::vec3 sunDirection{-0.45f, -0.6f, 0.66f}; // toward the sun; normalized by the renderer
    glm::vec3 sunColor{1.0f};                     // linear RGB × intensity (1 = standard daylight)
    float skyIntensity = 1.0f;
    bool shadows = true;
    float shadowSoftness = 1.0f; // 0 … 4
};

enum class ToneMapping { Filmic, Neutral, None };

struct LookSettings {
    float exposureEv = 0.0f;
    ToneMapping toneMapping = ToneMapping::Filmic;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float warmth = 0.0f; // -1 … +1
    float vignette = 0.15f;
    bool clay = false; // white model: ignore material colors
};

struct FrameInput {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 cameraPosition{0.0f};
    std::span<const DrawItem> draws;
    std::span<const MaterialParams> materials; // referenced by DrawItem::material; never empty
                                               // when draws exist
    Lighting lighting;
    LookSettings look;
    bool showGrid = true;
};

// A rendered frame read back to the CPU: tightly packed 8-bit sRGB pixels, top row first.
struct CapturedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool bgra = true; // channel order in memory: BGRA (true) or RGBA (false); alpha is opaque
    std::vector<std::uint8_t> pixels;
};

struct DeviceInfo {
    std::string name;
    std::string vulkanVersion; // e.g. "1.3.290"
    std::string driverVersion;
    bool discreteGpu = false;
    bool hardwareRayTracing = false; // VK_KHR_acceleration_structure + VK_KHR_ray_query available
    std::uint64_t deviceLocalBytes = 0;
    std::uint32_t msaaSamples = 1;
};

} // namespace lb::render
