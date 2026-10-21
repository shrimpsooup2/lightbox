#version 460

// Overlay lines: flat color with a one-pixel anti-aliased edge. Not affected by the look (exposure,
// tone mapping), so handles keep their colors in any lighting.
layout(location = 0) noperspective in vec4 inColor;
layout(location = 1) noperspective in vec2 inEdge;
layout(location = 0) out vec4 outColor;

void main() {
    float coverage = clamp(inEdge.y - abs(inEdge.x), 0.0, 1.0);
    outColor = vec4(inColor.rgb, inColor.a * coverage);
}
