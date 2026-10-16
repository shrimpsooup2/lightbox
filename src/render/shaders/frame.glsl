// Shared declarations for all LightBox shaders. Must match src/render/frame_data.hpp exactly.
#extension GL_EXT_buffer_reference : require

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
    vec4 look2;        // x: tone mapping (0 filmic, 1 neutral, 2 none), y: clay, z: warmth
    vec4 viewport;     // xy: size in pixels, zw: 1/size
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FrameBuffer {
    FrameData data;
};

const uint kNoTexture = 0xFFFFFFFFu;

// One material (std430, 96 bytes). Texture slots index the bindless arrays in mesh.frag.
struct GpuMaterial {
    vec4 baseColor;    // rgb: linear factor, a: normal map strength
    vec4 factors;      // x: roughness, y: metalness, z: normal map green sign,
                       // w: mapping (0 UV, 1 box)
    vec4 texTransform; // xy: scale (UV: repeats per UV unit; box: tiles per meter), zw: offset
    vec4 texRotation;  // x: cos, y: sin of the texture rotation
    uvec4 maps;        // texture slots: x base color, y roughness, z metalness, w normal
    uvec4 maps2;       // x: occlusion slot; y, z, w: channel (+4 = invert) of roughness, metalness,
                       // occlusion
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MaterialBuffer {
    GpuMaterial materials[];
};

// 84 bytes used; the CPU struct pads to 96.
layout(push_constant, std430) uniform PushConstants {
    mat4 model;
    FrameBuffer frame;
    MaterialBuffer materials;
    uint material; // index into materials
} pc;

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
    } else {
        color = clamp(color, 0.0, 1.0);
    }

    vec2 uv = fragCoord * f.viewport.zw - 0.5;
    float falloff = clamp(dot(uv, uv) * 2.0, 0.0, 1.0);
    return color * (1.0 - f.look.w * falloff);
}
