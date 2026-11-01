// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/File.h>

#include "Archive.h"
#include "Migration.h"

#include <lcad/core/Log.h>
#include <lcad/core/Path.h>
#include <lcad/core/Version.h>
#include <lcad/kernel/Error.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Serialization.h>
#include <lcad/kernel/Version.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace lcad::document {

namespace {

using detail::ArchiveContents;
using nlohmann::json;

const char* const manifestName = "manifest.json";
const char* const documentName = "document.json";
const char* const thumbnailName = "thumbnail.png";
const char* const formatName = "lcad";

// Forces what was written to `path` (a file or a directory) onto the disk. A rename is only
// crash-safe once the new file's data, and then the directory entry, have been flushed.
void flushToDisk(const std::filesystem::path& path, bool isDirectory) {
#if defined(__unix__) || defined(__APPLE__)
    const int fd = ::open(path.c_str(), isDirectory ? O_RDONLY | O_DIRECTORY : O_RDONLY);
    if (fd < 0) {
        throw FileError("Could not flush " + core::toUtf8(path) + " to disk.");
    }
    const int result = ::fsync(fd);
    ::close(fd);
    // Some file systems cannot flush a directory; that is not a failed save.
    if (result != 0 && !isDirectory) {
        throw FileError("Could not flush " + core::toUtf8(path) + " to disk.");
    }
#elif defined(_WIN32)
    // Windows flushes a file through a handle to it, and has no way to flush a folder: a rename
    // there is recorded by the file system's own journal.
    if (isDirectory) {
        return;
    }
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw FileError("Could not flush " + core::toUtf8(path) + " to disk.");
    }
    const BOOL flushed = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!flushed) {
        throw FileError("Could not flush " + core::toUtf8(path) + " to disk.");
    }
#else
    (void)path;
    (void)isDirectory;
#endif
}

std::string randomSuffix() {
    std::random_device device;
    static const char digits[] = "0123456789abcdef";
    std::string suffix;
    for (int i = 0; i < 12; ++i) {
        suffix += digits[device() % 16];
    }
    return suffix;
}

void writeAtomically(const std::filesystem::path& requested,
                     const std::vector<std::pair<std::string, std::string>>& entries) {
    std::error_code ec;
    std::filesystem::path target = std::filesystem::absolute(requested);
    // Saving through a symbolic link updates the file it points to, not the link.
    if (std::filesystem::is_symlink(target, ec)) {
        const std::filesystem::path resolved = std::filesystem::canonical(target, ec);
        if (!ec) {
            target = resolved;
        }
    }
    const std::filesystem::path directory = target.parent_path();
    if (!std::filesystem::is_directory(directory, ec)) {
        throw FileError("The folder " + core::toUtf8(directory) + " does not exist.");
    }

    // In the same folder as the target, so that the rename below stays within one file system.
    const std::filesystem::path temporary =
        directory / ("." + core::toUtf8(target.filename()) + "." + randomSuffix() + ".tmp");
    try {
        detail::writeArchive(temporary, entries);
        if (std::filesystem::exists(target, ec)) {
            // The new file takes the place of the old one, so it keeps its permissions.
            std::filesystem::permissions(temporary, std::filesystem::status(target).permissions(),
                                         ec);
        }
        flushToDisk(temporary, false);
        std::filesystem::rename(temporary, target, ec);
        if (ec) {
            const std::string reason = ec.message();
            if (std::filesystem::is_directory(target, ec)) {
                throw FileError(core::toUtf8(target) +
                                " is a folder. Choose a file name inside it.");
            }
            throw FileError("Could not save " + core::toUtf8(target) + ": " + reason + ".");
        }
    } catch (...) {
        std::filesystem::remove(temporary, ec);
        throw;
    }
    flushToDisk(directory, true);
}

// Reading document.json: every wrong or missing value is reported, never guessed.

[[noreturn]] void damaged(const std::filesystem::path& path, const std::string& problem) {
    throw FileError(core::toUtf8(path) + " is damaged: " + problem + ".");
}

json parse(const detail::Entries& entries, const char* name, const std::filesystem::path& path) {
    const auto entry = entries.find(name);
    if (entry == entries.end()) {
        throw FileError(core::toUtf8(path) + " is not an LCAD document (it has no " + name + ").");
    }
    json value = json::parse(entry->second, nullptr, /*allow_exceptions=*/false);
    if (value.is_discarded() || !value.is_object()) {
        damaged(path, std::string(name) + " is not valid JSON");
    }
    return value;
}

const json& member(const json& object, const char* key, std::string_view where,
                   const std::filesystem::path& path) {
    const auto found = object.find(key);
    if (found == object.end()) {
        damaged(path, std::string(where) + " has no \"" + key + "\"");
    }
    return *found;
}

std::string stringMember(const json& object, const char* key, std::string_view where,
                         const std::filesystem::path& path) {
    const json& value = member(object, key, where, path);
    if (!value.is_string()) {
        damaged(path, "\"" + std::string(key) + "\" in " + std::string(where) + " is not text");
    }
    return value.get<std::string>();
}

std::uint64_t idMember(const json& object, const char* key, std::string_view where,
                       const std::filesystem::path& path) {
    const json& value = member(object, key, where, path);
    if (!value.is_number_unsigned()) {
        damaged(path,
                "\"" + std::string(key) + "\" in " + std::string(where) + " is not a whole number");
    }
    return value.get<std::uint64_t>();
}

bool boolMember(const json& object, const char* key, std::string_view where,
                const std::filesystem::path& path) {
    const json& value = member(object, key, where, path);
    if (!value.is_boolean()) {
        damaged(path,
                "\"" + std::string(key) + "\" in " + std::string(where) + " is not true or false");
    }
    return value.get<bool>();
}

const json& objectMember(const json& object, const char* key, std::string_view where,
                         const std::filesystem::path& path) {
    const json& value = member(object, key, where, path);
    if (!value.is_object()) {
        damaged(path,
                "\"" + std::string(key) + "\" in " + std::string(where) + " is not an object");
    }
    return value;
}

std::string describeWriter(const json& manifest) {
    const auto writer = manifest.find("writtenBy");
    if (writer == manifest.end() || !writer->is_object()) {
        return "an unknown program";
    }
    const std::string application = writer->value("application", std::string("an unknown program"));
    const std::string version = writer->value("version", std::string());
    return version.empty() ? application : application + " " + version;
}

// A list of exactly `count` numbers.
std::vector<double> numbersMember(const json& object, const char* key, std::size_t count,
                                  std::string_view where, const std::filesystem::path& path) {
    const json& value = member(object, key, where, path);
    if (!value.is_array() || value.size() != count) {
        damaged(path, "\"" + std::string(key) + "\" in " + std::string(where) +
                          " is not a list of " + std::to_string(count) + " numbers");
    }
    std::vector<double> numbers;
    for (const json& entry : value) {
        if (!entry.is_number()) {
            damaged(path, "\"" + std::string(key) + "\" in " + std::string(where) +
                              " holds something that is not a number");
        }
        numbers.push_back(entry.get<double>());
    }
    return numbers;
}

double numberMember(const json& object, const char* key, std::string_view where,
                    const std::filesystem::path& path) {
    const json& value = member(object, key, where, path);
    if (!value.is_number()) {
        damaged(path, "\"" + std::string(key) + "\" in " + std::string(where) + " is not a number");
    }
    return value.get<double>();
}

const char* entityKindName(sketch::EntityKind kind) {
    switch (kind) {
    case sketch::EntityKind::Point:
        return "point";
    case sketch::EntityKind::Line:
        return "line";
    case sketch::EntityKind::Circle:
        return "circle";
    case sketch::EntityKind::Arc:
        return "arc";
    case sketch::EntityKind::Ellipse:
        return "ellipse";
    case sketch::EntityKind::Spline:
        return "spline";
    }
    return "point";
}

