// PBR texture sets (Poly Haven, ambientCG, Quixel…): recognizes maps by file-name tokens and builds
// one box-mapped material from them.

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "import/formats.hpp"
#include "import/importer.hpp"

namespace lb::import {
namespace {

bool isSeparator(char c) { return c == '_' || c == '-' || c == '.' || c == ' '; }
bool isLower(char c) { return std::islower(static_cast<unsigned char>(c)) != 0; }
bool isUpper(char c) { return std::isupper(static_cast<unsigned char>(c)) != 0; }

// "Bricks076C_1K-PNG_NormalDX" → {"bricks076c", "1k", "png", "normal", "dx"}.
std::vector<std::string> nameTokens(std::string_view stem) {
    std::vector<std::string> tokens;
    std::string current;
    const auto flush = [&] {
        if (!current.empty()) {
            tokens.push_back(detail::toLowerAscii(current));
            current.clear();
        }
    };
    for (std::size_t i = 0; i < stem.size(); ++i) {
        const char c = stem[i];
        if (isSeparator(c)) {
            flush();
            continue;
        }
        if (i > 0 && isLower(stem[i - 1]) && isUpper(c)) {
            flush(); // camelCase boundary: "AmbientOcclusion"
        }
        current.push_back(c);
    }
    flush();
    return tokens;
}

struct TokenKind {
    std::string_view token;
    MapKind kind;
};

constexpr std::array<TokenKind, 29> kTokens = {{
    {"albedo", MapKind::BaseColor},
    {"basecolor", MapKind::BaseColor},
    {"diffuse", MapKind::BaseColor},
    {"diff", MapKind::BaseColor},
    {"color", MapKind::BaseColor},
    {"colour", MapKind::BaseColor},
    {"col", MapKind::BaseColor},
    {"normal", MapKind::Normal},
    {"normals", MapKind::Normal},
    {"nor", MapKind::Normal},
    {"nrm", MapKind::Normal},
    {"norm", MapKind::Normal},
    {"roughness", MapKind::Roughness},
    {"rough", MapKind::Roughness},
    {"rgh", MapKind::Roughness},
    {"gloss", MapKind::Gloss},
    {"glossiness", MapKind::Gloss},
    {"metal", MapKind::Metalness},
    {"metallic", MapKind::Metalness},
    {"metalness", MapKind::Metalness},
    {"ao", MapKind::Occlusion},
    {"occlusion", MapKind::Occlusion},
    {"ambientocclusion", MapKind::Occlusion},
    {"height", MapKind::Height},
    {"disp", MapKind::Height},
    {"displacement", MapKind::Height},
    {"bump", MapKind::Height},
    {"arm", MapKind::PackedOcclusionRoughnessMetalness},
    {"orm", MapKind::PackedOcclusionRoughnessMetalness},
}};

MapKind tokenKind(std::string_view token) {
    for (const TokenKind& entry : kTokens) {
        if (entry.token == token) {
            return entry.kind;
        }
    }
    return MapKind::Unknown;
}

MapKind classifyTokens(const std::vector<std::string>& tokens) {
    MapKind kind = MapKind::Unknown;
    bool directX = false;
    for (const std::string& token : tokens) {
        if (const MapKind k = tokenKind(token); k != MapKind::Unknown) {
            kind = k;
        }
        directX = directX || token == "dx" || token == "directx";
    }
    return kind == MapKind::Normal && directX ? MapKind::NormalDirectX : kind;
}

// Tokens that end a set name without belonging to it: resolutions, formats, map names, qualifiers.
bool isNoiseToken(const std::string& token) {
    // "base" is the first half of a camelCase "BaseColor" once "Color" is gone.
    static constexpr std::array<std::string_view, 22> kNoise = {
        "1k",  "2k",   "3k",  "4k",  "6k", "8k", "16k",    "png",     "jpg", "jpeg", "tga",
        "tif", "tiff", "exr", "bmp", "gl", "dx", "opengl", "directx", "map", "maps", "base"};
    if (std::find(kNoise.begin(), kNoise.end(), token) != kNoise.end()) {
        return true;
    }
    return tokenKind(token) != MapKind::Unknown;
}

std::string_view mapKindName(MapKind kind) {
    switch (kind) {
    case MapKind::BaseColor:
        return "base color";
    case MapKind::Normal:
    case MapKind::NormalDirectX:
        return "normal";
    case MapKind::Roughness:
        return "roughness";
    case MapKind::Gloss:
        return "gloss";
    case MapKind::Metalness:
        return "metalness";
    case MapKind::Occlusion:
        return "ambient occlusion";
    case MapKind::Height:
        return "height";
    case MapKind::PackedOcclusionRoughnessMetalness:
        return "packed AO/roughness/metalness";
    case MapKind::Unknown:
        break;
    }
    return "unknown";
}

// True when `prefix` (a prefix of `stem`) ends between two name parts.
bool endsOnBoundary(std::string_view stem, std::size_t length) {
    if (length == 0 || length >= stem.size()) {
        return true;
    }
    return isSeparator(stem[length - 1]) || isSeparator(stem[length]) ||
           (isLower(stem[length - 1]) && isUpper(stem[length]));
}

} // namespace

MapKind classifyTextureFile(const std::filesystem::path& path) {
    return classifyTokens(nameTokens(detail::pathToUtf8(path.stem())));
}

std::string textureSetName(std::span<const std::filesystem::path> files) {
    std::vector<std::string> stems;
    for (const std::filesystem::path& file : files) {
        if (classifyTextureFile(file) != MapKind::Unknown) {
            stems.push_back(detail::pathToUtf8(file.stem()));
        }
    }
    if (stems.empty()) {
        for (const std::filesystem::path& file : files) {
            stems.push_back(detail::pathToUtf8(file.stem()));
        }
    }
    if (stems.empty()) {
        return "Material";
    }

    // Longest common prefix (case-insensitive), cut back to a name-part boundary.
    std::size_t length = stems.front().size();
    for (const std::string& stem : stems) {
        std::size_t same = 0;
        while (same < length && same < stem.size() &&
               std::tolower(static_cast<unsigned char>(stem[same])) ==
                   std::tolower(static_cast<unsigned char>(stems.front()[same]))) {
            ++same;
        }
        length = same;
    }
    while (length > 0 && !std::all_of(stems.begin(), stems.end(), [&](const std::string& stem) {
               return endsOnBoundary(stem, length);
           })) {
        --length;
    }
    std::string name = stems.front().substr(0, length);

    // Drop trailing separators and noise tokens ("_1K-PNG_", "_diff_1k"), keeping at least one part.
    while (true) {
        while (!name.empty() && isSeparator(name.back())) {
            name.pop_back();
        }
        std::size_t start = name.size();
        while (start > 0 && !isSeparator(name[start - 1]) &&
               !(isLower(name[start - 1]) && start < name.size() && isUpper(name[start]))) {
            --start;
        }
        if (start == 0) {
            break; // the only part left stays, even if it looks like a map name
        }
        if (!isNoiseToken(detail::toLowerAscii(std::string_view(name).substr(start)))) {
            break;
        }
        name.resize(start);
    }
    return name.empty() ? "Material" : name;
}

ImportedModel importTextureSet(std::span<const std::filesystem::path> files,
                               const Progress& progress, const std::atomic<bool>* cancel) {
    const detail::Job job(progress, cancel);
    job.report(0.0f, "Reading textures");
    if (files.empty()) {
        throw ImportError("No texture files were given.");
    }

    ImportedModel model;
    model.name = textureSetName(files);
    model.format = "Texture set";
    model.sourcePath = detail::pathToUtf8(files.front().parent_path());
    model.unitsFromFile = true;

    std::map<MapKind, std::filesystem::path> chosen;
    for (const std::filesystem::path& file : files) {
        const std::string name = detail::displayName(file);
        if (!isImageFile(file)) {
            model.warnings.push_back(
                std::format("{} is not an image LightBox can read; it was ignored.", name));
            continue;
        }
        const MapKind kind = classifyTextureFile(file);
        if (kind == MapKind::Unknown) {
            model.warnings.push_back(std::format("{} was ignored: its name doesn't say which map it "
                                                 "is (like _Color, _Normal or _Roughness).",
                                                 name));
            continue;
        }
        if (kind == MapKind::Height) {
            model.warnings.push_back(std::format(
                "{} is a height map; it was ignored because displacement isn't supported yet.",
                name));
            continue;
        }
        if (const auto it = chosen.find(kind); it != chosen.end()) {
            model.warnings.push_back(std::format("{} was ignored because {} is already the {} map.",
                                                 name, detail::displayName(it->second),
                                                 mapKindName(kind)));
            continue;
        }
        chosen.emplace(kind, file);
    }
    const auto has = [&](MapKind kind) { return chosen.contains(kind); };
    // With both conventions present, the OpenGL map matches LightBox directly.
    if (has(MapKind::Normal)) {
        chosen.erase(MapKind::NormalDirectX);
    }
    if (has(MapKind::Gloss) && has(MapKind::Roughness)) {
        model.warnings.push_back(std::format("{} was ignored because the set has a roughness map.",
                                             detail::displayName(chosen.at(MapKind::Gloss))));
        chosen.erase(MapKind::Gloss);
    }
    const bool packedNeeded =
        has(MapKind::PackedOcclusionRoughnessMetalness) &&
        (!has(MapKind::Occlusion) || (!has(MapKind::Roughness) && !has(MapKind::Gloss)) ||
         !has(MapKind::Metalness));
    if (has(MapKind::PackedOcclusionRoughnessMetalness) && !packedNeeded) {
        chosen.erase(MapKind::PackedOcclusionRoughnessMetalness);
    }

    detail::TextureCache textures(model, job);
    const float total = static_cast<float>(std::max<std::size_t>(chosen.size(), 1));
    float loaded = 0.0f;
    const auto load = [&](MapKind kind) {
        const auto it = chosen.find(kind);
        if (it == chosen.end()) {
            return -1;
        }
        job.report(loaded / total, "Loading textures");
        loaded += 1.0f;
        return textures.fromFile(it->second);
    };

    ImportedMaterial imported;
    imported.name = model.name;
    Material& material = imported.material;
    material.mapping = TextureMapping::Box;
    material.textureScale = glm::vec2(1.0f); // one tile per meter

    imported.baseColor.texture = load(MapKind::BaseColor);
    if (imported.baseColor.texture >= 0) {
        material.baseColor = glm::vec3(1.0f);
    }

    imported.normal.texture = load(MapKind::Normal);
    if (imported.normal.texture < 0) {
        imported.normal.texture = load(MapKind::NormalDirectX);
        material.normalFlipY = imported.normal.texture >= 0;
    }

    const int packed = load(MapKind::PackedOcclusionRoughnessMetalness);
    if (const int roughness = load(MapKind::Roughness); roughness >= 0) {
        imported.roughness = {roughness, 0, false};
    } else if (const int gloss = load(MapKind::Gloss); gloss >= 0) {
        imported.roughness = {gloss, 0, true};
    } else if (packed >= 0) {
        imported.roughness = {packed, 1, false};
    }
    if (const int metalness = load(MapKind::Metalness); metalness >= 0) {
        imported.metalness = {metalness, 0, false};
    } else if (packed >= 0) {
        imported.metalness = {packed, 2, false};
    }
    if (const int occlusion = load(MapKind::Occlusion); occlusion >= 0) {
        imported.occlusion = {occlusion, 0, false};
    } else if (packed >= 0) {
        imported.occlusion = {packed, 0, false};
    }
    // Maps multiply the scalar values, so the scalars become neutral where a map exists.
    if (imported.roughness.texture >= 0) {
        material.roughness = 1.0f;
    }
    if (imported.metalness.texture >= 0) {
        material.metalness = 1.0f;
    }

    const bool any = imported.baseColor.texture >= 0 || imported.normal.texture >= 0 ||
                     imported.roughness.texture >= 0 || imported.metalness.texture >= 0 ||
                     imported.occlusion.texture >= 0;
    if (!any) {
        throw ImportError(model.warnings.empty()
                              ? std::string("None of the files is a texture map LightBox can use.")
                              : std::format("None of the files is a texture map LightBox can use. "
                                            "{}",
                                            model.warnings.front()));
    }
    model.materials.push_back(std::move(imported));
    job.report(1.0f, "Done");
    return model;
}

} // namespace lb::import
