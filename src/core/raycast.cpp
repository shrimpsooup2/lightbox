#include "core/raycast.hpp"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>

#include "core/scene/scene.hpp"

namespace lb {
namespace {

// Ray vs axis-aligned box (slab test); returns false when the box is entirely missed or behind.
bool hitsBounds(const glm::vec3& origin, const glm::vec3& direction, const Bounds& bounds,
                float maxT) {
    float tMin = 0.0f;
    float tMax = maxT;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1e-12f) {
            if (origin[axis] < bounds.min[axis] || origin[axis] > bounds.max[axis]) {
                return false;
            }
            continue;
        }
        float t0 = (bounds.min[axis] - origin[axis]) / direction[axis];
        float t1 = (bounds.max[axis] - origin[axis]) / direction[axis];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<RayHit> raycast(const Scene& scene, const glm::vec3& origin,
                              const glm::vec3& direction, float maxDistance) {
    const float length = glm::length(direction);
    if (!(length > 0.0f)) {
        return std::nullopt;
    }
    const glm::vec3 dir = direction / length;
    std::optional<RayHit> best;
    float bestT = maxDistance;

    scene.traverse([&](const Entity& entity, const glm::mat4& world) {
        if (!entity.meshInstance) {
            return;
        }
        const MeshResource* mesh = scene.mesh(entity.meshInstance->mesh);
        if (mesh == nullptr) {
            return;
        }
        // Test in object space: the ray parameter t stays in world units because the direction is
        // transformed without renormalizing.
        const glm::mat4 toObject = glm::inverse(world);
        const glm::vec3 o = glm::vec3(toObject * glm::vec4(origin, 1.0f));
        const glm::vec3 d = glm::vec3(toObject * glm::vec4(dir, 0.0f));
        if (!hitsBounds(o, d, mesh->bounds, bestT)) {
            return;
        }
        const MeshData& data = mesh->data;
        bool found = false;
        glm::vec3 foundNormal{};
        for (std::size_t i = 0; i + 2 < data.indices.size(); i += 3) {
            const glm::vec3& a = data.positions[data.indices[i]];
            const glm::vec3& b = data.positions[data.indices[i + 1]];
            const glm::vec3& c = data.positions[data.indices[i + 2]];
            // Möller–Trumbore, two-sided.
            const glm::vec3 e1 = b - a;
            const glm::vec3 e2 = c - a;
            const glm::vec3 p = glm::cross(d, e2);
            const float det = glm::dot(e1, p);
            if (std::abs(det) < 1e-12f) {
                continue;
            }
            const float inv = 1.0f / det;
            const glm::vec3 s = o - a;
            const float u = glm::dot(s, p) * inv;
            if (u < 0.0f || u > 1.0f) {
                continue;
            }
            const glm::vec3 q = glm::cross(s, e1);
            const float v = glm::dot(d, q) * inv;
            if (v < 0.0f || u + v > 1.0f) {
                continue;
            }
            const float t = glm::dot(e2, q) * inv;
            if (t > 1e-5f && t < bestT) {
                bestT = t;
                found = true;
                foundNormal = glm::cross(e1, e2);
            }
        }
        if (found) {
            const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
            glm::vec3 n = glm::normalize(normalMatrix * foundNormal);
            if (glm::dot(n, dir) > 0.0f) {
                n = -n;
            }
            best = RayHit{entity.id, origin + dir * bestT, n, bestT};
        }
    });
    return best;
}

} // namespace lb
