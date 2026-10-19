// SPDX-License-Identifier: GPL-3.0-or-later

#include "Rebuild.h"

#include <lcad/document/SketchShapes.h>
#include <lcad/kernel/Error.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/SubShapes.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

namespace lcad::document::detail {

namespace {

const double pi = 3.14159265358979323846;

std::string capitalized(std::string text) {
    if (!text.empty() && text[0] >= 'a' && text[0] <= 'z') {
        text[0] = static_cast<char>(text[0] - 'a' + 'A');
    }
    return text;
}

// A kernel failure as a sentence about a feature.
FeatureFailure failed(const kernel::Error& error) {
    return FeatureFailure(capitalized(error.reason()) + ".");
}

// The face a name such as "7/side/12" belongs to, in words.
std::string describeFace(const Document& document, const std::string& name) {
    const std::size_t slash = name.find('/');
    std::string owner = "something that is gone";
    try {
        const ObjectId id{std::stoull(name.substr(0, slash))};
        if (const Feature* feature = document.findFeature(id)) {
            owner = feature->name;
        } else if (const Body* body = document.findBody(id)) {
            owner = body->name;
        }
    } catch (const std::exception&) {
        return "a face";
    }
    const std::string rest = slash == std::string::npos ? std::string() : name.substr(slash + 1);
    const auto begins = [&rest](const char* with) { return rest.rfind(with, 0) == 0; };
    // A copy made by a mirror or a pattern: "12/1.3/" and the name of the face copied.
    if (const std::size_t next = rest.find('/');
        next != std::string::npos && next > 0 && rest.find_first_not_of("0123456789.") == next) {
        return "a copy, by " + owner + ", of " + describeFace(document, rest.substr(next + 1));
    }
    if (begins("side/")) {
        return "a side of " + owner;
    }
    if (begins("start/")) {
        return "the start of " + owner;
    }
    if (begins("end/")) {
        return "the end of " + owner;
    }
    if (begins("edge/") || begins("corner/")) {
        return "a face " + owner + " made";
    }
    if (begins("inner/")) {
        return "the inside of " + owner;
    }
    return "a face of " + owner;
}

// The regions of its sketch that a pull or a revolve uses, as the kernel takes them.
std::vector<kernel::Region> regionsOf(const Feature& feature, const SketchItem& sketch) {
    const std::vector<sketch::Region> all = sketch.geometry.regions();
    if (all.empty()) {
        throw FeatureFailure("Its sketch, " + sketch.name + ", has no closed outline.");
    }
    std::vector<const sketch::Region*> chosen;
    if (feature.regions.empty()) {
        for (const sketch::Region& region : all) {
            chosen.push_back(&region);
        }
    }
    for (const std::vector<sketch::EntityId>& wanted : feature.regions) {
        const auto found = std::find_if(all.begin(), all.end(), [&wanted](const sketch::Region& r) {
            std::vector<sketch::EntityId> outline = r.outer;
            std::sort(outline.begin(), outline.end());
            return outline == wanted;
        });
        if (found == all.end()) {
            throw FeatureFailure("An outline it was made from is no longer in " + sketch.name +
                                 ": its curves were changed or removed, or it is no longer "
                                 "closed.");
        }
        chosen.push_back(&*found);
    }
    std::vector<kernel::Region> regions;
    for (const sketch::Region* region : chosen) {
        try {
            regions.push_back(kernelRegion(sketch, *region));
        } catch (const std::invalid_argument&) {
            throw FeatureFailure("A curve of its outline in " + sketch.name +
                                 " has shrunk to nothing.");
        }
    }
    return regions;
}

const kernel::NamedShape& targetOf(const Feature& feature, const Document& document,
                                   const BodyStates& bodies) {
    const auto found = bodies.find(feature.body);
    if (found == bodies.end() || found->second.shape.isNull()) {
        const Body* body = document.findBody(feature.body);
        throw FeatureFailure(body != nullptr ? "The body it works on, " + body->name +
                                                   ", does not exist at this point in the timeline."
                                             : std::string("The body it works on is gone."));
    }
    return found->second;
}

kernel::Axis axisNamed(const std::string& name, const kernel::NamedShape& body,
                       const Document& document);

// The solid a pull or a revolve makes, before it is combined with a body. `bodies` is needed
// for a pull up to a face.
kernel::NamedShape madeBy(const Feature& feature, const Document& document,
                          const BodyStates* bodies) {
    const std::string tag = std::to_string(feature.id.value);
    const SketchItem* sketch = document.findSketch(feature.sketch);
    if (sketch == nullptr) {
        throw FeatureFailure("The sketch it was made from is gone.");
    }
    const std::vector<kernel::Region> regions = regionsOf(feature, *sketch);
    if (feature.kind == FeatureKind::Pull && !feature.extentFace.empty()) {
        const Body* target = document.findBody(feature.extentBody);
        const bool exists = target != nullptr && bodies != nullptr &&
                            bodies->find(feature.extentBody) != bodies->end() &&
                            !bodies->at(feature.extentBody).shape.isNull();
        if (!exists) {
            throw FeatureFailure(
                target != nullptr ? "The body with the face it goes up to, " + target->name +
                                        ", does not exist at this point in the timeline."
                                  : std::string("The body with the face it goes up to is gone."));
        }
        const kernel::NamedShape& limitBody = bodies->at(feature.extentBody);
        const int index = kernel::findFace(limitBody, feature.extentFace);
        if (index < 0) {
            throw FeatureFailure("The face it goes up to no longer exists: " +
                                 describeName(document, feature.extentFace) + ".");
        }
        const auto limit = kernel::planeOfFace(
            kernel::subShape(limitBody.shape, kernel::SubShapeKind::Face, index));
        if (!limit.has_value()) {
            throw FeatureFailure("The face it goes up to, " +
                                 describeName(document, feature.extentFace) +
                                 ", is not flat. Only flat faces can end a pull for now.");
        }
        return kernel::pullTo(sketch->plane, regions, *limit, tag);
    }
    if (feature.kind == FeatureKind::Pull) {
        double from = 0.0;
        double to = feature.distance;
        if (feature.symmetric) {
            from = -feature.distance / 2.0;
            to = feature.distance / 2.0;
        } else if (feature.distanceBack != 0.0) {
            from = -feature.distanceBack;
        }
        if (!std::isfinite(from) || !std::isfinite(to) || std::abs(to - from) < 1e-7) {
            throw FeatureFailure("Its distance is zero.");
        }
        if (feature.taper != 0.0 && from != 0.0) {
            throw FeatureFailure("A taper needs a pull to one side.");
        }
        if (!std::isfinite(feature.taper) || std::abs(feature.taper) >= 89.0) {
            throw FeatureFailure("Its taper has to be less than 89 degrees either way.");
        }
        return kernel::pull(sketch->plane, regions, from, to, tag, feature.taper * pi / 180.0);
    }
    if (!std::isfinite(feature.angle) || std::abs(feature.angle) < 1e-9 ||
        std::abs(feature.angle) > 360.0 + 1e-9) {
        throw FeatureFailure("Its angle has to be more than zero and at most 360 degrees.");
    }
    if (!feature.reference.empty()) {
        // About an origin axis or a construction axis.
        const kernel::Axis about = axisNamed(feature.reference, kernel::NamedShape{}, document);
        return kernel::revolve(sketch->plane, regions, about, feature.angle * pi / 180.0, tag);
    }
    const sketch::Entity* axis = sketch->geometry.find(feature.axis);
    if (axis == nullptr || axis->kind != sketch::EntityKind::Line) {
        throw FeatureFailure("The line it turns about is no longer in " + sketch->name + ".");
    }
    const sketch::Point2 a = sketch->geometry.position(axis->start);
    const sketch::Point2 b = sketch->geometry.position(axis->end);
    return kernel::revolve(sketch->plane, regions, a.x, a.y, b.x, b.y, feature.angle * pi / 180.0,
                           tag);
}

// The plane a mirror is in: an origin plane, or a flat face of the body.
kernel::Plane planeNamed(const std::string& name, const kernel::NamedShape& body,
                         const Document& document) {
    if (name == "XY") {
        return kernel::planeXY();
    }
    if (name == "XZ") {
        return kernel::planeXZ();
    }
    if (name == "YZ") {
        return kernel::planeYZ();
    }
    if (name.empty()) {
        throw FeatureFailure("It has no plane to mirror in.");
    }
    if (name.rfind("plane:", 0) == 0) {
        const ObjectId id{std::stoull(name.substr(6))};
        const Feature* plane = document.findFeature(id);
        const auto state = document.planeState(id);
        if (plane == nullptr || !state.has_value()) {
            throw FeatureFailure(plane != nullptr ? "The plane " + plane->name +
                                                        " does not exist at this point in the "
                                                        "timeline."
                                                  : std::string("The plane it uses is gone."));
        }
        return state->plane;
    }
    const int face = kernel::findFace(body, name);
    if (face < 0) {
        throw FeatureFailure(
            "The face it mirrors in no longer exists: " + describeName(document, name) + ".");
    }
    const auto plane =
        kernel::planeOfFace(kernel::subShape(body.shape, kernel::SubShapeKind::Face, face));
    if (!plane.has_value()) {
        throw FeatureFailure("The face it mirrors in, " + describeName(document, name) +
                             ", is not flat.");
    }
    return *plane;
}

// The line a pattern goes along or around: an origin axis, or what an edge or a face of the
// body stands for.
kernel::Axis axisNamed(const std::string& name, const kernel::NamedShape& body,
                       const Document& document) {
    if (name.rfind("axis:", 0) == 0) {
        // A construction axis, by its id.
        const ObjectId id{std::stoull(name.substr(5))};
        const Feature* axis = document.findFeature(id);
        const auto state = document.axisState(id);
        if (axis == nullptr || !state.has_value()) {
            throw FeatureFailure(axis != nullptr ? "The axis " + axis->name +
                                                       " does not exist at this point in the "
                                                       "timeline."
                                                 : std::string("The axis it uses is gone."));
        }
        return state->axis;
    }
    if (name == "X") {
        return {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}};
    }
    if (name == "Y") {
        return {{0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}};
    }
    if (name == "Z") {
        return {{0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}};
    }
    if (name.empty()) {
        throw FeatureFailure("It has no direction or axis.");
    }
    kernel::Shape part;
    if (const int edge = kernel::findEdge(body, name); edge >= 0) {
        part = kernel::subShape(body.shape, kernel::SubShapeKind::Edge, edge);
    } else if (const int face = kernel::findFace(body, name); face >= 0) {
        part = kernel::subShape(body.shape, kernel::SubShapeKind::Face, face);
    } else {
        throw FeatureFailure("The edge or face that gives its direction no longer exists: " +
                             describeName(document, name) + ".");
    }
    const auto axis = kernel::axisOf(part);
    if (!axis.has_value()) {
        throw FeatureFailure(describeName(document, name) + " gives no direction or axis.");
    }
    return *axis;
}

// Mirror and patterns: copies of features' solids, or of the body, each doing to the body what
// its original does.
kernel::NamedShape copiesOf(const Feature& feature, const Document& document,
                            const kernel::NamedShape& body, const BodyStates& bodies) {
    const std::string tag = std::to_string(feature.id.value);
    struct Piece {
        kernel::NamedShape shape;
        kernel::Combination how = kernel::Combination::Join;
    };
    std::vector<Piece> pieces;
    if (feature.sources.empty()) {
        pieces.push_back({body, kernel::Combination::Join});
    }
    for (const ObjectId id : feature.sources) {
        const Feature* source = document.findFeature(id);
        if (source == nullptr) {
            throw FeatureFailure("A feature it copies is gone.");
        }
        if (source->kind != FeatureKind::Pull && source->kind != FeatureKind::Revolve) {
            throw FeatureFailure("It copies only pulls and revolves, and " + source->name +
                                 " is neither.");
        }
        if (source->body != feature.body) {
            throw FeatureFailure(source->name + ", which it copies, works on another body.");
        }
        if (source->suppressed) {
            throw FeatureFailure(source->name + ", which it copies, is suppressed.");
        }
        Piece piece;
        try {
            piece.shape = madeBy(*source, document, &bodies);
        } catch (const FeatureFailure& failure) {
            throw FeatureFailure(source->name + ", which it copies, cannot be made. " +
                                 failure.what());
        }
        piece.how = source->operation == Operation::Cut         ? kernel::Combination::Cut
                    : source->operation == Operation::Intersect ? kernel::Combination::Intersect
                                                                : kernel::Combination::Join;
        pieces.push_back(std::move(piece));
    }

    // Where the copies go, as what makes each one from its original.
    using Copier = std::function<kernel::NamedShape(const kernel::NamedShape&, const std::string&)>;
    std::vector<Copier> copiers;
    switch (feature.kind) {
    case FeatureKind::Mirror: {
        const kernel::Plane plane = planeNamed(feature.reference, body, document);
        copiers.emplace_back([plane](const kernel::NamedShape& shape, const std::string& prefix) {
            return kernel::mirrored(shape, plane, prefix);
        });
        break;
    }
    case FeatureKind::RectangularPattern: {
        const int rows = feature.reference2.empty() ? 1 : feature.count2;
        if (feature.count < 1 || rows < 1) {
            throw FeatureFailure("It needs at least one in each direction.");
        }
        if (feature.count * rows < 2) {
            throw FeatureFailure("With one in each direction, there is nothing to repeat.");
        }
        if (feature.count * rows > 1000) {
            throw FeatureFailure("It would make more than 1000 copies.");
        }
        if (!std::isfinite(feature.spacing) || !std::isfinite(feature.spacing2)) {
            throw FeatureFailure("Its spacing is not a number.");
        }
        const kernel::Vector first = axisNamed(feature.reference, body, document).direction;
        const kernel::Vector second = feature.reference2.empty()
                                          ? kernel::Vector{}
                                          : axisNamed(feature.reference2, body, document).direction;
        for (int i = 0; i < feature.count; ++i) {
            for (int j = 0; j < rows; ++j) {
                if (i == 0 && j == 0) {
                    continue;
                }
                const double a = feature.spacing * i;
                const double b = feature.spacing2 * j;
                const kernel::Vector by{first.x * a + second.x * b, first.y * a + second.y * b,
                                        first.z * a + second.z * b};
                copiers.emplace_back(
                    [by](const kernel::NamedShape& shape, const std::string& prefix) {
                        return kernel::moved(shape, by, prefix);
                    });
            }
        }
        break;
    }
    case FeatureKind::CircularPattern: {
        if (feature.count < 2) {
            throw FeatureFailure("With only one, there is nothing to repeat.");
        }
        if (feature.count > 1000) {
            throw FeatureFailure("It would make more than 1000 copies.");
        }
        if (!std::isfinite(feature.angle) || std::abs(feature.angle) < 1e-9 ||
            std::abs(feature.angle) > 360.0 + 1e-9) {
            throw FeatureFailure("Its angle has to be more than zero and at most 360 degrees.");
        }
        const kernel::Axis axis = axisNamed(feature.reference, body, document);
        // All the way round, the last one would land on the first.
        const bool round = std::abs(std::abs(feature.angle) - 360.0) < 1e-9;
        const double step =
            feature.angle / (round ? feature.count : feature.count - 1) * pi / 180.0;
        for (int k = 1; k < feature.count; ++k) {
            copiers.emplace_back([axis, angle = step * k](const kernel::NamedShape& shape,
                                                          const std::string& prefix) {
                return kernel::turned(shape, axis, angle, prefix);
            });
        }
        break;
    }
    default:
        break;
    }

    kernel::NamedShape result = body;
    for (std::size_t p = 0; p < pieces.size(); ++p) {
        for (std::size_t c = 0; c < copiers.size(); ++c) {
            const std::string prefix =
                tag + "/" + std::to_string(p + 1) + "." + std::to_string(c + 1);
            const kernel::NamedShape copy = copiers[c](pieces[p].shape, prefix);
            if (pieces[p].how == kernel::Combination::Cut) {
                // A copy that falls beside the body takes nothing away, which is no failure:
                // a pattern may run off the edge.
                try {
                    result = kernel::combine(result, copy, kernel::Combination::Cut);
                } catch (const kernel::Error&) {
                    throw FeatureFailure("A copy cuts away all that is left of the body.");
                }
            } else {
                result = kernel::combine(result, copy, pieces[p].how);
            }
        }
    }
    return result;
}

// Holes drilled into the body at the free-standing points of the feature's sketch.
kernel::NamedShape drilled(const Feature& feature, const Document& document,
                           const kernel::NamedShape& body) {
    const std::string tag = std::to_string(feature.id.value);
    const SketchItem* sketch = document.findSketch(feature.sketch);
    if (sketch == nullptr) {
        throw FeatureFailure("The sketch that says where the holes go is gone.");
    }
    std::vector<const sketch::Entity*> points;
    for (const sketch::Entity& entity : sketch->geometry.entities()) {
        if (entity.kind == sketch::EntityKind::Point && entity.owner == 0 &&
            entity.id != sketch->geometry.origin() && !entity.construction) {
            points.push_back(&entity);
        }
    }
    if (points.empty()) {
        throw FeatureFailure("Its sketch, " + sketch->name +
                             ", has no points to drill at. Draw a point where each hole goes.");
    }
    kernel::HoleShape shape;
    shape.kind = feature.holeKind == HoleKind::Counterbore   ? kernel::HoleKind::Counterbore
                 : feature.holeKind == HoleKind::Countersink ? kernel::HoleKind::Countersink
                                                             : kernel::HoleKind::Simple;
    shape.diameter = feature.size;
    shape.depth = feature.distance;
    shape.tipAngle = feature.tipAngle;
    shape.headDiameter = feature.headDiameter;
    shape.headDepth = feature.headDepth;
    shape.countersinkAngle = feature.countersinkAngle;
    const kernel::Vector& n = sketch->plane.normal;
    const double way = feature.flipped ? 1.0 : -1.0;
    const kernel::Vector direction{n.x * way, n.y * way, n.z * way};
    double through = 0.0;
    if (feature.throughAll) {
        // Far enough to leave the body wherever it starts: across the box around the body and
        // the sketch's plane.
        const auto box = kernel::boundingBox(body.shape);
        if (box.has_value()) {
            double reach = 0.0;
            for (const sketch::Entity* point : points) {
                const kernel::Point at = kernel::toWorld(sketch->plane, point->x, point->y);
                for (const double x : {box->min.x, box->max.x}) {
                    for (const double y : {box->min.y, box->max.y}) {
                        for (const double z : {box->min.z, box->max.z}) {
                            reach = std::max(reach, std::hypot(x - at.x, y - at.y, z - at.z));
                        }
                    }
                }
            }
            through = reach + 1.0;
        }
    }
    kernel::NamedShape result = body;
    const double before = kernel::volume(body.shape);
    for (const sketch::Entity* point : points) {
        kernel::NamedShape tool;
        try {
            tool = kernel::holeTool(kernel::toWorld(sketch->plane, point->x, point->y), direction,
                                    shape, through, tag, std::to_string(point->id));
        } catch (const std::invalid_argument& error) {
            throw FeatureFailure(capitalized(error.what()) + ".");
        }
        try {
            result = kernel::combine(result, tool, kernel::Combination::Cut);
        } catch (const kernel::Error&) {
            throw FeatureFailure("The holes take away all that is left of the body.");
        }
    }
    if (std::abs(before - kernel::volume(result.shape)) <= 1e-9 * std::max(before, 1.0)) {
        const Body* target = document.findBody(feature.body);
        throw FeatureFailure("The holes do not reach " +
                             (target != nullptr ? target->name : std::string("the body")) +
                             ". Flip them if they go the wrong way.");
    }
    return result;
}

kernel::NamedShape placed(const Feature& feature, const Document& document,
                          const BodyStates& bodies, const kernel::NamedShape& made) {
    switch (feature.operation) {
    case Operation::NewBody:
        return made;
    case Operation::Join:
        return kernel::combine(targetOf(feature, document, bodies), made,
                               kernel::Combination::Join);
    case Operation::Cut: {
        const kernel::NamedShape& target = targetOf(feature, document, bodies);
        kernel::NamedShape cut = kernel::combine(target, made, kernel::Combination::Cut);
        // A cut that takes nothing away is a mistake worth telling: the tool misses the body.
        const double before = kernel::volume(target.shape);
        if (std::abs(before - kernel::volume(cut.shape)) <= 1e-9 * std::max(before, 1.0)) {
            const Body* body = document.findBody(feature.body);
            throw FeatureFailure("It does not reach " +
                                 (body != nullptr ? body->name : std::string("the body")) +
                                 ", so there is nothing to cut away.");
        }
        return cut;
    }
    case Operation::Intersect:
        return kernel::combine(targetOf(feature, document, bodies), made,
                               kernel::Combination::Intersect);
    }
    return made;
}

} // namespace

