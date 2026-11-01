// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/recover/Recover.h>

#include "Plan.h"
#include "Sharpen.h"

#include <lcad/kernel/Error.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/MeshSolid.h>
#include <lcad/kernel/MeshSurfaces.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/sketch/Edit.h>

#include <lcad/kernel/Stop.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace lcad::recover {

namespace {

using document::Document;
using document::Feature;
using document::FeatureKind;
using document::ObjectId;
using document::Operation;
using kernel::Mesh;
using sketch::ConstraintKind;

constexpr double pi = 3.14159265358979323846;
// How far a sketch's sizes go to round numbers: a thousandth of a millimeter.
constexpr double roundTo = 0.001;
// A rebuilt part has this much of the mesh's volume and its area, give or take.
constexpr double sameVolume = 0.012;
constexpr double sameArea = 0.02;
// How many times a part is tried again without what could not be put back.
constexpr int mostAttempts = 4;
// How many points of the mesh are measured against a rebuilt part.
constexpr std::size_t pointsChecked = 400;

// To a round number, if that is as good as what was measured.
double clean(double value, double tolerance) {
    const double rounded = std::round(value / roundTo) * roundTo;
    return std::abs(rounded - value) <= tolerance ? rounded : value;
}

sketch::Point2 cleanPoint(const kernel::Point2D& p, double tolerance) {
    return {clean(p.x, tolerance), clean(p.y, tolerance)};
}

// Adds an outline as curves joined end to end, level and upright where they are.
std::vector<sketch::EntityId> draw(sketch::Sketch& g, const Outline& outline, double tolerance) {
    std::vector<sketch::EntityId> curves;
    if (outline.curves.size() == 1 && outline.curves[0].whole) {
        const Curve& c = outline.curves[0];
        const sketch::EntityId circle =
            g.addCircle(cleanPoint(c.center, tolerance), clean(c.radius, tolerance));
        sketch::tryConstraint(g, ConstraintKind::Diameter, circle, 0, 0,
                              2.0 * clean(c.radius, tolerance));
        curves.push_back(circle);
        return curves;
    }
    struct Ends {
        sketch::EntityId first;
        sketch::EntityId last;
    };
    std::vector<Ends> ends;
    for (const Curve& c : outline.curves) {
        const sketch::Point2 a = cleanPoint(c.start, tolerance);
        const sketch::Point2 b = cleanPoint(c.end, tolerance);
        if (!c.round) {
            const sketch::EntityId line = g.addLine(a, b);
            const sketch::Entity& entity = *g.find(line);
            ends.push_back({entity.start, entity.end});
            curves.push_back(line);
            continue;
        }
        const sketch::Point2 center = cleanPoint(c.center, tolerance);
        const sketch::EntityId arc = c.counterClockwise ? g.addArc(center, a, b) : g.addArc(center, b, a);
        const sketch::Entity& entity = *g.find(arc);
        ends.push_back(c.counterClockwise ? Ends{entity.start, entity.end}
                                          : Ends{entity.end, entity.start});
        curves.push_back(arc);
    }
    for (std::size_t i = 0; i < ends.size(); ++i) {
        sketch::tryConstraint(g, ConstraintKind::Coincident, ends[i].last,
                              ends[(i + 1) % ends.size()].first);
    }
    // What a designer would have said, as far as it holds: the sketch moves if a size is changed.
    for (std::size_t i = 0; i < curves.size(); ++i) {
        const sketch::Entity& entity = *g.find(curves[i]);
        if (entity.kind == sketch::EntityKind::Line) {
            const sketch::Point2 a = g.position(entity.start);
            const sketch::Point2 b = g.position(entity.end);
            if (std::abs(a.y - b.y) < tolerance) {
                sketch::tryConstraint(g, ConstraintKind::Horizontal, curves[i]);
            } else if (std::abs(a.x - b.x) < tolerance) {
                sketch::tryConstraint(g, ConstraintKind::Vertical, curves[i]);
            }
            sketch::tryConstraint(g, ConstraintKind::Distance, entity.start, entity.end, 0,
                                  std::hypot(a.x - b.x, a.y - b.y));
        } else {
            sketch::tryConstraint(g, ConstraintKind::Radius, curves[i], 0, 0, g.radiusOf(curves[i]));
        }
    }
    return curves;
}

kernel::Plane planeAt(const kernel::Plane& base, double height) {
    const kernel::Vector& n = base.normal;
    return kernel::makePlane({base.origin.x + n.x * height, base.origin.y + n.y * height,
                              base.origin.z + n.z * height},
                             n, base.xDirection);
}

// Puts a plan into a document, as features. The caller has a transaction open.
class Builder {
public:
    Builder(Document& document, const std::string& name, ObjectId component, double tolerance)
        : m_document(document), m_name(name), m_component(component), m_tolerance(tolerance) {}

