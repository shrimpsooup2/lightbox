#include "project/project_file.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <new>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <miniz.h>
#include <nlohmann/json.hpp>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace lb::project {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::string_view kFormatName = "lightbox-project";
constexpr const char* kProjectEntry = "project.json";
constexpr const char* kThumbnailEntry = "thumbnail.png";

// meshes/<id>.bin: a 32-byte header, then positions, normals, uvs (only with the flag) and indices.
//   0  magic "LBMS"            4  u32 version (1)       8  u32 flags (bit 0: has uvs)
//   12 u32 reserved (0)        16 u64 vertex count      24 u64 index count
constexpr std::array<std::uint8_t, 4> kMeshMagic{'L', 'B', 'M', 'S'};
constexpr std::uint32_t kMeshBlobVersion = 1;
constexpr std::uint32_t kMeshHasUvs = 1;
constexpr std::size_t kMeshHeaderSize = 32;
// Far beyond any real mesh; keeps the blob size arithmetic free of overflow.
constexpr std::uint64_t kMaxMeshElements = std::uint64_t{1} << 40;

// Deflate can't expand data by more than 1032:1, so a larger claimed size is a lie that would make
// us allocate huge buffers for a tiny malicious file.
constexpr std::uint64_t kMaxDeflateRatio = 1032;

// Blobs are written straight from (and read straight into) the scene's arrays. All LightBox targets
// (x64, arm64) are little-endian; a big-endian port would byte-swap in the chain callbacks.
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(glm::vec3) == 12 && sizeof(glm::vec2) == 8);

// A damaged or inconsistent file (details for the message; wrapped into ProjectError).
class Damaged : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Why a save failed (wrapped into ProjectError naming the file).
class SaveFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void damaged(const std::string& detail) {
    throw Damaged(detail);
}

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string dumpJson(const json& value) {
    // Replace invalid UTF-8 (e.g. names from old model files) instead of refusing to save.
    return value.dump(2, ' ', false, json::error_handler_t::replace);
}

// --- JSON values ---------------------------------------------------------------------------------

// Floats are written as the double nearest to their shortest round-trip decimal (std::to_chars), so
// 0.1f appears as 0.1 instead of the widened 0.10000000149011612; nlohmann prints that double back
// as the same short decimal, and converting it to float gives the original bits. The check covers
// the theoretical double-rounding case by storing the exact widened value instead. JSON has no NaN
// or infinity: non-finite values are written as null and read back as NaN.
json floatJson(float value) {
    if (!std::isfinite(value)) {
        return nullptr;
    }
    std::array<char, 32> text{};
    const auto written = std::to_chars(text.data(), text.data() + text.size(), value);
    double shortest = 0.0;
    std::from_chars(text.data(), written.ptr, shortest);
    if (static_cast<float>(shortest) != value) {
        shortest = static_cast<double>(value);
    }
    return shortest;
}

json doubleJson(double value) {
    return std::isfinite(value) ? json(value) : json(nullptr);
}

json vecJson(const glm::vec2& v) {
    return json::array({floatJson(v.x), floatJson(v.y)});
}

json vecJson(const glm::vec3& v) {
    return json::array({floatJson(v.x), floatJson(v.y), floatJson(v.z)});
}

// IDs are 16-digit lowercase hex strings: 64-bit integers don't survive JSON readers that use
// doubles.
std::string idText(std::uint64_t value) {
    return std::format("{:016x}", value);
}

template <class Tag>
json idJson(Id<Tag> id) {
    return id ? json(idText(id.value)) : json(nullptr);
}

const json& field(const json& object, const char* key) {
    if (!object.is_object()) {
        damaged(std::format("expected an object with \"{}\"", key));
    }
    const auto it = object.find(key);
    if (it == object.end()) {
        damaged(std::format("\"{}\" is missing", key));
    }
    return *it;
}

const json& arrayAt(const json& object, const char* key) {
    const json& value = field(object, key);
    if (!value.is_array()) {
        damaged(std::format("\"{}\" is not a list", key));
    }
    return value;
}

const std::string& stringAt(const json& object, const char* key) {
    const json& value = field(object, key);
    if (!value.is_string()) {
        damaged(std::format("\"{}\" is not text", key));
    }
    return value.get_ref<const std::string&>();
}

bool boolAt(const json& object, const char* key) {
    const json& value = field(object, key);
    if (!value.is_boolean()) {
        damaged(std::format("\"{}\" is not true or false", key));
    }
    return value.get<bool>();
}

