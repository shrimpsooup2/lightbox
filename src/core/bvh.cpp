#include "core/bvh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace lb {
namespace {

constexpr int kBins = 12;
constexpr std::uint32_t kMaxLeafSize = 4;
constexpr float kTraversalCost = 1.0f; // relative to one triangle test

struct Box {
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{-std::numeric_limits<float>::max()};

    void grow(const glm::vec3& p) {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }
    void grow(const Box& b) {
        min = glm::min(min, b.min);
        max = glm::max(max, b.max);
    }
    float area() const {
        const glm::vec3 e = max - min;
        return e.x < 0.0f ? 0.0f : 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
    }
};

struct Bin {
    Box box;
    std::uint32_t count = 0;
};

} // namespace

Bvh buildBvh(std::span<const glm::vec3> corners) {
    Bvh bvh;
    const auto count = static_cast<std::uint32_t>(corners.size() / 3);
    if (count == 0) {
        return bvh;
    }
    std::vector<Box> boxes(count);
    std::vector<glm::vec3> centroids(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        for (int k = 0; k < 3; ++k) {
            boxes[i].grow(corners[3 * i + static_cast<std::uint32_t>(k)]);
        }
        centroids[i] = (boxes[i].min + boxes[i].max) * 0.5f;
    }
    bvh.triangles.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        bvh.triangles[i] = i;
    }
    bvh.nodes.reserve(static_cast<std::size_t>(count) * 2);
    bvh.nodes.push_back({});
    bvh.nodes[0].first = 0;
    bvh.nodes[0].count = count;

    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const std::uint32_t nodeIndex = stack.back();
        stack.pop_back();
        const std::uint32_t first = bvh.nodes[nodeIndex].first;
        const std::uint32_t n = bvh.nodes[nodeIndex].count;

        Box bounds;
        Box centroidBounds;
        for (std::uint32_t i = first; i < first + n; ++i) {
            bounds.grow(boxes[bvh.triangles[i]]);
            centroidBounds.grow(centroids[bvh.triangles[i]]);
        }
        bvh.nodes[nodeIndex].boundsMin = bounds.min;
        bvh.nodes[nodeIndex].boundsMax = bounds.max;
        if (n <= 1) {
            continue;
        }

        // Best split plane over all axes by the surface area heuristic.
        float bestCost = std::numeric_limits<float>::max();
        int bestAxis = -1;
        int bestSplit = 0;
        for (int axis = 0; axis < 3; ++axis) {
            const float lo = centroidBounds.min[axis];
            const float extent = centroidBounds.max[axis] - lo;
            if (!(extent > 1e-9f)) {
                continue;
            }
            std::array<Bin, kBins> bins{};
            const float scale = static_cast<float>(kBins) / extent;
            for (std::uint32_t i = first; i < first + n; ++i) {
                const std::uint32_t t = bvh.triangles[i];
                const int b = std::min(kBins - 1, static_cast<int>((centroids[t][axis] - lo) * scale));
                bins[static_cast<std::size_t>(b)].count++;
                bins[static_cast<std::size_t>(b)].box.grow(boxes[t]);
            }
            std::array<float, kBins - 1> leftArea{};
            std::array<std::uint32_t, kBins - 1> leftCount{};
            Box left;
            std::uint32_t leftSum = 0;
            for (int i = 0; i < kBins - 1; ++i) {
                left.grow(bins[static_cast<std::size_t>(i)].box);
                leftSum += bins[static_cast<std::size_t>(i)].count;
                leftArea[static_cast<std::size_t>(i)] = left.area();
                leftCount[static_cast<std::size_t>(i)] = leftSum;
            }
            Box right;
            std::uint32_t rightSum = 0;
            for (int i = kBins - 1; i > 0; --i) {
                right.grow(bins[static_cast<std::size_t>(i)].box);
                rightSum += bins[static_cast<std::size_t>(i)].count;
                const auto split = static_cast<std::size_t>(i - 1);
                if (leftCount[split] == 0 || rightSum == 0) {
                    continue;
                }
                const float cost = leftArea[split] * static_cast<float>(leftCount[split]) +
                                   right.area() * static_cast<float>(rightSum);
                if (cost < bestCost) {
                    bestCost = cost;
                    bestAxis = axis;
                    bestSplit = i;
                }
            }
        }

        const float parentArea = std::max(bounds.area(), 1e-12f);
        const float splitCost = kTraversalCost + bestCost / parentArea;
        const float leafCost = static_cast<float>(n);
        if (bestAxis < 0 || (n <= kMaxLeafSize && splitCost >= leafCost)) {
            if (bestAxis < 0 && n > kMaxLeafSize) {
                // All centroids coincide: halve the range so leaves stay small.
                bestAxis = 0;
                bestSplit = -1;
            } else {
                continue; // stays a leaf
            }
        }

        std::uint32_t middle = first;
        if (bestSplit < 0) {
            middle = first + n / 2;
        } else {
            const float lo = centroidBounds.min[bestAxis];
            const float scale = static_cast<float>(kBins) / (centroidBounds.max[bestAxis] - lo);
            const auto goesLeft = [&](std::uint32_t t) {
                return std::min(kBins - 1, static_cast<int>((centroids[t][bestAxis] - lo) * scale)) <
                       bestSplit;
            };
            middle = static_cast<std::uint32_t>(
                std::partition(bvh.triangles.begin() + first, bvh.triangles.begin() + first + n,
                               goesLeft) -
                bvh.triangles.begin());
            if (middle == first || middle == first + n) {
                middle = first + n / 2;
            }
        }

        const auto leftIndex = static_cast<std::uint32_t>(bvh.nodes.size());
        bvh.nodes.push_back({});
        bvh.nodes.push_back({});
        bvh.nodes[leftIndex].first = first;
        bvh.nodes[leftIndex].count = middle - first;
        bvh.nodes[leftIndex + 1].first = middle;
        bvh.nodes[leftIndex + 1].count = first + n - middle;
        bvh.nodes[nodeIndex].first = leftIndex;
        bvh.nodes[nodeIndex].count = 0;
        stack.push_back(leftIndex + 1);
        stack.push_back(leftIndex);
    }
    return bvh;
}