    /// Starts from a solid, rather than from the plan's layers, if it is given one.
    bool build(const Plan& plan, const kernel::Shape* solid, PartResult& result) {
        if (solid != nullptr) {
            m_body = m_document.addBody(m_document.uniqueBodyName(m_name), *solid, m_component);
        } else {
            pulls(plan, result);
        }
        if (!m_body.isValid()) {
            return false;
        }
        holes(plan, result);
        edges(plan, result);
        return true;
    }

    [[nodiscard]] ObjectId body() const { return m_body; }
    /// The edges that could not be rounded.
    [[nodiscard]] const std::vector<kernel::Point>& refused() const { return m_refused; }

private:
    struct Drawn {
        ObjectId sketch;
        /// The regions to pull: the insides of its outer outlines, less the inner ones.
        std::vector<std::vector<sketch::EntityId>> solid;
    };

    Drawn sketchOf(const kernel::Plane& plane, const std::vector<const Outline*>& outlines) {
        sketch::Sketch g;
        Drawn drawn;
        for (const Outline* outline : outlines) {
            std::vector<sketch::EntityId> curves = draw(g, *outline, m_tolerance);
            if (outline->area > 0.0) {
                std::sort(curves.begin(), curves.end());
                drawn.solid.push_back(std::move(curves));
            }
        }
        drawn.sketch = m_document.addSketch(m_document.uniqueSketchName(m_name + " sketch"), plane,
                                            {}, {}, m_component);
        m_document.setSketchGeometry(drawn.sketch, std::move(g), "Draw");
        m_document.setSketchVisible(drawn.sketch, false);
        return drawn;
    }

    bool pull(const Drawn& drawn, double distance, Operation operation, PartResult& result) {
        Feature feature;
        feature.kind = FeatureKind::Pull;
        feature.sketch = drawn.sketch;
        feature.regions = drawn.solid;
        feature.distance = distance;
        feature.operation = operation;
        feature.body = m_body;
        feature.component = m_component;
        const ObjectId made = m_document.addFeature(feature);
        if (!m_document.featureError(made).empty()) {
            return false;
        }
        if (!m_body.isValid()) {
            m_body = m_document.findFeature(made)->body;
        }
        ++result.pulls;
        return true;
    }

    // Layers with the same outside are one pull, with what differs inside cut out of it. Where
    // the outside changes, a new pull joins on.
    void pulls(const Plan& plan, PartResult& result) {
        const auto outsides = [](const Layer& layer) {
            std::set<std::string> found;
            for (const Outline& o : layer.outlines) {
                if (o.area > 0.0) {
                    found.insert(o.signature);
                }
            }
            return found;
        };
        for (std::size_t i = 0; i < plan.layers.size();) {
            std::size_t end = i + 1;
            while (end < plan.layers.size() && outsides(plan.layers[end]) == outsides(plan.layers[i])) {
                ++end;
            }
            std::vector<const Outline*> common;
            for (const Outline& o : plan.layers[i].outlines) {
                bool everywhere = true;
                for (std::size_t k = i; k < end && everywhere; ++k) {
                    everywhere = std::any_of(plan.layers[k].outlines.begin(), plan.layers[k].outlines.end(),
                                             [&o](const Outline& other) { return other.signature == o.signature; });
                }
                if (everywhere) {
                    common.push_back(&o);
                }
            }
            const double low = plan.layers[i].low;
            const double high = plan.layers[end - 1].high;
            if (!pull(sketchOf(planeAt(plan.base, low), common), high - low,
                      m_body.isValid() ? Operation::Join : Operation::NewBody, result)) {
                m_body = {};
                return;
            }
            for (std::size_t k = i; k < end; ++k) {
                std::vector<const Outline*> extra;
                for (const Outline& o : plan.layers[k].outlines) {
                    if (std::none_of(common.begin(), common.end(), [&o](const Outline* c) {
                            return c->signature == o.signature;
                        })) {
                        extra.push_back(&o);
                    }
                }
                if (extra.empty()) {
                    continue;
                }
                const Layer& layer = plan.layers[k];
                Drawn cut = sketchOf(planeAt(plan.base, layer.high), extra);
                cut.solid.clear();
                if (!pull(cut, layer.low - layer.high, Operation::Cut, result)) {
                    m_body = {};
                    return;
                }
            }
            i = end;
        }
    }

