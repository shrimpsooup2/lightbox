#version 460
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
#include "material.glsl"

// The sky image (HDRI) or the procedural sky (colors derived from the sun height on the CPU), with
// a sun disc and soft glow. With an image the sun was taken out of it and is drawn here instead.
layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

void main() {
    FrameData f = pc.frame.data;
    vec3 dir = viewRayDirection(f, inNdc);
    vec3 color = skyLight(f, dir, 0.0);

    float sun = max(dot(dir, f.sunDirection.xyz), 0.0);
    float visible = step(0.0, dir.z) * smoothstep(-0.03, 0.02, f.sunDirection.z);
    color += f.sunColor.rgb * (pow(sun, 4000.0) * 30.0 + pow(sun, 48.0) * 0.25) * visible;
    outColor = vec4(color, 1.0); // linear HDR; the composite pass applies the look
}
