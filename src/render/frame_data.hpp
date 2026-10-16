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
    glm::vec4 look2;        // x: tone mapping (0 filmic, 1 neutral, 2 none), y: clay, z: warmth
    glm::vec4 viewport;     // xy: size in pixels, zw: 1/size
};

// Texture slot value for "no texture" in GpuMaterial::maps/maps2.
constexpr std::uint32_t kNoTexture = 0xFFFFFFFFu;

// One material in the per-frame material buffer (std430 array element, 96 bytes).
struct GpuMaterial {
    glm::vec4 baseColor;    // rgb: linear factor, a: normal map strength
    glm::vec4 factors;      // x: roughness, y: metalness, z: normal map green sign (+1 OpenGL,
                            // −1 DirectX), w: mapping (0 UV, 1 box)
    glm::vec4 texTransform; // xy: scale (UV: repeats per UV unit; box: tiles per meter),
                            // zw: offset (tiles)
    glm::vec4 texRotation;  // x: cos, y: sin of the texture rotation; zw: unused
    glm::uvec4 maps;        // texture slots: x base color, y roughness, z metalness, w normal
    glm::uvec4 maps2;       // x: occlusion slot; y, z, w: channel (0-3, +4 = invert) of the
                            // roughness, metalness and occlusion maps
};
static_assert(offsetof(GpuMaterial, factors) == 16);
static_assert(offsetof(GpuMaterial, texTransform) == 32);
static_assert(offsetof(GpuMaterial, texRotation) == 48);
static_assert(offsetof(GpuMaterial, maps) == 64);
static_assert(offsetof(GpuMaterial, maps2) == 80);
static_assert(sizeof(GpuMaterial) == 96);

struct PushConstants {
    glm::mat4 model;
    VkDeviceAddress frame;     // this frame's FrameData buffer
    VkDeviceAddress materials; // this frame's GpuMaterial array
    std::uint32_t material;    // index into the material array
    std::uint32_t pad[3];      // explicit tail padding (the shader block uses bytes 0-83)
};
static_assert(offsetof(PushConstants, frame) == 64);
static_assert(offsetof(PushConstants, materials) == 72);
static_assert(offsetof(PushConstants, material) == 80);
static_assert(sizeof(PushConstants) == 96);
static_assert(sizeof(PushConstants) <= 128, "Vulkan guarantees only 128 bytes of push constants");

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv; // (0, 0) = image top-left; zero for meshes without texture coordinates
};
static_assert(offsetof(Vertex, uv) == 24);
static_assert(sizeof(Vertex) == 32);

} // namespace lb::render