nlohmann::ordered_json writeSketch(const SketchItem& item) {
    using nlohmann::ordered_json;
    ordered_json out;
    out["id"] = item.id.value;
    out["name"] = item.name;
    out["visible"] = item.visible;
    out["plane"] =
        ordered_json{{"origin", {item.plane.origin.x, item.plane.origin.y, item.plane.origin.z}},
                     {"normal", {item.plane.normal.x, item.plane.normal.y, item.plane.normal.z}},
                     {"xDirection",
                      {item.plane.xDirection.x, item.plane.xDirection.y, item.plane.xDirection.z}}};
    if (item.component.isValid()) {
        out["component"] = item.component.value;
    }
    // Only for a sketch that follows something, so that other files read as they did.
    if (item.attachedTo.isValid()) {
        out["attachedTo"] = item.attachedTo.value;
        out["attachedFace"] = item.attachedFace;
    }
    // Only for a sketch with dimensions worked out from parameters.
    if (!item.expressions.empty()) {
        out["expressions"] = ordered_json::object();
        for (const auto& [dimension, expression] : item.expressions) {
            out["expressions"][std::to_string(dimension)] = expression;
        }
    }
    out["nextEntityId"] = item.geometry.nextEntityId();
    out["nextConstraintId"] = item.geometry.nextConstraintId();
    out["entities"] = ordered_json::array();
    for (const sketch::Entity& entity : item.geometry.entities()) {
        ordered_json e;
        e["id"] = entity.id;
        e["kind"] = entityKindName(entity.kind);
        switch (entity.kind) {
        case sketch::EntityKind::Point:
            e["x"] = entity.x;
            e["y"] = entity.y;
            e["owner"] = entity.owner;
            break;
        case sketch::EntityKind::Line:
            e["start"] = entity.start;
            e["end"] = entity.end;
            break;
        case sketch::EntityKind::Circle:
            e["center"] = entity.center;
            e["radius"] = entity.radius;
            break;
        case sketch::EntityKind::Arc:
            e["center"] = entity.center;
            e["start"] = entity.start;
            e["end"] = entity.end;
            break;
        case sketch::EntityKind::Ellipse:
            e["center"] = entity.center;
            e["major"] = entity.start;
            e["minorRadius"] = entity.radius;
            break;
        case sketch::EntityKind::Spline:
            e["controlPoints"] = entity.points;
            break;
        }
        e["construction"] = entity.construction;
        e["fixed"] = entity.fixed;
        out["entities"].push_back(std::move(e));
    }
    out["constraints"] = ordered_json::array();
    for (const sketch::Constraint& constraint : item.geometry.constraints()) {
        ordered_json c;
        c["id"] = constraint.id;
        c["kind"] = sketch::toString(constraint.kind);
        c["entities"] = {constraint.first, constraint.second, constraint.third};
        c["value"] = constraint.value;
        c["value2"] = constraint.value2;
        out["constraints"].push_back(std::move(c));
    }
    return out;
}

SketchItem readSketch(const json& entry, const std::filesystem::path& path) {
    if (!entry.is_object()) {
        damaged(path, "an entry of \"sketches\" is not an object");
    }
    SketchItem item;
    item.id = ObjectId{idMember(entry, "id", "a sketch", path)};
    const std::string where = "sketch " + std::to_string(item.id.value);
    item.name = stringMember(entry, "name", where, path);
    item.visible = boolMember(entry, "visible", where, path);
    if (entry.contains("component")) {
        item.component = ObjectId{idMember(entry, "component", where, path)};
    }

    const json& plane = objectMember(entry, "plane", where, path);
    const std::vector<double> origin = numbersMember(plane, "origin", 3, where, path);
    const std::vector<double> normal = numbersMember(plane, "normal", 3, where, path);
    const std::vector<double> along = numbersMember(plane, "xDirection", 3, where, path);
    try {
        item.plane =
            kernel::makePlane({origin[0], origin[1], origin[2]}, {normal[0], normal[1], normal[2]},
                              {along[0], along[1], along[2]});
    } catch (const std::invalid_argument&) {
        damaged(path, "the plane of " + where + " is not a plane");
    }

    const auto smallId = [&](const json& object, const char* key, const std::string& in) {
        const std::uint64_t value = idMember(object, key, in, path);
        if (value > 0xFFFFFFFFULL) {
            damaged(path, "\"" + std::string(key) + "\" in " + in + " is out of range");
        }
        return static_cast<std::uint32_t>(value);
    };

    std::vector<sketch::Entity> entities;
    const json& entityList = member(entry, "entities", where, path);
    if (!entityList.is_array()) {
        damaged(path, "\"entities\" in " + where + " is not a list");
    }
    for (const json& e : entityList) {
        if (!e.is_object()) {
            damaged(path, "an entity of " + where + " is not an object");
        }
        sketch::Entity entity;
        entity.id = smallId(e, "id", "an entity of " + where);
        const std::string in = "entity " + std::to_string(entity.id) + " of " + where;
        const std::string kind = stringMember(e, "kind", in, path);
        if (kind == "point") {
            entity.kind = sketch::EntityKind::Point;
            entity.x = numberMember(e, "x", in, path);
            entity.y = numberMember(e, "y", in, path);
            entity.owner = smallId(e, "owner", in);
        } else if (kind == "line") {
            entity.kind = sketch::EntityKind::Line;
            entity.start = smallId(e, "start", in);
            entity.end = smallId(e, "end", in);
        } else if (kind == "circle") {
            entity.kind = sketch::EntityKind::Circle;
            entity.center = smallId(e, "center", in);
            entity.radius = numberMember(e, "radius", in, path);
        } else if (kind == "arc") {
            entity.kind = sketch::EntityKind::Arc;
            entity.center = smallId(e, "center", in);
            entity.start = smallId(e, "start", in);
            entity.end = smallId(e, "end", in);
        } else if (kind == "ellipse") {
            entity.kind = sketch::EntityKind::Ellipse;
            entity.center = smallId(e, "center", in);
            entity.start = smallId(e, "major", in);
            entity.radius = numberMember(e, "minorRadius", in, path);
        } else if (kind == "spline") {
            entity.kind = sketch::EntityKind::Spline;
            const json& controls = member(e, "controlPoints", in, path);
            if (!controls.is_array()) {
                damaged(path, "\"controlPoints\" in " + in + " is not a list");
            }
            for (const json& point : controls) {
                if (!point.is_number_unsigned() || point.get<std::uint64_t>() > 0xffffffffULL) {
                    damaged(path, "\"controlPoints\" in " + in +
                                      " holds something that is not "
                                      "a point");
                }
                entity.points.push_back(point.get<sketch::EntityId>());
            }
            if (!entity.points.empty()) {
                entity.start = entity.points.front();
                entity.end = entity.points.back();
            }
        } else {
            damaged(path, in + " is of the unknown kind \"" + kind + "\"");
        }
        entity.construction = boolMember(e, "construction", in, path);
        entity.fixed = boolMember(e, "fixed", in, path);
        entities.push_back(entity);
    }

    std::vector<sketch::Constraint> constraints;
    const json& constraintList = member(entry, "constraints", where, path);
    if (!constraintList.is_array()) {
        damaged(path, "\"constraints\" in " + where + " is not a list");
    }
    for (const json& c : constraintList) {
        if (!c.is_object()) {
            damaged(path, "a constraint of " + where + " is not an object");
        }
        sketch::Constraint constraint;
        constraint.id = smallId(c, "id", "a constraint of " + where);
        const std::string in = "constraint " + std::to_string(constraint.id) + " of " + where;
        const std::string kind = stringMember(c, "kind", in, path);
        const auto known = sketch::constraintKindFromString(kind);
        if (!known.has_value()) {
            damaged(path, in + " is of the unknown kind \"" + kind + "\"");
        }
        constraint.kind = *known;
        const json& on = member(c, "entities", in, path);
        if (!on.is_array() || on.size() != 3 || !on[0].is_number_unsigned() ||
            !on[1].is_number_unsigned() || !on[2].is_number_unsigned()) {
            damaged(path, "\"entities\" in " + in + " is not a list of three ids");
        }
        constraint.first = on[0].get<std::uint32_t>();
        constraint.second = on[1].get<std::uint32_t>();
        constraint.third = on[2].get<std::uint32_t>();
        constraint.value = numberMember(c, "value", in, path);
        constraint.value2 = numberMember(c, "value2", in, path);
        constraints.push_back(constraint);
    }

    try {
        item.geometry = sketch::Sketch(std::move(entities), std::move(constraints),
                                       smallId(entry, "nextEntityId", where),
                                       smallId(entry, "nextConstraintId", where));
    } catch (const std::invalid_argument& error) {
        damaged(path, where + " does not hold together (" + error.what() + ")");
    }
    if (entry.contains("attachedTo")) {
        item.attachedTo = ObjectId{idMember(entry, "attachedTo", where, path)};
        item.attachedFace = stringMember(entry, "attachedFace", where, path);
    }
    if (entry.contains("expressions")) {
        const json& expressions = entry.at("expressions");
        if (!expressions.is_object()) {
            damaged(path, "\"expressions\" in " + where + " is not an object");
        }
        for (const auto& [key, value] : expressions.items()) {
            if (!value.is_string() || key.empty() ||
                key.find_first_not_of("0123456789") != std::string::npos) {
                damaged(path, "\"expressions\" in " + where +
                                  " holds something that is not "
                                  "an expression for a dimension");
            }
            item.expressions[static_cast<sketch::ConstraintId>(std::stoul(key))] =
                value.get<std::string>();
        }
    }
    return item;
}