double asDouble(const json& value, const char* what) {
    if (value.is_null()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (!value.is_number()) {
        damaged(std::format("\"{}\" is not a number", what));
    }
    return value.get<double>();
}

float asFloat(const json& value, const char* what) {
    const double number = asDouble(value, what);
    if (std::isnan(number)) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    // Converting an out-of-range double to float is undefined behavior.
    if (!(std::abs(number) <= static_cast<double>(std::numeric_limits<float>::max()))) {
        damaged(std::format("\"{}\" is out of range", what));
    }
    return static_cast<float>(number);
}

float floatAt(const json& object, const char* key) {
    return asFloat(field(object, key), key);
}

double doubleAt(const json& object, const char* key) {
    return asDouble(field(object, key), key);
}

std::int64_t integerAt(const json& object, const char* key, std::int64_t min, std::int64_t max) {
    const json& value = field(object, key);
    std::int64_t number = 0;
    if (value.is_number_unsigned()) {
        const auto unsignedNumber = value.get<std::uint64_t>();
        if (unsignedNumber > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            damaged(std::format("\"{}\" is out of range", key));
        }
        number = static_cast<std::int64_t>(unsignedNumber);
    } else if (value.is_number_integer()) {
        number = value.get<std::int64_t>();
    } else {
        damaged(std::format("\"{}\" is not a whole number", key));
    }
    if (number < min || number > max) {
        damaged(std::format("\"{}\" is out of range", key));
    }
    return number;
}

int intAt(const json& object, const char* key, int min = INT_MIN, int max = INT_MAX) {
    return static_cast<int>(integerAt(object, key, min, max));
}

std::uint64_t countAt(const json& object, const char* key) {
    const json& value = field(object, key);
    if (!value.is_number_unsigned()) {
        damaged(std::format("\"{}\" is not a count", key));
    }
    return value.get<std::uint64_t>();
}

template <glm::length_t N>
glm::vec<N, float> asVec(const json& value, const char* what) {
    if (!value.is_array() || value.size() != N) {
        damaged(std::format("\"{}\" is not a list of {} numbers", what, N));
    }
    glm::vec<N, float> result;
    for (glm::length_t i = 0; i < N; ++i) {
        result[i] = asFloat(value[static_cast<std::size_t>(i)], what);
    }
    return result;
}

template <glm::length_t N>
glm::vec<N, float> vecAt(const json& object, const char* key) {
    return asVec<N>(field(object, key), key);
}

template <class IdType>
IdType asId(const json& value, const char* what) {
    if (!value.is_string()) {
        damaged(std::format("\"{}\" is not an id", what));
    }
    const std::string& text = value.get_ref<const std::string&>();
    const bool canonical =
        text.size() == 16 && std::all_of(text.begin(), text.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    IdType id;
    if (canonical) {
        std::from_chars(text.data(), text.data() + text.size(), id.value, 16);
    }
    if (!id) {
        damaged(std::format("\"{}\" is not a valid id: \"{}\"", what, text));
    }
    return id;
}

template <class IdType>
IdType optionalIdAt(const json& object, const char* key) {
    const json& value = field(object, key);
    return value.is_null() ? IdType{} : asId<IdType>(value, key);
}

template <class IdType>
IdType idAt(const json& object, const char* key) {
    return asId<IdType>(field(object, key), key);
}

template <class IdType>
std::vector<IdType> idsAt(const json& object, const char* key) {
    const json& list = arrayAt(object, key);
    std::vector<IdType> ids;
    ids.reserve(list.size());
    for (const json& value : list) {
        ids.push_back(asId<IdType>(value, key));
    }
    return ids;
}

// --- Enums ---------------------------------------------------------------------------------------
// Stored by name so the file stays readable and reordering the C++ enums can't break old files.

std::string_view enumName(SunMode mode) {
    switch (mode) {
    case SunMode::Manual: return "manual";
    case SunMode::Location: return "location";
    }
    throw std::logic_error("unknown SunMode");
}

std::string_view enumName(ToneMapping mapping) {
    switch (mapping) {
    case ToneMapping::Filmic: return "filmic";
    case ToneMapping::Neutral: return "neutral";
    case ToneMapping::None: return "none";
    case ToneMapping::AgX: return "agx";
    }
    throw std::logic_error("unknown ToneMapping");
}

std::string_view enumName(TextureMapping mapping) {
    switch (mapping) {
    case TextureMapping::UV: return "uv";
    case TextureMapping::Box: return "box";
    }
    throw std::logic_error("unknown TextureMapping");
}

std::string_view enumName(LightType type) {
    switch (type) {
    case LightType::Point: return "point";
    case LightType::Spot: return "spot";
    case LightType::Area: return "area";
    }
    throw std::logic_error("unknown LightType");
}

template <class Enum>
json enumJson(Enum value) {
    return std::string(enumName(value));
}

template <class Enum>
Enum enumAt(const json& object, const char* key, std::initializer_list<Enum> values) {
    const std::string& text = stringAt(object, key);
    for (Enum value : values) {
        if (enumName(value) == text) {
            return value;
        }
    }
    damaged(std::format("\"{}\" has the unknown value \"{}\"", key, text));
}

// --- Document → JSON -----------------------------------------------------------------------------

std::string meshBlobName(ResourceId id) {
    return std::format("meshes/{}.bin", idText(id.value));
}

std::string textureBlobName(ResourceId id) {
    return std::format("textures/{}.rgba", idText(id.value));
}

json environmentJson(const Environment& environment) {
    const Sun& sun = environment.sun;
    json location;
    location["latitudeDegrees"] = doubleJson(sun.location.latitudeDegrees);
    location["longitudeDegrees"] = doubleJson(sun.location.longitudeDegrees);
    location["utcOffsetHours"] = floatJson(sun.location.utcOffsetHours);
    location["northAngleDegrees"] = floatJson(sun.location.northAngleDegrees);
    json dateTime;
    dateTime["year"] = sun.dateTime.year;
    dateTime["month"] = sun.dateTime.month;
    dateTime["day"] = sun.dateTime.day;
    dateTime["hours"] = floatJson(sun.dateTime.hours);
    json sunJson;
    sunJson["mode"] = enumJson(sun.mode);
    sunJson["azimuthDegrees"] = floatJson(sun.azimuthDegrees);
    sunJson["elevationDegrees"] = floatJson(sun.elevationDegrees);
    sunJson["location"] = std::move(location);
    sunJson["dateTime"] = std::move(dateTime);
    sunJson["intensity"] = floatJson(sun.intensity);
    sunJson["temperatureKelvin"] = floatJson(sun.temperatureKelvin);
    json result;
    result["sun"] = std::move(sunJson);
    result["sky"]["intensity"] = floatJson(environment.sky.intensity);
    if (!environment.sky.image.empty()) {
        result["sky"]["image"] = environment.sky.image;
        result["sky"]["imageRotationDegrees"] = floatJson(environment.sky.imageRotationDegrees);
    }
    result["shadows"]["enabled"] = environment.shadows.enabled;
    result["shadows"]["softness"] = floatJson(environment.shadows.softness);
    return result;
}

json lookJson(const Look& look) {
    json result;
    result["exposureEv"] = floatJson(look.exposureEv);
    result["toneMapping"] = enumJson(look.toneMapping);
    result["contrast"] = floatJson(look.contrast);
    result["saturation"] = floatJson(look.saturation);
    result["warmth"] = floatJson(look.warmth);
    result["vignette"] = floatJson(look.vignette);
    result["clay"] = look.clay;
    result["autoExposure"] = look.autoExposure;
    return result;
}

json slotJson(const TextureSlot& slot) {
    json result;
    result["texture"] = idJson(slot.texture);
    result["channel"] = slot.channel;
    result["invert"] = slot.invert;
    return result;
}

json materialJson(const MaterialResource& resource) {
    const Material& material = resource.material;
    json result;
    result["id"] = idJson(resource.id);
    result["name"] = resource.name;
    result["baseColor"] = vecJson(material.baseColor);
    result["roughness"] = floatJson(material.roughness);
    result["metalness"] = floatJson(material.metalness);
    result["baseColorMap"] = slotJson(material.baseColorMap);
    result["roughnessMap"] = slotJson(material.roughnessMap);
    result["metalnessMap"] = slotJson(material.metalnessMap);
    result["normalMap"] = slotJson(material.normalMap);
    result["occlusionMap"] = slotJson(material.occlusionMap);
    result["heightMap"] = slotJson(material.heightMap);
    result["heightDepth"] = floatJson(material.heightDepth);
    result["normalStrength"] = floatJson(material.normalStrength);
    result["normalFlipY"] = material.normalFlipY;
    result["mapping"] = enumJson(material.mapping);
    result["textureScale"] = vecJson(material.textureScale);
    result["textureRotationDegrees"] = floatJson(material.textureRotationDegrees);
    result["textureOffset"] = vecJson(material.textureOffset);
    result["variation"] = floatJson(material.variation);
    result["randomTileRotation"] = material.randomTileRotation;
    result["transmission"] = floatJson(material.transmission);
    result["ior"] = floatJson(material.ior);
    result["thinWalled"] = material.thinWalled;
    result["emissionColor"] = vecJson(material.emissionColor);
    result["emissionStrength"] = floatJson(material.emissionStrength);
    return result;
}

json textureJson(const TextureResource& texture) {
    json result;
    result["id"] = idJson(texture.id);
    result["name"] = texture.name;
    result["sourcePath"] = texture.sourcePath;
    result["width"] = texture.image.width;
    result["height"] = texture.image.height;
    result["blob"] = textureBlobName(texture.id);
    return result;
}

json meshJson(const MeshResource& mesh) {
    json result;
    result["id"] = idJson(mesh.id);
    result["name"] = mesh.name;
    result["vertexCount"] = mesh.data.positions.size();
    result["indexCount"] = mesh.data.indices.size();
    result["hasUvs"] = !mesh.data.uvs.empty();
    result["blob"] = meshBlobName(mesh.id);
    return result;
}

json transformJson(const Transform& transform) {
    const glm::quat& r = transform.rotation;
    json result;
    result["position"] = vecJson(transform.position);
    result["rotation"] = json::array({floatJson(r.x), floatJson(r.y), floatJson(r.z), floatJson(r.w)});
    result["scale"] = vecJson(transform.scale);
    return result;
}

json entityJson(const Entity& entity) {
    json children = json::array();
    for (EntityId child : entity.children) {
        children.push_back(idJson(child));
    }
    json result;
    result["id"] = idJson(entity.id);
    result["name"] = entity.name;
    result["parent"] = idJson(entity.parent);
    result["children"] = std::move(children);
    result["transform"] = transformJson(entity.transform);
    if (entity.meshInstance) {
        result["meshInstance"]["mesh"] = idJson(entity.meshInstance->mesh);
        result["meshInstance"]["material"] = idJson(entity.meshInstance->material);
    } else {
        result["meshInstance"] = nullptr;
    }
    if (entity.light) {
        const Light& light = *entity.light;
        json lightJson;
        lightJson["type"] = enumJson(light.type);
        lightJson["lumens"] = floatJson(light.lumens);
        lightJson["temperatureKelvin"] = floatJson(light.temperatureKelvin);
        lightJson["radius"] = floatJson(light.radius);
        lightJson["areaSize"] = vecJson(light.areaSize);
        lightJson["spotAngleDegrees"] = floatJson(light.spotAngleDegrees);
        lightJson["spotSoftness"] = floatJson(light.spotSoftness);
        result["light"] = std::move(lightJson);
    }
    return result;
}

template <class T>
void sortById(std::vector<const T*>& items) {
    std::sort(items.begin(), items.end(), [](const T* a, const T* b) { return a->id < b->id; });
}

template <class T>
std::vector<const T*> sortedMeshesOrTextures(const Scene& scene) {
    std::vector<const T*> items;
    if constexpr (std::is_same_v<T, MeshResource>) {
        scene.forEachMesh([&items](const MeshResource& mesh) { items.push_back(&mesh); });
    } else {
        scene.forEachTexture([&items](const TextureResource& texture) { items.push_back(&texture); });
    }
    sortById(items);
    return items;
}

// The canonical document: arrays sorted by ID and object keys sorted (nlohmann's std::map), so equal
// scenes produce byte-identical JSON.
json documentObject(const Scene& scene) {
    std::vector<const MaterialResource*> materials;
    scene.forEachMaterial(
        [&materials](const MaterialResource& material) { materials.push_back(&material); });
    sortById(materials);
    std::vector<const Entity*> entities;
    scene.traverse([&entities](const Entity& entity, const glm::mat4&) { entities.push_back(&entity); });
    sortById(entities);

    json result;
    result["environment"] = environmentJson(scene.environment());
    result["look"] = lookJson(scene.look());
    result["materials"] = json::array();
    for (const MaterialResource* material : materials) {
        result["materials"].push_back(materialJson(*material));
    }
    result["textures"] = json::array();
    for (const TextureResource* texture : sortedMeshesOrTextures<TextureResource>(scene)) {
        result["textures"].push_back(textureJson(*texture));
    }
    result["meshes"] = json::array();
    for (const MeshResource* mesh : sortedMeshesOrTextures<MeshResource>(scene)) {
        result["meshes"].push_back(meshJson(*mesh));
    }
    result["entities"] = json::array();
    for (const Entity* entity : entities) {
        result["entities"].push_back(entityJson(*entity));
    }
    result["roots"] = json::array();
    for (EntityId root : scene.roots()) {
        result["roots"].push_back(idJson(root));
    }
    return result;
}

std::string isoUtc(std::int64_t unixSeconds) {
    using namespace std::chrono;
    const sys_seconds time{seconds{unixSeconds}};
    const sys_days day = floor<days>(time);
    const year_month_day date{day};
    const hh_mm_ss clock{time - day};
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                       clock.hours().count(), clock.minutes().count(), clock.seconds().count());
}

json cameraJson(const CameraState& camera) {
    json result;
    result["target"] = vecJson(camera.target);
    result["distance"] = floatJson(camera.distance);
    result["yawRadians"] = floatJson(camera.yawRadians);
    result["pitchRadians"] = floatJson(camera.pitchRadians);
    result["focalLength"] = floatJson(camera.focalLength);
    result["twoPoint"] = camera.twoPoint;
    return result;
}

// --- JSON → document -----------------------------------------------------------------------------

std::int64_t parseIsoUtc(const std::string& text) {
    // Exactly the form isoUtc writes: YYYY-MM-DDTHH:MM:SSZ.
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
        damaged(std::format("\"savedAt\" is not a UTC time: \"{}\"", text));
    }
    const auto number = [&text](std::size_t at, std::size_t length) {
        int value = 0;
        const char* end = text.data() + at + length;
        const auto [ptr, error] = std::from_chars(text.data() + at, end, value);
        if (error != std::errc{} || ptr != end || value < 0) {
            damaged(std::format("\"savedAt\" is not a UTC time: \"{}\"", text));
        }
        return value;
    };
    using namespace std::chrono;
    const year_month_day date{year{number(0, 4)}, month{static_cast<unsigned>(number(5, 2))},
                              day{static_cast<unsigned>(number(8, 2))}};
    const int hour = number(11, 2);
    const int minute = number(14, 2);
    const int second = number(17, 2);
    if (!date.ok() || hour > 23 || minute > 59 || second > 60) {
        damaged(std::format("\"savedAt\" is not a UTC time: \"{}\"", text));
    }
    const seconds sinceEpoch =
        sys_days{date}.time_since_epoch() + hours{hour} + minutes{minute} + seconds{second};
    return sinceEpoch.count();
}

