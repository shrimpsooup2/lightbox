#include "import/mesh_processing.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>
#include <glm/trigonometric.hpp>

namespace lb::import {
namespace {

constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();

bool isFinite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Exact position key for welding; +0.0f folds −0 into +0 so both weld together.
struct PositionKey {
    std::uint32_t x, y, z;
    bool operator==(const PositionKey&) const = default;
};

PositionKey keyOf(const glm::vec3& p) {
    return {std::bit_cast<std::uint32_t>(p.x + 0.0f), std::bit_cast<std::uint32_t>(p.y + 0.0f),
            std::bit_cast<std::uint32_t>(p.z + 0.0f)};
}

struct PositionKeyHash {
    std::size_t operator()(const PositionKey& k) const noexcept {
        std::size_t h = k.x;
        h = h * 0x9E3779B97F4A7C15ull ^ k.y;
        h = h * 0x9E3779B97F4A7C15ull ^ k.z;
        return h ^ (h >> 29);
    }
};

// Interior angle at `a` of the triangle (a, b, c); atan2 stays accurate for very thin triangles.
float cornerAngle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 u = b - a;
    const glm::vec3 v = c - a;
    return std::atan2(glm::length(glm::cross(u, v)), glm::dot(u, v));
}

} // namespace

std::size_t removeDegenerateTriangles(MeshData& mesh) {
    const std::size_t vertexCount = mesh.positions.size();
    const std::size_t triangleCount = mesh.indices.size() / 3;
    std::size_t kept = 0;
    for (std::size_t t = 0; t < triangleCount; ++t) {
        const std::uint32_t i0 = mesh.indices[t * 3];
        const std::uint32_t i1 = mesh.indices[t * 3 + 1];
        const std::uint32_t i2 = mesh.indices[t * 3 + 2];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount || i0 == i1 || i1 == i2 ||
            i0 == i2) {
            continue;
        }
        const glm::vec3& p0 = mesh.positions[i0];
        const glm::vec3& p1 = mesh.positions[i1];
        const glm::vec3& p2 = mesh.positions[i2];
        if (!isFinite(p0) || !isFinite(p1) || !isFinite(p2)) {
            continue;
        }
        // Relative test (twice the area against the longest edge squared, i.e. height / length), so
        // it behaves the same for millimeter and kilometer models.
        const float longest = std::max({glm::dot(p1 - p0, p1 - p0), glm::dot(p2 - p1, p2 - p1),
                                        glm::dot(p0 - p2, p0 - p2)});
        const float doubleArea = glm::length(glm::cross(p1 - p0, p2 - p0));
        if (!(doubleArea > 1e-7f * longest)) {
            continue;
        }
        mesh.indices[kept * 3] = i0;
        mesh.indices[kept * 3 + 1] = i1;
        mesh.indices[kept * 3 + 2] = i2;
        ++kept;
    }
    mesh.indices.resize(kept * 3);
    return triangleCount - kept;
}