// A list of whole numbers.
std::vector<std::uint64_t> idsMember(const json& object, const char* key, std::string_view where,
                                     const std::filesystem::path& path) {
    const json& value = member(object, key, where, path);
    if (!value.is_array()) {
        damaged(path, "\"" + std::string(key) + "\" in " + std::string(where) + " is not a list");
    }
    std::vector<std::uint64_t> ids;
    for (const json& entry : value) {
        if (!entry.is_number_unsigned()) {
            damaged(path, "\"" + std::string(key) + "\" in " + std::string(where) +
                              " holds something that is not a whole number");
        }
        ids.push_back(entry.get<std::uint64_t>());
    }
    return ids;
}

// A list of texts.
std::vector<std::string> stringsMember(const json& object, const char* key, std::string_view where,
                                       const std::filesystem::path& path) {
    const json& value = member(object, key, where, path);
    if (!value.is_array()) {
        damaged(path, "\"" + std::string(key) + "\" in " + std::string(where) + " is not a list");
    }
    std::vector<std::string> texts;
    for (const json& entry : value) {
        if (!entry.is_string()) {
            damaged(path, "\"" + std::string(key) + "\" in " + std::string(where) +
                              " holds something that is not text");
        }
        texts.push_back(entry.get<std::string>());
    }
    return texts;
}

nlohmann::ordered_json writeFeature(const Feature& feature) {
    using nlohmann::ordered_json;
    ordered_json out;
    out["id"] = feature.id.value;
    out["name"] = feature.name;
    out["kind"] = toString(feature.kind);
    out["suppressed"] = feature.suppressed;
    // Only for a feature of a component, and for a copy, so that other files read as they did.
    if (feature.component.isValid()) {
        out["component"] = feature.component.value;
    }
    if (!feature.tag.empty()) {
        out["tag"] = feature.tag;
    }
    out["body"] = feature.body.value;
    out["sketch"] = feature.sketch.value;
    out["regions"] = ordered_json::array();
    for (const std::vector<sketch::EntityId>& outline : feature.regions) {
        out["regions"].push_back(outline);
    }
    out["operation"] = toString(feature.operation);
    out["distance"] = feature.distance;
    out["distanceBack"] = feature.distanceBack;
    out["symmetric"] = feature.symmetric;
    out["axis"] = feature.axis;
    out["angle"] = feature.angle;
    out["edges"] = feature.edges;
    out["faces"] = feature.faces;
    out["size"] = feature.size;
    // Construction planes and mirrors and patterns. Written only for them, so that files without
    // them read as they did.
    if (feature.kind == FeatureKind::Plane || feature.kind == FeatureKind::Axis) {
        out["reference"] = feature.reference;
        out["visible"] = feature.visible;
    }
    // A revolve about an origin axis or a construction axis; the direction of a face moved.
    if ((feature.kind == FeatureKind::Revolve && !feature.reference.empty()) ||
        feature.kind == FeatureKind::MoveFace) {
        out["reference"] = feature.reference;
    }
    if (!feature.expressions.empty()) {
        out["expressions"] = feature.expressions;
    }
    // A taper and a pull up to a face: written only when there is one.
    if (feature.taper != 0.0) {
        out["taper"] = feature.taper;
    }
    if (!feature.extentFace.empty()) {
        out["extentBody"] = feature.extentBody.value;
        out["extentFace"] = feature.extentFace;
    }
    if (feature.kind == FeatureKind::Hole) {
        out["holeKind"] = toString(feature.holeKind);
        out["throughAll"] = feature.throughAll;
        out["tipAngle"] = feature.tipAngle;
        out["headDiameter"] = feature.headDiameter;
        out["headDepth"] = feature.headDepth;
        out["countersinkAngle"] = feature.countersinkAngle;
        out["flipped"] = feature.flipped;
    }
    if (feature.kind == FeatureKind::Mirror || feature.kind == FeatureKind::RectangularPattern ||
        feature.kind == FeatureKind::CircularPattern) {
        out["sources"] = ordered_json::array();
        for (const ObjectId source : feature.sources) {
            out["sources"].push_back(source.value);
        }
        out["reference"] = feature.reference;
        out["reference2"] = feature.reference2;
        out["count"] = feature.count;
        out["count2"] = feature.count2;
        out["spacing"] = feature.spacing;
        out["spacing2"] = feature.spacing2;
    }
    return out;
}

Feature readFeature(const json& entry, const std::filesystem::path& path) {
    if (!entry.is_object()) {
        damaged(path, "an entry of \"features\" is not an object");
    }
    Feature feature;
    feature.id = ObjectId{idMember(entry, "id", "a feature", path)};
    const std::string where = "feature " + std::to_string(feature.id.value);
    feature.name = stringMember(entry, "name", where, path);
    const std::string kind = stringMember(entry, "kind", where, path);
    const auto knownKind = featureKindFromString(kind);
    if (!knownKind.has_value()) {
        damaged(path, where + " is of the unknown kind \"" + kind + "\"");
    }
    feature.kind = *knownKind;
    feature.suppressed = boolMember(entry, "suppressed", where, path);
    if (entry.contains("component")) {
        feature.component = ObjectId{idMember(entry, "component", where, path)};
    }
    if (entry.contains("tag")) {
        feature.tag = stringMember(entry, "tag", where, path);
    }
    feature.body = ObjectId{idMember(entry, "body", where, path)};
    feature.sketch = ObjectId{idMember(entry, "sketch", where, path)};
    const json& regions = member(entry, "regions", where, path);
    if (!regions.is_array()) {
        damaged(path, "\"regions\" in " + where + " is not a list");
    }
    for (const json& outline : regions) {
        std::vector<sketch::EntityId> curves;
        if (!outline.is_array()) {
            damaged(path, "\"regions\" in " + where + " holds something that is not a list");
        }
        for (const json& curve : outline) {
            if (!curve.is_number_unsigned()) {
                damaged(path, "an outline in " + where + " holds something that is not a curve");
            }
            curves.push_back(curve.get<sketch::EntityId>());
        }
        std::sort(curves.begin(), curves.end());
        feature.regions.push_back(std::move(curves));
    }
    const std::string operation = stringMember(entry, "operation", where, path);
    const auto knownOperation = operationFromString(operation);
    if (!knownOperation.has_value()) {
        damaged(path, where + " has the unknown operation \"" + operation + "\"");
    }
    feature.operation = *knownOperation;
    feature.distance = numberMember(entry, "distance", where, path);
    feature.distanceBack = numberMember(entry, "distanceBack", where, path);
    feature.symmetric = boolMember(entry, "symmetric", where, path);
    feature.axis = static_cast<sketch::EntityId>(idMember(entry, "axis", where, path));
    feature.angle = numberMember(entry, "angle", where, path);
    feature.edges = stringsMember(entry, "edges", where, path);
    feature.faces = stringsMember(entry, "faces", where, path);
    feature.size = numberMember(entry, "size", where, path);
    if (feature.kind == FeatureKind::Plane || feature.kind == FeatureKind::Axis) {
        feature.reference = stringMember(entry, "reference", where, path);
        feature.visible = boolMember(entry, "visible", where, path);
    }
    if ((feature.kind == FeatureKind::Revolve && entry.contains("reference")) ||
        feature.kind == FeatureKind::MoveFace) {
        feature.reference = stringMember(entry, "reference", where, path);
    }
    if (entry.contains("expressions")) {
        const json& expressions = entry.at("expressions");
        if (!expressions.is_object()) {
            damaged(path, "\"expressions\" in " + where + " is not an object");
        }
        for (const auto& [key, value] : expressions.items()) {
            if (!value.is_string()) {
                damaged(path, "\"expressions\" in " + where +
                                  " holds something that is not "
                                  "text");
            }
            feature.expressions[key] = value.get<std::string>();
        }
    }
    if (entry.contains("taper")) {
        feature.taper = numberMember(entry, "taper", where, path);
    }
    if (entry.contains("extentFace")) {
        feature.extentBody = ObjectId{idMember(entry, "extentBody", where, path)};
        feature.extentFace = stringMember(entry, "extentFace", where, path);
    }
    if (feature.kind == FeatureKind::Hole) {
        const std::string holeKind = stringMember(entry, "holeKind", where, path);
        const auto knownHole = holeKindFromString(holeKind);
        if (!knownHole.has_value()) {
            damaged(path, where + " is a hole of the unknown kind \"" + holeKind + "\"");
        }
        feature.holeKind = *knownHole;
        feature.throughAll = boolMember(entry, "throughAll", where, path);
        feature.tipAngle = numberMember(entry, "tipAngle", where, path);
        feature.headDiameter = numberMember(entry, "headDiameter", where, path);
        feature.headDepth = numberMember(entry, "headDepth", where, path);
        feature.countersinkAngle = numberMember(entry, "countersinkAngle", where, path);
        feature.flipped = boolMember(entry, "flipped", where, path);
    }
    if (entry.contains("sources")) {
        const json& sources = entry.at("sources");
        if (!sources.is_array()) {
            damaged(path, "\"sources\" in " + where + " is not a list");
        }
        for (const json& source : sources) {
            if (!source.is_number_unsigned() || source.get<std::uint64_t>() == 0) {
                damaged(path, "\"sources\" in " + where + " holds something that is not an id");
            }
            feature.sources.push_back(ObjectId{source.get<std::uint64_t>()});
        }
        feature.reference = stringMember(entry, "reference", where, path);
        feature.reference2 = stringMember(entry, "reference2", where, path);
        const auto whole = [&](const char* key) {
            const double value = numberMember(entry, key, where, path);
            if (value != std::floor(value) || value < 0.0 || value > 1.0e6) {
                damaged(path,
                        "\"" + std::string(key) + "\" in " + where + " is not a whole number");
            }
            return static_cast<int>(value);
        };
        feature.count = whole("count");
        feature.count2 = whole("count2");
        feature.spacing = numberMember(entry, "spacing", where, path);
        feature.spacing2 = numberMember(entry, "spacing2", where, path);
    }
    return feature;
}