ProjectInfo asInfo(const json& object) {
    ProjectInfo info;
    info.name = stringAt(object, "name");
    info.appVersion = stringAt(object, "appVersion");
    info.savedAtUnix = parseIsoUtc(stringAt(object, "savedAt"));
    if (object.contains("camera") && !object.at("camera").is_null()) {
        const json& camera = object.at("camera");
        CameraState state;
        state.target = vecAt<3>(camera, "target");
        state.distance = floatAt(camera, "distance");
        state.yawRadians = floatAt(camera, "yawRadians");
        state.pitchRadians = floatAt(camera, "pitchRadians");
        state.focalLength = floatAt(camera, "focalLength");
        state.twoPoint = boolAt(camera, "twoPoint");
        info.camera = state;
    }
    return info;
}

Environment asEnvironment(const json& object) {
    Environment environment;
    Sun& sun = environment.sun;
    const json& sunJson = field(object, "sun");
    sun.mode = enumAt(sunJson, "mode", {SunMode::Manual, SunMode::Location});
    sun.azimuthDegrees = floatAt(sunJson, "azimuthDegrees");
    sun.elevationDegrees = floatAt(sunJson, "elevationDegrees");
    const json& location = field(sunJson, "location");
    sun.location.latitudeDegrees = doubleAt(location, "latitudeDegrees");
    sun.location.longitudeDegrees = doubleAt(location, "longitudeDegrees");
    sun.location.utcOffsetHours = floatAt(location, "utcOffsetHours");
    sun.location.northAngleDegrees = floatAt(location, "northAngleDegrees");
    const json& dateTime = field(sunJson, "dateTime");
    sun.dateTime.year = intAt(dateTime, "year");
    sun.dateTime.month = intAt(dateTime, "month");
    sun.dateTime.day = intAt(dateTime, "day");
    sun.dateTime.hours = floatAt(dateTime, "hours");
    sun.intensity = floatAt(sunJson, "intensity");
    sun.temperatureKelvin = floatAt(sunJson, "temperatureKelvin");
    const json& sky = field(object, "sky");
    environment.sky.intensity = floatAt(sky, "intensity");
    // Sky images were added after format 1 shipped (optional; a linked file, not embedded).
    if (sky.contains("image")) {
        environment.sky.image = stringAt(sky, "image");
        environment.sky.imageRotationDegrees = floatAt(sky, "imageRotationDegrees");
    }
    const json& shadows = field(object, "shadows");
    environment.shadows.enabled = boolAt(shadows, "enabled");
    environment.shadows.softness = floatAt(shadows, "softness");
    return environment;
}

