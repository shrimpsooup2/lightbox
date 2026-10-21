// Material textures and surface parameters, shared by the raster mesh shader and the path tracer
// (include after frame.glsl). Raster passes screen-space derivatives; the path tracer passes a
// footprint estimated from its ray cone, so both filter textures the same way.
//
// Define LB_DIVERGENT_TEXTURES before including when texture indices vary per invocation (the path
// tracer): indices are then marked nonuniform.
#extension GL_EXT_nonuniform_qualifier : require

#ifdef LB_DIVERGENT_TEXTURES
#define TEXTURE_INDEX(i) nonuniformEXT(i)
#else
#define TEXTURE_INDEX(i) (i)
#endif

// Set 0 (renderer-wide): material textures. Every texture slot has an sRGB view (base color) and a
// linear view (data maps) of the same image.
layout(set = 0, binding = 0) uniform sampler materialSampler;
layout(set = 0, binding = 1) uniform texture2D srgbTextures[];
layout(set = 0, binding = 2) uniform texture2D linearTextures[];

const float PI = 3.14159265;

vec3 safeNormalize(vec3 v) {
    float lengthSq = dot(v, v);
    return lengthSq > 0.0 ? v * inversesqrt(lengthSq) : vec3(0.0);
}

// --- Sky image (HDRI) ----------------------------------------------------------------------------
// Equirectangular, row 0 = zenith, image center facing project north (+Y), columns turning
// clockwise seen from above; FrameData::skyImageParams.x turns it further (core/environment_map).

bool hasSkyImage(FrameData f) {
    return f.skyImage.x != kNoTexture;
}

vec2 skyImageUv(FrameData f, vec3 dir) {
    float bearing = atan(dir.x, dir.y) - f.skyImageParams.x;
    return vec2(fract(0.5 + bearing / (2.0 * PI)), acos(clamp(dir.z, -1.0, 1.0)) / PI);
}

vec3 skyImageDirection(FrameData f, vec2 uv) {
    float bearing = (uv.x - 0.5) * 2.0 * PI + f.skyImageParams.x;
    float theta = uv.y * PI;
    float s = sin(theta);
    return vec3(s * sin(bearing), s * cos(bearing), cos(theta));
}

// lod > 0 blurs (reflections on rough surfaces, sky light of the raster preview).
vec3 skyImageRadiance(FrameData f, vec3 dir, float lod) {
    return textureLod(sampler2D(linearTextures[f.skyImage.x], materialSampler),
                      skyImageUv(f, dir), lod).rgb * f.skyImageParams.y;
}

// What the sky looks like along dir: the sky image, or the procedural sky without one.
vec3 skyLight(FrameData f, vec3 dir, float lod) {
    return hasSkyImage(f) ? skyImageRadiance(f, dir, lod) : skyRadiance(f, dir);
}

// Where texture lookups land: UV mapping uses one projection, box mapping up to three.
struct Projection {
    vec2 uv; // after the material's scale, rotation and offset
    // Derivatives of uv per screen pixel (raster) or across the ray footprint (path tracer). Box
    // mapping skips zero-weight projections, so all lookups use textureGrad with these.
    vec2 dx;
    vec2 dy;
    float weight;
    vec3 tangent; // world direction of +u in the image
    vec3 up;      // world direction of "up" in the image (−v): +Y of OpenGL-convention normal maps
    vec3 gradU;   // world-space gradient of uv.x (change per meter along the surface): relief
    vec3 gradV;   // world-space gradient of uv.y
    // Far-away anti-tiling (see setupBombing): two randomly shifted copies of the texture and how
    // much of them replaces the exact lookup.
    vec2 offsetA;
    vec2 offsetB;
    float blend;
    float far;
    bool tiles; // random per-tile rotation (material option)
};

Projection gProjections[3];
int gProjectionCount = 0;

// The material's scale and rotation (the linear part of its texture transform).
vec2 linearUv(GpuMaterial m, vec2 uv) {
    vec2 scaled = uv * m.texTransform.xy;
    vec2 cs = m.texRotation.xy;
    return vec2(cs.x * scaled.x - cs.y * scaled.y, cs.y * scaled.x + cs.x * scaled.y);
}

