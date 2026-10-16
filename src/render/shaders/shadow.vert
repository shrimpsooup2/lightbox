#version 460
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"

// Depth-only pass from the sun into the shadow map.
layout(location = 0) in vec3 inPosition;

void main() {
    gl_Position = pc.frame.data.shadowViewProj * pc.model * vec4(inPosition, 1.0);
}
