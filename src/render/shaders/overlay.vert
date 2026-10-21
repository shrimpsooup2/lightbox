#version 460
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"

// Overlay lines (gizmos) as screen-space quads, six vertices per line, no vertex buffer. In this
// pass the push constants' second address points at the frame's line array instead of materials.
struct OverlayLine {
    vec4 a;     // xyz: world position, w: width in pixels
    vec4 b;
    vec4 color;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer OverlayBuffer {
    OverlayLine lines[];
};

layout(location = 0) noperspective out vec4 outColor;
layout(location = 1) noperspective out vec2 outEdge; // x: pixels from the center line, y: half width

void main() {
    FrameData f = pc.frame.data;
    OverlayLine line = OverlayBuffer(pc.materials).lines[gl_VertexIndex / 6];
    const int corners[6] = int[6](0, 1, 2, 2, 1, 3); // 0, 1 at end a; 2, 3 at end b
    int corner = corners[gl_VertexIndex % 6];

    vec4 clipA = f.viewProj * vec4(line.a.xyz, 1.0);
    vec4 clipB = f.viewProj * vec4(line.b.xyz, 1.0);
    // Cut the part behind the camera (w is the view depth).
    const float nearW = 1e-3;
    if (clipA.w < nearW && clipB.w < nearW) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // outside the depth range: clipped
        return;
    }
    if (clipA.w < nearW) {
        clipA = mix(clipA, clipB, (nearW - clipA.w) / (clipB.w - clipA.w));
    }
    if (clipB.w < nearW) {
        clipB = mix(clipB, clipA, (nearW - clipB.w) / (clipA.w - clipB.w));
    }

    vec2 halfSize = 0.5 * f.viewport.xy;
    vec2 a = clipA.xy / clipA.w * halfSize;
    vec2 b = clipB.xy / clipB.w * halfSize;
    vec2 along = b - a;
    float len = length(along);
    along = len > 1e-4 ? along / len : vec2(1.0, 0.0);
    vec2 across = vec2(-along.y, along.x);

    bool atB = corner >= 2;
    float side = (corner & 1) == 1 ? 1.0 : -1.0;
    float halfWidth = 0.5 * (atB ? line.b.w : line.a.w) + 1.0; // +1 px for the soft edge
    vec2 p = (atB ? b : a) + across * side * halfWidth;
    gl_Position = vec4(p / halfSize, 0.5, 1.0);
    outColor = line.color;
    outEdge = vec2(side * halfWidth, halfWidth);
}
