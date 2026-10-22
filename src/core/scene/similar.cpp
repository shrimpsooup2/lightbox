#include "core/scene/similar.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <glm/geometric.hpp>

#include "core/scene/scene.hpp"

namespace lb {
namespace {

// scene_builder names the parts of a multi-material node "<node> · <material>".
constexpr std::string_view kPartSeparator = " \xC2\xB7 ";

// Words exporters use when nothing was named (or that describe no kind of thing on their own).
constexpr std::array<std::string_view, 48> kGenericWords = {
    "object",    "obj",       "mesh",     "meshes",  "group",    "grp",      "node",
    "component", "comp",      "geometry", "geom",    "default",  "untitled", "unnamed",
    "noname",    "body",      "solid",    "shape",   "surface",  "polysurface", "brep",
    "instance",  "inst",      "layer",    "model",   "part",     "primitive", "entity",
    "item",      "element",   "material", "mat",     "cube",     "box",      "plane",
    "cylinder",  "sphere",    "circle",   "cone",    "torus",    "face",     "faces",
    "extrusion", "block",     "visual",   "scene",   "root",     "sketchfab"};

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}
bool isUpper(char c) {
    return c >= 'A' && c <= 'Z';
}
bool isLower(char c) {
    return c >= 'a' && c <= 'z';
}
// UTF-8 bytes of non-ASCII letters count as letters, so "Fenêtre" stays one word.
bool isLetter(char c) {
    return isUpper(c) || isLower(c) || static_cast<unsigned char>(c) >= 0x80;
}
char lower(char c) {
    return isUpper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}
bool allDigits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), isDigit);
}
std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}
bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

// Removes copy markers from the end (and "Copy of" from the start), repeatedly.
std::string_view stripCopyMarkers(std::string_view s) {
    s = trimmed(s);
    if (s.size() > 8 && equalsIgnoreCase(s.substr(0, 8), "copy of ")) {
        s = trimmed(s.substr(8));
    }
    for (bool changed = true; changed && !s.empty();) {
        changed = false;
        const std::string_view before = s;
        // "(2)", "[123]"
        if (s.back() == ')' || s.back() == ']') {
            const char open = s.back() == ')' ? '(' : '[';
            const std::size_t at = s.rfind(open);
            if (at != std::string_view::npos && allDigits(s.substr(at + 1, s.size() - at - 2))) {
                s = trimmed(s.substr(0, at));
            }
        }
        // ".001" (Blender), "#3" (SketchUp), ": 345678" (Revit element ids)
        if (const std::size_t at = s.find_last_of(".#:"); at != std::string_view::npos && at > 0 &&
                                                         allDigits(trimmed(s.substr(at + 1)))) {
            s = trimmed(s.substr(0, at));
        }
        // "copy", "copy 2", "_copy3"
        std::size_t end = s.size();
        while (end > 0 && isDigit(s[end - 1])) {
            --end;
        }
        std::string_view head = s.substr(0, end);
        while (!head.empty() && !isLetter(head.back()) && !isDigit(head.back())) {
            head.remove_suffix(1);
        }
        if (head.size() > 4 && equalsIgnoreCase(head.substr(head.size() - 4), "copy") &&
            !isLetter(head[head.size() - 5])) {
            s = trimmed(head.substr(0, head.size() - 4));
            while (!s.empty() && !isLetter(s.back()) && !isDigit(s.back())) {
                s.remove_suffix(1);
            }
        }
        changed = s != before;
    }
    return s;
}

// Lower-case words, split at separators, letter/digit changes and camelCase ("WindowFrame2" →
// window, frame, 2).
std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> result;
    std::string word;
    const auto flush = [&] {
        if (!word.empty()) {
            result.push_back(std::move(word));
            word.clear();
        }
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (!isLetter(c) && !isDigit(c)) {
            flush();
            continue;
        }
        if (!word.empty()) {
            const char previous = s[i - 1];
            const bool digitChange = isDigit(c) != isDigit(previous);
            const bool camel = isUpper(c) && isLower(previous);
            if (digitChange || camel) {
                flush();
            }
        }
        word.push_back(lower(c));
    }
    flush();
    return result;
}

bool isGeneric(const std::vector<std::string>& tokens) {
    return std::all_of(tokens.begin(), tokens.end(), [](const std::string& token) {
        return allDigits(token) || token.size() == 1 ||
               std::find(kGenericWords.begin(), kGenericWords.end(), token) != kGenericWords.end();
    });
}

std::string joined(const std::vector<std::string>& tokens, bool keepNumbers) {
    std::string result;
    for (const std::string& token : tokens) {
        if (!keepNumbers && allDigits(token)) {
            continue;
        }
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += token;
    }
    return result;
}

struct NameKey {
    std::string key;     // empty: the name says nothing (no name matching)
    bool generic = true; // only generic words: identical names within one model only
};