vec2 transformUv(GpuMaterial m, vec2 uv) {
    return linearUv(m, uv) + m.texTransform.zw;
}

// What a surface point offers for texture lookups.
struct SurfaceCoords {
    vec2 uv;      // mesh texture coordinates
    vec2 uvDx;    // their change across one pixel / the ray footprint
    vec2 uvDy;
    // World-space gradients of u and v along the surface (change per meter): the cotangent frame
    // for UV-mapped normal maps and the parallax shift of height maps. Zero without UVs.
    vec3 gradU;
    vec3 gradV;
    vec3 boxPosition; // object space scaled to meters (box mapping)
    vec3 boxDx;       // its change across one pixel (raster)
    vec3 boxDy;
    float footprint;  // > 0: isotropic footprint in meters for box mapping (path tracer)
    vec3 boxNormal;   // normal in scaled object space, facing the viewer
    vec3 axisX;       // object axes in world space (unit length)
    vec3 axisY;
    vec3 axisZ;
};

Projection uvProjection(GpuMaterial m, SurfaceCoords s) {
    Projection p;
    p.uv = transformUv(m, s.uv);
    p.dx = linearUv(m, s.uvDx);
    p.dy = linearUv(m, s.uvDy);
    p.weight = 1.0;
    // The transformed coordinates' gradients are the same linear combination of the raw ones.
    vec2 cs = m.texRotation.xy;
    vec2 k = m.texTransform.xy;
    vec3 t = s.gradU * (cs.x * k.x) - s.gradV * (cs.y * k.y);
    vec3 b = s.gradU * (cs.y * k.x) + s.gradV * (cs.x * k.y);
    p.gradU = t;
    p.gradV = b;
    float lengthSq = max(dot(t, t), dot(b, b));
    // Without UVs (all zero) the frame vanishes and normal maps have no effect.
    float scale = lengthSq > 0.0 ? inversesqrt(lengthSq) : 0.0;
    p.tangent = t * scale;
    p.up = -b * scale;
    return p;
}

// One box-mapping projection: `coords` in meters along the projection's `tangent` (+u) and `up`
// (−v) world directions.
Projection boxProjection(GpuMaterial m, vec2 coords, vec2 coordsDx, vec2 coordsDy, vec3 tangent,
                         vec3 up, float weight) {
    Projection p;
    p.uv = transformUv(m, coords);
    p.dx = linearUv(m, coordsDx);
    p.dy = linearUv(m, coordsDy);
    p.weight = weight;
    // Image axes after transformUv's scale and rotation (columns of its inverse Jacobian).
    vec2 sc = m.texTransform.xy;
    vec2 cs = m.texRotation.xy;
    p.tangent = safeNormalize(tangent * (cs.x / sc.x) + up * (cs.y / sc.y));
    p.up = safeNormalize(up * (cs.x / sc.y) - tangent * (cs.y / sc.x));
    // coords.x grows along `tangent`, coords.y along −`up` (meters), then scale and rotation.
    p.gradU = tangent * (cs.x * sc.x) + up * (cs.y * sc.y);
    p.gradV = tangent * (cs.y * sc.x) - up * (cs.x * sc.y);
    return p;
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x),
               mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x), u.y);
}

// Texture bombing (after Inigo Quilez, "texture repetition"), faded in with distance. Seen from
// far away, a tiled texture's lighter and darker patches line up into an obvious grid; replacing
// the lookup with a blend of two randomly shifted copies (shift chosen by a smooth noise field)
// breaks the grid. Up close the exact texture stays, so brick courses and tile joints are never
// misaligned where they can be seen. `variation` (the material's 0–1 setting) scales it.
void setupBombing(inout Projection p, float variation) {
    float index = valueNoise(p.uv * 0.37) * 8.0;
    float cell = floor(index);
    p.offsetA = fract(sin(vec2(3.0, 7.0) * cell) * 43758.5453);
    p.offsetB = fract(sin(vec2(3.0, 7.0) * (cell + 1.0)) * 43758.5453);
    p.blend = smoothstep(0.2, 0.8, fract(index));
    // Tiles per pixel: starts when a tile is ~100 px wide on screen, complete below ~30 px.
    float footprint = max(length(p.dx), length(p.dy));
    p.far = smoothstep(1.0 / 100.0, 1.0 / 30.0, footprint) * clamp(variation * 2.0, 0.0, 1.0);
}