Look asLook(const json& object) {
    Look look;
    look.exposureEv = floatAt(object, "exposureEv");
    look.toneMapping = enumAt(object, "toneMapping",
                              {ToneMapping::Filmic, ToneMapping::Neutral, ToneMapping::None,
                               ToneMapping::AgX});
    look.contrast = floatAt(object, "contrast");
    look.saturation = floatAt(object, "saturation");
    look.warmth = floatAt(object, "warmth");
    look.vignette = floatAt(object, "vignette");
    look.clay = boolAt(object, "clay");
    // Added after format 1 shipped; older projects keep their fixed exposure.
    look.autoExposure = object.contains("autoExposure") && boolAt(object, "autoExposure");
    return look;
}

TextureSlot slotAt(const json& object, const char* key) {
    const json& slotJson = field(object, key);
    TextureSlot slot;
    slot.texture = optionalIdAt<ResourceId>(slotJson, "texture");
    slot.channel = intAt(slotJson, "channel", 0, 3);
    slot.invert = boolAt(slotJson, "invert");
    return slot;
}

MaterialResource asMaterial(const json& object) {
    MaterialResource resource;
    resource.id = idAt<ResourceId>(object, "id");
    resource.name = stringAt(object, "name");
    Material& material = resource.material;
    material.baseColor = vecAt<3>(object, "baseColor");
    material.roughness = floatAt(object, "roughness");
    material.metalness = floatAt(object, "metalness");
    material.baseColorMap = slotAt(object, "baseColorMap");
    material.roughnessMap = slotAt(object, "roughnessMap");
    material.metalnessMap = slotAt(object, "metalnessMap");
    material.normalMap = slotAt(object, "normalMap");
    material.occlusionMap = slotAt(object, "occlusionMap");
    // Added after format 1 shipped; older projects have no height map.
    if (object.contains("heightMap")) {
        material.heightMap = slotAt(object, "heightMap");
        material.heightDepth = floatAt(object, "heightDepth");
    }
    material.normalStrength = floatAt(object, "normalStrength");
    material.normalFlipY = boolAt(object, "normalFlipY");
    material.mapping = enumAt(object, "mapping", {TextureMapping::UV, TextureMapping::Box});
    material.textureScale = vecAt<2>(object, "textureScale");
    material.textureRotationDegrees = floatAt(object, "textureRotationDegrees");
    material.textureOffset = vecAt<2>(object, "textureOffset");
    // Added after format 1 shipped; projects saved before it have no variation.
    material.variation = object.contains("variation") ? floatAt(object, "variation") : 0.0f;
    material.randomTileRotation =
        object.contains("randomTileRotation") && boolAt(object, "randomTileRotation");
    // Glass and emission were added after format 1 shipped; older projects are opaque, unlit.
    if (object.contains("transmission")) {
        material.transmission = floatAt(object, "transmission");
        material.ior = floatAt(object, "ior");
        material.thinWalled = boolAt(object, "thinWalled");
    }
    if (object.contains("emissionStrength")) {
        material.emissionColor = vecAt<3>(object, "emissionColor");
        material.emissionStrength = floatAt(object, "emissionStrength");
    }
    return resource;
}

