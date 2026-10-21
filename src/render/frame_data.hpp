#pragma once

#include <cstddef>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "render/vulkan.hpp"

// CPU mirrors of structures read by shaders. Must match shaders/frame.glsl exactly (std430).
namespace lb::render {

constexpr std::uint32_t kMaxLights = 32;
constexpr std::uint32_t kLightPoint = 0;
constexpr std::uint32_t kLightSpot = 1;
constexpr std::uint32_t kLightArea = 2;

struct GpuLight {
    glm::vec4 position;  // xyz: world position, w: type
    glm::vec4 direction; // xyz: emission axis (spot, area), w: bulb radius (point, spot)
    glm::vec4 intensity; // rgb: point/spot intensity per steradian, area radiance (render units)
    glm::vec4 params;    // spot: x cos(outer half angle), y cos(inner); area: x/y half size
    glm::vec4 axisX;     // area: world direction of its width
    glm::vec4 axisY;     // area: world direction of its height
};
static_assert(sizeof(GpuLight) == 96);

struct FrameData {
    glm::mat4 viewProj;
    glm::mat4 invViewProj;
    glm::mat4 shadowViewProj; // world → shadow map clip space (depth 0..1)
    glm::vec4 cameraPosition;
    glm::vec4 sunDirection; // xyz: unit vector toward the sun
    glm::vec4 sunColor;     // rgb: sun radiance scale (color × intensity)
    glm::vec4 skyZenith;    // rgb, sky intensity applied
    glm::vec4 skyHorizon;
    glm::vec4 groundColor;
    glm::vec4 gridParams;   // x: minor spacing (m), y: major spacing (m), z: fade distance (m)
    glm::vec4 shadowParams; // x: enabled, y: filter radius (texels), z: 1/size, w: normal bias (m)
    glm::vec4 look;         // x: exposure scale, y: contrast, z: saturation, w: vignette
    glm::vec4 look2;        // x: tone mapping (0 filmic, 1 neutral, 2 none, 3 AgX), y: clay, z: warmth,
                            // w: auto exposure (1/0)
    glm::vec4 viewport;     // xy: size in pixels, zw: 1/size
    glm::uvec4 skyImage;    // x: texture slot (kNoTexture: procedural sky), y/z: sampling grid
                            // width/height, w: mip levels
    glm::vec4 skyImageParams; // x: rotation (radians, clockwise seen from above), y: brightness
    glm::uvec4 lightInfo;   // x: number of lights
    GpuLight lights[kMaxLights];
};
static_assert(offsetof(FrameData, lights) % 16 == 0);

// Texture slot value for "no texture" in GpuMaterial::maps/maps2.
constexpr std::uint32_t kNoTexture = 0xFFFFFFFFu;

// One material in the per-frame material buffer (std430 array element, 144 bytes).
struct GpuMaterial {
    glm::vec4 baseColor;    // rgb: linear factor, a: normal map strength
    glm::vec4 factors;      // x: roughness, y: metalness, z: normal map green sign (+1 OpenGL,
                            // −1 DirectX), w: mapping (0 UV, 1 box)
    glm::vec4 texTransform; // xy: scale (UV: repeats per UV unit; box: tiles per meter),
                            // zw: offset (tiles)
    glm::vec4 texRotation;  // x: cos, y: sin of the texture rotation; z: variation (0-1);
                            // w: height map depth (meters)
    glm::uvec4 maps;        // texture slots: x base color, y roughness, z metalness, w normal
    glm::uvec4 maps2;       // x: occlusion slot; y, z, w: channel (0-3, +4 = invert) of the
                            // roughness, metalness and occlusion maps
    glm::uvec4 maps3;       // x: height slot; y: flags (bit 0: random tile rotation); zw: unused
    glm::vec4 glass;        // x: transmission, y: index of refraction, z: thin-walled (1/0)
    glm::vec4 emission;     // rgb: emitted radiance (render units)
};
static_assert(offsetof(GpuMaterial, factors) == 16);
static_assert(offsetof(GpuMaterial, texTransform) == 32);
static_assert(offsetof(GpuMaterial, texRotation) == 48);
static_assert(offsetof(GpuMaterial, maps) == 64);
static_assert(offsetof(GpuMaterial, maps2) == 80);
static_assert(offsetof(GpuMaterial, maps3) == 96);
static_assert(offsetof(GpuMaterial, glass) == 112);
static_assert(sizeof(GpuMaterial) == 144);

// One overlay line (std430 array element, 48 bytes), from lb::OverlayLine.
struct GpuOverlayLine {
    glm::vec4 a;     // xyz: world position, w: width in pixels
    glm::vec4 b;
    glm::vec4 color;
};
static_assert(sizeof(GpuOverlayLine) == 48);

struct PushConstants {
    glm::mat4 model;
    VkDeviceAddress frame;     // this frame's FrameData buffer
    VkDeviceAddress materials; // this frame's GpuMaterial array (overlay pass: GpuOverlayLine)
    std::uint32_t material;    // index into the material array
    std::uint32_t pad[3];      // explicit tail padding (the shader block uses bytes 0-83)
};
static_assert(offsetof(PushConstants, frame) == 64);
static_assert(offsetof(PushConstants, materials) == 72);
static_assert(offsetof(PushConstants, material) == 80);
static_assert(sizeof(PushConstants) == 96);
static_assert(sizeof(PushConstants) <= 128, "Vulkan guarantees only 128 bytes of push constants");

// Push constants of post passes (composite), matching PostPush in the shaders.
struct PostPushConstants {
    glm::vec4 params;         // x: source scale, y: source (0 raster, 1 path tracer), z: frame time
                              // in seconds, w: auto exposure (1/0)
    VkDeviceAddress frame;    // this frame's FrameData
    VkDeviceAddress exposure; // ExposureState (auto exposure)
};
static_assert(offsetof(PostPushConstants, frame) == 16);
static_assert(sizeof(PostPushConstants) == 32);

// Push constants of the path tracer (pathtrace.comp).
struct PathPushConstants {
    VkDeviceAddress frame;     // FrameData
    VkDeviceAddress materials; // GpuMaterial[]
    VkDeviceAddress vertices;  // RayVertex[]
    VkDeviceAddress triangles; // uvec4[]
    VkDeviceAddress nodes;     // BvhNode[]
    VkDeviceAddress instances; // RayInstance[]
    VkDeviceAddress skySampling; // float[]: row CDF, column CDFs, cell probabilities (sky image)
    VkDeviceAddress exposure;    // ExposureState: the firefly clamp follows the exposure
    alignas(16) glm::uvec4 tile; // xy: first pixel, zw: size
    glm::uvec4 params;           // x: sample index, y: max bounces, z: node count, w: material count
};
static_assert(offsetof(PathPushConstants, tile) == 64);
static_assert(sizeof(PathPushConstants) == 96);

// Auto exposure state (exposure.comp writes it, the composite reads it).
struct ExposureState {
    float adaptedLog2 = 0.0f; // smoothed log2 of the scene's average luminance
    float initialized = 0.0f; // 0 until the first measurement (then it jumps there)
    float pad[2]{};
};

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv; // (0, 0) = image top-left; zero for meshes without texture coordinates
};
static_assert(offsetof(Vertex, uv) == 24);
static_assert(sizeof(Vertex) == 32);

} // namespace lb::render
