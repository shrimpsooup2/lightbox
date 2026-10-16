#version 460
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUv;
layout(location = 3) out vec3 outBoxPosition; // object space scaled to meters (box mapping)
layout(location = 4) out vec3 outObjectNormal;

void main() {
    vec4 world = pc.model * vec4(inPosition, 1.0);
    outWorldPosition = world.xyz;
    outNormal = transpose(inverse(mat3(pc.model))) * inNormal;
    outUv = inUv;
    // Box mapping projects in object space so textures stick to moving objects, but at real-world
    // size: the per-axis scale of the model matrix (e.g. the unit scale on an import root) converts
    // object units to meters.
    vec3 axisScale =
        vec3(length(pc.model[0].xyz), length(pc.model[1].xyz), length(pc.model[2].xyz));
    outBoxPosition = inPosition * axisScale;
    outObjectNormal = inNormal;
    gl_Position = pc.frame.data.viewProj * world;
}
