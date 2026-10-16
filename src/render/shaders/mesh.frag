#version 460
#extension GL_GOOGLE_include_directive : require
// Runtime-sized descriptor arrays (the bindless texture tables). Texture indices come from the
// per-draw material, so they are dynamically uniform and need no nonuniformEXT qualifier.
#extension GL_EXT_nonuniform_qualifier : require
#include "frame.glsl"

// Metal/roughness shading: GGX specular + Lambert diffuse from the sun (with soft shadows), plus sky
// fill light (hemisphere diffuse, sky reflections). Stage 3 adds GI, IBL and local lights.

// Set 0 (renderer-wide): material textures. Every texture slot has an sRGB view (base color) and a
// linear view (data maps) of the same image.
layout(set = 0, binding = 0) uniform sampler materialSampler;
layout(set = 0, binding = 1) uniform texture2D srgbTextures[];
layout(set = 0, binding = 2) uniform texture2D linearTextures[];
// Set 1 (per viewport): the sun shadow map.
layout(set = 1, binding = 0) uniform sampler2DShadow shadowMap;

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec3 inBoxPosition; // object space scaled to meters
layout(location = 4) in vec3 inObjectNormal;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265;
const float kSunIrradiance = 2.0; // sun intensity 1 ≈ bright daylight in viewport units

const vec2 kPoisson[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870),
    vec2(0.34495938, 0.29387760), vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379), vec2(0.44323325, -0.97511554),
    vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367),
    vec2(0.14383161, -0.14100790));

float interleavedGradientNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

float sunShadow(FrameData f, vec3 position, vec3 normal) {
    if (f.shadowParams.x < 0.5) {
        return 1.0;
    }
    vec4 clip = f.shadowViewProj * vec4(position + normal * f.shadowParams.w, 1.0);
    vec3 coord = clip.xyz / clip.w;
    vec2 uv = coord.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || coord.z >= 1.0) {
        return 1.0;
    }
    float radius = max(f.shadowParams.y, 0.0) * f.shadowParams.z;
    if (radius <= 0.0) {
        return texture(shadowMap, vec3(uv, coord.z));
    }
    // Rotated Poisson disc: soft penumbrae without banding (MSAA smooths the remaining noise).
    float angle = interleavedGradientNoise(gl_FragCoord.xy) * 2.0 * PI;
    mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
    float lit = 0.0;
    for (int i = 0; i < 16; ++i) {
        lit += texture(shadowMap, vec3(uv + rotation * kPoisson[i] * radius, coord.z));
    }
    return lit / 16.0;
}

