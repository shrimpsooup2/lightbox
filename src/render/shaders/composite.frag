#version 460
#extension GL_GOOGLE_include_directive : require
#define LB_CUSTOM_PUSH
#include "frame.glsl"

// Composite: the linear HDR image through the look (exposure, white balance, contrast, saturation,
// tone mapping, vignette) into the swapchain. The source is the raster image, or the path tracer's
// running sum divided by each pixel's sample count (raster where it has no sample yet).
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) uniform sampler2D accumulated;

layout(push_constant, std430) uniform PostPush {
    vec4 params; // x: source scale, y: path tracer (1/0), z: frame time (s), w: auto exposure (1/0)
    FrameBuffer frame;
    ExposureBuffer exposure;
} post;

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
    outColor = vec4(applyLook(f, color, gl_FragCoord.xy), 1.0);
}
