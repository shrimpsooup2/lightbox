// PBR texture sets (Poly Haven, ambientCG, Quixel…): recognizes maps by file-name tokens and builds
// one box-mapped material from them.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <miniz.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

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

constexpr std::array<TokenKind, 49> kTokens = {{
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
    {"refl", MapKind::Specular},
    {"reflection", MapKind::Specular},
    {"specular", MapKind::Specular},
    {"spec", MapKind::Specular},
    {"opacity", MapKind::Opacity},
    {"alpha", MapKind::Opacity},
    {"alphamasked", MapKind::Opacity},
    {"mask", MapKind::Opacity},
    {"sss", MapKind::Other},
    {"subsurface", MapKind::Other},
    {"transmission", MapKind::Other},
    {"translucency", MapKind::Other},
    {"sheen", MapKind::Other},
    {"fuzz", MapKind::Other},
    {"emission", MapKind::Other},
    {"emissive", MapKind::Other},
    {"idmap", MapKind::Other},
    {"cavity", MapKind::Other},
    {"curvature", MapKind::Other},
    {"preview", MapKind::Other},
}};

MapKind exactTokenKind(std::string_view token) {
    for (const TokenKind& entry : kTokens) {
        if (entry.token == token) {
            return entry.kind;
        }
    }
    return MapKind::Unknown;
}

MapKind tokenKind(std::string_view token) {
    if (const MapKind kind = exactTokenKind(token); kind != MapKind::Unknown) {
        return kind;
    }
    // 16/32-bit variants: Poliigon's "NRM16", "DISP16".
    for (const std::string_view depth : {std::string_view("16"), std::string_view("32")}) {
        if (token.size() > depth.size() + 1 && token.ends_with(depth)) {
            return exactTokenKind(token.substr(0, token.size() - depth.size()));
        }
    }
    return MapKind::Unknown;
}

// "1k", "2K", "16k": a resolution part of a file name.
bool isResolutionToken(std::string_view token) {
    return token.size() >= 2 && (token.back() == 'k') &&
           std::all_of(token.begin(), token.end() - 1,
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

MapKind classifyTokens(const std::vector<std::string>& tokens) {
    std::size_t count = tokens.size();
    // Poliigon's workflow marker ("…_COL_8K_METALNESS"): a trailing METALNESS/SPECULAR after a
    // resolution, with the real map name before the resolution. ambientCG puts the map name after
    // the resolution ("Metal032_1K-PNG_Metalness"), which this leaves alone.
    if (count >= 3 && (tokens.back() == "metalness" || tokens.back() == "specular") &&
        isResolutionToken(tokens[count - 2])) {
        for (std::size_t i = 0; i + 2 < count; ++i) {
            if (tokenKind(tokens[i]) != MapKind::Unknown) {
                --count;
                break;
            }
        }
    }
    MapKind kind = MapKind::Unknown;
    bool directX = false;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string& token = tokens[i];
        const std::string_view previous = i > 0 ? std::string_view(tokens[i - 1]) : "";
        MapKind k = tokenKind(token);
        if (token == "color" && (previous == "sheen" || previous == "subsurface" ||
                                 previous == "emission" || previous == "emissive")) {
            k = MapKind::Other; // "SheenColor": a sheen map, not the base color
        } else if (token == "opacity" && previous == "color") {
            k = MapKind::BaseColor; // Poliigon "BaseColorOpacity": base color with alpha
        }
        if (k != MapKind::Unknown) {
            kind = k;
        }
        directX = directX || token == "dx" || token == "directx";
    }
    return kind == MapKind::Normal && directX ? MapKind::NormalDirectX : kind;
}

// Tokens that end a set name without belonging to it: resolutions, formats, map names, qualifiers.
bool isNoiseToken(const std::string& token) {
    // "base" is the first half of a camelCase "BaseColor" once "Color" is gone.
    static constexpr std::array<std::string_view, 19> kNoise = {
        "png", "jpg", "jpeg", "tga",    "tif",     "tiff",      "exr",       "bmp",  "gl", "dx",
        "opengl", "directx", "map", "maps", "base", "metalness", "specular", "var1", "var2"};
    if (isResolutionToken(token) ||
        std::find(kNoise.begin(), kNoise.end(), token) != kNoise.end()) {
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
    case MapKind::Specular:
        return "specular";
    case MapKind::Opacity:
        return "opacity";
    case MapKind::Other:
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

// Archive entries larger than this are refused (a 16K PNG is ~1 GB; nothing usable is bigger).
constexpr mz_uint64 kMaxEntryBytes = mz_uint64{1} << 30;

// Reads a zip archive through a stream, so non-ASCII Windows paths work (miniz's own file functions
// use the narrow fopen). Must not move after construction: miniz keeps a pointer to it.
class ZipReader {
public:
    explicit ZipReader(std::filesystem::path path) : path_(std::move(path)) {
        stream_.open(path_, std::ios::binary | std::ios::ate);
        if (!stream_) {
            throw ImportError(std::format("Could not open {}.", detail::displayName(path_)));
        }
        const std::streamoff size = stream_.tellg();
        mz_zip_zero_struct(&zip_);
        zip_.m_pRead = &ZipReader::readAt;
        zip_.m_pIO_opaque = this;
        if (size <= 0 || !mz_zip_reader_init(&zip_, static_cast<mz_uint64>(size), 0)) {
            throw ImportError(std::format("{} is not a zip archive LightBox can read.",
                                          detail::displayName(path_)));
        }
        open_ = true;
    }
    ~ZipReader() {
        if (open_) {
            mz_zip_reader_end(&zip_);
        }
    }
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    const std::filesystem::path& path() const { return path_; }

    // Image entries (index, name), sorted by name; macOS resource forks are skipped.
    std::vector<std::pair<mz_uint, std::string>> imageEntries() {
        std::vector<std::pair<mz_uint, std::string>> entries;
        const mz_uint count = mz_zip_reader_get_num_files(&zip_);
        for (mz_uint i = 0; i < count; ++i) {
            if (mz_zip_reader_is_file_a_directory(&zip_, i)) {
                continue;
            }
            const mz_uint length = mz_zip_reader_get_filename(&zip_, i, nullptr, 0);
            std::string name(length, '\0');
            mz_zip_reader_get_filename(&zip_, i, name.data(), length);
            name.resize(length > 0 ? length - 1 : 0);
            if (name.starts_with("__MACOSX/") || !isImageFile(detail::pathFromUtf8(name))) {
                continue;
            }
            entries.emplace_back(i, std::move(name));
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });
        return entries;
    }

    std::vector<std::uint8_t> read(mz_uint index, const std::string& name) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip_, index, &stat) || stat.m_uncomp_size > kMaxEntryBytes) {
            throw ImportError(std::format("{} in {} can't be read.", name,
                                          detail::displayName(path_)));
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.m_uncomp_size));
        if (!mz_zip_reader_extract_to_mem(&zip_, index, bytes.data(), bytes.size(), 0)) {
            throw ImportError(std::format("{} in {} is damaged.", name, detail::displayName(path_)));
        }
        return bytes;
    }