void setupProjections(GpuMaterial m, SurfaceCoords s) {
    if (m.factors.w < 0.5) {
        gProjections[0] = uvProjection(m, s);
        gProjectionCount = 1;
    } else {
        // Box mapping: triplanar with sharp weights. Axis-aligned faces (most of CAD geometry) get
        // a single projection, so textures never ghost; sloped and curved surfaces get a short
        // blend (roughly 35°–55° off an axis) instead of the hard seam of a dominant-axis pick.
        vec3 nb = s.boxNormal;
        vec3 w = nb * nb;
        w *= w;
        w *= w; // |n|^8
        w /= w.x + w.y + w.z;
        w = max(w - 0.05, 0.0);
        w /= w.x + w.y + w.z;

        // Projections are oriented as seen from outside each face (u right, image top up, Z-up
        // world), so text and brick courses read correctly on every side and are never mirrored.
        vec3 side = mix(vec3(-1.0), vec3(1.0), greaterThanEqual(nb, vec3(0.0)));
        vec3 p = s.boxPosition;
        vec3 px = s.boxDx;
        vec3 py = s.boxDy;
        vec2 isoX = vec2(s.footprint, 0.0);
        vec2 isoY = vec2(0.0, s.footprint);
        bool iso = s.footprint > 0.0;
        gProjections[0] = boxProjection(m, vec2(side.x * p.y, -p.z),
                                        iso ? isoX : vec2(side.x * px.y, -px.z),
                                        iso ? isoY : vec2(side.x * py.y, -py.z), side.x * s.axisY,
                                        s.axisZ, w.x);
        gProjections[1] = boxProjection(m, vec2(-side.y * p.x, -p.z),
                                        iso ? isoX : vec2(-side.y * px.x, -px.z),
                                        iso ? isoY : vec2(-side.y * py.x, -py.z), -side.y * s.axisX,
                                        s.axisZ, w.y);
        gProjections[2] = boxProjection(m, vec2(side.z * p.x, -p.y),
                                        iso ? isoX : vec2(side.z * px.x, -px.y),
                                        iso ? isoY : vec2(side.z * py.x, -py.y), side.z * s.axisX,
                                        s.axisY, w.z);
        gProjectionCount = 3;
    }
    bool tiles = (m.maps3.y & 1u) != 0u;
    for (int i = 0; i < gProjectionCount; ++i) {
        setupBombing(gProjections[i], m.texRotation.z);
        gProjections[i].tiles = tiles;
        if (tiles) {
            gProjections[i].far = 0.0; // random tiles already break the repeat
        }
    }
}

vec4 fetch(uint slot, bool srgb, vec2 uv, vec2 dx, vec2 dy) {
    return srgb ? textureGrad(sampler2D(srgbTextures[TEXTURE_INDEX(slot)], materialSampler), uv,
                              dx, dy)
                : textureGrad(sampler2D(linearTextures[TEXTURE_INDEX(slot)], materialSampler), uv,
                              dx, dy);
}

mat2 quarterTurn(float h) {
    int turns = int(h * 4.0) & 3;
    return turns == 0 ? mat2(1.0, 0.0, 0.0, 1.0)
         : turns == 1 ? mat2(0.0, 1.0, -1.0, 0.0)
         : turns == 2 ? mat2(-1.0, 0.0, 0.0, -1.0)
                      : mat2(0.0, -1.0, 1.0, 0.0);
}

// Random tiles (after Inigo Quilez, "texture repetition", technique 1): every tile corner gets
// its own random quarter turn and shift of the whole texture; inside a tile the corners' copies
// are blended, with the change concentrated in a band across the middle, so there are no hard
// seams. Normal maps (`normal`) come back decoded, with their tilt turned back into the surface's
// frame (and the DirectX green flip applied before the turn, in the image's own frame).
const float kTileBand = 0.15; // half width of the blend band, in tiles

