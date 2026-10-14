// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/File.h>

#include "Archive.h"
#include "Migration.h"

#include <lcad/core/Log.h>
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
        throw FileError("Could not flush " + path.string() + " to disk.");
    }
    const int result = ::fsync(fd);
    ::close(fd);
    // Some file systems cannot flush a directory; that is not a failed save.
    if (result != 0 && !isDirectory) {
        throw FileError("Could not flush " + path.string() + " to disk.");
    }
#else
    // The Windows port needs FlushFileBuffers here.
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
        throw FileError("The folder " + directory.string() + " does not exist.");
    }

    // In the same folder as the target, so that the rename below stays within one file system.
    const std::filesystem::path temporary =
        directory / ("." + target.filename().string() + "." + randomSuffix() + ".tmp");
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
                throw FileError(target.string() + " is a folder. Choose a file name inside it.");
            }
            throw FileError("Could not save " + target.string() + ": " + reason + ".");
        }
    } catch (...) {
        std::filesystem::remove(temporary, ec);
        throw;
    }
    flushToDisk(directory, true);
}

// Reading document.json: every wrong or missing value is reported, never guessed.

[[noreturn]] void damaged(const std::filesystem::path& path, const std::string& problem) {
    throw FileError(path.string() + " is damaged: " + problem + ".");
}

json parse(const detail::Entries& entries, const char* name, const std::filesystem::path& path) {
    const auto entry = entries.find(name);
    if (entry == entries.end()) {
        throw FileError(path.string() + " is not an LCAD document (it has no " + name + ").");
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
    // A revolve about an origin axis or a construction axis.
    if (feature.kind == FeatureKind::Revolve && !feature.reference.empty()) {
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
    if (feature.kind == FeatureKind::Revolve && entry.contains("reference")) {
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

DocumentState interpret(const ArchiveContents& contents, const std::filesystem::path& path,
                        std::vector<std::string>& warnings) {
    const json& units = objectMember(contents.manifest, "units", manifestName, path);
    const std::string lengthUnit = stringMember(units, "length", "the units", path);
    if (lengthUnit != "mm") {
        throw FileError(path.string() + " uses the length unit \"" + lengthUnit +
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
        if (entry.contains("madeBy")) {
            // A body that a feature makes has no geometry in the file: it is built again.
            body.madeBy = ObjectId{idMember(entry, "madeBy", where, path)};
            state.bodies.push_back(std::move(body));
            continue;
        }

        const json& geometry = objectMember(entry, "geometry", where, path);
        const std::string role = stringMember(geometry, "role", where, path);
        if (role != "source") {
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
            throw FileError("Could not save " + path.string() + ": the geometry of \"" + body.name +
                            "\" cannot be written (" + error.reason() + ").");
        }
        entry["geometry"] = ordered_json{{"file", file}, {"role", "source"}};
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
        throw FileError(path.string() + " is not an LCAD document.");
    }
    const json& versionValue = member(contents.manifest, "formatVersion", manifestName, path);
    if (!versionValue.is_number_integer()) {
        damaged(path, "its format version is not a whole number");
    }
    const auto version = versionValue.get<std::int64_t>();
    const std::string writer = describeWriter(contents.manifest);
    if (version > currentFormatVersion) {
        throw FileError(path.string() + " was saved by " + writer + " in format version " +
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
        core::log(core::LogLevel::Warning, "document", path.string() + ": " + warning);
    }
    return loaded;
}

std::string readThumbnail(const std::filesystem::path& path) {
    return detail::readArchiveEntry(path, thumbnailName).value_or(std::string());
}

} // namespace lcad::document