std::optional<BvhHit> intersectBvh(const Bvh& bvh, std::span<const glm::vec3> corners,
                                   const glm::vec3& origin, const glm::vec3& direction,
                                   float minDistance, float maxDistance) {
    if (bvh.nodes.empty()) {
        return std::nullopt;
    }
    const glm::vec3 inverse = 1.0f / direction;
    const auto slab = [&](const BvhNode& node) {
        const glm::vec3 t0 = (node.boundsMin - origin) * inverse;
        const glm::vec3 t1 = (node.boundsMax - origin) * inverse;
        const glm::vec3 near = glm::min(t0, t1);
        const glm::vec3 far = glm::max(t0, t1);
        const float enter = std::max({near.x, near.y, near.z, minDistance});
        const float exit = std::min({far.x, far.y, far.z, maxDistance});
        return enter <= exit ? enter : std::numeric_limits<float>::infinity();
    };

    std::optional<BvhHit> best;
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const BvhNode& node = bvh.nodes[stack.back()];
        stack.pop_back();
        if (slab(node) > maxDistance) {
            continue;
        }
        if (node.count == 0) {
            stack.push_back(node.first);
            stack.push_back(node.first + 1);
            continue;
        }
        for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
            const std::uint32_t t = bvh.triangles[i];
            const glm::vec3& a = corners[3 * t];
            const glm::vec3 e1 = corners[3 * t + 1] - a;
            const glm::vec3 e2 = corners[3 * t + 2] - a;
            const glm::vec3 p = glm::cross(direction, e2);
            const float det = glm::dot(e1, p);
            if (std::abs(det) < 1e-12f) {
                continue;
            }
            const float inv = 1.0f / det;
            const glm::vec3 s = origin - a;
            const float u = glm::dot(s, p) * inv;
            if (u < 0.0f || u > 1.0f) {
                continue;
            }
            const glm::vec3 q = glm::cross(s, e1);
            const float v = glm::dot(direction, q) * inv;
            if (v < 0.0f || u + v > 1.0f) {
                continue;
            }
            const float distance = glm::dot(e2, q) * inv;
            if (distance > minDistance && distance < maxDistance) {
                maxDistance = distance;
                best = BvhHit{t, distance, u, v};
            }
        }
    }
    return best;
}

} // namespace lb
