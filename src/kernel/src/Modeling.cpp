// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Modeling.h>

#include <lcad/kernel/occt/Guard.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeShape.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <ShapeFix_Face.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <tuple>

namespace lcad::kernel {

namespace {

// Ends of curves closer than this, in millimeters, are joined into one corner of an outline.
constexpr double outlineTolerance = 1e-5;

gp_Pnt toOcct(const Point& p) {
    return gp_Pnt(p.x, p.y, p.z);
}

gp_Vec normalOf(const Plane& plane) {
    return gp_Vec(plane.normal.x, plane.normal.y, plane.normal.z);
}

gp_Pln toOcct(const Plane& plane) {
    return gp_Pln(gp_Ax3(toOcct(plane.origin),
                         gp_Dir(plane.normal.x, plane.normal.y, plane.normal.z),
                         gp_Dir(plane.xDirection.x, plane.xDirection.y, plane.xDirection.z)));
}

// A short number that stands for a long name, so that names made from names stay short.
std::string digest(const std::string& text) {
    std::uint32_t hash = 2166136261U;
    for (const char c : text) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    char buffer[9];
    std::snprintf(buffer, sizeof(buffer), "%08x", hash);
    return buffer;
}

TopTools_IndexedMapOfShape mapOf(const TopoDS_Shape& shape, TopAbs_ShapeEnum type) {
    TopTools_IndexedMapOfShape map;
    if (!shape.IsNull()) {
        TopExp::MapShapes(shape, type, map);
    }
    return map;
}

gp_Pnt middleOf(const TopoDS_Edge& edge) {
    const BRepAdaptor_Curve curve(edge);
    return curve.Value((curve.FirstParameter() + curve.LastParameter()) / 2.0);
}

gp_Pnt centerOf(const TopoDS_Shape& face) {
    GProp_GProps properties;
    BRepGProp::SurfaceProperties(face, properties);
    return properties.CentreOfMass();
}

// Where something is, as a key that sorts the same way every time.
std::tuple<double, double, double> placeKey(const gp_Pnt& p) {
    const auto rounded = [](double value) { return std::round(value * 1e6) / 1e6; };
    return {rounded(p.X()), rounded(p.Y()), rounded(p.Z())};
}

void requireValid(const TopoDS_Shape& shape, const char* operation) {
    if (shape.IsNull()) {
        throw Error(operation, "it produced nothing");
    }
    if (!BRepCheck_Analyzer(shape).IsValid()) {
        throw Error(operation, "the result is not a valid solid");
    }
}

bool hasSolid(const TopoDS_Shape& shape) {
    return !shape.IsNull() && TopExp_Explorer(shape, TopAbs_SOLID).More();
}

// Collects, for the faces of a result, the names they could take, and settles on one each.
class FaceNamer {
public:
    explicit FaceNamer(const TopoDS_Shape& result) : m_faces(mapOf(result, TopAbs_FACE)) {
        m_candidates.resize(static_cast<std::size_t>(m_faces.Extent()));
    }

    // Offers `name` to a face of the result. Faces that are not in the result are ignored.
    void offer(const TopoDS_Shape& face, const std::string& name) {
        if (face.IsNull()) {
            return;
        }
        const int index = m_faces.FindIndex(face);
        if (index > 0) {
            m_candidates[static_cast<std::size_t>(index - 1)].push_back(name);
        }
    }

    // Offers `name` to each of the faces something became: the name itself to the first, and
    // the name with a number to the others, in the order of where they are.
    void offerAll(const TopTools_ListOfShape& faces, const std::string& name) {
        std::vector<TopoDS_Shape> present;
        for (TopTools_ListOfShape::Iterator it(faces); it.More(); it.Next()) {
            if (it.Value().ShapeType() == TopAbs_FACE && m_faces.Contains(it.Value())) {
                present.push_back(it.Value());
            }
        }
        std::sort(present.begin(), present.end(), [](const TopoDS_Shape& a, const TopoDS_Shape& b) {
            return placeKey(centerOf(a)) < placeKey(centerOf(b));
        });
        for (std::size_t i = 0; i < present.size(); ++i) {
            offer(present[i], i == 0 ? name : name + ";" + std::to_string(i + 1));
        }
    }