// Components, occurrences, joints, and materials.

nlohmann::ordered_json writePlacement(const kernel::Placement& p) {
    using nlohmann::ordered_json;
    return ordered_json{{"origin", {p.origin.x, p.origin.y, p.origin.z}},
                        {"xAxis", {p.xAxis.x, p.xAxis.y, p.xAxis.z}},
                        {"yAxis", {p.yAxis.x, p.yAxis.y, p.yAxis.z}},
                        {"zAxis", {p.zAxis.x, p.zAxis.y, p.zAxis.z}}};
}

kernel::Placement readPlacement(const json& entry, const std::string& where,
                                const std::filesystem::path& path) {
    const json& value = objectMember(entry, "placement", where, path);
    const auto point = [&](const char* key) {
        const std::vector<double> n = numbersMember(value, key, 3, where, path);
        return kernel::Point{n[0], n[1], n[2]};
    };
    const auto vector = [&](const char* key) {
        const std::vector<double> n = numbersMember(value, key, 3, where, path);
        return kernel::Vector{n[0], n[1], n[2]};
    };
    kernel::Placement placement{point("origin"), vector("xAxis"), vector("yAxis"), vector("zAxis")};
    if (!kernel::isRigid(placement)) {
        damaged(path, "the placement of " + where + " does not only turn and move");
    }
    return kernel::normalized(placement);
}

const char* snapName(kernel::FrameSnap snap) {
    switch (snap) {
    case kernel::FrameSnap::Center:
        return "center";
    case kernel::FrameSnap::Start:
        return "start";
    case kernel::FrameSnap::End:
        return "end";
    }
    return "center";
}

const char* sideKindName(JointSide::Kind kind) {
    switch (kind) {
    case JointSide::Kind::Origin:
        return "origin";
    case JointSide::Kind::Face:
        return "face";
    case JointSide::Kind::Edge:
        return "edge";
    }
    return "origin";
}

nlohmann::ordered_json writeSide(const JointSide& side) {
    using nlohmann::ordered_json;
    ordered_json out;
    out["path"] = ordered_json::array();
    for (const ObjectId id : side.path) {
        out["path"].push_back(id.value);
    }
    out["kind"] = sideKindName(side.kind);
    out["body"] = side.body.value;
    out["name"] = side.name;
    out["snap"] = snapName(side.snap);
    return out;
}

JointSide readSide(const json& entry, const char* key, const std::string& where,
                   const std::filesystem::path& path) {
    const json& value = objectMember(entry, key, where, path);
    const std::string at = std::string(key) + " side of " + where;
    JointSide side;
    for (const std::uint64_t id : idsMember(value, "path", at, path)) {
        side.path.push_back(ObjectId{id});
    }
    const std::string kind = stringMember(value, "kind", at, path);
    if (kind == "origin") {
        side.kind = JointSide::Kind::Origin;
    } else if (kind == "face") {
        side.kind = JointSide::Kind::Face;
    } else if (kind == "edge") {
        side.kind = JointSide::Kind::Edge;
    } else {
        damaged(path, "the " + at + " is on something of the unknown kind \"" + kind + "\"");
    }
    side.body = ObjectId{idMember(value, "body", at, path)};
    side.name = stringMember(value, "name", at, path);
    const std::string snap = stringMember(value, "snap", at, path);
    if (snap == "center") {
        side.snap = kernel::FrameSnap::Center;
    } else if (snap == "start") {
        side.snap = kernel::FrameSnap::Start;
    } else if (snap == "end") {
        side.snap = kernel::FrameSnap::End;
    } else {
        damaged(path, "the " + at + " is placed at the unknown \"" + snap + "\"");
    }
    return side;
}

nlohmann::ordered_json writeJoint(const Joint& joint) {
    using nlohmann::ordered_json;
    ordered_json out;
    out["id"] = joint.id.value;
    out["name"] = joint.name;
    out["kind"] = toString(joint.kind);
    out["component"] = joint.component.value;
    out["moving"] = writeSide(joint.moving);
    out["fixed"] = writeSide(joint.fixed);
    out["offset"] = joint.offset;
    out["angle"] = joint.angle;
    out["flipped"] = joint.flipped;
    out["values"] = joint.values;
    out["limits"] = ordered_json::array();
    for (const JointLimits& limits : joint.limits) {
        ordered_json entry = ordered_json::object();
        if (limits.minimum.has_value()) {
            entry["minimum"] = *limits.minimum;
        }
        if (limits.maximum.has_value()) {
            entry["maximum"] = *limits.maximum;
        }
        out["limits"].push_back(std::move(entry));
    }
    out["suppressed"] = joint.suppressed;
    return out;
}

Joint readJoint(const json& entry, const std::filesystem::path& path) {
    if (!entry.is_object()) {
        damaged(path, "an entry of \"joints\" is not an object");
    }
    Joint joint;
    joint.id = ObjectId{idMember(entry, "id", "a joint", path)};
    const std::string where = "joint " + std::to_string(joint.id.value);
    joint.name = stringMember(entry, "name", where, path);
    const std::string kind = stringMember(entry, "kind", where, path);
    const auto known = jointKindFromString(kind);
    if (!known.has_value()) {
        damaged(path, where + " is of the unknown kind \"" + kind + "\"");
    }
    joint.kind = *known;
    joint.component = ObjectId{idMember(entry, "component", where, path)};
    joint.moving = readSide(entry, "moving", where, path);
    joint.fixed = readSide(entry, "fixed", where, path);
    joint.offset = numberMember(entry, "offset", where, path);
    joint.angle = numberMember(entry, "angle", where, path);
    joint.flipped = boolMember(entry, "flipped", where, path);
    const std::vector<double> values = numbersMember(entry, "values", 3, where, path);
    std::copy(values.begin(), values.end(), joint.values.begin());
    const json& limits = member(entry, "limits", where, path);
    if (!limits.is_array() || limits.size() != 3) {
        damaged(path, "\"limits\" in " + where + " is not a list of three");
    }
    for (std::size_t i = 0; i < 3; ++i) {
        const json& limit = limits[i];
        if (!limit.is_object()) {
            damaged(path, "a limit of " + where + " is not an object");
        }
        if (limit.contains("minimum")) {
            joint.limits[i].minimum = numberMember(limit, "minimum", where, path);
        }
        if (limit.contains("maximum")) {
            joint.limits[i].maximum = numberMember(limit, "maximum", where, path);
        }
    }
    joint.suppressed = boolMember(entry, "suppressed", where, path);
    return joint;
}