// Karis' analytic fit of the split-sum environment BRDF.
vec2 environmentBrdf(float roughness, float nDotV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// --- Material textures ---------------------------------------------------------------------------

// Where texture lookups land: UV mapping uses one projection, box mapping up to three.
struct Projection {
    vec2 uv; // after the material's scale, rotation and offset
    // Screen-space derivatives of uv. Box mapping skips zero-weight projections per pixel, so all
    // lookups use textureGrad with derivatives taken beforehand in uniform control flow.
    vec2 dx;
    vec2 dy;
    float weight;
    vec3 tangent; // world direction of +u in the image
    vec3 up;      // world direction of "up" in the image (−v): +Y of OpenGL-convention normal maps
};

Projection gProjections[3];
int gProjectionCount = 0;

vec3 safeNormalize(vec3 v) {
    float lengthSq = dot(v, v);
    return lengthSq > 0.0 ? v * inversesqrt(lengthSq) : vec3(0.0);
}

vec2 transformUv(GpuMaterial m, vec2 uv) {
    vec2 scaled = uv * m.texTransform.xy;
    vec2 cs = m.texRotation.xy;
    return vec2(cs.x * scaled.x - cs.y * scaled.y, cs.y * scaled.x + cs.x * scaled.y) +
           m.texTransform.zw;
}

Projection uvProjection(GpuMaterial m, vec3 n) {
    Projection p;
    p.uv = transformUv(m, inUv);
    p.dx = dFdx(p.uv);
    p.dy = dFdy(p.uv);
    p.weight = 1.0;
    // Cotangent frame from screen-space derivatives (Schüler, "Followup: Normal Mapping Without
    // Precomputed Tangents"): meshes carry no tangents, and the frame follows the UV transform and
    // mirrored UVs. The determinant sign makes it independent of the screen's handedness.
    vec3 dp1 = dFdx(inWorldPosition);
    vec3 dp2 = dFdy(inWorldPosition);
    vec3 dp2perp = cross(dp2, n);
    vec3 dp1perp = cross(n, dp1);
    vec3 t = dp2perp * p.dx.x + dp1perp * p.dy.x;
    vec3 b = dp2perp * p.dx.y + dp1perp * p.dy.y;
    float lengthSq = max(dot(t, t), dot(b, b));
    // Without UVs (all zero) the frame vanishes and normal maps have no effect.
    float scale = lengthSq > 0.0 ? inversesqrt(lengthSq) : 0.0;
    scale *= dot(dp1, dp2perp) < 0.0 ? -1.0 : 1.0;
    p.tangent = t * scale;
    p.up = -b * scale;
    return p;
}

// One box-mapping projection: `coords` in meters along the projection's `tangent` (+u) and `up`
// (−v) world directions.
Projection boxProjection(GpuMaterial m, vec2 coords, vec3 tangent, vec3 up, float weight) {
    Projection p;
    p.uv = transformUv(m, coords);
    p.dx = dFdx(p.uv);
    p.dy = dFdy(p.uv);
    p.weight = weight;
    // Image axes after transformUv's scale and rotation (columns of its inverse Jacobian).
    vec2 s = m.texTransform.xy;
    vec2 cs = m.texRotation.xy;
    p.tangent = safeNormalize(tangent * (cs.x / s.x) + up * (cs.y / s.y));
    p.up = safeNormalize(up * (cs.x / s.y) - tangent * (cs.y / s.x));
    return p;
}

// Must run in uniform control flow (screen-space derivatives).
void setupProjections(GpuMaterial m, vec3 n, vec3 objectNormal) {
    if (m.factors.w < 0.5) {
        gProjections[0] = uvProjection(m, n);
        gProjectionCount = 1;
        return;
    }
    // Box mapping: triplanar with sharp weights. Axis-aligned faces (most of CAD geometry) get a
    // single projection, so textures never ghost; sloped and curved surfaces get a short blend
    // (roughly 35°–55° off an axis) instead of the hard seam of a dominant-axis pick.
    vec3 axisScale =
        vec3(length(pc.model[0].xyz), length(pc.model[1].xyz), length(pc.model[2].xyz));
    // Normal in scaled object space (normals transform with the inverse scale).
    vec3 nb = normalize(objectNormal / max(axisScale, vec3(1e-12)));
    vec3 w = nb * nb;
    w *= w;
    w *= w; // |n|^8
    w /= w.x + w.y + w.z;
    w = max(w - 0.05, 0.0);
    w /= w.x + w.y + w.z;

    // Projections are oriented as seen from outside each face (u right, image top up, Z-up world),
    // so text and brick courses read correctly on every side and are never mirrored.
    vec3 side = mix(vec3(-1.0), vec3(1.0), greaterThanEqual(nb, vec3(0.0)));
    vec3 axisX = safeNormalize(pc.model[0].xyz);
    vec3 axisY = safeNormalize(pc.model[1].xyz);
    vec3 axisZ = safeNormalize(pc.model[2].xyz);
    vec3 p = inBoxPosition;
    gProjections[0] = boxProjection(m, vec2(side.x * p.y, -p.z), side.x * axisY, axisZ, w.x);
    gProjections[1] = boxProjection(m, vec2(-side.y * p.x, -p.z), -side.y * axisX, axisZ, w.y);
    gProjections[2] = boxProjection(m, vec2(side.z * p.x, -p.y), side.z * axisX, axisY, w.z);
    gProjectionCount = 3;
}

vec4 sampleMap(uint slot, bool srgb) {
    vec4 sum = vec4(0.0);
    for (int i = 0; i < gProjectionCount; ++i) {
        Projection p = gProjections[i];
        if (p.weight <= 0.0) {
            continue;
        }
        vec4 texel = srgb
            ? textureGrad(sampler2D(srgbTextures[slot], materialSampler), p.uv, p.dx, p.dy)
            : textureGrad(sampler2D(linearTextures[slot], materialSampler), p.uv, p.dx, p.dy);
        sum += texel * p.weight;
    }
    return sum;
}

// Scalar maps read one channel (packed maps), optionally inverted (e.g. gloss → roughness).
float sampleChannel(uint slot, uint selector) {
    float value = sampleMap(slot, false)[int(selector & 3u)];
    return (selector & 4u) != 0u ? 1.0 - value : value;
}

// Tangent-space normal map around the camera-facing normal n.
vec3 perturbNormal(GpuMaterial m, vec3 n) {
    vec3 sum = vec3(0.0);
    for (int i = 0; i < gProjectionCount; ++i) {
        Projection p = gProjections[i];
        if (p.weight <= 0.0) {
            continue;
        }
        vec3 t = textureGrad(sampler2D(linearTextures[m.maps.w], materialSampler), p.uv, p.dx,
                             p.dy).xyz * 2.0 - 1.0;
        t.xy *= m.baseColor.a;  // strength
        t.y *= m.factors.z;     // DirectX maps store green = −Y
        // Box projections are not tangent to sloped faces: keep only the in-surface part.
        vec3 tangent = p.tangent - n * dot(n, p.tangent);
        vec3 up = p.up - n * dot(n, p.up);
        sum += safeNormalize(tangent * t.x + up * t.y + n * t.z) * p.weight;
    }
    return dot(sum, sum) > 1e-12 ? normalize(sum) : n;
}

void main() {
    FrameData f = pc.frame.data;
    GpuMaterial m = pc.materials.materials[pc.material];
    vec3 n = normalize(inNormal);
    vec3 objectNormal = inObjectNormal;
    vec3 v = normalize(f.cameraPosition.xyz - inWorldPosition);
    // Two-sided: CAD exports have unreliable winding, so always shade the side facing the camera.
    if (dot(n, v) < 0.0) {
        n = -n;
        objectNormal = -objectNormal;
    }
    vec3 geometricNormal = n; // shadow lookups offset along the unperturbed normal

    bool clay = f.look2.y > 0.5;
    vec3 baseColor = clay ? vec3(0.75) : m.baseColor.rgb;
    float roughness = clay ? 0.8 : m.factors.x;
    float metalness = clay ? 0.0 : m.factors.y;
    float occlusion = 1.0;
    // Dynamically uniform (frame + per-draw material), so derivatives inside are well defined.
    bool textured = !clay && (any(notEqual(m.maps, uvec4(kNoTexture))) || m.maps2.x != kNoTexture);
    if (textured) {
        setupProjections(m, n, objectNormal);
        if (m.maps.x != kNoTexture) {
            baseColor *= sampleMap(m.maps.x, true).rgb;
        }
        if (m.maps.y != kNoTexture) {
            roughness *= sampleChannel(m.maps.y, m.maps2.y);
        }
        if (m.maps.z != kNoTexture) {
            metalness *= sampleChannel(m.maps.z, m.maps2.z);
        }
        if (m.maps2.x != kNoTexture) {
            occlusion = sampleChannel(m.maps2.x, m.maps2.w);
        }
        if (m.maps.w != kNoTexture) {
            n = perturbNormal(m, n);
        }
    }
    roughness = clamp(roughness, 0.03, 1.0);
    metalness = clamp(metalness, 0.0, 1.0);
    vec3 f0 = mix(vec3(0.04), baseColor, metalness);
    vec3 diffuseColor = baseColor * (1.0 - metalness);

    // Sun.
    vec3 l = f.sunDirection.xyz;
    vec3 h = normalize(l + v);
    float nDotL = max(dot(n, l), 0.0);
    float nDotV = max(dot(n, v), 1e-4);
    float nDotH = max(dot(n, h), 0.0);
    float vDotH = max(dot(v, h), 0.0);
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = nDotH * nDotH * (a2 - 1.0) + 1.0;
    float distribution = a2 / (PI * denom * denom);
    float visibility = 0.5 / max(nDotL * sqrt(nDotV * nDotV * (1.0 - a2) + a2) +
                                     nDotV * sqrt(nDotL * nDotL * (1.0 - a2) + a2),
                                 1e-5);
    vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vDotH, 5.0);
    vec3 brdf = diffuseColor * (1.0 - fresnel) / PI + distribution * visibility * fresnel;
    float shadow = nDotL > 0.0 ? sunShadow(f, inWorldPosition, geometricNormal) : 0.0;
    vec3 color = brdf * f.sunColor.rgb * (kSunIrradiance * PI) * nDotL * shadow;

    // Sky fill light: hemisphere diffuse plus glossy sky reflections. Ambient occlusion maps
    // darken only this indirect light; direct sun is handled by the shadow map.
    vec3 skyAverage = mix(f.skyHorizon.rgb, f.skyZenith.rgb, 0.5);
    vec3 hemisphere = mix(f.groundColor.rgb, skyAverage, n.z * 0.5 + 0.5);
    vec3 reflected = reflect(-v, n);
    vec3 reflection = mix(skyRadiance(f, reflected), hemisphere, roughness);
    vec2 brdfScale = environmentBrdf(roughness, nDotV);
    vec3 indirect = diffuseColor * hemisphere + reflection * (f0 * brdfScale.x + brdfScale.y);
    color += indirect * occlusion;

    outColor = vec4(applyLook(f, color, gl_FragCoord.xy), 1.0);
}
