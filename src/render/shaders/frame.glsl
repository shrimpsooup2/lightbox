// Shared declarations for all LightBox shaders. Must match src/render/frame_data.hpp exactly.
#extension GL_EXT_buffer_reference : require

// Artificial lights (kMaxLights per frame). Intensities are in render units (1 = 15 915 lux or nits).
const uint kMaxLights = 32u;
const uint kLightPoint = 0u;
const uint kLightSpot = 1u;
const uint kLightArea = 2u;

struct GpuLight {
    vec4 position;  // xyz: world position, w: type (kLightPoint/Spot/Area)
    vec4 direction; // xyz: emission axis (spot, area), w: bulb radius (point, spot)
    vec4 intensity; // rgb: point/spot intensity per steradian, area radiance; w: unused
    vec4 params;    // spot: x cos(outer half angle), y cos(inner); area: x half width, y half height
    vec4 axisX;     // area: world direction of its width
    vec4 axisY;     // area: world direction of its height
};

struct FrameData {
    mat4 viewProj;
    mat4 invViewProj;
    mat4 shadowViewProj;
    vec4 cameraPosition;
    vec4 sunDirection; // xyz: unit vector toward the sun
    vec4 sunColor;     // rgb: sun radiance scale
    vec4 skyZenith;
    vec4 skyHorizon;
    vec4 groundColor;
    vec4 gridParams;   // x: minor spacing (m), y: major spacing (m), z: fade distance (m)
    vec4 shadowParams; // x: enabled, y: filter radius (texels), z: 1/size, w: normal bias (m)
    vec4 look;         // x: exposure scale, y: contrast, z: saturation, w: vignette
    vec4 look2;        // x: tone mapping (0 filmic, 1 neutral, 2 none, 3 AgX), y: clay, z: warmth,
                       // w: auto exposure (1/0)
    vec4 viewport;     // xy: size in pixels, zw: 1/size
    uvec4 skyImage;    // x: texture slot (kNoTexture: procedural sky), y/z: sampling grid size,
                       // w: mip levels
    vec4 skyImageParams; // x: rotation (radians, clockwise seen from above), y: brightness
    uvec4 lightInfo;   // x: number of lights
    GpuLight lights[kMaxLights];
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FrameBuffer {
    FrameData data;
};

const uint kNoTexture = 0xFFFFFFFFu;

// Auto exposure state: x = smoothed log2 average scene luminance, y = 1 once measured.
layout(buffer_reference, std430, buffer_reference_align = 16) buffer ExposureBuffer {
    vec4 state;
};
// Average luminance that auto exposure maps to the standard exposure (≈ a sunlit daytime view).
const float kExposureKey = 0.5;

// One material (std430, 144 bytes). Texture slots index the bindless arrays in mesh.frag.
struct GpuMaterial {
    vec4 baseColor;    // rgb: linear factor, a: normal map strength
    vec4 factors;      // x: roughness, y: metalness, z: normal map green sign,
                       // w: mapping (0 UV, 1 box)
    vec4 texTransform; // xy: scale (UV: repeats per UV unit; box: tiles per meter), zw: offset
    vec4 texRotation;  // x: cos, y: sin of the texture rotation; z: variation (0-1);
                       // w: height map depth (meters)
    uvec4 maps;        // texture slots: x base color, y roughness, z metalness, w normal
    uvec4 maps2;       // x: occlusion slot; y, z, w: channel (+4 = invert) of roughness, metalness,
                       // occlusion
    uvec4 maps3;       // x: height slot; y: flags (bit 0: random tile rotation)
    vec4 glass;        // x: transmission, y: index of refraction, z: thin-walled (1/0)
    vec4 emission;     // rgb: emitted radiance (render units)
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MaterialBuffer {
    GpuMaterial materials[];
};

// Scene passes: 84 bytes used; the CPU struct pads to 96. Passes with their own push constant block
// (composite, path tracer) define LB_CUSTOM_PUSH before including this file.
#ifndef LB_CUSTOM_PUSH
layout(push_constant, std430) uniform PushConstants {
    mat4 model;
    FrameBuffer frame;
    MaterialBuffer materials;
    uint material; // index into materials
} pc;
#endif

// World-space direction of the view ray through an NDC position. With an infinite reverse-Z
// projection, depth 0 is the point at infinity: unprojecting it yields w = 0 and xyz = direction.
vec3 viewRayDirection(FrameData f, vec2 ndc) {
    return normalize((f.invViewProj * vec4(ndc, 0.0, 1.0)).xyz);
}

// Sky radiance along a direction: horizon-to-zenith gradient, darker ground below the horizon.
vec3 skyRadiance(FrameData f, vec3 dir) {
    float h = dir.z;
    vec3 sky = mix(f.skyHorizon.rgb, f.skyZenith.rgb, pow(clamp(h, 0.0, 1.0), 0.6));
    vec3 ground = mix(f.skyHorizon.rgb * 0.8, f.groundColor.rgb, clamp(-h * 8.0, 0.0, 1.0));
    return mix(ground, sky, smoothstep(-0.002, 0.002, h));
}

vec3 tonemapFilmic(vec3 x) {
    // Narkowicz's fit of the ACES reference rendering transform.
    x *= 0.6;
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 tonemapNeutral(vec3 color) {
    // Khronos PBR Neutral tone mapper.
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) {
        return color;
    }
    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}

// AgX (Troy Sobotka), as fitted by Benjamin Wrensch: a log encoding in a slightly inset gamut, then a
// sigmoid. Bright, saturated light desaturates toward white instead of skewing hue. Returns linear.
vec3 tonemapAgx(vec3 color) {
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    vec3 x = clamp(log2(max(inset * color, vec3(1e-10))), minEv, maxEv);
    x = (x - minEv) / (maxEv - minEv);
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    x = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x -
        0.00232;
    return pow(max(outset * x, vec3(0.0)), vec3(2.2));
}

// Exposure, white balance, contrast, saturation, tone mapping and vignette: the "look" of the image.
// Input is scene-referred linear light; output is display-referred linear (the sRGB swapchain encodes).
vec3 applyLook(FrameData f, vec3 color, vec2 fragCoord) {
    color *= f.look.x;
    float warmth = f.look2.z;
    color *= vec3(1.0 + 0.12 * warmth, 1.0, 1.0 - 0.12 * warmth);
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = max(mix(vec3(luminance), color, f.look.z), 0.0);
    color = 0.18 * pow(color / 0.18, vec3(f.look.y)); // contrast pivots on mid grey

    int toneMapping = int(f.look2.x + 0.5);
    if (toneMapping == 0) {
        color = tonemapFilmic(color);
    } else if (toneMapping == 1) {
        color = tonemapNeutral(color);
    } else if (toneMapping == 3) {
        color = tonemapAgx(color);
    } else {
        color = clamp(color, 0.0, 1.0);
    }

    vec2 uv = fragCoord * f.viewport.zw - 0.5;
    float falloff = clamp(dot(uv, uv) * 2.0, 0.0, 1.0);
    return color * (1.0 - f.look.w * falloff);
}