private:
    static std::size_t readAt(void* opaque, mz_uint64 offset, void* buffer, std::size_t size) {
        auto* self = static_cast<ZipReader*>(opaque);
        self->stream_.clear();
        self->stream_.seekg(static_cast<std::streamoff>(offset));
        self->stream_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
        return static_cast<std::size_t>(self->stream_.gcount());
    }

    std::filesystem::path path_;
    std::ifstream stream_;
    mz_zip_archive zip_;
    bool open_ = false;
};

// One image of a texture set: a loose file, or an entry of an open archive.
struct ImageSource {
    std::filesystem::path path; // the file, or the entry name (for classification and messages)
    ZipReader* archive = nullptr;
    mz_uint index = 0;
    std::string entry;
};

std::vector<ImageSource> gatherSources(std::span<const std::filesystem::path> inputs,
                                       std::vector<std::unique_ptr<ZipReader>>& archives,
                                       std::vector<std::string>& warnings, const detail::Job& job) {
    std::vector<ImageSource> sources;
    for (const std::filesystem::path& input : inputs) {
        job.checkCancelled();
        const std::string name = detail::displayName(input);
        std::error_code error;
        if (std::filesystem::is_directory(input, error)) {
            std::vector<std::filesystem::path> images;
            const auto options = std::filesystem::directory_options::skip_permission_denied;
            for (auto it = std::filesystem::recursive_directory_iterator(input, options, error);
                 !error && it != std::filesystem::recursive_directory_iterator();
                 it.increment(error)) {
                if (it->is_regular_file(error) && isImageFile(it->path())) {
                    images.push_back(it->path());
                }
            }
            std::sort(images.begin(), images.end());
            if (images.empty()) {
                warnings.push_back(std::format("The folder {} has no images LightBox can read.", name));
            }
            for (std::filesystem::path& image : images) {
                sources.push_back({std::move(image), nullptr, 0, {}});
            }
        } else if (detail::toLowerAscii(detail::pathToUtf8(input.extension())) == ".zip") {
            auto archive = std::make_unique<ZipReader>(input);
            const auto entries = archive->imageEntries();
            if (entries.empty()) {
                warnings.push_back(std::format("{} has no images LightBox can read.", name));
            }
            for (const auto& [index, entry] : entries) {
                sources.push_back({detail::pathFromUtf8(entry), archive.get(), index, entry});
            }
            archives.push_back(std::move(archive));
        } else if (isImageFile(input)) {
            sources.push_back({input, nullptr, 0, {}});
        } else {
            warnings.push_back(
                std::format("{} is not an image LightBox can read; it was ignored.", name));
        }
    }
    return sources;
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
    // Poliigon's newer names start with the brand: "Poliigon_BrickReclaimedCommon_8442".
    if (name.size() > 9 && detail::toLowerAscii(std::string_view(name).substr(0, 9)) == "poliigon_") {
        name.erase(0, 9);
    }
    return name.empty() ? "Material" : name;
}