    void holes(const Plan& plan, PartResult& result) {
        // Those of one size from one plane are one feature, with a point each.
        struct Group {
            kernel::Plane plane;
            double diameter;
            double depth;
            bool through;
            double tipAngle;
            std::vector<kernel::Point2D> points;
        };
        std::vector<Group> groups;
        for (const PlannedHole& hole : plan.holes) {
            const auto found = std::find_if(groups.begin(), groups.end(), [&hole](const Group& g) {
                const kernel::Vector d{hole.plane.origin.x - g.plane.origin.x,
                                       hole.plane.origin.y - g.plane.origin.y,
                                       hole.plane.origin.z - g.plane.origin.z};
                return std::abs(g.diameter - hole.diameter) < 1e-3 &&
                       std::abs(g.depth - hole.depth) < 1e-3 && g.through == hole.through &&
                       std::abs(g.tipAngle - hole.tipAngle) < 1e-3 &&
                       g.plane.normal.x * hole.plane.normal.x + g.plane.normal.y * hole.plane.normal.y +
                               g.plane.normal.z * hole.plane.normal.z >
                           0.999999 &&
                       std::hypot(d.x, d.y, d.z) < 1e-9;
            });
            if (found != groups.end()) {
                found->points.push_back(hole.at);
            } else {
                groups.push_back(
                    {hole.plane, hole.diameter, hole.depth, hole.through, hole.tipAngle, {hole.at}});
            }
        }
        for (const Group& group : groups) {
            sketch::Sketch g;
            for (const kernel::Point2D& at : group.points) {
                g.addPoint(cleanPoint(at, m_tolerance));
            }
            const ObjectId sketch = m_document.addSketch(m_document.uniqueSketchName(m_name + " holes"),
                                                         group.plane, {}, {}, m_component);
            m_document.setSketchGeometry(sketch, std::move(g), "Draw");
            m_document.setSketchVisible(sketch, false);
            Feature feature;
            feature.kind = FeatureKind::Hole;
            feature.sketch = sketch;
            feature.body = m_body;
            feature.component = m_component;
            feature.size = clean(group.diameter, m_tolerance);
            feature.distance = clean(group.depth, m_tolerance);
            feature.throughAll = group.through;
            feature.tipAngle = group.tipAngle;
            const ObjectId made = m_document.addFeature(feature);
            if (m_document.featureError(made).empty()) {
                result.holes += static_cast<int>(group.points.size());
            } else {
                m_document.removeFeature(made);
            }
        }
    }

    void edges(const Plan& plan, PartResult& result) {
        const auto sameStep = [this](const PlannedEdge& a, const PlannedEdge& b) {
            return a.chamfer == b.chamfer && std::abs(a.size - b.size) <= m_tolerance;
        };
        for (std::size_t i = 0; i < plan.edges.size();) {
            std::size_t end = i + 1;
            while (end < plan.edges.size() && sameStep(plan.edges[i], plan.edges[end])) {
                ++end;
            }
            const std::vector<PlannedEdge> group(plan.edges.begin() + static_cast<std::ptrdiff_t>(i),
                                                 plan.edges.begin() + static_cast<std::ptrdiff_t>(end));
            if (!round(group, result)) {
                // Together they may be too much for it, where one at a time is not.
                for (const PlannedEdge& edge : group) {
                    if (!round({edge}, result)) {
                        m_refused.push_back(edge.at);
                    }
                }
            }
            i = end;
        }
    }