Entity asEntity(const json& object) {
    Entity entity;
    entity.id = idAt<EntityId>(object, "id");
    entity.name = stringAt(object, "name");
    entity.parent = optionalIdAt<EntityId>(object, "parent");
    entity.children = idsAt<EntityId>(object, "children");
    const json& transform = field(object, "transform");
    entity.transform.position = vecAt<3>(transform, "position");
    const glm::vec4 rotation = vecAt<4>(transform, "rotation"); // x, y, z, w (glTF order)
    entity.transform.rotation.x = rotation.x;
    entity.transform.rotation.y = rotation.y;
    entity.transform.rotation.z = rotation.z;
    entity.transform.rotation.w = rotation.w;
    entity.transform.scale = vecAt<3>(transform, "scale");
    const json& instance = field(object, "meshInstance");
    if (!instance.is_null()) {
        entity.meshInstance = MeshInstance{idAt<ResourceId>(instance, "mesh"),
                                           optionalIdAt<ResourceId>(instance, "material")};
    }
    // Lights were added after format 1 shipped (optional key).
    if (object.contains("light")) {
        const json& lightJson = field(object, "light");
        Light light;
        light.type = enumAt(lightJson, "type", {LightType::Point, LightType::Spot, LightType::Area});
        light.lumens = floatAt(lightJson, "lumens");
        light.temperatureKelvin = floatAt(lightJson, "temperatureKelvin");
        light.radius = floatAt(lightJson, "radius");
        light.areaSize = vecAt<2>(lightJson, "areaSize");
        light.spotAngleDegrees = floatAt(lightJson, "spotAngleDegrees");
        light.spotSoftness = floatAt(lightJson, "spotSoftness");
        entity.light = light;
    }
    return entity;
}

// --- Byte chains ---------------------------------------------------------------------------------

// Several byte ranges handled as one stream (a mesh blob), so meshes go into and out of the zip
// without an intermediate copy of the whole blob.
template <class Byte>
struct ByteChain {
    std::array<std::span<Byte>, 5> parts;

    std::uint64_t size() const {
        std::uint64_t total = 0;
        for (const std::span<Byte>& part : parts) {
            total += part.size();
        }
        return total;
    }
};

// Calls `copy(chainPart, bufferOffset)` for the chain bytes [offset, offset + size); returns how
// many bytes exist there.
template <class Byte, class Copy>
std::size_t visitChain(const ByteChain<Byte>& chain, std::uint64_t offset, std::size_t size,
                       const Copy& copy) {
    std::size_t done = 0;
    std::uint64_t partStart = 0;
    for (const std::span<Byte>& part : chain.parts) {
        const std::uint64_t partEnd = partStart + part.size();
        const std::uint64_t position = offset + done;
        if (done < size && position >= partStart && position < partEnd) {
            const std::size_t inPart = position - partStart;
            const std::size_t count = std::min(size - done, part.size() - inPart);
            copy(part.subspan(inPart, count), done);
            done += count;
        }
        partStart = partEnd;
    }
    return done;
}

std::size_t readChain(void* opaque, mz_uint64 offset, void* buffer, std::size_t size) {
    const auto& chain = *static_cast<const ByteChain<const std::uint8_t>*>(opaque);
    auto* out = static_cast<std::uint8_t*>(buffer);
    return visitChain(chain, offset, size, [out](std::span<const std::uint8_t> part, std::size_t at) {
        std::memcpy(out + at, part.data(), part.size());
    });
}

std::size_t writeChain(void* opaque, mz_uint64 offset, const void* buffer, std::size_t size) {
    const auto& chain = *static_cast<const ByteChain<std::uint8_t>*>(opaque);
    const auto* in = static_cast<const std::uint8_t*>(buffer);
    return visitChain(chain, offset, size, [in](std::span<std::uint8_t> part, std::size_t at) {
        std::memcpy(part.data(), in + at, part.size());
    });
}

template <class T>
std::span<const std::uint8_t> bytesOf(const std::vector<T>& values) {
    return {reinterpret_cast<const std::uint8_t*>(values.data()), values.size() * sizeof(T)};
}

template <class T>
std::span<std::uint8_t> writableBytesOf(std::vector<T>& values) {
    return {reinterpret_cast<std::uint8_t*>(values.data()), values.size() * sizeof(T)};
}

