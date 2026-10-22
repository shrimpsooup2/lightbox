#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "core/overlay.hpp"

namespace lb {
struct EnvironmentMap;
}

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
    MaterialTexture heightMap; // parallax relief (red channel)
    float normalStrength = 1.0f;
    bool normalFlipY = false;
    float heightDepth = 0.0f; // meters, white to black
    TextureMapping mapping = TextureMapping::UV;
    glm::vec2 textureScale{1.0f}; // UV: repeats per UV unit; Box: tile size in meters
    float textureRotationDegrees = 0.0f;
    glm::vec2 textureOffset{0.0f};
    float variation = 0.0f; // 0…1 large-scale brightness/roughness variation (hides tiling)
    bool randomTileRotation = false;
    float transmission = 0.0f; // glass: 0 opaque … 1 clear; tinted by baseColor
    float ior = 1.5f;
    bool thinWalled = true;
    glm::vec3 emission{0.0f}; // emitted radiance, render units
};

// Photometric quantities (lux, nits) to the renderer's linear units: the sun of intensity 1
// (100 000 lux) lights a white surface to radiance 2.
constexpr float kRenderUnitsPerLux = 6.2831853f / 100000.0f;

enum class LightKind { Point, Spot, Area };

// One artificial light in world space, intensities already in render units.
struct LightParams {
    std::uint64_t entity = 0; // the document entity carrying the light (picking, highlighting)
    LightKind kind = LightKind::Point;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f}; // emission axis (spot, area)
    glm::vec3 axisX{1.0f, 0.0f, 0.0f};      // area: width direction
    glm::vec3 axisY{0.0f, 1.0f, 0.0f};      // area: height direction
    glm::vec3 intensity{0.0f};  // point/spot: per steradian (candela × units); area: radiance
    float radius = 0.05f;       // point/spot bulb radius, meters
    glm::vec2 halfSize{0.3f};   // area: half width/height, meters
    float cosOuter = 0.5f;      // spot: cosine of the outer half angle
    float cosInner = 0.6f;      // spot: cosine where the edge fade starts
};

struct DrawItem {
    MeshHandle mesh;
    glm::mat4 transform{1.0f};
    std::uint32_t material = 0; // index into FrameInput::materials
    bool transparent = false;   // glass: drawn after opaque surfaces, blended
    std::uint64_t entity = 0;   // the document entity (selection outline)
};

struct Lighting {
    glm::vec3 sunDirection{-0.45f, -0.6f, 0.66f}; // toward the sun; normalized by the renderer
    glm::vec3 sunColor{1.0f};                     // linear RGB × intensity (1 = standard daylight)
    float skyIntensity = 1.0f;
    bool shadows = true;
    float shadowSoftness = 1.0f; // 0 … 4
    // Sky image (null: procedural sky), its texture from Renderer::uploadEnvironmentMap, and how far
    // it is turned (clockwise seen from above). The caller derives the sun from it.
    std::shared_ptr<const EnvironmentMap> skyImage;
    TextureHandle skyImageTexture;
    float skyImageRotationDegrees = 0.0f;
};

enum class ToneMapping { Filmic, Neutral, None, AgX };

struct LookSettings {
    float exposureEv = 0.0f;
    ToneMapping toneMapping = ToneMapping::Filmic;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float warmth = 0.0f; // -1 … +1
    float vignette = 0.15f;
    bool clay = false; // white model: ignore material colors
    bool autoExposure = false; // exposureEv is then a compensation on top of the measured value
};

// Ray tracing (the quality renderer, D-027). Quality sets how far each pixel converges; speed sets
// how much GPU time per frame it may use, so slow machines get the same image later instead of
// freezing and fast ones finish sooner.
enum class RayQuality { Draft, Good, Final, Ultra };
enum class RaySpeed { Gentle, Balanced, Fast };

struct RayTracingSettings {
    bool enabled = false;
    RayQuality quality = RayQuality::Good;
    RaySpeed speed = RaySpeed::Balanced;
    bool interacting = false; // camera or objects moving: show the raster view, restart afterwards
};

struct RayScene;

struct RayTracingStatus {
    bool supported = false; // the GPU can run the path tracer
    bool active = false;    // enabled and showing (or preparing) a ray-traced image
    bool preparing = false; // waiting for the scene's ray tracing data
    std::uint32_t samples = 0;
    std::uint32_t targetSamples = 0;
    double seconds = 0.0;   // since the current image started
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
    std::vector<OverlayLine> overlay; // gizmos, drawn last; widths in physical pixels
    // Selection outline: entity ids of draws to outline (sorted ascending), its color (linear RGB)
    // and width in physical pixels. Drawn over the final image like the overlay.
    std::span<const std::uint64_t> highlighted;
    glm::vec3 highlightColor{0.0f, 0.4f, 0.9f};
    float highlightWidth = 2.0f;
    std::span<const LightParams> lights; // up to kMaxLights are used
    RayTracingSettings rayTracing;
    // Ray-traced geometry for the current scene (null while it is being prepared) and a counter
    // that changes with anything that affects the image except the look (restarts accumulation).
    std::shared_ptr<const RayScene> rayScene;
    std::uint64_t sceneVersion = 0;
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
    bool pathTracing = false;        // compute path tracer supported (non-uniform texture indexing)
    std::uint64_t deviceLocalBytes = 0;
    std::uint32_t msaaSamples = 1;
};

} // namespace lb::render