    bool round(const std::vector<PlannedEdge>& group, PartResult& result) {
        const kernel::NamedShape named = m_document.namedBody(m_body);
        const std::vector<std::string> names = kernel::edgeNames(named);
        std::set<std::string> edgesHere;
        for (const PlannedEdge& edge : group) {
            const int index =
                kernel::nearestEdge(named.shape, edge.at, edge.size * 0.5 + m_tolerance * 8.0);
            if (index >= 0 && index < static_cast<int>(names.size())) {
                edgesHere.insert(names[static_cast<std::size_t>(index)]);
            }
        }
        if (edgesHere.empty()) {
            return false;
        }
        const bool chamfer = group.front().chamfer;
        Feature feature;
        feature.kind = chamfer ? FeatureKind::Chamfer : FeatureKind::Fillet;
        feature.body = m_body;
        feature.component = m_component;
        feature.size = clean(group.front().size, m_tolerance);
        feature.edges.assign(edgesHere.begin(), edgesHere.end());
        const ObjectId made = m_document.addFeature(feature);
        if (!m_document.featureError(made).empty()) {
            m_document.removeFeature(made);
            return false;
        }
        (chamfer ? result.chamfers : result.fillets) += 1;
        return true;
    }

    Document& m_document;
    std::string m_name;
    ObjectId m_component;
    double m_tolerance;
    ObjectId m_body;
    std::vector<kernel::Point> m_refused;
};

// The parts of a mesh that hang together, each with its own numbering.
std::vector<Mesh> partsOf(const Mesh& mesh) {
    std::vector<std::uint32_t> parent(mesh.vertices.size());
    for (std::uint32_t i = 0; i < parent.size(); ++i) {
        parent[i] = i;
    }
    const auto root = [&parent](std::uint32_t i) {
        while (parent[i] != i) {
            i = parent[i] = parent[parent[i]];
        }
        return i;
    };
    for (const auto& c : mesh.triangles) {
        parent[root(c[0])] = root(c[1]);
        parent[root(c[1])] = root(c[2]);
    }
    std::map<std::uint32_t, Mesh> parts;
    std::vector<std::uint32_t> number(mesh.vertices.size(), 0xFFFFFFFFU);
    for (const auto& c : mesh.triangles) {
        Mesh& part = parts[root(c[0])];
        std::array<std::uint32_t, 3> corners{};
        for (std::size_t k = 0; k < 3; ++k) {
            if (number[c[k]] == 0xFFFFFFFFU) {
                number[c[k]] = static_cast<std::uint32_t>(part.vertices.size());
                part.vertices.push_back(mesh.vertices[c[k]]);
            }
            corners[k] = number[c[k]];
        }
        part.triangles.push_back(corners);
    }
    std::vector<Mesh> out;
    for (auto& [key, part] : parts) {
        out.push_back(std::move(part));
    }
    return out;
}


double meshArea(const Mesh& mesh) {
    double area = 0.0;
    for (const auto& c : mesh.triangles) {
        const kernel::Point& p = mesh.vertices[c[0]];
        const kernel::Point& q = mesh.vertices[c[1]];
        const kernel::Point& r = mesh.vertices[c[2]];
        const std::array<double, 3> u{q.x - p.x, q.y - p.y, q.z - p.z};
        const std::array<double, 3> v{r.x - p.x, r.y - p.y, r.z - p.z};
        area += std::hypot(u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                           u[0] * v[1] - u[1] * v[0]) /
                2.0;
    }
    return area;
}

// Whether a rebuilt body is what the mesh is: as much of it, as much surface, in the same box, and
// with the points of the mesh on its faces.
bool matches(const kernel::Shape& body, const Mesh& mesh, double tolerance) {
    const double wanted = kernel::meshVolume(mesh);
    if (wanted <= 0.0 || std::abs(kernel::volume(body) - wanted) > wanted * sameVolume) {
        return false;
    }
    const double wantedArea = meshArea(mesh);
    if (std::abs(kernel::surfaceArea(body) - wantedArea) > wantedArea * sameArea) {
        return false;
    }
    const auto box = kernel::boundingBox(body);
    if (!box.has_value()) {
        return false;
    }
    kernel::Point low = mesh.vertices.front();
    kernel::Point high = low;
    for (const kernel::Point& p : mesh.vertices) {
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    const double size = std::hypot(high.x - low.x, high.y - low.y, high.z - low.z);
    const double slack = size * 0.01;
    if (std::abs(box->min.x - low.x) >= slack || std::abs(box->min.y - low.y) >= slack ||
        std::abs(box->min.z - low.z) >= slack || std::abs(box->max.x - high.x) >= slack ||
        std::abs(box->max.y - high.y) >= slack || std::abs(box->max.z - high.z) >= slack) {
        return false;
    }
    std::vector<kernel::Point> points;
    const std::size_t step = std::max<std::size_t>(mesh.vertices.size() / pointsChecked, 1);
    for (std::size_t i = 0; i < mesh.vertices.size(); i += step) {
        points.push_back(mesh.vertices[i]);
    }
    return kernel::farthestFromFaces(body, points) <= std::max(tolerance * 10.0, size * 1e-3);
}

// One part or, where parts lie inside others, all of them: what is to go into the document.
struct Worked {
    PartResult result;
    std::string name;
    /// Features to build, on `base` if there is one and else from their layers.
    std::optional<Plan> plan;
    std::optional<kernel::Shape> base;
    double tolerance = 0.0;
    /// Or just these.
    std::vector<kernel::Shape> solids;
};

[[noreturn]] void stopped() {
    throw kernel::Error("import a mesh", "it was stopped");
}

// Tries a plan on a document of its own: what does not come to the same shape leaves no trace.
bool tryPlan(const Plan& plan, const kernel::Shape* base, const Mesh& part, const std::string& name,
             double tolerance, PartResult& result, std::vector<kernel::Point>* refused = nullptr) {
    Document scratch;
    Builder builder(scratch, name, {}, tolerance);
    PartResult tried = result;
    bool built = false;
    try {
        built = builder.build(plan, base, tried);
    } catch (const std::invalid_argument&) {
        built = false; // a curve that shrank to nothing
    } catch (const kernel::Error&) {
        built = false;
    }
    if (kernel::stopRequested()) {
        stopped();
    }
    if (refused != nullptr) {
        refused->insert(refused->end(), builder.refused().begin(), builder.refused().end());
    }
    const document::Body* body = built ? scratch.findBody(builder.body()) : nullptr;
    if (body == nullptr || body->shape.isNull() || !matches(body->shape, part, tolerance)) {
        return false;
    }
    // A solid under no features at all is what the solid of the part already is.
    if (base != nullptr && tried.holes + tried.fillets + tried.chamfers == 0) {
        return false;
    }
    result = tried;
    return true;
}

void asSolids(Worked& worked, const kernel::MeshSolids& solids) {
    worked.solids = solids.solids;
    worked.result.result = solids.solids.empty() ? Result::Failed
                           : solids.facetedParts > 0 ? Result::Facets
                                                     : Result::ExactSolid;
    if (solids.solids.empty()) {
        worked.result.note = solids.openParts > 0 ? "it is not closed" : "nothing could be made of it";
    }
}

// Reads the plain shape under the rounds and holes of a part, and puts those back as features: on a
// stack of sketches if it is one, and if not on a solid. Whatever cannot be put back stays in that
// shape. Whether all of it came to the same is the last word: if not, `worked` stays as it was.
void undress(Worked& worked, const Mesh& part, const kernel::MeshSurfaces& original) {
    std::vector<kernel::Point> keep;
    for (int attempt = 0; attempt < mostAttempts; ++attempt) {
        const Sharpened sharp = sharpen(part, original, keep);
        if (sharp.holes.empty() && sharp.edges.empty()) {
            return;
        }
        const auto addSteps = [&sharp](Plan& plan) {
            plan.holes.insert(plan.holes.end(), sharp.holes.begin(), sharp.holes.end());
            plan.edges.insert(plan.edges.end(), sharp.edges.begin(), sharp.edges.end());
        };
        std::vector<kernel::Point> refused;
        std::string why;
        std::optional<Plan> stack = planPart(sharp.mesh, sharp.found, why);
        if (stack) {
            addSteps(*stack);
            if (tryPlan(*stack, nullptr, part, worked.name, worked.tolerance, worked.result, &refused)) {
                worked.result.result = Result::Features;
                worked.result.note.clear();
                worked.plan = std::move(stack);
                return;
            }
        } else {
            const kernel::MeshSolids solids = kernel::solidsFromMesh(sharp.mesh, sharp.found);
            if (solids.solids.size() != 1 || solids.facetedParts > 0) {
                return;
            }
            Plan onSolid;
            addSteps(onSolid);
            if (tryPlan(onSolid, &solids.solids.front(), part, worked.name, worked.tolerance,
                        worked.result, &refused)) {
                worked.result.result = Result::FeaturesOnSolid;
                worked.plan = std::move(onSolid);
                worked.base = solids.solids.front();
                return;
            }
        }
        if (refused.empty()) {
            return;
        }
        keep.insert(keep.end(), refused.begin(), refused.end());
    }
}

} // namespace

struct WorkedMesh::Parts {
    std::string name;
    std::vector<Worked> parts;
};

WorkedMesh::WorkedMesh(std::shared_ptr<const Parts> parts) : m_parts(std::move(parts)) {}

WorkedMesh workOutMesh(const Mesh& mesh, const std::string& name) {
    const std::vector<Mesh> parts = partsOf(mesh);
    // A part facing inward is a hollow in another, and only a solid made of faces can hold that.
    const bool hollows = std::any_of(parts.begin(), parts.end(), [](const Mesh& part) {
        return kernel::meshVolume(part) < 0.0;
    });
    auto out = std::make_shared<WorkedMesh::Parts>();
    out->name = name;
    if (hollows) {
        Worked worked;
        worked.name = name;
        const kernel::MeshSurfaces found = kernel::findSurfaces(mesh);
        worked.result.recognized = found.share;
        worked.result.note = "it has a hollow inside";
        asSolids(worked, kernel::solidsFromMesh(mesh, found));
        out->parts.push_back(std::move(worked));
        return WorkedMesh(std::move(out));
    }
    int number = 0;
    for (const Mesh& part : parts) {
        Worked worked;
        worked.name = parts.size() > 1 ? name + " " + std::to_string(++number) : name;
        const kernel::MeshSurfaces found = kernel::findSurfaces(part);
        worked.result.recognized = found.share;
        worked.tolerance = found.tolerance;
        std::string why;
        std::optional<Plan> plan = planPart(part, found, why);
        if (plan) {
            if (tryPlan(*plan, nullptr, part, worked.name, found.tolerance, worked.result)) {
                worked.result.result = Result::Features;
                worked.plan = std::move(plan);
            } else {
                why = "what it worked out did not come to the same shape";
            }
        }
        if (!worked.plan) {
            worked.result.note = why;
            undress(worked, part, found);
        }
        if (!worked.plan) {
            asSolids(worked, kernel::solidsFromMesh(part, found));
        }
        out->parts.push_back(std::move(worked));
    }
    return WorkedMesh(std::move(out));
}

MeshResult addWorkedMesh(Document& document, const WorkedMesh& mesh, ObjectId component) {
    MeshResult out;
    document::Transaction transaction(document, "Import " + mesh.parts().name);
    for (const Worked& worked : mesh.parts().parts) {
        if (worked.plan) {
            Builder builder(document, worked.name, component, worked.tolerance);
            PartResult again;
            builder.build(*worked.plan, worked.base ? &*worked.base : nullptr, again);
            if (!worked.base) {
                document.renameBody(builder.body(), document.uniqueBodyName(worked.name));
            }
            out.bodies.push_back(builder.body());
        }
        int made = 0;
        for (const kernel::Shape& solid : worked.solids) {
            const std::string bodyName = worked.solids.size() > 1
                                             ? worked.name + " " + std::to_string(++made)
                                             : worked.name;
            out.bodies.push_back(document.addBody(document.uniqueBodyName(bodyName), solid, component));
        }
        PartResult result = worked.result;
        result.name = worked.name;
        out.parts.push_back(std::move(result));
    }
    transaction.commit();
    return out;
}

MeshResult addMesh(Document& document, const Mesh& mesh, const std::string& name,
                   ObjectId component) {
    return addWorkedMesh(document, workOutMesh(mesh, name), component);
}

} // namespace lcad::recover
