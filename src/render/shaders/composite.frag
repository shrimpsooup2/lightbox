#version 460
#extension GL_GOOGLE_include_directive : require
#define LB_CUSTOM_PUSH
#include "frame.glsl"

// Composite: the linear HDR image through the look (exposure, white balance, contrast, saturation,
// tone mapping, vignette) into the swapchain. The source is the raster image, or the path tracer's
// running sum divided by each pixel's sample count (raster where it has no sample yet). Then the
// selection outline on top: the edge just outside the selection mask, in the accent color.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) uniform sampler2D accumulated;
layout(set = 0, binding = 2) uniform sampler2D selectionMask;

layout(push_constant, std430) uniform PostPush {
    vec4 params; // x: source scale, y: path tracer (1/0), z: frame time (s), w: auto exposure (1/0)
    FrameBuffer frame;
    ExposureBuffer exposure;
    vec4 outline; // rgb: color (linear), a: width in pixels (0: no outline)
} post;

// How much of a ring of samples around the pixel lies on selected objects (0 outside the band).
float outlineCoverage(ivec2 pixel, float width) {
    ivec2 last = textureSize(selectionMask, 0) - 1;
    float hits = 0.0;
    for (int i = 0; i < 12; ++i) {
        float angle = float(i) * (6.2831853 / 12.0);
        vec2 direction = vec2(cos(angle), sin(angle));
        ivec2 outer = clamp(pixel + ivec2(round(direction * width)), ivec2(0), last);
        ivec2 inner = clamp(pixel + ivec2(round(direction * width * 0.5)), ivec2(0), last);
        hits += texelFetch(selectionMask, outer, 0).r + texelFetch(selectionMask, inner, 0).r;
    }
    return clamp(hits / 3.0, 0.0, 1.0); // a few samples inside already make a solid line
}

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

void main() {
    FrameData f = post.frame.data;
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec3 color = texelFetch(source, pixel, 0).rgb;
    if (post.params.y > 0.5) {
        vec4 sum = texelFetch(accumulated, pixel, 0);
        if (sum.a > 0.0) {
            color = sum.rgb / sum.a;
        }
    }
    color *= post.params.x;
    // Guard against NaN/inf from any pass so one bad pixel can't poison the image.
    color = any(isnan(color)) || any(isinf(color)) ? vec3(0.0) : min(color, vec3(65000.0));
    if (post.params.w > 0.5 && post.exposure.state.y > 0.5) {
        color *= kExposureKey / exp2(post.exposure.state.x);
    }
    vec3 display = applyLook(f, color, gl_FragCoord.xy);
    if (post.outline.a > 0.0 && texelFetch(selectionMask, pixel, 0).r < 0.5) {
        display = mix(display, post.outline.rgb, outlineCoverage(pixel, post.outline.a));
    }
    outColor = vec4(display, 1.0);
}