vec4 fetchTiles(uint slot, bool srgb, bool normal, float greenSign, Projection p, vec2 uv) {
    vec2 cell = floor(uv);
    vec2 b = smoothstep(0.5 - kTileBand, 0.5 + kTileBand, uv - cell);
    vec4 sum = vec4(0.0);
    for (int k = 0; k < 4; ++k) {
        vec2 corner = vec2(float(k & 1), float(k >> 1));
        vec2 w2 = mix(1.0 - b, b, corner);
        float weight = w2.x * w2.y;
        if (weight <= 0.0) {
            continue;
        }
        vec2 c = cell + corner;
        mat2 r = quarterTurn(hash12(c));
        vec2 shift = vec2(hash12(c + 17.31), hash12(c + 41.73));
        vec4 t = fetch(slot, srgb, r * uv + shift, r * p.dx, r * p.dy);
        if (normal) {
            t.xyz = t.xyz * 2.0 - 1.0;
            t.y *= greenSign;
            // Image (u, v) → surface (u, v); the map's y points "up" in the image, i.e. along −v.
            vec2 tilt = transpose(r) * vec2(t.x, -t.y);
            t.xy = vec2(tilt.x, -tilt.y);
        }
        sum += t * weight;
    }
    return sum;
}

// One projection's lookup: random tiles, or the exact texture with the far-away anti-tiling blend
// (explicit gradients throughout, so the branches are safe).
vec4 sampleProjection(uint slot, bool srgb, Projection p) {
    if (p.tiles) {
        return fetchTiles(slot, srgb, false, 1.0, p, p.uv);
    }
    vec4 exact = fetch(slot, srgb, p.uv, p.dx, p.dy);
    if (p.far <= 0.0) {
        return exact;
    }
    vec4 shifted = mix(fetch(slot, srgb, p.uv + p.offsetA, p.dx, p.dy),
                       fetch(slot, srgb, p.uv + p.offsetB, p.dx, p.dy), p.blend);
    return mix(exact, shifted, p.far);
}

// Decoded tangent-space normal (x right, y up in the surface's texture frame).
vec3 sampleTangentNormal(GpuMaterial m, Projection p) {
    if (p.tiles) {
        return fetchTiles(m.maps.w, false, true, m.factors.z, p, p.uv).xyz;
    }
    vec3 t = sampleProjection(m.maps.w, false, p).xyz * 2.0 - 1.0;
    t.y *= m.factors.z; // DirectX maps store green = −Y
    return t;
}

vec4 sampleMap(uint slot, bool srgb) {
    vec4 sum = vec4(0.0);
    for (int i = 0; i < gProjectionCount; ++i) {
        Projection p = gProjections[i];
        if (p.weight <= 0.0) {
            continue;
        }
        sum += sampleProjection(slot, srgb, p) * p.weight;
    }
    return sum;
}

// Scalar maps read one channel (packed maps), optionally inverted (e.g. gloss → roughness).
float sampleChannel(uint slot, uint selector) {
    float value = sampleMap(slot, false)[int(selector & 3u)];
    return (selector & 4u) != 0u ? 1.0 - value : value;
}

// Tangent-space normal map around the viewer-facing normal n. Also widens `roughness` where the
// map's mip levels average many different normals (distant or grazing surfaces): the shorter the
// averaged normal, the rougher the surface really looks (Toksvig). Without this, fine relief turns
// into glittery noise from a distance.
vec3 perturbNormal(GpuMaterial m, vec3 n, inout float roughness) {
    vec3 sum = vec3(0.0);
    float averagedLength = 0.0;
    for (int i = 0; i < gProjectionCount; ++i) {
        Projection p = gProjections[i];
        if (p.weight <= 0.0) {
            continue;
        }
        vec3 t = sampleTangentNormal(m, p);
        averagedLength += length(t) * p.weight;
        t.xy *= m.baseColor.a;  // strength
        // Box projections are not tangent to sloped faces: keep only the in-surface part.
        vec3 tangent = p.tangent - n * dot(n, p.tangent);
        vec3 up = p.up - n * dot(n, p.up);
        sum += safeNormalize(tangent * t.x + up * t.y + n * t.z) * p.weight;
    }
    float len = clamp(averagedLength, 1e-3, 1.0);
    float variance = (1.0 - len) / len * m.baseColor.a * m.baseColor.a;
    roughness = sqrt(clamp(roughness * roughness + variance, 0.0, 1.0));
    return dot(sum, sum) > 1e-12 ? normalize(sum) : n;
}