// The list under `key` in document.json, or nothing if the file has none.
const json* optionalList(const json& document, const char* key, const std::filesystem::path& path) {
    const auto found = document.find(key);
    if (found == document.end()) {
        return nullptr;
    }
    if (!found->is_array()) {
        damaged(path, "\"" + std::string(key) + "\" in document.json is not a list");
    }
    for (const json& entry : *found) {
        if (!entry.is_object()) {
            damaged(path, "an entry of \"" + std::string(key) + "\" is not an object");
        }
    }
    return &*found;
}

void readAssembly(const json& document, const std::filesystem::path& path, DocumentState& state) {
    if (const json* components = optionalList(document, "components", path)) {
        for (const json& entry : *components) {
            Component component;
            component.id = ObjectId{idMember(entry, "id", "a component", path)};
            const std::string where = "component " + std::to_string(component.id.value);
            component.name = stringMember(entry, "name", where, path);
            if (entry.contains("link")) {
                component.link = stringMember(entry, "link", where, path);
                component.linkStamp = stringMember(entry, "linkStamp", where, path);
            }
            state.components.push_back(std::move(component));
        }
    }
    if (const json* occurrences = optionalList(document, "occurrences", path)) {
        for (const json& entry : *occurrences) {
            Occurrence occurrence;
            occurrence.id = ObjectId{idMember(entry, "id", "an occurrence", path)};
            const std::string where = "occurrence " + std::to_string(occurrence.id.value);
            occurrence.component = ObjectId{idMember(entry, "component", where, path)};
            occurrence.parent = ObjectId{idMember(entry, "parent", where, path)};
            occurrence.placement = readPlacement(entry, where, path);
            occurrence.grounded = boolMember(entry, "grounded", where, path);
            occurrence.visible = boolMember(entry, "visible", where, path);
            occurrence.suppressed = boolMember(entry, "suppressed", where, path);
            state.occurrences.push_back(occurrence);
        }
    }
    if (const json* joints = optionalList(document, "joints", path)) {
        for (const json& entry : *joints) {
            state.joints.push_back(readJoint(entry, path));
        }
    }
    if (const json* materials = optionalList(document, "materials", path)) {
        for (const json& entry : *materials) {
            Material material;
            material.name = stringMember(entry, "name", "a material", path);
            material.density =
                numberMember(entry, "density", "the material " + material.name, path);
            state.materials.push_back(std::move(material));
        }
    }
}

void writeAssembly(const Document& document, nlohmann::ordered_json& model,
                   const SaveOptions& options) {
    using nlohmann::ordered_json;
    // Only for documents that have them, so that other files read as they did.
    if (!document.components().empty()) {
        model["components"] = ordered_json::array();
        for (const Component& component : document.components()) {
            ordered_json entry{{"id", component.id.value}, {"name", component.name}};
            if (!component.link.empty()) {
                const auto link = options.links.find(component.id);
                const auto stamp = options.linkStamps.find(component.id);
                entry["link"] = link != options.links.end() ? link->second : component.link;
                entry["linkStamp"] =
                    stamp != options.linkStamps.end() ? stamp->second : component.linkStamp;
            }
            model["components"].push_back(std::move(entry));
        }
        model["occurrences"] = ordered_json::array();
        for (const Occurrence& occurrence : document.occurrences()) {
            model["occurrences"].push_back(
                ordered_json{{"id", occurrence.id.value},
                             {"component", occurrence.component.value},
                             {"parent", occurrence.parent.value},
                             {"placement", writePlacement(occurrence.placement)},
                             {"grounded", occurrence.grounded},
                             {"visible", occurrence.visible},
                             {"suppressed", occurrence.suppressed}});
        }
    }
    if (!document.joints().empty()) {
        model["joints"] = ordered_json::array();
        for (const Joint& joint : document.joints()) {
            model["joints"].push_back(writeJoint(joint));
        }
    }
    if (!document.materials().empty()) {
        model["materials"] = ordered_json::array();
        for (const Material& material : document.materials()) {
            model["materials"].push_back(
                ordered_json{{"name", material.name}, {"density", material.density}});
        }
    }
}

// Drawings, tools, and setups.

// A number that a file may leave out, with what it then is.
double numberOr(const json& object, const char* key, double fallback, const std::string& where,
                const std::filesystem::path& path) {
    return object.contains(key) ? numberMember(object, key, where, path) : fallback;
}

// The value of a kind, by the name it goes by in a file.
template <typename Kind>
Kind kindMember(const json& object, const char* key, bool (*fromString)(const std::string&, Kind&),
                const std::string& where, const std::filesystem::path& path) {
    const std::string name = stringMember(object, key, where, path);
    Kind kind{};
    if (!fromString(name, kind)) {
        damaged(path, where + " has the unknown " + key + " \"" + name + "\"");
    }
    return kind;
}

nlohmann::ordered_json writeDrawingPoint(const DrawingPoint& point) {
    using nlohmann::ordered_json;
    ordered_json out;
    out["body"] = point.body.value;
    out["path"] = ordered_json::array();
    for (const ObjectId id : point.path) {
        out["path"].push_back(id.value);
    }
    out["edge"] = point.edge;
    switch (point.where) {
    case DrawingPoint::Where::Start:
        out["where"] = "start";
        break;
    case DrawingPoint::Where::End:
        out["where"] = "end";
        break;
    case DrawingPoint::Where::Middle:
        out["where"] = "middle";
        break;
    case DrawingPoint::Where::Center:
        out["where"] = "center";
        break;
    }
    return out;
}

DrawingPoint readDrawingPoint(const json& entry, const char* key, const std::string& where,
                              const std::filesystem::path& path) {
    const json& value = objectMember(entry, key, where, path);
    const std::string at = "a point of " + where;
    DrawingPoint point;
    point.body = ObjectId{idMember(value, "body", at, path)};
    for (const std::uint64_t id : idsMember(value, "path", at, path)) {
        point.path.push_back(ObjectId{id});
    }
    point.edge = stringMember(value, "edge", at, path);
    const std::string name = stringMember(value, "where", at, path);
    if (name == "start") {
        point.where = DrawingPoint::Where::Start;
    } else if (name == "end") {
        point.where = DrawingPoint::Where::End;
    } else if (name == "middle") {
        point.where = DrawingPoint::Where::Middle;
    } else if (name == "center") {
        point.where = DrawingPoint::Where::Center;
    } else {
        damaged(path, at + " is at the unknown \"" + name + "\"");
    }
    return point;
}

nlohmann::ordered_json writeDrawing(const Drawing& drawing) {
    using nlohmann::ordered_json;
    ordered_json out;
    out["id"] = drawing.id.value;
    out["name"] = drawing.name;
    out["size"] = toString(drawing.size);
    out["standard"] = drawing.standard == DrawingStandard::Iso ? "iso" : "ansi";
    const TitleBlock& title = drawing.titleBlock;
    out["titleBlock"] = ordered_json{
        {"title", title.title},       {"partNumber", title.partNumber},
        {"revision", title.revision}, {"drawnBy", title.drawnBy},
        {"date", title.date},         {"material", title.material},
        {"company", title.company},
    };
    out["views"] = ordered_json::array();
    for (const DrawingView& view : drawing.views) {
        ordered_json entry;
        entry["id"] = view.id.value;
        entry["kind"] = toString(view.kind);
        entry["label"] = view.label;
        entry["scale"] = view.scale;
        entry["x"] = view.x;
        entry["y"] = view.y;
        entry["showHidden"] = view.showHidden;
        entry["centerMarks"] = view.centerMarks;
        if (view.kind == ViewKind::Section || view.kind == ViewKind::Detail) {
            entry["parent"] = view.parent.value;
        }
        if (view.kind == ViewKind::Section) {
            entry["sectionVertical"] = view.sectionVertical;
            entry["sectionPosition"] = view.sectionPosition;
            entry["sectionFlipped"] = view.sectionFlipped;
        }
        if (view.kind == ViewKind::Detail) {
            entry["detailX"] = view.detailX;
            entry["detailY"] = view.detailY;
            entry["detailRadius"] = view.detailRadius;
        }
        out["views"].push_back(std::move(entry));
    }
    out["dimensions"] = ordered_json::array();
    for (const DrawingDimension& dimension : drawing.dimensions) {
        ordered_json entry;
        entry["id"] = dimension.id.value;
        entry["view"] = dimension.view.value;
        entry["kind"] = toString(dimension.kind);
        entry["first"] = writeDrawingPoint(dimension.first);
        if (dimension.kind != DimensionKind::Radius && dimension.kind != DimensionKind::Diameter) {
            entry["second"] = writeDrawingPoint(dimension.second);
        }
        entry["offset"] = dimension.offset;
        entry["suffix"] = dimension.suffix;
        out["dimensions"].push_back(std::move(entry));
    }
    out["notes"] = ordered_json::array();
    for (const DrawingNote& note : drawing.notes) {
        out["notes"].push_back(ordered_json{{"id", note.id.value},
                                            {"text", note.text},
                                            {"x", note.x},
                                            {"y", note.y},
                                            {"height", note.height}});
    }
    return out;
}