bool isTextureSource(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
        return true;
    }
    return isImageFile(path) || detail::toLowerAscii(detail::pathToUtf8(path.extension())) == ".zip";
}

ImportedModel importTextureSet(std::span<const std::filesystem::path> files,
                               const Progress& progress, const std::atomic<bool>* cancel) {
    const detail::Job job(progress, cancel);
    job.report(0.0f, "Reading textures");
    if (files.empty()) {
        throw ImportError("No texture files were given.");
    }

    ImportedModel model;
    model.format = "Texture set";
    model.sourcePath = detail::pathToUtf8(files.size() == 1 && !isImageFile(files.front())
                                              ? files.front()
                                              : files.front().parent_path());
    model.unitsFromFile = true;

    // Archives stay open while their images are read.
    std::vector<std::unique_ptr<ZipReader>> archives;
    std::vector<ImageSource> sources = gatherSources(files, archives, model.warnings, job);
    std::vector<std::filesystem::path> names;
    names.reserve(sources.size());
    for (const ImageSource& source : sources) {
        names.push_back(source.path);
    }
    model.name = textureSetName(names);

    std::map<MapKind, const ImageSource*> chosen;
    for (const ImageSource& source : sources) {
        const std::string name = detail::displayName(source.path);
        const MapKind kind = classifyTextureFile(source.path);
        switch (kind) {
        case MapKind::Unknown:
            model.warnings.push_back(std::format("{} was ignored: its name doesn't say which map it "
                                                 "is (like _Color, _Normal or _Roughness).",
                                                 name));
            continue;
        case MapKind::Specular:
            model.warnings.push_back(std::format("{} is a specular-workflow map; LightBox uses the "
                                                 "roughness/metalness maps instead.",
                                                 name));
            continue;
        case MapKind::Opacity:
            model.warnings.push_back(std::format(
                "{} is an opacity (cut-out) map; it was ignored because cut-outs aren't supported "
                "yet.",
                name));
            continue;
        case MapKind::Other:
            model.warnings.push_back(std::format("{} isn't used by LightBox yet; it was ignored.",
                                                 name));
            continue;
        default:
            break;
        }
        if (const auto it = chosen.find(kind); it != chosen.end()) {
            model.warnings.push_back(std::format("{} was ignored because {} is already the {} map.",
                                                 name, detail::displayName(it->second->path),
                                                 mapKindName(kind)));
            continue;
        }
        chosen.emplace(kind, &source);
    }
    const auto has = [&](MapKind kind) { return chosen.contains(kind); };
    // With both conventions present, the OpenGL map matches LightBox directly.
    if (has(MapKind::Normal)) {
        chosen.erase(MapKind::NormalDirectX);
    }
    if (has(MapKind::Gloss) && has(MapKind::Roughness)) {
        model.warnings.push_back(std::format("{} was ignored because the set has a roughness map.",
                                             detail::displayName(chosen.at(MapKind::Gloss)->path)));
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
        const ImageSource& source = *it->second;
        if (source.archive == nullptr) {
            return textures.fromFile(source.path);
        }
        const std::string name = detail::displayName(source.path);
        const std::string key = detail::pathToUtf8(source.archive->path()) + "|" + source.entry;
        try {
            const std::vector<std::uint8_t> bytes = source.archive->read(source.index, name);
            return textures.fromMemory(key, bytes, name);
        } catch (const ImportError& e) {
            return textures.fail(key, e.what());
        }
    };

    ImportedMaterial imported;
    imported.name = model.name;
    Material& material = imported.material;
    material.mapping = TextureMapping::Box;
    material.textureScale = glm::vec2(kDefaultTextureTileMeters);
    material.variation = 0.5f; // texture sets tile; hide the repeat from a distance by default

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
    if (const int height = load(MapKind::Height); height >= 0) {
        imported.height = {height, 0, false};
        material.heightDepth = 0.02f; // typical mortar/joint depth; adjustable per material
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
                     imported.occlusion.texture >= 0 || imported.height.texture >= 0;
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