// −0.5…0.5, changing over several tiles: gentle large-scale changes in brightness and roughness
// that keep a tiled surface from looking uniform. Unlike the far-away bombing it applies at every
// distance, and it never shifts the pattern.
float macroVariation() {
    float sum = 0.0;
    for (int i = 0; i < gProjectionCount; ++i) {
        Projection p = gProjections[i];
        if (p.weight <= 0.0) {
            continue;
        }
        float n = valueNoise(p.uv * 0.21) * 0.65 + valueNoise(p.uv * 0.57 + 17.0) * 0.35;
        sum += (n - 0.5) * p.weight;
    }
    return sum;
}

float sampleHeight(uint slot, Projection p, vec2 uv) {
    return p.tiles ? fetchTiles(slot, false, false, 1.0, p, uv).r
                   : fetch(slot, false, uv, p.dx, p.dy).r;
}

// --- Relief (height map) ---------------------------------------------------------------------------

const int kReliefSteps = 16;

// Parallax occlusion mapping: walks the view ray into the height map (white = surface, black =
// `depth` meters below it) and moves each projection's uv to where the ray hits, so mortar joints
// and gaps between stones look recessed. Then walks from the hit toward the sun and returns how
// much of the sunlight the relief itself blocks (shadows inside the joints). Faded out once the
// texture is small on screen, where it can't be seen (and costs the most). Same in the raster view
// and the path tracer (camera rays and mirror bounces).
float applyRelief(GpuMaterial m, vec3 n, vec3 v, vec3 l) {
    uint slot = m.maps3.x;
    float depth = m.texRotation.w;
    if (slot == kNoTexture || depth <= 0.0) {
        return 1.0;
    }
    float nDotV = max(dot(n, v), 0.2);
    float nDotL = dot(n, l);
    float lit = 0.0;
    float weights = 0.0;
    for (int i = 0; i < gProjectionCount; ++i) {
        Projection p = gProjections[i];
        if (p.weight <= 0.0) {
            continue;
        }
        float footprint = max(length(p.dx), length(p.dy)); // tiles per pixel
        float fade = 1.0 - smoothstep(1.0 / 80.0, 1.0 / 20.0, footprint);
        weights += p.weight;
        if (fade <= 0.0) {
            lit += p.weight;
            continue;
        }
        // Box projections aren't tangent to sloped faces: keep the in-surface part.
        vec3 gradU = p.gradU - n * dot(n, p.gradU);
        vec3 gradV = p.gradV - n * dot(n, p.gradV);
        // uv shift while the view ray sinks the full depth below the surface.
        vec2 shift = -vec2(dot(gradU, v), dot(gradV, v)) / nDotV * depth * fade;

        float stepSize = 1.0 / float(kReliefSteps);
        float rayDepth = 0.0;
        float surfaceDepth = 1.0 - sampleHeight(slot, p, p.uv);
        float previousRay = 0.0;
        float previousSurface = surfaceDepth;
        for (int s = 0; s < kReliefSteps && rayDepth < surfaceDepth; ++s) {
            previousRay = rayDepth;
            previousSurface = surfaceDepth;
            rayDepth += stepSize;
            surfaceDepth = 1.0 - sampleHeight(slot, p, p.uv + shift * rayDepth);
        }
        // Refine between the last two steps (linear intersection).
        float before = previousSurface - previousRay;
        float after = surfaceDepth - rayDepth;
        float t = clamp(before / max(before - after, 1e-5), 0.0, 1.0);
        float hitDepth = mix(previousRay, rayDepth, t);
        vec2 hit = p.uv + shift * hitDepth;
        gProjections[i].uv = hit;

        // Self-shadow: climb from the hit toward the sun; relief above the ray blocks it.
        float visible = 1.0;
        if (nDotL > 0.0 && hitDepth > 0.0) {
            vec2 toSun = vec2(dot(gradU, l), dot(gradV, l)) / max(nDotL, 0.2) * depth * fade;
            for (int s = 1; s <= 8; ++s) {
                float climbed = float(s) / 8.0;
                float ray = hitDepth * (1.0 - climbed);
                float surface = 1.0 - sampleHeight(slot, p, hit + toSun * hitDepth * climbed);
                visible = min(visible, clamp(1.0 - (ray - surface) * 12.0, 0.0, 1.0));
            }
        }
        lit += mix(1.0, visible, fade) * p.weight;
    }
    return weights > 0.0 ? lit / weights : 1.0;
}