    // Carries the names of a source shape's faces over to what an operation made of them.
    void inherit(const NamedShape& source, BRepBuilderAPI_MakeShape& operation) {
        const TopTools_IndexedMapOfShape faces = mapOf(occt::unwrap(source.shape), TopAbs_FACE);
        for (int i = 1; i <= faces.Extent(); ++i) {
            const TopoDS_Shape& face = faces(i);
            const std::string& name = source.faceNames.at(static_cast<std::size_t>(i - 1));
            const TopTools_ListOfShape& modified = operation.Modified(face);
            if (!modified.IsEmpty()) {
                offerAll(modified, name);
            } else if (!operation.IsDeleted(face)) {
                offer(face, name);
            }
        }
    }

    // Offers `name` to the faces that pass through a point and have been offered nothing yet.
    void offerUnnamedAt(const gp_Pnt& point, const std::string& name) {
        const TopoDS_Vertex probe = BRepBuilderAPI_MakeVertex(point).Vertex();
        for (int i = 1; i <= m_faces.Extent(); ++i) {
            std::vector<std::string>& offered = m_candidates[static_cast<std::size_t>(i - 1)];
            if (!offered.empty()) {
                continue;
            }
            const BRepExtrema_DistShapeShape distance(probe, m_faces(i));
            if (distance.IsDone() && distance.Value() < 1e-6) {
                offered.push_back(name);
            }
        }
    }