void generateNormals(MeshData& mesh, float creaseAngleDegrees) {
    const std::size_t vertexCount = mesh.positions.size();
    const std::size_t cornerCount = mesh.indices.size() - mesh.indices.size() % 3;
    const std::size_t triangleCount = cornerCount / 3;

    // Weld vertices by position, so UV seams (split vertices) don't become shading seams.
    std::vector<std::uint32_t> groupOf(vertexCount);
    std::uint32_t groupCount = 0;
    {
        std::unordered_map<PositionKey, std::uint32_t, PositionKeyHash> groups;
        groups.reserve(vertexCount);
        for (std::size_t v = 0; v < vertexCount; ++v) {
            const auto [it, inserted] = groups.try_emplace(keyOf(mesh.positions[v]), groupCount);
            if (inserted) {
                ++groupCount;
            }
            groupOf[v] = it->second;
        }
    }

    // Face normals and corner angles (the weights).
    std::vector<glm::vec3> faceNormals(triangleCount, glm::vec3(0.0f));
    std::vector<float> angles(cornerCount, 0.0f);
    for (std::size_t t = 0; t < triangleCount; ++t) {
        const glm::vec3& p0 = mesh.positions[mesh.indices[t * 3]];
        const glm::vec3& p1 = mesh.positions[mesh.indices[t * 3 + 1]];
        const glm::vec3& p2 = mesh.positions[mesh.indices[t * 3 + 2]];
        const glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
        const float length = glm::length(n);
        if (length > 0.0f && std::isfinite(length)) {
            faceNormals[t] = n / length;
        }
        angles[t * 3] = cornerAngle(p0, p1, p2);
        angles[t * 3 + 1] = cornerAngle(p1, p2, p0);
        angles[t * 3 + 2] = cornerAngle(p2, p0, p1);
    }

    // Corners per position group (compressed adjacency lists).
    std::vector<std::uint32_t> groupStart(std::size_t{groupCount} + 1, 0);
    for (std::size_t c = 0; c < cornerCount; ++c) {
        ++groupStart[groupOf[mesh.indices[c]] + 1];
    }
    for (std::size_t g = 0; g < groupCount; ++g) {
        groupStart[g + 1] += groupStart[g];
    }
    std::vector<std::uint32_t> groupCorners(cornerCount);
    {
        std::vector<std::uint32_t> fill(groupStart.begin(), groupStart.end() - 1);
        for (std::size_t c = 0; c < cornerCount; ++c) {
            groupCorners[fill[groupOf[mesh.indices[c]]]++] = static_cast<std::uint32_t>(c);
        }
    }

    const float cosCrease = std::cos(glm::radians(std::clamp(creaseAngleDegrees, 0.0f, 180.0f)));
    MeshData out;
    out.positions.reserve(vertexCount);
    out.normals.reserve(vertexCount);
    const bool hasUvs = mesh.uvs.size() == vertexCount;
    if (hasUvs) {
        out.uvs.reserve(vertexCount);
    }
    out.indices.resize(cornerCount);
    // Output vertices made from each input vertex, as linked lists (one per distinct normal).
    std::vector<std::uint32_t> firstSplit(vertexCount, kNone);
    std::vector<std::uint32_t> nextSplit;
    nextSplit.reserve(vertexCount);

    for (std::size_t c = 0; c < cornerCount; ++c) {
        const std::uint32_t vertex = mesh.indices[c];
        const glm::vec3& faceNormal = faceNormals[c / 3];
        glm::vec3 sum(0.0f);
        const std::uint32_t group = groupOf[vertex];
        for (std::uint32_t k = groupStart[group]; k < groupStart[group + 1]; ++k) {
            const std::uint32_t other = groupCorners[k];
            const glm::vec3& otherNormal = faceNormals[other / 3];
            if (glm::dot(otherNormal, faceNormal) >= cosCrease) {
                sum += angles[other] * otherNormal;
            }
        }
        const float length = glm::length(sum);
        glm::vec3 normal = length > 1e-20f && std::isfinite(length) ? sum / length : faceNormal;
        if (normal == glm::vec3(0.0f)) {
            normal = glm::vec3(0.0f, 0.0f, 1.0f); // degenerate face: any unit normal is valid
        }

        std::uint32_t found = kNone;
        for (std::uint32_t s = firstSplit[vertex]; s != kNone; s = nextSplit[s]) {
            if (glm::dot(out.normals[s], normal) > 0.9999f) {
                found = s;
                break;
            }
        }
        if (found == kNone) {
            found = static_cast<std::uint32_t>(out.positions.size());
            out.positions.push_back(mesh.positions[vertex]);
            out.normals.push_back(normal);
            if (hasUvs) {
                out.uvs.push_back(mesh.uvs[vertex]);
            }
            nextSplit.push_back(firstSplit[vertex]);
            firstSplit[vertex] = found;
        }
        out.indices[c] = found;
    }
    mesh = std::move(out);
}

bool hasUsableNormals(const MeshData& mesh) {
    if (mesh.normals.size() != mesh.positions.size()) {
        return false;
    }
    return std::all_of(mesh.normals.begin(), mesh.normals.end(), [](const glm::vec3& n) {
        return isFinite(n) && glm::dot(n, n) > 1e-12f;
    });
}

void transformMesh(MeshData& mesh, const glm::mat4& transform) {
    const glm::mat3 linear(transform);
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(linear));
    for (glm::vec3& p : mesh.positions) {
        p = glm::vec3(transform * glm::vec4(p, 1.0f));
    }
    for (glm::vec3& n : mesh.normals) {
        const glm::vec3 t = normalMatrix * n;
        const float length = glm::length(t);
        n = length > 0.0f ? t / length : n;
    }
    if (glm::determinant(linear) < 0.0f) {
        flipWinding(mesh);
    }
}

void flipWinding(MeshData& mesh) {
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
    }
}

bool finalizeMesh(MeshData& mesh) {
    mesh.indices.resize(mesh.indices.size() - mesh.indices.size() % 3);
    if (!mesh.uvs.empty() && mesh.uvs.size() != mesh.positions.size()) {
        mesh.uvs.clear();
    }
    removeDegenerateTriangles(mesh);
    if (mesh.indices.empty()) {
        return false;
    }
    if (hasUsableNormals(mesh)) {
        for (glm::vec3& n : mesh.normals) {
            n = glm::normalize(n);
        }
    } else {
        generateNormals(mesh);
    }
    return true;
}

Bounds meshBounds(const MeshData& mesh) {
    if (mesh.positions.empty()) {
        return {};
    }
    Bounds bounds{mesh.positions.front(), mesh.positions.front()};
    for (const glm::vec3& p : mesh.positions) {
        bounds.min = glm::min(bounds.min, p);
        bounds.max = glm::max(bounds.max, p);
    }
    return bounds;
}

} // namespace lb::import
