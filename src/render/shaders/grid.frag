#version 460
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"

// Infinite ground grid on z = 0: 1 m minor lines, 10 m major lines, red X axis, green Y axis.
// Lines are a constant pixel width and fade out before they alias.
layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

float gridLines(vec2 uv, float widthPx) {
    vec2 derivative = max(fwidth(uv), vec2(1e-6));
    vec2 distancePx = abs(fract(uv - 0.5) - 0.5) / derivative;
    float coverage = 1.0 - clamp(min(distancePx.x, distancePx.y) - 0.5 * widthPx + 0.5, 0.0, 1.0);
    float density = max(derivative.x, derivative.y); // grid cells per pixel
    return coverage * (1.0 - smoothstep(0.08, 0.25, density));
}

float axisLine(float coordinate, float derivative, float widthPx) {
    return 1.0 - clamp(abs(coordinate) / max(derivative, 1e-6) - 0.5 * widthPx + 0.5, 0.0, 1.0);
}

void main() {
    FrameData f = pc.frame.data;
    vec3 origin = f.cameraPosition.xyz;
    vec3 dir = viewRayDirection(f, inNdc);

    // Derivatives must be computed in uniform control flow, so intersect first and discard last.
    bool hit = dir.z * origin.z < 0.0;
    float t = hit ? -origin.z / dir.z : 0.0;
    vec3 p = origin + dir * t;

    float minor = gridLines(p.xy / f.gridParams.x, 1.0);
    float major = gridLines(p.xy / f.gridParams.y, 1.5);
    vec2 d = fwidth(p.xy);
    float xAxis = axisLine(p.y, d.y, 2.0); // the X axis is the line y = 0
    float yAxis = axisLine(p.x, d.x, 2.0);

    vec3 color = vec3(0.9);
    float alpha = max(minor * 0.16, major * 0.32);
    color = mix(color, vec3(0.86, 0.24, 0.24), xAxis);
    alpha = max(alpha, xAxis * 0.85);
    color = mix(color, vec3(0.36, 0.74, 0.30), yAxis);
    alpha = max(alpha, yAxis * 0.85);
    alpha *= 1.0 - smoothstep(0.3 * f.gridParams.z, f.gridParams.z, t);

    if (!hit || alpha < 0.002) {
        discard;
    }
    vec4 clip = f.viewProj * vec4(p, 1.0);
    gl_FragDepth = clip.z / clip.w;
    // The grid is a guide, not part of the scene: undo the exposure so it keeps its brightness.
    outColor = vec4(color / max(f.look.x, 1e-4), alpha);
}
