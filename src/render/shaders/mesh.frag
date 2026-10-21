#version 460
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
#include "material.glsl"

// Metal/roughness shading: GGX specular + Lambert diffuse from the sun (with soft shadows) and the
// artificial lights, plus sky fill light (hemisphere diffuse, sky reflections) and emission. Glass
// writes its reflection with how much of the background shows through (premultiplied alpha). The
// ray-traced view (pathtrace.comp) reads materials through the same material.glsl.

// Set 1 (per viewport): the sun shadow map.
layout(set = 1, binding = 0) uniform sampler2DShadow shadowMap;

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec3 inBoxPosition; // object space scaled to meters
layout(location = 4) in vec3 inObjectNormal;
layout(location = 0) out vec4 outColor;

const float kSunIrradiance = 2.0; // sun intensity 1 = 100 000 lux: a white wall facing it reads 2

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

// GGX specular + Lambert diffuse for light arriving from direction l with irradiance E (measured
// perpendicular to l).
vec3 shadeLight(vec3 n, vec3 v, vec3 l, vec3 irradiance, vec3 diffuseColor, vec3 f0,
                float roughness) {
    float nDotL = dot(n, l);
    if (nDotL <= 0.0) {
        return vec3(0.0);
    }
    vec3 h = normalize(l + v);
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
    return brdf * irradiance * nDotL;
}