Drawing readDrawing(const json& entry, const std::filesystem::path& path) {
    Drawing drawing;
    drawing.id = ObjectId{idMember(entry, "id", "a drawing", path)};
    drawing.name = stringMember(entry, "name", "a drawing", path);
    const std::string where = "the drawing " + drawing.name;
    drawing.size = kindMember<SheetSize>(entry, "size", sheetSizeFromString, where, path);
    const std::string standard = stringMember(entry, "standard", where, path);
    if (standard == "iso") {
        drawing.standard = DrawingStandard::Iso;
    } else if (standard == "ansi") {
        drawing.standard = DrawingStandard::Ansi;
    } else {
        damaged(path, where + " follows the unknown standard \"" + standard + "\"");
    }
    const json& title = objectMember(entry, "titleBlock", where, path);
    const std::string titleWhere = "the title block of " + where;
    drawing.titleBlock.title = stringMember(title, "title", titleWhere, path);
    drawing.titleBlock.partNumber = stringMember(title, "partNumber", titleWhere, path);
    drawing.titleBlock.revision = stringMember(title, "revision", titleWhere, path);
    drawing.titleBlock.drawnBy = stringMember(title, "drawnBy", titleWhere, path);
    drawing.titleBlock.date = stringMember(title, "date", titleWhere, path);
    drawing.titleBlock.material = stringMember(title, "material", titleWhere, path);
    drawing.titleBlock.company = stringMember(title, "company", titleWhere, path);
    if (const json* views = optionalList(entry, "views", path)) {
        for (const json& item : *views) {
            DrawingView view;
            const std::string at = "a view of " + where;
            view.id = ObjectId{idMember(item, "id", at, path)};
            view.kind = kindMember<ViewKind>(item, "kind", viewKindFromString, at, path);
            view.label = stringMember(item, "label", at, path);
            view.scale = numberMember(item, "scale", at, path);
            view.x = numberMember(item, "x", at, path);
            view.y = numberMember(item, "y", at, path);
            view.showHidden = boolMember(item, "showHidden", at, path);
            view.centerMarks = boolMember(item, "centerMarks", at, path);
            if (view.kind == ViewKind::Section || view.kind == ViewKind::Detail) {
                view.parent = ObjectId{idMember(item, "parent", at, path)};
            }
            if (view.kind == ViewKind::Section) {
                view.sectionVertical = boolMember(item, "sectionVertical", at, path);
                view.sectionPosition = numberMember(item, "sectionPosition", at, path);
                view.sectionFlipped = boolMember(item, "sectionFlipped", at, path);
            }
            if (view.kind == ViewKind::Detail) {
                view.detailX = numberMember(item, "detailX", at, path);
                view.detailY = numberMember(item, "detailY", at, path);
                view.detailRadius = numberMember(item, "detailRadius", at, path);
            }
            drawing.views.push_back(std::move(view));
        }
    }
    if (const json* dimensions = optionalList(entry, "dimensions", path)) {
        for (const json& item : *dimensions) {
            DrawingDimension dimension;
            const std::string at = "a dimension of " + where;
            dimension.id = ObjectId{idMember(item, "id", at, path)};
            dimension.view = ObjectId{idMember(item, "view", at, path)};
            dimension.kind =
                kindMember<DimensionKind>(item, "kind", dimensionKindFromString, at, path);
            dimension.first = readDrawingPoint(item, "first", at, path);
            if (dimension.kind != DimensionKind::Radius &&
                dimension.kind != DimensionKind::Diameter) {
                dimension.second = readDrawingPoint(item, "second", at, path);
            }
            dimension.offset = numberMember(item, "offset", at, path);
            dimension.suffix = stringMember(item, "suffix", at, path);
            drawing.dimensions.push_back(std::move(dimension));
        }
    }
    if (const json* notes = optionalList(entry, "notes", path)) {
        for (const json& item : *notes) {
            DrawingNote note;
            const std::string at = "a note of " + where;
            note.id = ObjectId{idMember(item, "id", at, path)};
            note.text = stringMember(item, "text", at, path);
            note.x = numberMember(item, "x", at, path);
            note.y = numberMember(item, "y", at, path);
            note.height = numberMember(item, "height", at, path);
            drawing.notes.push_back(std::move(note));
        }
    }
    return drawing;
}

nlohmann::ordered_json writeTool(const Tool& tool) {
    return nlohmann::ordered_json{{"id", tool.id.value},
                                  {"name", tool.name},
                                  {"kind", toString(tool.kind)},
                                  {"diameter", tool.diameter},
                                  {"fluteLength", tool.fluteLength},
                                  {"stickout", tool.stickout},
                                  {"flutes", tool.flutes},
                                  {"tipAngle", tool.tipAngle},
                                  {"number", tool.number}};
}

Tool readTool(const json& entry, const std::filesystem::path& path) {
    Tool tool;
    tool.id = ObjectId{idMember(entry, "id", "a tool", path)};
    tool.name = stringMember(entry, "name", "a tool", path);
    const std::string where = "the tool " + tool.name;
    tool.kind = kindMember<ToolKind>(entry, "kind", toolKindFromString, where, path);
    tool.diameter = numberMember(entry, "diameter", where, path);
    tool.fluteLength = numberMember(entry, "fluteLength", where, path);
    tool.stickout = numberMember(entry, "stickout", where, path);
    tool.flutes = static_cast<int>(numberMember(entry, "flutes", where, path));
    tool.tipAngle = numberMember(entry, "tipAngle", where, path);
    tool.number = static_cast<int>(numberMember(entry, "number", where, path));
    return tool;
}

nlohmann::ordered_json writeOperation(const MachiningOperation& operation) {
    using nlohmann::ordered_json;
    ordered_json out;
    out["id"] = operation.id.value;
    out["name"] = operation.name;
    out["kind"] = toString(operation.kind);
    out["tool"] = operation.tool.value;
    out["targets"] = ordered_json::array();
    for (const OperationTarget& target : operation.targets) {
        ordered_json entry;
        entry["body"] = target.body.value;
        entry["path"] = ordered_json::array();
        for (const ObjectId id : target.path) {
            entry["path"].push_back(id.value);
        }
        entry["face"] = target.face;
        out["targets"].push_back(std::move(entry));
    }
    out["feed"] = operation.feed;
    out["plungeFeed"] = operation.plungeFeed;
    out["spindle"] = operation.spindle;
    out["stepover"] = operation.stepover;
    out["stepdown"] = operation.stepdown;
    out["clearance"] = operation.clearance;
    out["retract"] = operation.retract;
    out["stockToLeave"] = operation.stockToLeave;
    out["side"] = operation.side == ContourSide::Outside ? "outside" : "inside";
    out["peck"] = operation.peck;
    out["rampAngle"] = operation.rampAngle;
    out["tabs"] = operation.tabs;
    out["tabWidth"] = operation.tabWidth;
    out["tabHeight"] = operation.tabHeight;
    out["suppressed"] = operation.suppressed;
    return out;
}

MachiningOperation readOperation(const json& entry, const std::filesystem::path& path) {
    MachiningOperation operation;
    operation.id = ObjectId{idMember(entry, "id", "an operation", path)};
    operation.name = stringMember(entry, "name", "an operation", path);
    const std::string where = "the operation " + operation.name;
    operation.kind = kindMember<OperationKind>(entry, "kind", operationKindFromString, where, path);
    operation.tool = ObjectId{idMember(entry, "tool", where, path)};
    if (const json* targets = optionalList(entry, "targets", path)) {
        for (const json& item : *targets) {
            OperationTarget target;
            target.body = ObjectId{idMember(item, "body", where, path)};
            for (const std::uint64_t id : idsMember(item, "path", where, path)) {
                target.path.push_back(ObjectId{id});
            }
            target.face = stringMember(item, "face", where, path);
            operation.targets.push_back(std::move(target));
        }
    }
    operation.feed = numberMember(entry, "feed", where, path);
    operation.plungeFeed = numberMember(entry, "plungeFeed", where, path);
    operation.spindle = numberMember(entry, "spindle", where, path);
    operation.stepover = numberMember(entry, "stepover", where, path);
    operation.stepdown = numberMember(entry, "stepdown", where, path);
    operation.clearance = numberMember(entry, "clearance", where, path);
    operation.retract = numberMember(entry, "retract", where, path);
    operation.stockToLeave = numberMember(entry, "stockToLeave", where, path);
    const std::string side = stringMember(entry, "side", where, path);
    if (side == "outside") {
        operation.side = ContourSide::Outside;
    } else if (side == "inside") {
        operation.side = ContourSide::Inside;
    } else {
        damaged(path, where + " is on the unknown side \"" + side + "\"");
    }
    operation.peck = numberOr(entry, "peck", 0.0, where, path);
    // A file from before there were ramps goes straight down, as it did.
    operation.rampAngle = numberOr(entry, "rampAngle", 0.0, where, path);
    operation.tabs = static_cast<int>(numberOr(entry, "tabs", 0.0, where, path));
    operation.tabWidth = numberOr(entry, "tabWidth", operation.tabWidth, where, path);
    operation.tabHeight = numberOr(entry, "tabHeight", operation.tabHeight, where, path);
    operation.suppressed = boolMember(entry, "suppressed", where, path);
    return operation;
}

