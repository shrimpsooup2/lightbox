#include "core/placement.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/vec4.hpp>

namespace lb {
namespace {

constexpr float kFlatCosine = 0.985f; // faces within ~10° of facing along an axis count as flat
constexpr float kEndBand = 0.02f;     // "at the end": within 2% of the extent from the bounds

} // namespace

std::optional<glm::quat> uprightCorrection(const Scene& scene, EntityId root) {
    const std::optional<Bounds> bounds = worldBoundsOf(scene, {&root, 1});
    if (!bounds) {
        return std::nullopt;
    }
    const glm::vec3 extent = bounds->size();
    // Flat area at the low and the high end of each axis, and all area.
    std::array<float, 3> low{};
    std::array<float, 3> high{};
    float total = 0.0f;
    for (const EntityId id : meshEntitiesIn(scene, {&root, 1})) {
        const MeshResource* mesh = scene.mesh(scene.entity(id)->meshInstance->mesh);
        if (mesh == nullptr) {
            continue;
        }
        const glm::mat4 world = scene.worldMatrix(id);
        const MeshData& data = mesh->data;
        for (std::size_t i = 0; i + 2 < data.indices.size(); i += 3) {
            const glm::vec3 a(world * glm::vec4(data.positions[data.indices[i]], 1.0f));
            const glm::vec3 b(world * glm::vec4(data.positions[data.indices[i + 1]], 1.0f));
            const glm::vec3 c(world * glm::vec4(data.positions[data.indices[i + 2]], 1.0f));
            const glm::vec3 cross = glm::cross(b - a, c - a);
            const float area = 0.5f * glm::length(cross);
            if (!(area > 0.0f)) {
                continue;
            }
            total += area;
            const glm::vec3 normal = cross / (2.0f * area);
            const glm::vec3 centroid = (a + b + c) / 3.0f;
            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(normal[axis]) < kFlatCosine) {
                    continue;
                }
                const float band = std::max(kEndBand * extent[axis], 1e-4f);
                if (centroid[axis] - bounds->min[axis] < band) {
                    low[static_cast<std::size_t>(axis)] += area;
                }
                if (bounds->max[axis] - centroid[axis] < band) {
                    high[static_cast<std::size_t>(axis)] += area;
                }
            }
        }
    }
    if (!(total > 0.0f)) {
        return std::nullopt;
    }

    // Score of "up" = +axis: flat base minus flat top; "up" = −axis swaps the ends.
    const auto score = [&](int axis, bool positive) {
        const auto i = static_cast<std::size_t>(axis);
        return positive ? low[i] - high[i] : high[i] - low[i];
    };
    const float current = score(2, true);
    glm::vec3 best{0.0f, 0.0f, 1.0f};
    float bestScore = current;
    for (int axis = 0; axis < 3; ++axis) {
        for (const bool positive : {true, false}) {
            const float s = score(axis, positive);
            if (s > bestScore) {
                bestScore = s;
                best = glm::vec3(0.0f);
                best[axis] = positive ? 1.0f : -1.0f;
            }
        }
    }
    // Only turn on clear evidence: a sizable flat base, much more convincing than the current one.
    if (best.z > 0.5f || bestScore < 0.02f * total || bestScore < 2.0f * std::max(current, 0.0f)) {
        return std::nullopt;
    }
    if (best.z < -0.5f) {
        return glm::angleAxis(glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f)); // upside down
    }
    // A quarter turn about the horizontal axis perpendicular to `best` takes it to +Z.
    return glm::angleAxis(glm::half_pi<float>(),
                          glm::normalize(glm::cross(best, glm::vec3(0.0f, 0.0f, 1.0f))));
}

void placeOnGround(Scene& scene, EntityId root, const glm::vec2& at) {
    if (const std::optional<Bounds> bounds = worldBoundsOf(scene, {&root, 1})) {
        const glm::vec3 center = bounds->center();
        moveBy(scene, root, glm::vec3(at.x - center.x, at.y - center.y, -bounds->min.z));
    }
}

} // namespace lb