// Artificial lights without shadows (the ray-traced view has them). Area lights are approximated
// by their center, with the panel's size keeping the light finite up close.
vec3 localLights(FrameData f, vec3 p, vec3 n, vec3 v, vec3 diffuseColor, vec3 f0,
                 float roughness) {
    vec3 sum = vec3(0.0);
    uint count = min(f.lightInfo.x, kMaxLights);
    for (uint i = 0u; i < count; ++i) {
        GpuLight light = f.lights[i];
        vec3 toLight = light.position.xyz - p;
        float distanceSq = max(dot(toLight, toLight), 1e-6);
        vec3 l = toLight * inversesqrt(distanceSq);
        uint type = uint(light.position.w + 0.5);
        vec3 irradiance;
        if (type == kLightArea) {
            float area = 4.0 * light.params.x * light.params.y;
            float facing = max(dot(-l, light.direction.xyz), 0.0);
            irradiance = light.intensity.rgb * area * facing / (distanceSq + area / PI);
        } else {
            float radiusSq = light.direction.w * light.direction.w;
            irradiance = light.intensity.rgb / max(distanceSq, radiusSq);
            if (type == kLightSpot) {
                irradiance *= smoothstep(light.params.x, light.params.y,
                                         dot(-l, light.direction.xyz));
            }
        }
        sum += shadeLight(n, v, l, irradiance, diffuseColor, f0, roughness);
    }
    return sum;
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

    // Texture coordinates and their screen-space derivatives (uniform control flow).
    SurfaceCoords coords;
    coords.uv = inUv;
    coords.uvDx = dFdx(inUv);
    coords.uvDy = dFdy(inUv);
    // UV gradients from screen-space derivatives (Schüler, "Followup: Normal Mapping Without
    // Precomputed Tangents"): meshes carry no tangents, and the frame follows mirrored UVs.
    vec3 dp1 = dFdx(inWorldPosition);
    vec3 dp2 = dFdy(inWorldPosition);
    vec3 dp2perp = cross(dp2, n);
    vec3 dp1perp = cross(n, dp1);
    float det = dot(dp1, dp2perp);
    float inverseDet = abs(det) > 1e-20 ? 1.0 / det : 0.0;
    coords.gradU = (dp2perp * coords.uvDx.x + dp1perp * coords.uvDy.x) * inverseDet;
    coords.gradV = (dp2perp * coords.uvDx.y + dp1perp * coords.uvDy.y) * inverseDet;
    coords.boxPosition = inBoxPosition;
    coords.boxDx = dFdx(inBoxPosition);
    coords.boxDy = dFdy(inBoxPosition);
    coords.footprint = 0.0;
    vec3 axisScale =
        vec3(length(pc.model[0].xyz), length(pc.model[1].xyz), length(pc.model[2].xyz));
    // Normal in scaled object space (normals transform with the inverse scale).
    coords.boxNormal = normalize(objectNormal / max(axisScale, vec3(1e-12)));
    coords.axisX = safeNormalize(pc.model[0].xyz);
    coords.axisY = safeNormalize(pc.model[1].xyz);
    coords.axisZ = safeNormalize(pc.model[2].xyz);

    bool clay = f.look2.y > 0.5;
    // Dynamically uniform (frame + per-draw material), so derivatives inside are well defined.
    bool textured = !clay && materialTextured(m);
    float reliefShadow = 1.0;
    if (textured) {
        setupProjections(m, coords);
        reliefShadow = applyRelief(m, n, v, f.sunDirection.xyz);
    }
    SurfaceSample s = sampleSurface(m, n, textured, clay, true);
    n = s.normal;
    float transmission = s.transmission;
    vec3 f0 = mix(vec3(0.04), s.baseColor, s.metalness);
    if (transmission > 0.0) {
        float r = (s.ior - 1.0) / (s.ior + 1.0);
        f0 = mix(f0, vec3(r * r), transmission);
    }
    // Light that passes through glass isn't scattered by its surface.
    vec3 diffuseColor = s.baseColor * (1.0 - s.metalness) * (1.0 - transmission);
    float roughness = s.roughness;

    // Sun.
    vec3 l = f.sunDirection.xyz;
    float shadow = dot(n, l) > 0.0 ? sunShadow(f, inWorldPosition, geometricNormal) : 0.0;
    shadow *= reliefShadow;
    vec3 color = shadeLight(n, v, l, f.sunColor.rgb * (kSunIrradiance * PI) * shadow,
                            diffuseColor, f0, roughness);
    color += localLights(f, inWorldPosition, n, v, diffuseColor, f0, roughness);

    // Sky fill light: hemisphere diffuse plus glossy sky reflections. Ambient occlusion maps
    // darken only this indirect light; direct sun is handled by the shadow map.
    float nDotV = max(dot(n, v), 1e-4);
    vec3 reflected = reflect(-v, n);
    vec3 hemisphere;
    vec3 reflection;
    if (hasSkyImage(f)) {
        // A heavily blurred mip (≈ 8 × 4 pixels) toward the normal stands in for the irradiance;
        // reflections blur with roughness.
        float mips = float(f.skyImage.w);
        hemisphere = skyImageRadiance(f, n, mips - 3.5);
        reflection = mix(skyImageRadiance(f, reflected, roughness * (mips - 4.0)), hemisphere,
                         roughness * roughness);
    } else {
        vec3 skyAverage = mix(f.skyHorizon.rgb, f.skyZenith.rgb, 0.5);
        hemisphere = mix(f.groundColor.rgb, skyAverage, n.z * 0.5 + 0.5);
        reflection = mix(skyRadiance(f, reflected), hemisphere, roughness);
    }
    vec2 brdfScale = environmentBrdf(roughness, nDotV);
    vec3 indirect = diffuseColor * hemisphere + reflection * (f0 * brdfScale.x + brdfScale.y);
    color += indirect * s.occlusion;
    color += s.emission;

    // Glass: what shows through is the background, dimmed by the reflection (premultiplied
    // blending in the glass pass; opaque passes ignore alpha). The tint is approximated by its
    // brightness here; the ray-traced view tints exactly.
    float throughput = 0.0;
    if (transmission > 0.0) {
        vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - nDotV, 5.0);
        float tint = dot(s.baseColor, vec3(0.2126, 0.7152, 0.0722));
        throughput = transmission * (1.0 - dot(fresnel, vec3(1.0 / 3.0))) * tint;
    }
    outColor = vec4(color, 1.0 - throughput); // linear HDR; the composite pass applies the look
}
