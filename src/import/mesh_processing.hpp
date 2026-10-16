#pragma once

// Geometry clean-up shared by the format readers (internal to lb_import).

#include <cstddef>

#include <glm/mat4x4.hpp>

#include "core/mesh.hpp"

namespace lb::import {

// Faces meeting at a sharper angle than this keep a hard edge when normals are generated. 40° keeps
// walls/boxes crisp while smoothing tessellated curves (typical CAD segment angles are 5–30°).
constexpr float kDefaultCreaseAngleDegrees = 40.0f;

// Removes triangles that reference out-of-range or repeated vertices, have non-finite positions, or
// have (near) zero area. Returns the number removed. Unused vertices are kept.
std::size_t removeDegenerateTriangles(MeshData& mesh);

// Replaces the normals with angle-weighted vertex normals. Corners whose faces differ by more than
// `creaseAngleDegrees` get separate normals (vertices are split), so hard edges stay hard. Vertices
// at the same position are smoothed together even if the file stored them separately (UV seams).
void generateNormals(MeshData& mesh, float creaseAngleDegrees = kDefaultCreaseAngleDegrees);

// True when every vertex has a finite, non-zero normal.
bool hasUsableNormals(const MeshData& mesh);

// Applies `transform` to positions and (inverse-transpose) to normals; reverses the winding when the
// transform mirrors, so front faces stay front faces.
void transformMesh(MeshData& mesh, const glm::mat4& transform);

// Reverses every triangle's winding.
void flipWinding(MeshData& mesh);

// Makes a reader's mesh ready for the scene: removes degenerate triangles, generates normals when
// they are missing or unusable, normalizes the rest. Returns false when no triangle is left.
bool finalizeMesh(MeshData& mesh);

// Bounds of the positions (all zero for an empty mesh).
Bounds meshBounds(const MeshData& mesh);

} // namespace lb::import