    // One name per face of the result. A face nothing was offered to is named after
    // `fallback`; of several names offered, the first in the alphabet is taken, so that the
    // choice does not depend on the order things were found in.
    [[nodiscard]] std::vector<std::string> names(const std::string& fallback) const {
        std::vector<std::string> chosen;
        std::map<std::string, int> used;
        for (std::size_t i = 0; i < m_candidates.size(); ++i) {
            std::string name = m_candidates[i].empty() ? fallback + "/face/" + std::to_string(i)
                                                       : *std::min_element(m_candidates[i].begin(),
                                                                           m_candidates[i].end());
            const int seen = used[name]++;
            if (seen > 0) {
                name += ";" + std::to_string(seen + 1);
            }
            chosen.push_back(std::move(name));
        }
        return chosen;
    }

private:
    TopTools_IndexedMapOfShape m_faces;
    std::vector<std::vector<std::string>> m_candidates;
};

TopoDS_Wire wireOf(const std::vector<OutlineCurve>& curves) {
    Handle(TopTools_HSequenceOfShape) edges = new TopTools_HSequenceOfShape();
    for (const OutlineCurve& curve : curves) {
        const TopoDS_Shape& edge = occt::unwrap(curve.edge);
        if (edge.IsNull() || edge.ShapeType() != TopAbs_EDGE) {
            throw std::invalid_argument("an outline is made of edges");
        }
        edges->Append(edge);
    }
    Handle(TopTools_HSequenceOfShape) wires;
    ShapeAnalysis_FreeBounds::ConnectEdgesToWires(edges, outlineTolerance, Standard_False, wires);
    if (wires.IsNull() || wires->Length() != 1) {
        throw Error("make an outline", "the curves do not join into one closed outline");
    }
    const TopoDS_Wire wire = TopoDS::Wire(wires->Value(1));
    if (!BRep_Tool::IsClosed(wire)) {
        throw Error("make an outline", "the outline is not closed");
    }
    return wire;
}

TopoDS_Face faceOf(const Plane& plane, const Region& region) {
    if (region.outer.empty()) {
        throw std::invalid_argument("a region needs an outline");
    }
    BRepBuilderAPI_MakeFace maker(toOcct(plane), wireOf(region.outer), Standard_True);
    for (const std::vector<OutlineCurve>& hole : region.holes) {
        maker.Add(wireOf(hole));
    }
    if (!maker.IsDone()) {
        throw Error("make a region", "the outline does not bound a flat face");
    }
    // Puts the outer outline counter-clockwise and the holes clockwise, whichever way they came.
    ShapeFix_Face fix(maker.Face());
    fix.FixOrientation();
    fix.Perform();
    const TopoDS_Face face = fix.Face();
    if (!BRepCheck_Analyzer(face).IsValid()) {
        throw Error("make a region", "the outline crosses itself");
    }
    return face;
}

// The name of the curve an edge of a region's face came from: the one whose middle is where
// the edge's middle is.
std::string curveNameOf(const Region& region, const TopoDS_Edge& edge) {
    const gp_Pnt middle = middleOf(edge);
    const std::string* best = nullptr;
    double nearest = 0.0;
    const auto consider = [&](const std::vector<OutlineCurve>& curves) {
        for (const OutlineCurve& curve : curves) {
            const double away = middle.Distance(middleOf(TopoDS::Edge(occt::unwrap(curve.edge))));
            if (best == nullptr || away < nearest) {
                best = &curve.name;
                nearest = away;
            }
        }
    };
    consider(region.outer);
    for (const std::vector<OutlineCurve>& hole : region.holes) {
        consider(hole);
    }
    return best != nullptr ? *best : std::string("?");
}

// The name a region goes by: that of the first of its outline's curves in the alphabet. It
// does not change when other regions are added or taken away.
std::string keyOf(const Region& region) {
    std::string key = region.outer.front().name;
    for (const OutlineCurve& curve : region.outer) {
        if (curve.name.size() < key.size() ||
            (curve.name.size() == key.size() && curve.name < key)) {
            key = curve.name;
        }
    }
    return key;
}

NamedShape named(const TopoDS_Shape& shape, const FaceNamer& namer, const std::string& tag) {
    return NamedShape{occt::wrap(shape), namer.names(tag)};
}

// Sweeps one region, by whatever `sweep` does to its face, and names the faces that result.
template <typename Sweep>
NamedShape sweepRegion(const Plane& plane, const Region& region, const gp_Trsf* placement,
                       const std::string& tag, const char* operation, Sweep&& sweep) {
    TopoDS_Face face = faceOf(plane, region);
    // Which curve each edge of the face came from, before the face is moved.
    TopTools_IndexedMapOfShape edges = mapOf(face, TopAbs_EDGE);
    std::vector<std::string> curveNames;
    for (int i = 1; i <= edges.Extent(); ++i) {
        curveNames.push_back(curveNameOf(region, TopoDS::Edge(edges(i))));
    }
    if (placement != nullptr) {
        BRepBuilderAPI_Transform move(face, *placement, Standard_True);
        TopTools_IndexedMapOfShape moved;
        for (int i = 1; i <= edges.Extent(); ++i) {
            moved.Add(move.ModifiedShape(edges(i)));
        }
        face = TopoDS::Face(move.Shape());
        edges = moved;
    }

    auto maker = sweep(face);
    maker->Build();
    if (!maker->IsDone()) {
        throw Error(operation, "the outline could not be made into a solid");
    }
    const TopoDS_Shape solid = maker->Shape();
    requireValid(solid, operation);

    FaceNamer namer(solid);
    for (int i = 1; i <= edges.Extent(); ++i) {
        namer.offerAll(maker->Generated(edges(i)),
                       tag + "/side/" + curveNames[static_cast<std::size_t>(i - 1)]);
    }
    // A full turn does not say which face it made from a curve that is square to the axis.
    // Such a face is found by where it is: it is the one the middle of the curve lies on.
    for (int i = 1; i <= edges.Extent(); ++i) {
        if (maker->Generated(edges(i)).IsEmpty()) {
            namer.offerUnnamedAt(middleOf(TopoDS::Edge(edges(i))),
                                 tag + "/side/" + curveNames[static_cast<std::size_t>(i - 1)]);
        }
    }
    const std::string key = keyOf(region);
    namer.offer(maker->FirstShape(), tag + "/start/" + key);
    namer.offer(maker->LastShape(), tag + "/end/" + key);
    return named(solid, namer, tag);
}

NamedShape joinAll(std::vector<NamedShape> solids) {
    NamedShape whole = std::move(solids.front());
    for (std::size_t i = 1; i < solids.size(); ++i) {
        whole = combine(whole, solids[i], Combination::Join);
    }
    return whole;
}

TopoDS_Edge edgeAt(const TopTools_IndexedMapOfShape& edges, int index, const char* operation) {
    if (index < 0 || index >= edges.Extent()) {
        throw std::out_of_range(std::string(operation) + ": there is no edge " +
                                std::to_string(index));
    }
    return TopoDS::Edge(edges(index + 1));
}

// Names the faces of a body after rounding or beveling some of its edges.
template <typename Maker>
NamedShape namedAfterEdges(const NamedShape& body, Maker& maker, const std::string& tag) {
    const TopoDS_Shape result = maker.Shape();
    FaceNamer namer(result);
    namer.inherit(body, maker);
    const std::vector<std::string> edgesByName = edgeNames(body);
    const TopTools_IndexedMapOfShape edges = mapOf(occt::unwrap(body.shape), TopAbs_EDGE);
    for (int i = 1; i <= edges.Extent(); ++i) {
        const TopTools_ListOfShape& made = maker.Generated(edges(i));
        if (!made.IsEmpty()) {
            namer.offerAll(made,
                           tag + "/edge/" + digest(edgesByName[static_cast<std::size_t>(i - 1)]));
        }
    }
    const TopTools_IndexedMapOfShape corners = mapOf(occt::unwrap(body.shape), TopAbs_VERTEX);
    for (int i = 1; i <= corners.Extent(); ++i) {
        const TopTools_ListOfShape& made = maker.Generated(corners(i));
        if (!made.IsEmpty()) {
            namer.offerAll(made, tag + "/corner/" + std::to_string(i - 1));
        }
    }
    return named(result, namer, tag);
}

} // namespace

NamedShape nameFaces(const Shape& shape, const std::string& prefix) {
    NamedShape result{shape, {}};
    const int count = mapOf(occt::unwrap(shape), TopAbs_FACE).Extent();
    for (int i = 0; i < count; ++i) {
        result.faceNames.push_back(prefix + std::to_string(i));
    }
    return result;
}

std::vector<std::string> edgeNames(const NamedShape& shape) {
    const TopoDS_Shape& whole = occt::unwrap(shape.shape);
    std::vector<std::string> names;
    if (whole.IsNull()) {
        return names;
    }
    const TopTools_IndexedMapOfShape faces = mapOf(whole, TopAbs_FACE);
    TopTools_IndexedDataMapOfShapeListOfShape facesOfEdge;
    TopExp::MapShapesAndAncestors(whole, TopAbs_EDGE, TopAbs_FACE, facesOfEdge);
    const TopTools_IndexedMapOfShape edges = mapOf(whole, TopAbs_EDGE);

    // The names of the faces around each edge, and then the edges that share those sorted by
    // where they are.
    std::vector<std::string> between(static_cast<std::size_t>(edges.Extent()));
    std::map<std::string, std::vector<int>> groups;
    for (int i = 1; i <= edges.Extent(); ++i) {
        std::vector<std::string> around;
        const int at = facesOfEdge.FindIndex(edges(i));
        if (at > 0) {
            for (TopTools_ListOfShape::Iterator it(facesOfEdge(at)); it.More(); it.Next()) {
                const int face = faces.FindIndex(it.Value());
                if (face > 0 && static_cast<std::size_t>(face) <= shape.faceNames.size()) {
                    around.push_back(shape.faceNames[static_cast<std::size_t>(face - 1)]);
                }
            }
        }
        std::sort(around.begin(), around.end());
        around.erase(std::unique(around.begin(), around.end()), around.end());
        std::string key;
        for (const std::string& name : around) {
            key += key.empty() ? name : "|" + name;
        }
        between[static_cast<std::size_t>(i - 1)] = key;
        groups[key].push_back(i);
    }
    names.resize(between.size());
    for (auto& [key, members] : groups) {
        std::sort(members.begin(), members.end(), [&edges](int a, int b) {
            return placeKey(middleOf(TopoDS::Edge(edges(a)))) <
                   placeKey(middleOf(TopoDS::Edge(edges(b))));
        });
        for (std::size_t n = 0; n < members.size(); ++n) {
            names[static_cast<std::size_t>(members[n] - 1)] = key + "#" + std::to_string(n);
        }
    }
    return names;
}

int findFace(const NamedShape& shape, const std::string& name) {
    const auto found = std::find(shape.faceNames.begin(), shape.faceNames.end(), name);
    return found == shape.faceNames.end() ? -1 : static_cast<int>(found - shape.faceNames.begin());
}

int findEdge(const NamedShape& shape, const std::string& name) {
    const std::vector<std::string> names = edgeNames(shape);
    const auto found = std::find(names.begin(), names.end(), name);
    return found == names.end() ? -1 : static_cast<int>(found - names.begin());
}

Shape makeRegionFace(const Plane& plane, const Region& region) {
    return occt::guarded("make a region", [&] { return occt::wrap(faceOf(plane, region)); });
}

NamedShape pull(const Plane& plane, const std::vector<Region>& regions, double from, double to,
                const std::string& tag) {
    if (regions.empty()) {
        throw std::invalid_argument("pull: there is nothing to pull");
    }
    if (!std::isfinite(from) || !std::isfinite(to) || std::abs(to - from) < 1e-7) {
        throw std::invalid_argument("pull: the distance must not be zero");
    }
    return occt::guarded("pull", [&] {
        const double low = std::min(from, to);
        const double high = std::max(from, to);
        gp_Trsf placement;
        placement.SetTranslation(normalOf(plane) * low);
        const gp_Vec along = normalOf(plane) * (high - low);
        std::vector<NamedShape> solids;
        for (const Region& region : regions) {
            solids.push_back(sweepRegion(plane, region, std::abs(low) > 0.0 ? &placement : nullptr,
                                         tag, "pull", [&along](const TopoDS_Face& face) {
                                             return std::make_unique<BRepPrimAPI_MakePrism>(face,
                                                                                            along);
                                         }));
        }
        return joinAll(std::move(solids));
    });
}

NamedShape revolve(const Plane& plane, const std::vector<Region>& regions, double u1, double v1,
                   double u2, double v2, double angle, const std::string& tag) {
    if (regions.empty()) {
        throw std::invalid_argument("revolve: there is nothing to turn");
    }
    const double fullTurn = 2.0 * M_PI;
    if (!std::isfinite(angle) || std::abs(angle) < 1e-9 || std::abs(angle) > fullTurn + 1e-9) {
        throw std::invalid_argument("revolve: the angle must be between zero and a full turn");
    }
    const gp_Pnt first = toOcct(toWorld(plane, u1, v1));
    const gp_Pnt second = toOcct(toWorld(plane, u2, v2));
    if (first.Distance(second) < 1e-7) {
        throw std::invalid_argument("revolve: the axis needs two different points");
    }
    return occt::guarded("revolve", [&] {
        const gp_Ax1 axis(first, gp_Dir(gp_Vec(first, second)));
        const bool whole = std::abs(std::abs(angle) - fullTurn) < 1e-9;
        std::vector<NamedShape> solids;
        for (const Region& region : regions) {
            solids.push_back(sweepRegion(
                plane, region, nullptr, tag, "revolve",
                [&axis, whole, angle](const TopoDS_Face& face) {
                    return whole ? std::make_unique<BRepPrimAPI_MakeRevol>(face, axis)
                                 : std::make_unique<BRepPrimAPI_MakeRevol>(face, axis, angle);
                }));
        }
        return joinAll(std::move(solids));
    });
}

NamedShape combine(const NamedShape& body, const NamedShape& tool, Combination how) {
    if (body.shape.isNull() || tool.shape.isNull()) {
        throw std::invalid_argument("combine: both shapes are needed");
    }
    return occt::guarded("combine", [&] {
        const TopoDS_Shape& first = occt::unwrap(body.shape);
        const TopoDS_Shape& second = occt::unwrap(tool.shape);
        std::unique_ptr<BRepAlgoAPI_BooleanOperation> operation;
        switch (how) {
        case Combination::Join:
            operation = std::make_unique<BRepAlgoAPI_Fuse>(first, second);
            break;
        case Combination::Cut:
            operation = std::make_unique<BRepAlgoAPI_Cut>(first, second);
            break;
        case Combination::Intersect:
            operation = std::make_unique<BRepAlgoAPI_Common>(first, second);
            break;
        }
        if (!operation->IsDone() || operation->HasErrors()) {
            throw Error("combine", "the two shapes could not be combined");
        }
        // Faces left lying in one plane, and edges left in one line, become one.
        operation->SimplifyResult();
        const TopoDS_Shape result = operation->Shape();
        if (!hasSolid(result)) {
            throw Error("combine", how == Combination::Cut ? "the cut leaves nothing of the body"
                                   : how == Combination::Intersect
                                       ? "the two do not overlap"
                                       : "joining them produced no solid");
        }
        requireValid(result, "combine");
        FaceNamer namer(result);
        namer.inherit(body, *operation);
        namer.inherit(tool, *operation);
        return named(result, namer, "combined");
    });
}

NamedShape fillet(const NamedShape& body, const std::vector<int>& edges, double radius,
                  const std::string& tag) {
    if (edges.empty()) {
        throw std::invalid_argument("fillet: there are no edges to round");
    }
    if (!std::isfinite(radius) || radius <= 1e-7) {
        throw std::invalid_argument("fillet: the radius must be greater than zero");
    }
    return occt::guarded("fillet", [&] {
        const TopTools_IndexedMapOfShape all = mapOf(occt::unwrap(body.shape), TopAbs_EDGE);
        BRepFilletAPI_MakeFillet maker(occt::unwrap(body.shape));
        for (const int edge : edges) {
            maker.Add(radius, edgeAt(all, edge, "fillet"));
        }
        maker.Build();
        if (!maker.IsDone() || !hasSolid(maker.Shape())) {
            throw Error("fillet", "the radius does not fit these edges");
        }
        requireValid(maker.Shape(), "fillet");
        return namedAfterEdges(body, maker, tag);
    });
}

NamedShape chamfer(const NamedShape& body, const std::vector<int>& edges, double distance,
                   const std::string& tag) {
    if (edges.empty()) {
        throw std::invalid_argument("chamfer: there are no edges to bevel");
    }
    if (!std::isfinite(distance) || distance <= 1e-7) {
        throw std::invalid_argument("chamfer: the distance must be greater than zero");
    }
    return occt::guarded("chamfer", [&] {
        const TopTools_IndexedMapOfShape all = mapOf(occt::unwrap(body.shape), TopAbs_EDGE);
        BRepFilletAPI_MakeChamfer maker(occt::unwrap(body.shape));
        for (const int edge : edges) {
            maker.Add(distance, edgeAt(all, edge, "chamfer"));
        }
        maker.Build();
        if (!maker.IsDone() || !hasSolid(maker.Shape())) {
            throw Error("chamfer", "the distance does not fit these edges");
        }
        requireValid(maker.Shape(), "chamfer");
        return namedAfterEdges(body, maker, tag);
    });
}

NamedShape shell(const NamedShape& body, const std::vector<int>& openFaces, double thickness,
                 const std::string& tag) {
    if (!std::isfinite(thickness) || thickness <= 1e-7) {
        throw std::invalid_argument("shell: the thickness must be greater than zero");
    }
    return occt::guarded("shell", [&] {
        const TopoDS_Shape& solid = occt::unwrap(body.shape);
        const TopTools_IndexedMapOfShape faces = mapOf(solid, TopAbs_FACE);
        TopTools_ListOfShape open;
        for (const int face : openFaces) {
            if (face < 0 || face >= faces.Extent()) {
                throw std::out_of_range("shell: there is no face " + std::to_string(face));
            }
            open.Append(faces(face + 1));
        }
        BRepOffsetAPI_MakeThickSolid maker;
        // Inward, so that the outside keeps its size.
        maker.MakeThickSolidByJoin(solid, open, -thickness, 1e-6);
        maker.Build();
        if (!maker.IsDone() || !hasSolid(maker.Shape())) {
            throw Error("shell", "the walls are too thick for this body");
        }
        requireValid(maker.Shape(), "shell");
        const TopoDS_Shape result = maker.Shape();
        FaceNamer namer(result);
        namer.inherit(body, maker);
        for (int i = 1; i <= faces.Extent(); ++i) {
            const TopTools_ListOfShape& made = maker.Generated(faces(i));
            if (!made.IsEmpty()) {
                namer.offerAll(made,
                               tag + "/inner/" +
                                   digest(body.faceNames.at(static_cast<std::size_t>(i - 1))));
            }
        }
        return named(result, namer, tag);
    });
}

std::optional<Axis> axisOf(const Shape& part) {
    if (part.isNull()) {
        return std::nullopt;
    }
    return occt::guarded("find the axis of a shape", [&]() -> std::optional<Axis> {
        const TopoDS_Shape& shape = occt::unwrap(part);
        const auto axis = [](const gp_Pnt& at, const gp_Dir& along) {
            return Axis{{at.X(), at.Y(), at.Z()}, {along.X(), along.Y(), along.Z()}};
        };
        if (shape.ShapeType() == TopAbs_EDGE) {
            const BRepAdaptor_Curve curve(TopoDS::Edge(shape));
            switch (curve.GetType()) {
            case GeomAbs_Line: {
                const gp_Lin line = curve.Line();
                // Through the edge's start, which is where the user sees the line.
                return axis(curve.Value(curve.FirstParameter()), line.Direction());
            }
            case GeomAbs_Circle:
                return axis(curve.Circle().Location(), curve.Circle().Axis().Direction());
            default:
                return std::nullopt;
            }
        }
        if (shape.ShapeType() == TopAbs_FACE) {
            const BRepAdaptor_Surface surface(TopoDS::Face(shape));
            switch (surface.GetType()) {
            case GeomAbs_Plane: {
                std::optional<Plane> plane = planeOfFace(part);
                if (!plane.has_value()) {
                    return std::nullopt;
                }
                GProp_GProps properties;
                BRepGProp::SurfaceProperties(shape, properties);
                const gp_Pnt middle = properties.CentreOfMass();
                return Axis{{middle.X(), middle.Y(), middle.Z()}, plane->normal};
            }
            case GeomAbs_Cylinder:
                return axis(surface.Cylinder().Location(), surface.Cylinder().Axis().Direction());
            case GeomAbs_Cone:
                return axis(surface.Cone().Location(), surface.Cone().Axis().Direction());
            case GeomAbs_Sphere:
                return axis(surface.Sphere().Location(), surface.Sphere().Position().Direction());
            case GeomAbs_Torus:
                return axis(surface.Torus().Location(), surface.Torus().Axis().Direction());
            default:
                return std::nullopt;
            }
        }
        return std::nullopt;
    });
}

namespace {

// A copy of a shape placed by `placement`, its faces named after those they were copied from.
NamedShape copied(const NamedShape& shape, const gp_Trsf& placement, const std::string& prefix,
                  const char* operation) {
    if (shape.shape.isNull()) {
        throw std::invalid_argument(std::string(operation) + ": there is nothing to copy");
    }
    return occt::guarded(operation, [&] {
        const TopoDS_Shape& original = occt::unwrap(shape.shape);
        // A copy of the geometry, not a placement of the same geometry: a mirror image cannot
        // be a placement, and a copy behaves the same in later operations whatever made it.
        BRepBuilderAPI_Transform maker(original, placement, Standard_True);
        if (!maker.IsDone()) {
            throw Error(operation, "the shape could not be copied");
        }
        const TopoDS_Shape result = maker.Shape();
        requireValid(result, operation);
        FaceNamer namer(result);
        const TopTools_IndexedMapOfShape faces = mapOf(original, TopAbs_FACE);
        for (int i = 1; i <= faces.Extent(); ++i) {
            namer.offer(maker.ModifiedShape(faces(i)),
                        prefix + "/" + shape.faceNames.at(static_cast<std::size_t>(i - 1)));
        }
        return named(result, namer, prefix);
    });
}

} // namespace

NamedShape moved(const NamedShape& shape, const Vector& by, const std::string& prefix) {
    if (!std::isfinite(by.x) || !std::isfinite(by.y) || !std::isfinite(by.z)) {
        throw std::invalid_argument("moved: the distance is not finite");
    }
    gp_Trsf placement;
    placement.SetTranslation(gp_Vec(by.x, by.y, by.z));
    return copied(shape, placement, prefix, "move");
}

NamedShape turned(const NamedShape& shape, const Axis& axis, double angle,
                  const std::string& prefix) {
    const Vector& d = axis.direction;
    const double length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (!std::isfinite(angle) || !std::isfinite(length) || length < 1e-12) {
        throw std::invalid_argument("turned: the axis or the angle is not usable");
    }
    gp_Trsf placement;
    placement.SetRotation(gp_Ax1(gp_Pnt(axis.origin.x, axis.origin.y, axis.origin.z),
                                 gp_Dir(axis.direction.x, axis.direction.y, axis.direction.z)),
                          angle);
    return copied(shape, placement, prefix, "turn");
}

NamedShape mirrored(const NamedShape& shape, const Plane& plane, const std::string& prefix) {
    gp_Trsf placement;
    placement.SetMirror(gp_Ax2(gp_Pnt(plane.origin.x, plane.origin.y, plane.origin.z),
                               gp_Dir(plane.normal.x, plane.normal.y, plane.normal.z)));
    return copied(shape, placement, prefix, "mirror");
}

} // namespace lcad::kernel