nlohmann::ordered_json writeSetup(const Setup& setup) {
    using nlohmann::ordered_json;
    ordered_json out;
    out["id"] = setup.id.value;
    out["name"] = setup.name;
    const Machine& machine = setup.machine;
    out["machine"] =
        ordered_json{{"name", machine.name},       {"dialect", toString(machine.dialect)},
                     {"travelX", machine.travelX}, {"travelY", machine.travelY},
                     {"travelZ", machine.travelZ}, {"rapidRate", machine.rapidRate},
                     {"maxFeed", machine.maxFeed}, {"maxSpindle", machine.maxSpindle},
                     {"arcs", machine.arcs}};
    out["stock"] = ordered_json{
        {"side", setup.stock.side}, {"top", setup.stock.top}, {"bottom", setup.stock.bottom}};
    out["origin"] = toString(setup.origin);
    out["operations"] = ordered_json::array();
    for (const MachiningOperation& operation : setup.operations) {
        out["operations"].push_back(writeOperation(operation));
    }
    return out;
}

Setup readSetup(const json& entry, const std::filesystem::path& path) {
    Setup setup;
    setup.id = ObjectId{idMember(entry, "id", "a setup", path)};
    setup.name = stringMember(entry, "name", "a setup", path);
    const std::string where = "the setup " + setup.name;
    const json& machine = objectMember(entry, "machine", where, path);
    const std::string at = "the machine of " + where;
    setup.machine.name = stringMember(machine, "name", at, path);
    setup.machine.dialect =
        kindMember<PostDialect>(machine, "dialect", postDialectFromString, at, path);
    setup.machine.travelX = numberMember(machine, "travelX", at, path);
    setup.machine.travelY = numberMember(machine, "travelY", at, path);
    setup.machine.travelZ = numberMember(machine, "travelZ", at, path);
    setup.machine.rapidRate = numberMember(machine, "rapidRate", at, path);
    setup.machine.maxFeed = numberMember(machine, "maxFeed", at, path);
    setup.machine.maxSpindle = numberMember(machine, "maxSpindle", at, path);
    // A file from before there were arcs writes lines, as it did.
    setup.machine.arcs = machine.contains("arcs") && boolMember(machine, "arcs", at, path);
    const json& stock = objectMember(entry, "stock", where, path);
    setup.stock.side = numberMember(stock, "side", where, path);
    setup.stock.top = numberMember(stock, "top", where, path);
    setup.stock.bottom = numberMember(stock, "bottom", where, path);
    setup.origin = kindMember<WorkOrigin>(entry, "origin", workOriginFromString, where, path);
    if (const json* operations = optionalList(entry, "operations", path)) {
        for (const json& item : *operations) {
            setup.operations.push_back(readOperation(item, path));
        }
    }
    return setup;
}

void readOutputs(const json& document, const std::filesystem::path& path, DocumentState& state) {
    if (const json* drawings = optionalList(document, "drawings", path)) {
        for (const json& entry : *drawings) {
            state.drawings.push_back(readDrawing(entry, path));
        }
    }
    if (const json* tools = optionalList(document, "tools", path)) {
        for (const json& entry : *tools) {
            state.tools.push_back(readTool(entry, path));
        }
    }
    if (const json* setups = optionalList(document, "setups", path)) {
        for (const json& entry : *setups) {
            state.setups.push_back(readSetup(entry, path));
        }
    }
}

void writeOutputs(const Document& document, nlohmann::ordered_json& model) {
    using nlohmann::ordered_json;
    if (!document.drawings().empty()) {
        model["drawings"] = ordered_json::array();
        for (const Drawing& drawing : document.drawings()) {
            model["drawings"].push_back(writeDrawing(drawing));
        }
    }
    if (!document.tools().empty()) {
        model["tools"] = ordered_json::array();
        for (const Tool& tool : document.tools()) {
            model["tools"].push_back(writeTool(tool));
        }
    }
    if (!document.setups().empty()) {
        model["setups"] = ordered_json::array();
        for (const Setup& setup : document.setups()) {
            model["setups"].push_back(writeSetup(setup));
        }
    }
}

DocumentState interpret(const ArchiveContents& contents, const std::filesystem::path& path,
                        std::vector<std::string>& warnings) {
    const json& units = objectMember(contents.manifest, "units", manifestName, path);
    const std::string lengthUnit = stringMember(units, "length", "the units", path);
    if (lengthUnit != "mm") {
        throw FileError(core::toUtf8(path) + " uses the length unit \"" + lengthUnit +
                        "\", which this version of LCAD does not support.");
    }

    DocumentState state;
    state.nextId = idMember(contents.document, "nextId", documentName, path);
    const json& bodies = member(contents.document, "bodies", documentName, path);
    if (!bodies.is_array()) {
        damaged(path, "\"bodies\" in document.json is not a list");
    }
    for (const json& entry : bodies) {
        if (!entry.is_object()) {
            damaged(path, "an entry of \"bodies\" is not an object");
        }
        Body body;
        body.id = ObjectId{idMember(entry, "id", "a body", path)};
        const std::string where = "body " + std::to_string(body.id.value);
        body.name = stringMember(entry, "name", where, path);
        body.visible = boolMember(entry, "visible", where, path);
        if (entry.contains("component")) {
            body.component = ObjectId{idMember(entry, "component", where, path)};
        }
        if (entry.contains("material")) {
            body.material = stringMember(entry, "material", where, path);
        }
        if (entry.contains("tag")) {
            body.tag = stringMember(entry, "tag", where, path);
        }
        if (entry.contains("piece")) {
            body.piece = static_cast<int>(idMember(entry, "piece", where, path));
        }
        if (entry.contains("discarded")) {
            body.discarded = boolMember(entry, "discarded", where, path);
        }
        if (entry.contains("madeBy")) {
            // A body that a feature makes has no geometry in the file: it is built again.
            body.madeBy = ObjectId{idMember(entry, "madeBy", where, path)};
            state.bodies.push_back(std::move(body));
            continue;
        }

        const json& geometry = objectMember(entry, "geometry", where, path);
        const std::string role = stringMember(geometry, "role", where, path);
        if (role == "linked") {
            // A copy of a body of a linked document, kept for when that cannot be found.
            body.linkKey = stringMember(entry, "linkKey", where, path);
            body.sourceFaceNames = stringsMember(entry, "faceNames", where, path);
        } else if (role != "source") {
            damaged(path, where + " has geometry of the unknown kind \"" + role + "\"");
        }
        const std::string file = stringMember(geometry, "file", where, path);
        const auto bytes = contents.entries.find(file);
        if (bytes == contents.entries.end()) {
            damaged(path, "the geometry of \"" + body.name + "\" (" + file + ") is missing");
        }
        try {
            body.shape = kernel::fromBrep(bytes->second);
        } catch (const kernel::Error& error) {
            damaged(path, "the geometry of \"" + body.name + "\" cannot be read (" +
                              error.reason() + ")");
        }
        if (!kernel::isValid(body.shape)) {
            warnings.push_back("The geometry of \"" + body.name +
                               "\" does not pass the validity check. It was loaded as it is.");
        }
        state.bodies.push_back(std::move(body));
    }

    // Sketches were added to the format before its first release; a file without the list
    // has none.
    const auto sketches = contents.document.find("sketches");
    if (sketches != contents.document.end()) {
        if (!sketches->is_array()) {
            damaged(path, "\"sketches\" in document.json is not a list");
        }
        for (const json& entry : *sketches) {
            state.sketches.push_back(readSketch(entry, path));
        }
    }

    // So were features, and the order things are applied in.
    const auto features = contents.document.find("features");
    if (features != contents.document.end()) {
        if (!features->is_array()) {
            damaged(path, "\"features\" in document.json is not a list");
        }
        for (const json& entry : *features) {
            state.features.push_back(readFeature(entry, path));
        }
    }
    // And parameters.
    const auto parameters = contents.document.find("parameters");
    if (parameters != contents.document.end()) {
        if (!parameters->is_array()) {
            damaged(path, "\"parameters\" in document.json is not a list");
        }
        for (const json& entry : *parameters) {
            if (!entry.is_object()) {
                damaged(path, "an entry of \"parameters\" is not an object");
            }
            Parameter parameter;
            parameter.id = ObjectId{idMember(entry, "id", "a parameter", path)};
            const std::string where = "parameter " + std::to_string(parameter.id.value);
            parameter.name = stringMember(entry, "name", where, path);
            parameter.expression = stringMember(entry, "expression", where, path);
            parameter.comment = stringMember(entry, "comment", where, path);
            state.parameters.push_back(std::move(parameter));
        }
    }
    if (contents.document.contains("timeline")) {
        for (const std::uint64_t id :
             idsMember(contents.document, "timeline", documentName, path)) {
            state.timeline.push_back(ObjectId{id});
        }
    }
    if (contents.document.contains("marker")) {
        state.marker =
            static_cast<std::size_t>(idMember(contents.document, "marker", documentName, path));
    }
    // And, in Phase 5, components, occurrences, joints, and materials.
    readAssembly(contents.document, path, state);
    readOutputs(contents.document, path, state);
    return state;
}

} // namespace