template <class T>
void storeLittleEndian(std::uint8_t* at, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        at[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

template <class T>
T loadLittleEndian(const std::uint8_t* at) {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(static_cast<T>(at[i]) << (8 * i));
    }
    return value;
}

std::array<std::uint8_t, kMeshHeaderSize> meshHeader(const MeshData& data) {
    std::array<std::uint8_t, kMeshHeaderSize> header{};
    std::copy(kMeshMagic.begin(), kMeshMagic.end(), header.begin());
    storeLittleEndian<std::uint32_t>(header.data() + 4, kMeshBlobVersion);
    storeLittleEndian<std::uint32_t>(header.data() + 8, data.uvs.empty() ? 0 : kMeshHasUvs);
    storeLittleEndian<std::uint64_t>(header.data() + 16, data.positions.size());
    storeLittleEndian<std::uint64_t>(header.data() + 24, data.indices.size());
    return header;
}

// --- Zip files -----------------------------------------------------------------------------------
// miniz reads and writes through callbacks on C++ streams: its own fopen can't open non-ASCII paths
// on Windows, while std::fstream takes std::filesystem::path.

class ZipWriter {
public:
    ZipWriter(const fs::path& path, std::time_t fileTime) : fileTime_(fileTime) {
        out_.open(path, std::ios::binary | std::ios::trunc);
        if (!out_) {
            std::error_code error;
            const fs::path folder = path.parent_path();
            throw SaveFailure(!folder.empty() && !fs::is_directory(folder, error)
                                  ? "the folder does not exist"
                                  : "the file cannot be written (is the folder read-only?)");
        }
        zip_.m_pWrite = &ZipWriter::write;
        zip_.m_pIO_opaque = this;
        if (!mz_zip_writer_init_v2(&zip_, 0, 0)) {
            fail();
        }
        open_ = true;
    }

    ~ZipWriter() {
        if (open_) {
            mz_zip_writer_end(&zip_);
        }
    }

    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    void add(const std::string& name, std::span<const std::uint8_t> data, mz_uint level) {
        if (!mz_zip_writer_add_mem_ex_v2(&zip_, name.c_str(), data.data(), data.size(), nullptr, 0,
                                         level, 0, 0, &fileTime_, nullptr, 0, nullptr, 0)) {
            fail();
        }
    }

    void add(const std::string& name, const ByteChain<const std::uint8_t>& chain, mz_uint level) {
        // miniz takes a non-const opaque pointer; readChain only reads through it.
        void* opaque = const_cast<ByteChain<const std::uint8_t>*>(&chain);
        if (!mz_zip_writer_add_read_buf_callback(&zip_, name.c_str(), &readChain, opaque,
                                                 chain.size(), &fileTime_, nullptr, 0, level,
                                                 nullptr, 0, nullptr, 0)) {
            fail();
        }
    }

    void finish() {
        if (!mz_zip_writer_finalize_archive(&zip_)) {
            fail();
        }
        open_ = false;
        if (!mz_zip_writer_end(&zip_)) {
            fail();
        }
        out_.close();
        if (out_.fail()) {
            throw SaveFailure("writing failed (is the disk full?)");
        }
    }

private:
    [[noreturn]] void fail() {
        const mz_zip_error error = mz_zip_get_last_error(&zip_);
        if (error == MZ_ZIP_FILE_WRITE_FAILED || !out_) {
            throw SaveFailure("writing failed (is the disk full?)");
        }
        if (error == MZ_ZIP_ALLOC_FAILED) {
            throw std::bad_alloc();
        }
        throw SaveFailure(std::format("the package could not be written ({})",
                                      mz_zip_get_error_string(error)));
    }

    static std::size_t write(void* opaque, mz_uint64 offset, const void* data, std::size_t size) {
        auto& self = *static_cast<ZipWriter*>(opaque);
        if (offset != self.position_) {
            self.out_.seekp(static_cast<std::streamoff>(offset));
            self.position_ = offset;
        }
        self.out_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!self.out_) {
            return 0;
        }
        self.position_ += size;
        return size;
    }

    std::ofstream out_;
    std::uint64_t position_ = 0;
    std::time_t fileTime_;
    mz_zip_archive zip_{};
    bool open_ = false;
};

