// SPDX-License-Identifier: GPL-3.0-or-later

#include "Rebuild.h"

#include <lcad/document/SketchShapes.h>
#include <lcad/kernel/Error.h>
#include <lcad/kernel/Measure.h>

#include <algorithm>
#include <cmath>

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
                            const BodyStates& bodies) {
    const std::string tag = std::to_string(feature.id.value);
    try {
        switch (feature.kind) {
        case FeatureKind::Pull:
        case FeatureKind::Revolve: {
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
                return placed(feature, document, bodies,
                              kernel::pull(sketch->plane, regions, from, to, tag));
            }
            const sketch::Entity* axis = sketch->geometry.find(feature.axis);
            if (axis == nullptr || axis->kind != sketch::EntityKind::Line) {
                throw FeatureFailure("The line it turns about is no longer in " + sketch->name +
                                     ".");
            }
            if (!std::isfinite(feature.angle) || std::abs(feature.angle) < 1e-9 ||
                std::abs(feature.angle) > 360.0 + 1e-9) {
                throw FeatureFailure("Its angle has to be more than zero and at most 360 "
                                     "degrees.");
            }
            const sketch::Point2 a = sketch->geometry.position(axis->start);
            const sketch::Point2 b = sketch->geometry.position(axis->end);
            return placed(feature, document, bodies,
                          kernel::revolve(sketch->plane, regions, a.x, a.y, b.x, b.y,
                                          feature.angle * pi / 180.0, tag));
        }
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