void saveDocument(const Document& document, const std::filesystem::path& path,
                  const SaveOptions& options) {
    // Ordered, so that the files read top to bottom the way the specification lists them.
    using nlohmann::ordered_json;
    ordered_json manifest;
    manifest["format"] = formatName;
    manifest["formatVersion"] = currentFormatVersion;
    manifest["writtenBy"] = ordered_json{{"application", "LCAD"},
                                         {"version", core::version()},
                                         {"kernel", "OCCT " + kernel::occtVersion()}};
    manifest["units"] = ordered_json{{"length", "mm"}};

    ordered_json model;
    model["nextId"] = document.nextObjectId();
    model["bodies"] = ordered_json::array();

    std::vector<std::pair<std::string, std::string>> geometry;
    for (const Body& body : document.bodies()) {
        ordered_json entry;
        entry["id"] = body.id.value;
        entry["name"] = body.name;
        entry["visible"] = body.visible;
        if (body.component.isValid()) {
            entry["component"] = body.component.value;
        }
        if (!body.material.empty()) {
            entry["material"] = body.material;
        }
        if (!body.tag.empty()) {
            entry["tag"] = body.tag;
        }
        if (body.piece > 0) {
            entry["piece"] = body.piece;
        }
        if (body.discarded) {
            entry["discarded"] = true;
        }
        if (body.madeBy.isValid()) {
            // What features make is not stored. The features are, and it is built from them.
            entry["madeBy"] = body.madeBy.value;
            model["bodies"].push_back(std::move(entry));
            continue;
        }
        // The shape the body was given, not what features have since made of it.
        const std::string file = "geometry/" + std::to_string(body.id.value) + ".brep";
        try {
            geometry.emplace_back(file, kernel::toBrep(body.source));
        } catch (const kernel::Error& error) {
            throw FileError("Could not save " + core::toUtf8(path) + ": the geometry of \"" +
                            body.name + "\" cannot be written (" + error.reason() + ").");
        }
        if (!body.linkKey.empty()) {
            entry["geometry"] = ordered_json{{"file", file}, {"role", "linked"}};
            entry["linkKey"] = body.linkKey;
            entry["faceNames"] = body.sourceFaceNames;
        } else {
            entry["geometry"] = ordered_json{{"file", file}, {"role", "source"}};
        }
        model["bodies"].push_back(std::move(entry));
    }

    model["sketches"] = ordered_json::array();
    for (const SketchItem& item : document.sketches()) {
        model["sketches"].push_back(writeSketch(item));
    }
    model["features"] = ordered_json::array();
    for (const Feature& feature : document.features()) {
        model["features"].push_back(writeFeature(feature));
    }
    if (!document.parameters().empty()) {
        model["parameters"] = ordered_json::array();
        for (const Parameter& parameter : document.parameters()) {
            model["parameters"].push_back(ordered_json{{"id", parameter.id.value},
                                                       {"name", parameter.name},
                                                       {"expression", parameter.expression},
                                                       {"comment", parameter.comment}});
        }
    }
    model["timeline"] = ordered_json::array();
    for (const ObjectId item : document.timeline()) {
        model["timeline"].push_back(item.value);
    }
    if (document.historyMarker() < document.timeline().size()) {
        model["marker"] = document.historyMarker();
    }
    writeAssembly(document, model, options);
    writeOutputs(document, model);

    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back(manifestName, manifest.dump(2) + "\n");
    entries.emplace_back(documentName, model.dump(2) + "\n");
    for (auto& entry : geometry) {
        entries.push_back(std::move(entry));
    }
    if (!options.thumbnailPng.empty()) {
        entries.emplace_back(thumbnailName, options.thumbnailPng);
    }
    writeAtomically(path, entries);
}

LoadedDocument loadDocument(const std::filesystem::path& path) {
    ArchiveContents contents;
    contents.entries = detail::readArchive(path);
    contents.manifest = parse(contents.entries, manifestName, path);

    const auto format = contents.manifest.find("format");
    if (format == contents.manifest.end() || *format != formatName) {
        throw FileError(core::toUtf8(path) + " is not an LCAD document.");
    }
    const json& versionValue = member(contents.manifest, "formatVersion", manifestName, path);
    if (!versionValue.is_number_integer()) {
        damaged(path, "its format version is not a whole number");
    }
    const auto version = versionValue.get<std::int64_t>();
    const std::string writer = describeWriter(contents.manifest);
    if (version > currentFormatVersion) {
        throw FileError(core::toUtf8(path) + " was saved by " + writer + " in format version " +
                        std::to_string(version) + ". This is LCAD " + core::version() +
                        ", which reads up to version " + std::to_string(currentFormatVersion) +
                        ". Open the file with " + writer + " or newer.");
    }
    if (version < 1) {
        damaged(path, "its format version is " + std::to_string(version));
    }

    contents.document = parse(contents.entries, documentName, path);
    detail::migrate(contents, static_cast<int>(version), currentFormatVersion,
                    detail::migrations());

    LoadedDocument loaded;
    loaded.formatVersion = static_cast<int>(version);
    loaded.writtenBy = writer;
    DocumentState state = interpret(contents, path, loaded.warnings);
    try {
        loaded.document = std::make_unique<Document>(std::move(state));
    } catch (const std::invalid_argument& error) {
        damaged(path, error.what());
    }
    // The bodies have been built from the features. One that could not be is reported, and
    // stays in the timeline to be put right.
    for (const Parameter& parameter : loaded.document->parameters()) {
        const std::string why = loaded.document->parameterError(parameter.id);
        if (!why.empty()) {
            loaded.warnings.push_back("The parameter " + parameter.name +
                                      " cannot be worked out. " + why);
        }
    }
    for (const Feature& feature : loaded.document->features()) {
        const std::string why = loaded.document->featureError(feature.id);
        if (!why.empty()) {
            loaded.warnings.push_back(feature.name + " could not be applied. " + why);
        }
    }
    for (const std::string& warning : loaded.warnings) {
        core::log(core::LogLevel::Warning, "document", core::toUtf8(path) + ": " + warning);
    }
    return loaded;
}

std::vector<Component> readLinkedComponents(const std::filesystem::path& path) {
    const std::optional<std::string> text = detail::readArchiveEntry(path, documentName);
    if (!text.has_value()) {
        throw FileError(core::toUtf8(path) + " is not an LCAD document (it has no " + documentName +
                        ").");
    }
    const json document = json::parse(*text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) {
        damaged(path, std::string(documentName) + " is not valid JSON");
    }
    DocumentState state;
    readAssembly(document, path, state);
    std::vector<Component> linked;
    for (Component& component : state.components) {
        if (!component.link.empty()) {
            linked.push_back(std::move(component));
        }
    }
    return linked;
}

std::string readThumbnail(const std::filesystem::path& path) {
    return detail::readArchiveEntry(path, thumbnailName).value_or(std::string());
}

} // namespace lcad::document