NameKey keyOf(std::string_view name) {
    NameKey result;
    std::size_t start = 0;
    while (start <= name.size()) {
        std::size_t end = name.find(kPartSeparator, start);
        if (end == std::string_view::npos) {
            end = name.size();
        }
        const std::string_view segment = name.substr(start, end - start);
        const std::vector<std::string> tokens = words(stripCopyMarkers(segment));
        std::string part;
        if (!tokens.empty() && isGeneric(tokens)) {
            part = joined(words(segment), true); // the number tells generic names apart
        } else if (!tokens.empty()) {
            part = joined(tokens, false);
            result.generic = false;
        }
        if (!result.key.empty() || start > 0) {
            result.key += " \xC2\xB7 ";
        }
        result.key += part;
        start = end + kPartSeparator.size();
    }
    // Nothing but separators: no key.
    if (std::all_of(result.key.begin(), result.key.end(),
                    [](char c) { return c == ' ' || c == '\xC2' || c == '\xB7'; })) {
        result.key.clear();
    }
    return result;
}

// World-space size factors along the object's own axes (mirroring ignored).
glm::vec3 scaleOf(const glm::mat4& world) {
    return {glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
            glm::length(glm::vec3(world[2]))};
}

bool sameScale(glm::vec3 a, glm::vec3 b) {
    for (int i = 0; i < 3; ++i) {
        if (std::abs(a[i] - b[i]) > 0.01f * std::max({std::abs(a[i]), std::abs(b[i]), 1e-6f})) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string similarityKey(std::string_view name) {
    return keyOf(name).key;
}

std::vector<EntityId> withSimilarObjects(const Scene& scene, std::span<const EntityId> ids) {
    struct Probe {
        NameKey name;
        EntityId root; // top-level ancestor (generic names match within it only)
        bool mesh;     // mesh objects match mesh objects, groups match groups
    };
    const auto rootOf = [&scene](EntityId id) {
        for (const Entity* e = scene.entity(id); e != nullptr && e->parent; e = scene.entity(id)) {
            id = e->parent;
        }
        return id;
    };
    // Material names are keyed once per material, not once per object using it.
    std::unordered_map<ResourceId, NameKey> materialKeys;
    const auto materialKey = [&](ResourceId material) -> const NameKey& {
        auto it = materialKeys.find(material);
        if (it == materialKeys.end()) {
            const MaterialResource* resource = scene.material(material);
            it = materialKeys.emplace(material, resource != nullptr ? keyOf(resource->name) : NameKey{})
                     .first;
        }
        return it->second;
    };

    std::vector<Probe> probes;
    // Selected mesh objects also bring objects with the same material or a similarly named one
    // ("Brick_01", "brick.002"); objects without a material (default clay) don't group together.
    std::unordered_set<ResourceId> materials;
    std::vector<Probe> materialProbes;
    // Copies of the same geometry at the same size. Scaled instances of one shape (LightBox's
    // massing boxes are one unit cube) are different objects.
    std::vector<std::pair<ResourceId, glm::vec3>> geometry;
    for (const EntityId id : ids) {
        const Entity* entity = scene.entity(id);
        if (entity == nullptr) {
            continue;
        }
        const bool mesh = entity->meshInstance.has_value();
        if (mesh && entity->meshInstance->mesh) {
            geometry.emplace_back(entity->meshInstance->mesh, scaleOf(scene.worldMatrix(id)));
        }
        if (mesh && entity->meshInstance->material) {
            materials.insert(entity->meshInstance->material);
            if (const NameKey& key = materialKey(entity->meshInstance->material); !key.key.empty()) {
                materialProbes.push_back({key, rootOf(id), true});
            }
        }
        if (NameKey name = keyOf(entity->name); !name.key.empty()) {
            probes.push_back({std::move(name), rootOf(id), mesh});
        }
    }

    std::vector<EntityId> matches(ids.begin(), ids.end());
    if (!probes.empty() || !geometry.empty() || !materials.empty()) {
        std::unordered_map<EntityId, EntityId> roots; // parents are visited before children
        scene.traverse([&](const Entity& entity, const glm::mat4& world) {
            const EntityId root = entity.parent ? roots.at(entity.parent) : entity.id;
            roots.emplace(entity.id, root);
            const bool mesh = entity.meshInstance.has_value();
            if (mesh) {
                const glm::vec3 scale = scaleOf(world);
                for (const auto& [shape, size] : geometry) {
                    if (shape == entity.meshInstance->mesh && sameScale(scale, size)) {
                        matches.push_back(entity.id);
                        return;
                    }
                }
                if (const ResourceId material = entity.meshInstance->material; material) {
                    if (materials.contains(material)) {
                        matches.push_back(entity.id);
                        return;
                    }
                    const NameKey& key = materialKey(material);
                    for (const Probe& probe : materialProbes) {
                        if (!key.key.empty() && probe.name.key == key.key &&
                            (!probe.name.generic || probe.root == root)) {
                            matches.push_back(entity.id);
                            return;
                        }
                    }
                }
            }
            if (probes.empty()) {
                return;
            }
            const NameKey name = keyOf(entity.name);
            if (name.key.empty()) {
                return;
            }
            for (const Probe& probe : probes) {
                if (probe.mesh == mesh && probe.name.key == name.key &&
                    (!probe.name.generic || probe.root == root)) {
                    matches.push_back(entity.id);
                    return;
                }
            }
        });
    }
    return meshEntitiesIn(scene, matches);
}

} // namespace lb