// Everything a renderer needs to light a surface point.
struct SurfaceSample {
    vec3 baseColor;
    float roughness;
    float metalness;
    float occlusion;    // ambient occlusion map (raster sky light only)
    vec3 normal;        // shading normal (normal map applied), facing the viewer
    float transmission; // 0 opaque … 1 clear glass
    float ior;
    bool thinWalled;
    vec3 emission;      // emitted radiance, render units
};

bool materialTextured(GpuMaterial m) {
    return any(notEqual(m.maps, uvec4(kNoTexture))) || m.maps2.x != kNoTexture ||
           m.maps3.x != kNoTexture;
}

// Reads the material at a point whose projections are set up (setupProjections, and in raster the
// parallax shift). `n` faces the viewer. `clay`: white model, materials ignored (glass stays glass,
// so interiors keep their daylight). `detailed`: all maps; without it only the color map (light
// that has already bounced blurs fine relief and roughness detail away, so the path tracer skips
// them after the first hit).
SurfaceSample sampleSurface(GpuMaterial m, vec3 n, bool textured, bool clay, bool detailed) {
    SurfaceSample s;
    s.baseColor = clay ? vec3(0.75) : m.baseColor.rgb;
    s.roughness = clay ? 0.8 : m.factors.x;
    s.metalness = clay ? 0.0 : m.factors.y;
    s.occlusion = 1.0;
    s.normal = n;
    s.transmission = m.glass.x;
    s.ior = m.glass.y;
    s.thinWalled = m.glass.z > 0.5;
    s.emission = clay ? vec3(0.0) : m.emission.rgb;
    if (clay && s.transmission > 0.0) {
        s.baseColor = vec3(1.0);
        s.roughness = m.factors.x;
    }
    if (textured && !clay) {
        if (m.maps.x != kNoTexture) {
            s.baseColor *= sampleMap(m.maps.x, true).rgb;
        }
        if (m.maps.z != kNoTexture) {
            s.metalness *= sampleChannel(m.maps.z, m.maps2.z);
        }
        if (detailed) {
            if (m.maps.y != kNoTexture) {
                s.roughness *= sampleChannel(m.maps.y, m.maps2.y);
            }
            if (m.maps2.x != kNoTexture) {
                s.occlusion = sampleChannel(m.maps2.x, m.maps2.w);
            }
            if (m.maps3.x != kNoTexture && m.texRotation.w > 0.0) {
                // Cavities (low parts of the height map) get less sky light.
                s.occlusion *= mix(0.6, 1.0, sampleMap(m.maps3.x, false).r);
            }
            if (m.maps.w != kNoTexture) {
                s.normal = perturbNormal(m, n, s.roughness);
            }
        }
        if (m.texRotation.z > 0.0) {
            float variation = macroVariation() * m.texRotation.z;
            s.baseColor *= 1.0 + variation * 0.7;
            s.roughness *= 1.0 + variation * 0.35;
        }
    }
    s.roughness = clamp(s.roughness, 0.03, 1.0);
    s.metalness = clamp(s.metalness, 0.0, 1.0);
    return s;
}