AxisState placeAxis(const Feature& feature, const Document& document, const BodyStates& bodies) {
    AxisState state;
    std::optional<kernel::BoundingBox> extent;
    const auto widen = [&extent](const std::optional<kernel::BoundingBox>& box) {
        if (!box.has_value()) {
            return;
        }
        if (!extent.has_value()) {
            extent = box;
            return;
        }
        extent->min = {std::min(extent->min.x, box->min.x), std::min(extent->min.y, box->min.y),
                       std::min(extent->min.z, box->min.z)};
        extent->max = {std::max(extent->max.x, box->max.x), std::max(extent->max.y, box->max.y),
                       std::max(extent->max.z, box->max.z)};
    };
    if (feature.reference == "X" || feature.reference == "Y" || feature.reference == "Z") {
        state.axis = axisNamed(feature.reference, kernel::NamedShape{}, document);
        // Drawn across the model.
        for (const auto& entry : bodies) {
            widen(kernel::boundingBox(entry.second.shape));
        }
    } else {
        const auto found = bodies.find(feature.body);
        const Body* body = document.findBody(feature.body);
        if (found == bodies.end() || found->second.shape.isNull()) {
            throw FeatureFailure(body != nullptr
                                     ? "The body it is placed on, " + body->name +
                                           ", does not exist at this point in the timeline."
                                     : std::string("The body it is placed on is gone."));
        }
        kernel::Shape part;
        if (const int edge = kernel::findEdge(found->second, feature.reference); edge >= 0) {
            part = kernel::subShape(found->second.shape, kernel::SubShapeKind::Edge, edge);
        } else if (const int face = kernel::findFace(found->second, feature.reference); face >= 0) {
            part = kernel::subShape(found->second.shape, kernel::SubShapeKind::Face, face);
        } else {
            throw FeatureFailure("The edge or face it is placed along no longer exists: " +
                                 describeName(document, feature.reference) + ".");
        }
        const auto axis = kernel::axisOf(part);
        if (!axis.has_value()) {
            throw FeatureFailure(describeName(document, feature.reference) +
                                 " gives no axis: choose a straight edge, a round edge, or a "
                                 "round face.");
        }
        state.axis = *axis;
        widen(kernel::boundingBox(part));
    }
    const kernel::Point& o = state.axis.origin;
    const kernel::Vector& d = state.axis.direction;
    kernel::Point middle = o;
    if (extent.has_value()) {
        middle = {(extent->min.x + extent->max.x) / 2.0, (extent->min.y + extent->max.y) / 2.0,
                  (extent->min.z + extent->max.z) / 2.0};
        const double dx = extent->max.x - extent->min.x;
        const double dy = extent->max.y - extent->min.y;
        const double dz = extent->max.z - extent->min.z;
        state.length = std::max(20.0, 1.3 * std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    // The middle, dropped onto the line.
    const double along = (middle.x - o.x) * d.x + (middle.y - o.y) * d.y + (middle.z - o.z) * d.z;
    state.center = {o.x + d.x * along, o.y + d.y * along, o.z + d.z * along};
    return state;
}

PlaneState placePlane(const Feature& feature, const Document& document, const BodyStates& bodies) {
    if (!std::isfinite(feature.distance)) {
        throw FeatureFailure("Its distance is not a number.");
    }
    PlaneState state;
    kernel::Plane base;
    if (feature.reference == "XY" || feature.reference == "XZ" || feature.reference == "YZ") {
        base = feature.reference == "XY"   ? kernel::planeXY()
               : feature.reference == "XZ" ? kernel::planeXZ()
                                           : kernel::planeYZ();
        // Drawn over the model, as large as the model, where there is one.
        state.center = base.origin;
        std::optional<kernel::BoundingBox> all;
        for (const auto& entry : bodies) {
            const auto box = kernel::boundingBox(entry.second.shape);
            if (!box.has_value()) {
                continue;
            }
            if (!all.has_value()) {
                all = box;
                continue;
            }
            all->min = {std::min(all->min.x, box->min.x), std::min(all->min.y, box->min.y),
                        std::min(all->min.z, box->min.z)};
            all->max = {std::max(all->max.x, box->max.x), std::max(all->max.y, box->max.y),
                        std::max(all->max.z, box->max.z)};
        }
        if (all.has_value()) {
            state.center = {(all->min.x + all->max.x) / 2.0, (all->min.y + all->max.y) / 2.0,
                            (all->min.z + all->max.z) / 2.0};
            const double dx = all->max.x - all->min.x;
            const double dy = all->max.y - all->min.y;
            const double dz = all->max.z - all->min.z;
            state.size = std::max(state.size, 1.2 * std::sqrt(dx * dx + dy * dy + dz * dz));
        }
    } else {
        const auto found = bodies.find(feature.body);
        const Body* body = document.findBody(feature.body);
        if (found == bodies.end() || found->second.shape.isNull()) {
            throw FeatureFailure(body != nullptr
                                     ? "The body it is placed on, " + body->name +
                                           ", does not exist at this point in the timeline."
                                     : std::string("The body it is placed on is gone."));
        }
        const int index = kernel::findFace(found->second, feature.reference);
        if (index < 0) {
            throw FeatureFailure("The face it is placed on no longer exists: " +
                                 describeName(document, feature.reference) + ".");
        }
        const kernel::Shape face =
            kernel::subShape(found->second.shape, kernel::SubShapeKind::Face, index);
        const auto plane = kernel::planeOfFace(face);
        if (!plane.has_value()) {
            throw FeatureFailure("The face it is placed on, " +
                                 describeName(document, feature.reference) + ", is not flat.");
        }
        base = *plane;
        const auto box = kernel::boundingBox(face);
        if (box.has_value()) {
            state.center = {(box->min.x + box->max.x) / 2.0, (box->min.y + box->max.y) / 2.0,
                            (box->min.z + box->max.z) / 2.0};
            const double dx = box->max.x - box->min.x;
            const double dy = box->max.y - box->min.y;
            const double dz = box->max.z - box->min.z;
            state.size = std::max(20.0, 1.2 * std::sqrt(dx * dx + dy * dy + dz * dz));
        }
    }
    const kernel::Vector& n = base.normal;
    const double d = feature.distance;
    base.origin = {base.origin.x + n.x * d, base.origin.y + n.y * d, base.origin.z + n.z * d};
    // The middle, dropped onto the plane where it now is.
    const kernel::Point& c = state.center;
    const double above =
        (c.x - base.origin.x) * n.x + (c.y - base.origin.y) * n.y + (c.z - base.origin.z) * n.z;
    state.center = {c.x - above * n.x, c.y - above * n.y, c.z - above * n.z};
    state.plane = base;
    return state;
}

kernel::Plane placeSketch(const SketchItem& sketch, const Document& document,
                          const BodyStates& bodies,
                          const std::unordered_map<ObjectId, PlaneState>& planes) {
    if (const Feature* plane = document.findFeature(sketch.attachedTo)) {
        const auto found = planes.find(plane->id);
        if (found == planes.end()) {
            throw FeatureFailure("The plane it is drawn on, " + plane->name +
                                 ", does not exist at this point in the timeline.");
        }
        return found->second.plane;
    }
    const Body* body = document.findBody(sketch.attachedTo);
    const auto found = bodies.find(sketch.attachedTo);
    if (body == nullptr) {
        throw FeatureFailure("What it was drawn on is gone.");
    }
    if (found == bodies.end() || found->second.shape.isNull()) {
        throw FeatureFailure("The body it was drawn on, " + body->name +
                             ", does not exist at this point in the timeline.");
    }
    const int index = kernel::findFace(found->second, sketch.attachedFace);
    if (index < 0) {
        throw FeatureFailure("The face it was drawn on no longer exists: " +
                             describeName(document, sketch.attachedFace) + ".");
    }
    const auto plane = kernel::planeOfFace(
        kernel::subShape(found->second.shape, kernel::SubShapeKind::Face, index));
    if (!plane.has_value()) {
        throw FeatureFailure("The face it was drawn on is no longer flat.");
    }
    return *plane;
}

std::string describeName(const Document& document, const std::string& name) {
    // An edge is named by the faces it lies between: "a|b#0".
    const std::size_t hash = name.rfind('#');
    const std::string faces = hash == std::string::npos ? name : name.substr(0, hash);
    const std::size_t bar = faces.find('|');
    if (bar == std::string::npos) {
        return describeFace(document, faces);
    }
    return "the edge between " + describeFace(document, faces.substr(0, bar)) + " and " +
           describeFace(document, faces.substr(bar + 1));
}

kernel::NamedShape evaluate(const Feature& feature, const Document& document,
                            const BodyStates& bodies, kernel::NamedShape* tool) {
    const auto keep = [tool](kernel::NamedShape made) {
        if (tool != nullptr) {
            *tool = made;
        }
        return made;
    };
    const std::string tag = std::to_string(feature.id.value);
    try {
        switch (feature.kind) {
        case FeatureKind::Pull:
        case FeatureKind::Revolve:
            return placed(feature, document, bodies, keep(madeBy(feature, document, &bodies)));
        case FeatureKind::Mirror:
        case FeatureKind::RectangularPattern:
        case FeatureKind::CircularPattern:
            return copiesOf(feature, document, targetOf(feature, document, bodies), bodies);
        case FeatureKind::Plane:
            throw FeatureFailure("A construction plane makes no solid.");
        case FeatureKind::Axis:
            throw FeatureFailure("A construction axis makes no solid.");
        case FeatureKind::Hole:
            return drilled(feature, document, targetOf(feature, document, bodies));
        case FeatureKind::Fillet:
        case FeatureKind::Chamfer: {
            const kernel::NamedShape& body = targetOf(feature, document, bodies);
            if (feature.edges.empty()) {
                throw FeatureFailure("It has no edges to work on.");
            }
            if (!std::isfinite(feature.size) || feature.size <= 1e-7) {
                throw FeatureFailure("Its size has to be more than zero.");
            }
            const std::vector<std::string> names = kernel::edgeNames(body);
            std::vector<int> edges;
            for (const std::string& name : feature.edges) {
                const auto found = std::find(names.begin(), names.end(), name);
                if (found == names.end()) {
                    throw FeatureFailure("An edge it works on no longer exists: " +
                                         describeName(document, name) + ".");
                }
                edges.push_back(static_cast<int>(found - names.begin()));
            }
            return feature.kind == FeatureKind::Fillet
                       ? kernel::fillet(body, edges, feature.size, tag)
                       : kernel::chamfer(body, edges, feature.size, tag);
        }
        case FeatureKind::OffsetFace:
        case FeatureKind::MoveFace:
        case FeatureKind::DeleteFace: {
            const kernel::NamedShape& body = targetOf(feature, document, bodies);
            if (feature.faces.empty()) {
                throw FeatureFailure("It has no faces to work on.");
            }
            std::vector<int> faces;
            for (const std::string& name : feature.faces) {
                const int face = kernel::findFace(body, name);
                if (face < 0) {
                    throw FeatureFailure("A face it works on no longer exists: " +
                                         describeName(document, name) + ".");
                }
                faces.push_back(face);
            }
            if (feature.kind == FeatureKind::DeleteFace) {
                return kernel::removeFaces(body, faces, tag);
            }
            if (!std::isfinite(feature.distance) || std::abs(feature.distance) < 1e-7) {
                throw FeatureFailure("Its distance has to be other than zero.");
            }
            if (feature.kind == FeatureKind::OffsetFace) {
                return kernel::offsetFaces(body, faces, feature.distance, tag);
            }
            const kernel::Vector d = axisNamed(feature.reference, body, document).direction;
            const double length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (!(length > 1e-12)) {
                throw FeatureFailure("Its direction is not a direction.");
            }
            const double scale = feature.distance / length;
            return kernel::moveFaces(body, faces, {d.x * scale, d.y * scale, d.z * scale}, tag);
        }
        case FeatureKind::Repair:
            if (!std::isfinite(feature.size) || feature.size <= 0.0) {
                throw FeatureFailure("The widest gap it closes has to be more than zero.");
            }
            return kernel::repaired(targetOf(feature, document, bodies), tag, feature.size);
        case FeatureKind::Shell: {
            const kernel::NamedShape& body = targetOf(feature, document, bodies);
            if (!std::isfinite(feature.size) || feature.size <= 1e-7) {
                throw FeatureFailure("Its wall thickness has to be more than zero.");
            }
            std::vector<int> faces;
            for (const std::string& name : feature.faces) {
                const int face = kernel::findFace(body, name);
                if (face < 0) {
                    throw FeatureFailure(
                        "A face it opens no longer exists: " + describeName(document, name) + ".");
                }
                faces.push_back(face);
            }
            return kernel::shell(body, faces, feature.size, tag);
        }
        }
    } catch (const kernel::Error& error) {
        throw failed(error);
    } catch (const std::invalid_argument& error) {
        throw FeatureFailure(std::string("It could not be applied: ") + error.what() + ".");
    }
    throw FeatureFailure("It is of a kind this version of LCAD does not know.");
}

} // namespace lcad::document::detail
