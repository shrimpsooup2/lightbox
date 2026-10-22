#version 460

// Selection mask: 1 wherever a selected object covers the pixel (no depth test, so hidden parts
// count too). composite.frag draws the outline around it.
layout(location = 0) out vec4 outMask;

void main() {
    outMask = vec4(1.0);
}
