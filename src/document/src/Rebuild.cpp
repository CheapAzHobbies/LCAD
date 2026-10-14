// SPDX-License-Identifier: GPL-3.0-or-later

#include "Rebuild.h"

#include <lcad/document/SketchShapes.h>
#include <lcad/kernel/Error.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/SubShapes.h>

#include <algorithm>
#include <cmath>
#include <functional>

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

// The solid a pull or a revolve makes, before it is combined with a body.
kernel::NamedShape madeBy(const Feature& feature, const Document& document) {
    const std::string tag = std::to_string(feature.id.value);
    const SketchItem* sketch = document.findSketch(feature.sketch);
    if (sketch == nullptr) {
        throw FeatureFailure("The sketch it was made from is gone.");
    }
    const std::vector<kernel::Region> regions = regionsOf(feature, *sketch);
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
        return kernel::pull(sketch->plane, regions, from, to, tag);
    }
    const sketch::Entity* axis = sketch->geometry.find(feature.axis);
    if (axis == nullptr || axis->kind != sketch::EntityKind::Line) {
        throw FeatureFailure("The line it turns about is no longer in " + sketch->name + ".");
    }
    if (!std::isfinite(feature.angle) || std::abs(feature.angle) < 1e-9 ||
        std::abs(feature.angle) > 360.0 + 1e-9) {
        throw FeatureFailure("Its angle has to be more than zero and at most 360 degrees.");
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
                            const kernel::NamedShape& body) {
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
            piece.shape = madeBy(*source, document);
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
            return placed(feature, document, bodies, keep(madeBy(feature, document)));
        case FeatureKind::Mirror:
        case FeatureKind::RectangularPattern:
        case FeatureKind::CircularPattern:
            return copiesOf(feature, document, targetOf(feature, document, bodies));
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