class ZipReader {
public:
    ZipReader(const fs::path& path, const std::string& name) {
        in_.open(path, std::ios::binary);
        if (!in_) {
            std::error_code error;
            throw ProjectError(fs::exists(path, error)
                                   ? std::format("Could not open \"{}\": the file cannot be read.",
                                                 name)
                                   : std::format("Could not open \"{}\": the file does not exist.",
                                                 name));
        }
        in_.seekg(0, std::ios::end);
        const std::streamoff end = in_.tellg();
        if (end < 0) {
            throw ProjectError(std::format("Could not open \"{}\": the file cannot be read.", name));
        }
        size_ = static_cast<std::uint64_t>(end);
        zip_.m_pRead = &ZipReader::read;
        zip_.m_pIO_opaque = this;
        if (!mz_zip_reader_init(&zip_, size_, 0)) {
            throw ProjectError(
                std::format("\"{}\" is not a LightBox project, or it is damaged.", name));
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

    bool contains(const std::string& entry) { return find(entry) >= 0; }

    // Uncompressed size of an entry, checked to be plausible for its compressed size.
    std::uint64_t entrySize(const std::string& entry) { return require(entry).stat.m_uncomp_size; }

    // Entry contents into `out`, which must be exactly the entry's size.
    void extract(const std::string& entry, std::span<std::uint8_t> out) {
        const auto [index, stat] = require(entry);
        if (stat.m_uncomp_size != out.size()) {
            damaged(std::format("\"{}\" has the wrong size", entry));
        }
        if (!mz_zip_reader_extract_to_mem(&zip_, index, out.data(), out.size(), 0)) {
            failed(entry);
        }
    }

    void extract(const std::string& entry, const ByteChain<std::uint8_t>& chain) {
        const auto [index, stat] = require(entry);
        if (stat.m_uncomp_size != chain.size()) {
            damaged(std::format("\"{}\" has the wrong size", entry));
        }
        // miniz takes a non-const opaque pointer; writeChain writes through the spans, not the chain.
        void* opaque = const_cast<ByteChain<std::uint8_t>*>(&chain);
        if (!mz_zip_reader_extract_to_callback(&zip_, index, &writeChain, opaque, 0)) {
            failed(entry);
        }
    }

    template <class Container>
    Container readAll(const std::string& entry) {
        const auto [index, stat] = require(entry);
        Container contents(stat.m_uncomp_size, typename Container::value_type{});
        if (!contents.empty() &&
            !mz_zip_reader_extract_to_mem(&zip_, index, contents.data(), contents.size(), 0)) {
            failed(entry);
        }
        return contents;
    }

private:
    struct Located {
        mz_uint index;
        mz_zip_archive_file_stat stat;
    };

    int find(const std::string& entry) {
        return mz_zip_reader_locate_file(&zip_, entry.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
    }

    Located require(const std::string& entry) {
        const int found = find(entry);
        if (found < 0) {
            damaged(std::format("\"{}\" is missing", entry));
        }
        Located located{static_cast<mz_uint>(found), {}};
        if (!mz_zip_reader_file_stat(&zip_, located.index, &located.stat) ||
            located.stat.m_is_directory || !located.stat.m_is_supported) {
            damaged(std::format("\"{}\" cannot be read", entry));
        }
        const mz_zip_archive_file_stat& stat = located.stat;
        const bool plausible =
            stat.m_comp_size <= size_ &&
            (stat.m_method == 0 ? stat.m_uncomp_size == stat.m_comp_size
                                : stat.m_uncomp_size <= stat.m_comp_size * kMaxDeflateRatio + 1024);
        if (!plausible) {
            damaged(std::format("\"{}\" has an impossible size", entry));
        }
        return located;
    }

    [[noreturn]] void failed(const std::string& entry) {
        const mz_zip_error error = mz_zip_get_last_error(&zip_);
        if (error == MZ_ZIP_ALLOC_FAILED) {
            throw std::bad_alloc();
        }
        damaged(std::format("\"{}\" could not be unpacked ({})", entry,
                            mz_zip_get_error_string(error)));
    }

    static std::size_t read(void* opaque, mz_uint64 offset, void* buffer, std::size_t size) {
        auto& self = *static_cast<ZipReader*>(opaque);
        if (offset >= self.size_) {
            return 0;
        }
        const std::size_t count = std::min<std::uint64_t>(size, self.size_ - offset);
        self.in_.clear();
        self.in_.seekg(static_cast<std::streamoff>(offset));
        self.in_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(count));
        return static_cast<std::size_t>(self.in_.gcount());
    }

    std::ifstream in_;
    std::uint64_t size_ = 0;
    mz_zip_archive zip_{};
    bool open_ = false;
};

// --- Loading -------------------------------------------------------------------------------------

using MigrationStep = void (*)(json& root);
// kMigrations[i] upgrades project.json from format i + 1 to i + 2, one step at a time so each change
// to the format comes with a small, testable upgrade. Format 1 is the first.
constexpr std::array<MigrationStep, kFormatVersion - 1> kMigrations{};

void migrate(json& root, int fromVersion) {
    for (int version = fromVersion; version < kFormatVersion; ++version) {
        kMigrations[static_cast<std::size_t>(version - 1)](root);
    }
}

// Reads project.json, refusing other files and newer formats, and migrates older ones.
// `skipDocument` leaves the (possibly large) document out of the parsed tree.
json readRoot(ZipReader& zip, const std::string& name, bool skipDocument, int& formatVersion) {
    if (!zip.contains(kProjectEntry)) {
        throw ProjectError(std::format("\"{}\" is not a LightBox project.", name));
    }
    const auto text = zip.readAll<std::string>(kProjectEntry);
    const json::parser_callback_t withoutDocument = [](int depth, json::parse_event_t event,
                                                       json& parsed) {
        return !(depth == 1 && event == json::parse_event_t::key && parsed == "document");
    };
    json root = json::parse(text, skipDocument ? withoutDocument : nullptr);
    if (!root.is_object() || root.value("format", std::string{}) != kFormatName) {
        throw ProjectError(std::format("\"{}\" is not a LightBox project.", name));
    }
    formatVersion = intAt(root, "formatVersion", 1);
    if (formatVersion > kFormatVersion) {
        throw ProjectError(std::format(
            "\"{}\" was saved with a newer version of LightBox (format {}); this version reads up "
            "to format {}. Please update LightBox.",
            name, formatVersion, kFormatVersion));
    }
    migrate(root, formatVersion);
    return root;
}

ProjectInfo readInfo(ZipReader& zip, const json& root) {
    ProjectInfo info = asInfo(field(root, "info"));
    if (zip.contains(kThumbnailEntry)) {
        info.thumbnailPng = zip.readAll<std::vector<std::uint8_t>>(kThumbnailEntry);
    }
    return info;
}

TextureResource readTexture(ZipReader& zip, const json& object) {
    TextureResource texture;
    texture.id = idAt<ResourceId>(object, "id");
    texture.name = stringAt(object, "name");
    texture.sourcePath = stringAt(object, "sourcePath");
    ImageData& image = texture.image;
    image.width = static_cast<std::uint32_t>(integerAt(object, "width", 1, UINT32_MAX));
    image.height = static_cast<std::uint32_t>(integerAt(object, "height", 1, UINT32_MAX));
    // Two 32-bit sides times 4 bytes fit in 64 bits only up to 2^62 pixels.
    const std::uint64_t pixelCount = std::uint64_t{image.width} * image.height;
    if (pixelCount > std::numeric_limits<std::uint64_t>::max() / 4) {
        damaged(std::format("texture \"{}\" is too large", texture.name));
    }
    const std::string& blob = stringAt(object, "blob");
    // Check the stored size before allocating, so a lying header can't make us allocate.
    if (zip.entrySize(blob) != pixelCount * 4) {
        damaged(std::format("\"{}\" does not match the texture size", blob));
    }
    image.pixels.resize(pixelCount * 4);
    zip.extract(blob, std::span<std::uint8_t>(image.pixels));
    return texture;
}

MeshResource readMesh(ZipReader& zip, const json& object) {
    MeshResource mesh;
    mesh.id = idAt<ResourceId>(object, "id");
    mesh.name = stringAt(object, "name");
    const std::uint64_t vertexCount = countAt(object, "vertexCount");
    const std::uint64_t indexCount = countAt(object, "indexCount");
    const bool hasUvs = boolAt(object, "hasUvs");
    if (vertexCount > kMaxMeshElements || indexCount > kMaxMeshElements) {
        damaged(std::format("mesh \"{}\" is too large", mesh.name));
    }
    const std::uint64_t vertexSize = 2 * sizeof(glm::vec3) + (hasUvs ? sizeof(glm::vec2) : 0);
    const std::uint64_t blobSize =
        kMeshHeaderSize + vertexCount * vertexSize + indexCount * sizeof(std::uint32_t);
    const std::string& blob = stringAt(object, "blob");
    if (zip.entrySize(blob) != blobSize) {
        damaged(std::format("\"{}\" does not match the mesh size", blob));
    }

    MeshData& data = mesh.data;
    data.positions.resize(vertexCount);
    data.normals.resize(vertexCount);
    if (hasUvs) {
        data.uvs.resize(vertexCount);
    }
    data.indices.resize(indexCount);
    std::array<std::uint8_t, kMeshHeaderSize> header{};
    ByteChain<std::uint8_t> chain;
    chain.parts = {std::span<std::uint8_t>(header), writableBytesOf(data.positions),
                   writableBytesOf(data.normals), writableBytesOf(data.uvs),
                   writableBytesOf(data.indices)};
    zip.extract(blob, chain);

    const bool validHeader =
        std::equal(kMeshMagic.begin(), kMeshMagic.end(), header.begin()) &&
        loadLittleEndian<std::uint32_t>(header.data() + 4) == kMeshBlobVersion &&
        loadLittleEndian<std::uint32_t>(header.data() + 8) == (hasUvs ? kMeshHasUvs : 0) &&
        loadLittleEndian<std::uint64_t>(header.data() + 16) == vertexCount &&
        loadLittleEndian<std::uint64_t>(header.data() + 24) == indexCount;
    if (!validHeader) {
        damaged(std::format("\"{}\" has an invalid header", blob));
    }
    // The renderer trusts indices: one out of range would read outside the vertex buffers.
    if (std::any_of(data.indices.begin(), data.indices.end(),
                    [vertexCount](std::uint32_t index) { return index >= vertexCount; })) {
        damaged(std::format("\"{}\" refers to vertices that don't exist", blob));
    }
    return mesh;
}

SceneSnapshot readDocument(ZipReader& zip, const json& document) {
    SceneSnapshot snapshot;
    snapshot.environment = asEnvironment(field(document, "environment"));
    snapshot.look = asLook(field(document, "look"));
    const json& materials = arrayAt(document, "materials");
    snapshot.materials.reserve(materials.size());
    for (const json& material : materials) {
        snapshot.materials.push_back(asMaterial(material));
    }
    const json& textures = arrayAt(document, "textures");
    snapshot.textures.reserve(textures.size());
    for (const json& texture : textures) {
        snapshot.textures.push_back(readTexture(zip, texture));
    }
    const json& meshes = arrayAt(document, "meshes");
    snapshot.meshes.reserve(meshes.size());
    for (const json& mesh : meshes) {
        snapshot.meshes.push_back(readMesh(zip, mesh));
    }
    const json& entities = arrayAt(document, "entities");
    snapshot.entities.reserve(entities.size());
    for (const json& entity : entities) {
        snapshot.entities.push_back(asEntity(entity));
    }
    snapshot.roots = idsAt<EntityId>(document, "roots");
    return snapshot;
}

// Runs a load step, turning every failure into a ProjectError that names the file.
template <class Work>
auto guardLoad(const fs::path& path, const Work& work) {
    const std::string name = utf8(path.filename());
    const auto damagedFile = [&name](std::string_view detail) {
        return ProjectError(
            std::format("\"{}\" is damaged and cannot be opened ({}).", name, detail));
    };
    try {
        return work(name);
    } catch (const ProjectError&) {
        throw;
    } catch (const Damaged& error) {
        throw damagedFile(error.what());
    } catch (const json::exception& error) {
        throw damagedFile(std::format("invalid project.json: {}", error.what()));
    } catch (const std::invalid_argument& error) { // Scene::validate
        throw damagedFile(error.what());
    } catch (const std::length_error& error) {
        throw damagedFile(error.what());
    } catch (const std::bad_alloc&) {
        throw ProjectError(std::format("Not enough memory to open \"{}\".", name));
    }
}

// --- Saving --------------------------------------------------------------------------------------

void replaceFile(const fs::path& from, const fs::path& to, const std::string& name) {
    std::error_code error;
    fs::rename(from, to, error);
    if (!error) {
        return;
    }
    // Renaming over an existing file can fail on Windows. The new file is complete by now, so only
    // now is it safe to drop the old one.
    std::error_code ignored;
    if (!fs::exists(to, ignored)) {
        throw SaveFailure(std::format("the file could not be created ({})", error.message()));
    }
    std::error_code removeError;
    if (!fs::remove(to, removeError) || removeError) {
        throw SaveFailure(std::format("the old file could not be replaced ({}); is it open in "
                                      "another program?",
                                      error.message()));
    }
    fs::rename(from, to, error);
    if (error) {
        // The old file is gone: keep the temporary file, it is now the only copy.
        throw ProjectError(std::format("Could not save \"{}\": the project was written to \"{}\" "
                                       "but could not be renamed ({}).",
                                       name, utf8(from.filename()), error.message()));
    }
}

} // namespace

void saveProject(const Scene& scene, const ProjectInfo& info, const fs::path& path) {
    const std::string name = utf8(path.filename());
    fs::path temp = path;
    temp += ".tmp";
    const auto removeTemp = [&temp] {
        std::error_code ignored;
        fs::remove(temp, ignored);
    };
    try {
        const std::time_t now = std::time(nullptr);
        json infoJson;
        infoJson["name"] = info.name;
        infoJson["appVersion"] = LIGHTBOX_VERSION;
        infoJson["savedAt"] = isoUtc(now);
        if (info.camera) {
            infoJson["camera"] = cameraJson(*info.camera);
        }
        json root;
        root["format"] = std::string(kFormatName);
        root["formatVersion"] = kFormatVersion;
        root["info"] = std::move(infoJson);
        root["document"] = documentObject(scene);
        const std::string text = dumpJson(root);

        {
            ZipWriter zip(temp, now);
            zip.add(kProjectEntry,
                    {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()},
                    MZ_DEFAULT_LEVEL);
            // Blobs are large and compress modestly: favor speed.
            for (const MeshResource* mesh : sortedMeshesOrTextures<MeshResource>(scene)) {
                const MeshData& data = mesh->data;
                const auto header = meshHeader(data);
                ByteChain<const std::uint8_t> chain;
                chain.parts = {std::span<const std::uint8_t>(header), bytesOf(data.positions),
                               bytesOf(data.normals), bytesOf(data.uvs), bytesOf(data.indices)};
                zip.add(meshBlobName(mesh->id), chain, MZ_BEST_SPEED);
            }
            for (const TextureResource* texture : sortedMeshesOrTextures<TextureResource>(scene)) {
                zip.add(textureBlobName(texture->id), bytesOf(texture->image.pixels),
                        MZ_BEST_SPEED);
            }
            if (!info.thumbnailPng.empty()) { // PNG is compressed already
                zip.add(kThumbnailEntry, bytesOf(info.thumbnailPng), MZ_NO_COMPRESSION);
            }
            zip.finish();
        }
        replaceFile(temp, path, name);
    } catch (const ProjectError&) {
        throw;
    } catch (const SaveFailure& error) {
        removeTemp();
        throw ProjectError(std::format("Could not save \"{}\": {}.", name, error.what()));
    } catch (const std::bad_alloc&) {
        removeTemp();
        throw ProjectError(std::format("Not enough memory to save \"{}\".", name));
    } catch (const std::exception& error) {
        removeTemp();
        throw ProjectError(std::format("Could not save \"{}\": {}.", name, error.what()));
    }
}

LoadedProject loadProject(const fs::path& path) {
    return guardLoad(path, [&path](const std::string& name) {
        ZipReader zip(path, name);
        LoadedProject project;
        const json root = readRoot(zip, name, false, project.formatVersion);
        project.info = readInfo(zip, root);
        project.scene = readDocument(zip, field(root, "document"));
        // Reject inconsistent files here, on the loading thread, rather than in replaceContents.
        Scene::validate(project.scene);
        return project;
    });
}

ProjectInfo readProjectInfo(const fs::path& path) {
    return guardLoad(path, [&path](const std::string& name) {
        ZipReader zip(path, name);
        int formatVersion = 0;
        const json root = readRoot(zip, name, true, formatVersion);
        return readInfo(zip, root);
    });
}

std::string documentJson(const Scene& scene) {
    return dumpJson(documentObject(scene));
}

} // namespace lb::project
