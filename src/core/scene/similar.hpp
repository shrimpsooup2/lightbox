#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/scene/ids.hpp"

namespace lb {

class Scene;

// Finding "the same kind of object" by name, so a material can go onto every window at once.
//
// similarityKey() reduces a name to what stays the same between copies: lower case, words split at
// separators and camelCase, copy markers removed (".001", " (2)", "#3", "[12]", ": 345678",
// "copy", "Copy of") and numbers dropped ("Window_03", "window 12", "WindowFrame2" → "window",
// "window frame"). Imported multi-material parts ("Window_03 · Glass") keep their part name.
//
// Names made only of generic words ("Object 12", "Material14", "Mesh.003", "Cube") are what
// exporters write when nobody named anything; there the number IS the identity, so the key keeps
// every token and only identical names match, and only within the same top-level model.
std::string similarityKey(std::string_view name);
// The mesh objects to treat together with `ids`: the mesh objects in or inside `ids`, plus every
// object named like a selected object (similarityKey; mesh objects match mesh objects, groups match
// groups and bring their contents) and every copy of a selected object's geometry at the same size,
// whatever its name (scaled instances of one shape, like massing boxes, are not copies). Unnamed
// objects only bring their copies. Order: as meshEntitiesIn.
std::vector<EntityId> withSimilarObjects(const Scene& scene, std::span<const EntityId> ids);

} // namespace lb
