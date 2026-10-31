// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Modeling.h>

#include <lcad/kernel/occt/Guard.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Defeaturing.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeShape.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <ShapeBuild_ReShape.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_Solid.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include <Geom_Surface.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
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

    // Offers each face that has been offered nothing yet the name `choose` gives it, if any.
    void offerUnnamed(const std::function<std::string(const TopoDS_Face&)>& choose) {
        for (int i = 1; i <= m_faces.Extent(); ++i) {
            std::vector<std::string>& offered = m_candidates[static_cast<std::size_t>(i - 1)];
            if (offered.empty()) {
                std::string name = choose(TopoDS::Face(m_faces(i)));
                if (!name.empty()) {
                    offered.push_back(std::move(name));
                }
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

double solidVolume(const TopoDS_Shape& shape) {
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    return properties.Mass();
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

std::vector<NamedShape> separated(const NamedShape& shape) {
    const TopoDS_Shape& whole = occt::unwrap(shape.shape);
    if (whole.IsNull()) {
        return {shape};
    }
    const TopTools_IndexedMapOfShape solids = mapOf(whole, TopAbs_SOLID);
    const TopTools_IndexedMapOfShape faces = mapOf(whole, TopAbs_FACE);
    if (solids.Extent() < 2 || faces.Extent() != static_cast<int>(shape.faceNames.size())) {
        return {shape};
    }
    std::vector<NamedShape> pieces;
    int facesOfSolids = 0;
    for (int i = 1; i <= solids.Extent(); ++i) {
        NamedShape piece{occt::wrap(solids(i)), {}};
        const TopTools_IndexedMapOfShape own = mapOf(solids(i), TopAbs_FACE);
        for (int j = 1; j <= own.Extent(); ++j) {
            const int index = faces.FindIndex(own(j));
            if (index == 0) {
                return {shape};
            }
            piece.faceNames.push_back(shape.faceNames[static_cast<std::size_t>(index) - 1]);
        }
        facesOfSolids += own.Extent();
        pieces.push_back(std::move(piece));
    }
    if (facesOfSolids != faces.Extent()) {
        return {shape};
    }
    const auto lowest = [](const NamedShape& piece) {
        return *std::min_element(piece.faceNames.begin(), piece.faceNames.end());
    };
    std::sort(pieces.begin(), pieces.end(), [&lowest](const NamedShape& a, const NamedShape& b) {
        return lowest(a) < lowest(b);
    });
    return pieces;
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

namespace {

// Tilts the sides of a pulled solid (the faces named `tag`/side/...) about the plane it was
// pulled from, so that it narrows along `direction` for a positive angle.
NamedShape drafted(const NamedShape& solid, const Plane& plane, const gp_Dir& direction,
                   double angle, const std::string& tag) {
    const TopoDS_Shape& shape = occt::unwrap(solid.shape);
    BRepOffsetAPI_DraftAngle draft(shape);
    const gp_Pln neutral(toOcct(plane.origin),
                         gp_Dir(plane.normal.x, plane.normal.y, plane.normal.z));
    const TopTools_IndexedMapOfShape faces = mapOf(shape, TopAbs_FACE);
    const std::string side = tag + "/side/";
    for (int i = 1; i <= faces.Extent(); ++i) {
        if (solid.faceNames.at(static_cast<std::size_t>(i - 1)).rfind(side, 0) != 0) {
            continue;
        }
        draft.Add(TopoDS::Face(faces(i)), direction, angle, neutral);
        if (!draft.AddDone()) {
            throw Error("pull", "the taper is too steep for this outline");
        }
    }
    draft.Build();
    if (!draft.IsDone() || !hasSolid(draft.Shape())) {
        throw Error("pull", "the taper is too steep for this outline");
    }
    requireValid(draft.Shape(), "pull");
    // A draft says what became of each face one by one, not as a list.
    FaceNamer namer(draft.Shape());
    for (int i = 1; i <= faces.Extent(); ++i) {
        namer.offer(draft.ModifiedShape(faces(i)),
                    solid.faceNames.at(static_cast<std::size_t>(i - 1)));
    }
    return named(draft.Shape(), namer, tag);
}

} // namespace

NamedShape pull(const Plane& plane, const std::vector<Region>& regions, double from, double to,
                const std::string& tag, double taper) {
    if (regions.empty()) {
        throw std::invalid_argument("pull: there is nothing to pull");
    }
    if (!std::isfinite(from) || !std::isfinite(to) || std::abs(to - from) < 1e-7) {
        throw std::invalid_argument("pull: the distance must not be zero");
    }
    if (!std::isfinite(taper) || std::abs(taper) >= M_PI / 2.0 - 1e-6) {
        throw std::invalid_argument("pull: the taper must be less than a right angle");
    }
    if (taper != 0.0 && from != 0.0 && to != 0.0) {
        throw std::invalid_argument("pull: a taper needs a pull to one side");
    }
    if (taper != 0.0) {
        NamedShape straight = pull(plane, regions, from, to, tag);
        const double way =
            std::abs(to) > std::abs(from) ? (to > 0.0 ? 1.0 : -1.0) : (from > 0.0 ? 1.0 : -1.0);
        const gp_Dir along(plane.normal.x * way, plane.normal.y * way, plane.normal.z * way);
        return occt::guarded("pull", [&] { return drafted(straight, plane, along, taper, tag); });
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

NamedShape pullTo(const Plane& plane, const std::vector<Region>& regions, const Plane& limit,
                  const std::string& tag) {
    if (regions.empty()) {
        throw std::invalid_argument("pull: there is nothing to pull");
    }
    return occt::guarded("pull", [&] {
        const gp_Vec n(plane.normal.x, plane.normal.y, plane.normal.z);
        const gp_Vec m(limit.normal.x, limit.normal.y, limit.normal.z);
        const double facing = n.Dot(m);
        if (std::abs(facing) < 1e-6) {
            throw Error("pull", "the face runs along the pull, so it cannot end it");
        }
        // How far along the normal each corner of the outlines is from the limit.
        double nearest = std::numeric_limits<double>::infinity();
        double farthest = -std::numeric_limits<double>::infinity();
        const gp_Pnt on = toOcct(limit.origin);
        for (const Region& region : regions) {
            const TopoDS_Face face = faceOf(plane, region);
            for (TopExp_Explorer it(face, TopAbs_VERTEX); it.More(); it.Next()) {
                const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(it.Current()));
                const double t = gp_Vec(p, on).Dot(m) / facing;
                nearest = std::min(nearest, t);
                farthest = std::max(farthest, t);
            }
        }
        if (nearest * farthest <= 1e-9 || std::abs(nearest) < 1e-7) {
            throw Error("pull", "the face passes through the outline, so there is nothing to pull "
                                "up to it");
        }
        // A straight pull through the plane, cut back to it.
        const double reach =
            (farthest > 0.0 ? farthest : nearest) * 1.01 + (farthest > 0.0 ? 1.0 : -1.0);
        if (std::abs(nearest - farthest) < 1e-9) {
            // Parallel: exactly that far.
            return pull(plane, regions, 0.0, nearest, tag);
        }
        const NamedShape straight = pull(plane, regions, 0.0, reach, tag);
        const TopoDS_Face boundary = BRepBuilderAPI_MakeFace(gp_Pln(on, gp_Dir(m))).Face();
        const BRepPrimAPI_MakeHalfSpace keep(boundary, toOcct(plane.origin));
        BRepAlgoAPI_Common cut(occt::unwrap(straight.shape), keep.Solid());
        cut.Build();
        if (!cut.IsDone() || cut.HasErrors() || !hasSolid(cut.Shape())) {
            throw Error("pull", "the solid could not be cut back to the face");
        }
        requireValid(cut.Shape(), "pull");
        FaceNamer namer(cut.Shape());
        namer.inherit(straight, cut);
        // The new end is where the half space bounded it.
        const std::string key = keyOf(regions.front());
        for (TopExp_Explorer it(keep.Solid(), TopAbs_FACE); it.More(); it.Next()) {
            namer.offerAll(cut.Modified(it.Current()), tag + "/end/" + key);
        }
        return named(cut.Shape(), namer, tag);
    });
}

NamedShape revolve(const Plane& plane, const std::vector<Region>& regions, const Axis& axis,
                   double angle, const std::string& tag) {
    const Vector& d = axis.direction;
    if (std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) < 1e-12) {
        throw std::invalid_argument("revolve: the axis needs a direction");
    }
    // Two points of the axis, in the plane's own coordinates and height, give the same turn as
    // an axis drawn in the plane.
    const Point a = axis.origin;
    const Point b{a.x + d.x, a.y + d.y, a.z + d.z};
    const PlaneCoordinates first = toLocal(plane, a);
    const PlaneCoordinates second = toLocal(plane, b);
    if (std::abs(first.w) > 1e-9 || std::abs(second.w) > 1e-9) {
        // Out of the plane: turned about the axis as it is in space.
        if (regions.empty()) {
            throw std::invalid_argument("revolve: there is nothing to turn");
        }
        const double fullTurn = 2.0 * M_PI;
        if (!std::isfinite(angle) || std::abs(angle) < 1e-9 || std::abs(angle) > fullTurn + 1e-9) {
            throw std::invalid_argument("revolve: the angle must be between zero and a full turn");
        }
        return occt::guarded("revolve", [&] {
            const gp_Ax1 line(gp_Pnt(a.x, a.y, a.z), gp_Dir(d.x, d.y, d.z));
            const bool whole = std::abs(std::abs(angle) - fullTurn) < 1e-9;
            std::vector<NamedShape> solids;
            for (const Region& region : regions) {
                solids.push_back(sweepRegion(
                    plane, region, nullptr, tag, "revolve",
                    [&line, whole, angle](const TopoDS_Face& face) {
                        return whole ? std::make_unique<BRepPrimAPI_MakeRevol>(face, line)
                                     : std::make_unique<BRepPrimAPI_MakeRevol>(face, line, angle);
                    }));
            }
            return joinAll(std::move(solids));
        });
    }
    return revolve(plane, regions, first.u, first.v, second.u, second.v, angle, tag);
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
        // Where walls that thick cannot fit, some versions of OCCT still return a solid, its
        // walls passing through each other. A hollowed solid is smaller than the solid, and
        // lies inside it.
        const double was = solidVolume(solid);
        const double now = solidVolume(maker.Shape());
        bool hollowed = now > 1e-9 * was && now < was * (1.0 - 1e-9);
        if (hollowed) {
            BRepAlgoAPI_Common inside(maker.Shape(), solid);
            inside.Build();
            hollowed = inside.IsDone() && std::abs(solidVolume(inside.Shape()) - now) <= 1e-6 * was;
        }
        if (!hollowed) {
            throw Error("shell", "the walls are too thick for this body");
        }
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

NamedShape holeTool(const Point& start, const Vector& direction, const HoleShape& shape,
                    double through, const std::string& tag, const std::string& name) {
    const double length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                    direction.z * direction.z);
    if (!std::isfinite(length) || length < 1e-12) {
        throw std::invalid_argument("hole: it needs a direction");
    }
    const double r = shape.diameter / 2.0;
    if (!std::isfinite(r) || r <= 1e-7) {
        throw std::invalid_argument("the diameter has to be more than zero");
    }
    const bool isThrough = through > 0.0;
    const double depth = isThrough ? through : shape.depth;
    if (!std::isfinite(depth) || depth <= 1e-7) {
        throw std::invalid_argument("the depth has to be more than zero");
    }
    const gp_Dir down(direction.x / length, direction.y / length, direction.z / length);
    // A plane that holds the hole's axis: u across it, v up it, out of the hole.
    gp_Dir across = std::abs(down.X()) < 0.9 ? gp_Dir(1.0, 0.0, 0.0) : gp_Dir(0.0, 1.0, 0.0);
    across = gp_Dir(gp_Vec(across) - gp_Vec(down) * gp_Vec(across).Dot(gp_Vec(down)));
    const gp_Dir up = down.Reversed();
    const gp_Dir normal = across.Crossed(up);
    Plane section;
    section.origin = start;
    section.normal = {normal.X(), normal.Y(), normal.Z()};
    section.xDirection = {across.X(), across.Y(), across.Z()};

    // The outline of half the hole, from its middle at the top round to its middle at the
    // bottom, as (u, v) and the name of the curve that ends there.
    std::vector<std::pair<std::pair<double, double>, std::string>> outline;
    outline.push_back({{0.0, 0.0}, ""});
    const double head = shape.headDiameter / 2.0;
    if (shape.kind == HoleKind::Counterbore) {
        if (!std::isfinite(head) || head <= r + 1e-7) {
            throw std::invalid_argument("the counterbore has to be wider than the hole");
        }
        if (!std::isfinite(shape.headDepth) || shape.headDepth <= 1e-7 ||
            shape.headDepth >= depth - 1e-7) {
            throw std::invalid_argument(
                "the counterbore has to be deeper than zero and less deep than the hole");
        }
        outline.push_back({{head, 0.0}, "top"});
        outline.push_back({{head, -shape.headDepth}, "counterbore"});
        outline.push_back({{r, -shape.headDepth}, "shoulder"});
    } else if (shape.kind == HoleKind::Countersink) {
        const double half = shape.countersinkAngle / 2.0 * M_PI / 180.0;
        if (!std::isfinite(head) || head <= r + 1e-7) {
            throw std::invalid_argument("the countersink has to be wider than the hole");
        }
        if (!std::isfinite(half) || half <= 1e-3 || half >= M_PI / 2.0 - 1e-3) {
            throw std::invalid_argument("the countersink angle has to be between 0 and 180");
        }
        const double sinkDepth = (head - r) / std::tan(half);
        if (sinkDepth >= depth - 1e-7) {
            throw std::invalid_argument("the countersink has to be less deep than the hole");
        }
        outline.push_back({{head, 0.0}, "top"});
        outline.push_back({{r, -sinkDepth}, "countersink"});
    } else {
        outline.push_back({{r, 0.0}, "top"});
    }
    outline.push_back({{r, -depth}, "bore"});
    const double tip = shape.tipAngle / 2.0 * M_PI / 180.0;
    if (!isThrough && std::isfinite(tip) && tip > 1e-3 && tip < M_PI / 2.0 - 1e-3) {
        outline.push_back({{0.0, -depth - r / std::tan(tip)}, "tip"});
    } else {
        outline.push_back({{0.0, -depth}, "floor"});
    }

    Region region;
    for (std::size_t i = 1; i <= outline.size(); ++i) {
        const auto& from = outline[i - 1].first;
        const auto& to = outline[i % outline.size()].first;
        const std::string part = i < outline.size() ? outline[i].second : std::string("axis");
        region.outer.push_back({makeSegment(toWorld(section, from.first, from.second),
                                            toWorld(section, to.first, to.second)),
                                part + "@" + name});
    }
    return revolve(section, {region}, 0.0, 0.0, 0.0, 1.0, 2.0 * M_PI, tag);
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

namespace {

// Offsets chosen faces of a solid, each by a distance of its own, while the other faces stay
// where they are: OCCT's offset algorithm with no offset of its own and one set on each of
// those faces. Its history then says what became of every face.
class FaceOffsetter : public BRepOffsetAPI_MakeOffsetShape {
public:
    void perform(const TopoDS_Shape& solid,
                 const std::vector<std::pair<TopoDS_Face, double>>& offsets) {
        myLastUsedAlgo = OffsetAlgo_JOIN;
        myOffsetShape.Initialize(solid, 0.0, 1e-7, BRepOffset_Skin, Standard_False, Standard_False,
                                 GeomAbs_Intersection);
        for (const auto& [face, distance] : offsets) {
            myOffsetShape.SetOffsetOnFace(face, distance);
        }
        myOffsetShape.MakeOffsetShape();
        if (!myOffsetShape.IsDone()) {
            NotDone();
            return;
        }
        myShape = myOffsetShape.Shape();
        Done();
    }
};

// A shape made of closed shells as a solid with its faces facing out; any other as it is.
TopoDS_Shape asSolid(const TopoDS_Shape& shape) {
    if (shape.IsNull() || hasSolid(shape)) {
        return shape;
    }
    TopExp_Explorer shell(shape, TopAbs_SHELL);
    if (!shell.More()) {
        return shape;
    }
    BRepBuilderAPI_MakeSolid maker;
    for (; shell.More(); shell.Next()) {
        if (!BRep_Tool::IsClosed(shell.Current())) {
            return shape;
        }
        maker.Add(TopoDS::Shell(shell.Current()));
    }
    if (!maker.IsDone()) {
        return shape;
    }
    ShapeFix_Solid fix(maker.Solid());
    fix.Perform();
    return fix.Solid();
}

TopoDS_Face faceAt(const TopTools_IndexedMapOfShape& faces, int index, const char* operation) {
    if (index < 0 || index >= faces.Extent()) {
        throw std::out_of_range(std::string(operation) + ": there is no face " +
                                std::to_string(index));
    }
    return TopoDS::Face(faces(index + 1));
}

void requireSolid(const NamedShape& body, const char* operation) {
    if (body.shape.isNull()) {
        throw std::invalid_argument(std::string(operation) + ": there is no body");
    }
    if (!hasSolid(occt::unwrap(body.shape))) {
        throw Error(operation, "the body is not a solid");
    }
}

// Offsets faces by the numbers, each by its own distance.
NamedShape offsetBy(const NamedShape& body, const std::vector<std::pair<int, double>>& offsets,
                    const std::string& tag, const char* operation) {
    requireSolid(body, operation);
    return occt::guarded(operation, [&] {
        const TopoDS_Shape& solid = occt::unwrap(body.shape);
        const TopTools_IndexedMapOfShape faces = mapOf(solid, TopAbs_FACE);
        std::vector<std::pair<TopoDS_Face, double>> chosen;
        bool outward = true;
        bool inward = true;
        for (const auto& [index, distance] : offsets) {
            chosen.emplace_back(faceAt(faces, index, operation), distance);
            outward = outward && distance > 0.0;
            inward = inward && distance < 0.0;
        }
        FaceOffsetter maker;
        maker.perform(solid, chosen);
        const auto cannot = [operation] {
            return Error(operation, "the faces around them cannot be made to meet them there");
        };
        if (!maker.IsDone()) {
            throw cannot();
        }
        const TopoDS_Shape result = asSolid(maker.Shape());
        if (!hasSolid(result) || !BRepCheck_Analyzer(result).IsValid()) {
            throw cannot();
        }
        // Moving faces out adds material and moving them in takes it away; a result that
        // does otherwise has folded over itself.
        const double was = solidVolume(solid);
        const double now = solidVolume(result);
        if ((outward && now <= was) || (inward && now >= was) || now <= 0.0) {
            throw cannot();
        }
        FaceNamer namer(result);
        namer.inherit(body, maker);
        // The offset algorithm reports a face it moved as made from the face, not as changed.
        for (int i = 1; i <= faces.Extent(); ++i) {
            namer.offerAll(maker.Generated(faces(i)),
                           body.faceNames.at(static_cast<std::size_t>(i - 1)));
        }
        const std::vector<std::string> edgesByName = edgeNames(body);
        const TopTools_IndexedMapOfShape edges = mapOf(solid, TopAbs_EDGE);
        for (int i = 1; i <= edges.Extent(); ++i) {
            const TopTools_ListOfShape& made = maker.Generated(edges(i));
            if (!made.IsEmpty()) {
                namer.offerAll(made, tag + "/edge/" +
                                         digest(edgesByName[static_cast<std::size_t>(i - 1)]));
            }
        }
        return named(result, namer, tag);
    });
}

// The same shape in another place, each face keeping its name: the faces are the same faces,
// placed elsewhere.
NamedShape placedAt(const NamedShape& shape, const gp_Trsf& placement) {
    return NamedShape{occt::wrap(occt::unwrap(shape.shape).Moved(TopLoc_Location(placement))),
                      shape.faceNames};
}

// What is left of `body` with `tool` taken away, or nothing if nothing is left.
std::optional<NamedShape> remainderOf(const NamedShape& body, const NamedShape& tool) {
    try {
        NamedShape left = combine(body, tool, Combination::Cut);
        if (solidVolume(occt::unwrap(left.shape)) <=
            1e-9 * std::max(1.0, solidVolume(occt::unwrap(body.shape)))) {
            return std::nullopt;
        }
        return left;
    } catch (const Error&) {
        return std::nullopt;
    }
}

} // namespace

NamedShape offsetFaces(const NamedShape& body, const std::vector<int>& faces, double distance,
                       const std::string& tag) {
    if (faces.empty()) {
        throw std::invalid_argument("offsetFaces: there are no faces to offset");
    }
    if (!std::isfinite(distance) || std::abs(distance) < 1e-7) {
        throw std::invalid_argument("offsetFaces: the distance must not be zero");
    }
    std::vector<std::pair<int, double>> offsets;
    for (const int face : faces) {
        offsets.emplace_back(face, distance);
    }
    return offsetBy(body, offsets, tag, "offset faces");
}

NamedShape removeFaces(const NamedShape& body, const std::vector<int>& faces,
                       const std::string& tag) {
    if (faces.empty()) {
        throw std::invalid_argument("removeFaces: there are no faces to delete");
    }
    requireSolid(body, "delete faces");
    return occt::guarded("delete faces", [&] {
        const TopoDS_Shape& solid = occt::unwrap(body.shape);
        const TopTools_IndexedMapOfShape all = mapOf(solid, TopAbs_FACE);
        TopTools_ListOfShape chosen;
        for (const int face : faces) {
            chosen.Append(faceAt(all, face, "delete faces"));
        }
        BRepAlgoAPI_Defeaturing maker;
        maker.SetShape(solid);
        maker.AddFacesToRemove(chosen);
        maker.SetToFillHistory(Standard_True);
        maker.Build();
        const auto cannot = [] {
            return Error("delete faces",
                         "the faces around them cannot be extended to close the gap");
        };
        if (!maker.IsDone() || maker.HasErrors()) {
            throw cannot();
        }
        // Where it cannot take a face out, OCCT leaves the solid as it was and says nothing.
        for (TopTools_ListOfShape::Iterator it(chosen); it.More(); it.Next()) {
            if (!maker.IsDeleted(it.Value())) {
                throw cannot();
            }
        }
        const TopoDS_Shape result = maker.Shape();
        if (!hasSolid(result) || !BRepCheck_Analyzer(result).IsValid()) {
            throw cannot();
        }
        FaceNamer namer(result);
        namer.inherit(body, maker);
        return named(result, namer, tag);
    });
}

NamedShape moveFaces(const NamedShape& body, const std::vector<int>& faces, const Vector& by,
                     const std::string& tag) {
    if (faces.empty()) {
        throw std::invalid_argument("moveFaces: there are no faces to move");
    }
    if (!std::isfinite(by.x) || !std::isfinite(by.y) || !std::isfinite(by.z) ||
        std::sqrt(by.x * by.x + by.y * by.y + by.z * by.z) < 1e-7) {
        throw std::invalid_argument("moveFaces: the distance must not be zero");
    }
    requireSolid(body, "move faces");
    const TopTools_IndexedMapOfShape all = mapOf(occt::unwrap(body.shape), TopAbs_FACE);
    // Flat faces: a plane moved by `by` is the plane moved along its normal by the part of
    // `by` that lies along it.
    std::vector<std::pair<int, double>> offsets;
    bool flat = true;
    occt::guarded("move faces", [&] {
        for (const int index : faces) {
            const TopoDS_Face face = faceAt(all, index, "move faces");
            const BRepAdaptor_Surface surface(face);
            if (surface.GetType() != GeomAbs_Plane) {
                flat = false;
                return;
            }
            gp_Dir normal = surface.Plane().Axis().Direction();
            if (face.Orientation() == TopAbs_REVERSED) {
                normal.Reverse();
            }
            const double along = by.x * normal.X() + by.y * normal.Y() + by.z * normal.Z();
            if (std::abs(along) > 1e-9) {
                offsets.emplace_back(index, along);
            }
        }
    });
    if (flat) {
        if (offsets.empty()) {
            throw Error("move faces", "flat faces moved within their own planes do not change");
        }
        return offsetBy(body, offsets, tag, "move faces");
    }

    // Otherwise the material the faces bound is taken out, moved, and put back.
    NamedShape without;
    try {
        without = removeFaces(body, faces, tag);
    } catch (const Error&) {
        throw Error("move faces",
                    "taking them out leaves a gap that the faces around them cannot close");
    }
    gp_Trsf placement;
    placement.SetTranslation(gp_Vec(by.x, by.y, by.z));
    // Material the faces add, such as a boss, and material they take away, such as a hole.
    const std::optional<NamedShape> added = remainderOf(body, without);
    const std::optional<NamedShape> taken = remainderOf(without, body);
    if (!added.has_value() && !taken.has_value()) {
        throw Error("move faces", "the faces bound no material that could be moved");
    }
    NamedShape result = without;
    if (added.has_value()) {
        result = combine(result, placedAt(*added, placement), Combination::Join);
    }
    if (taken.has_value()) {
        result = combine(result, placedAt(*taken, placement), Combination::Cut);
    }
    return result;
}

NamedShape repaired(const NamedShape& body, const std::string& tag, double gap) {
    if (body.shape.isNull()) {
        throw std::invalid_argument("repaired: there is no body");
    }
    if (!std::isfinite(gap) || gap <= 0.0) {
        throw std::invalid_argument("repaired: the gap must be greater than zero");
    }
    return occt::guarded("repair", [&] {
        const TopoDS_Shape& original = occt::unwrap(body.shape);
        const TopTools_IndexedMapOfShape faces = mapOf(original, TopAbs_FACE);
        // What each face of the body has become so far.
        std::vector<TopoDS_Shape> became;
        for (int i = 1; i <= faces.Extent(); ++i) {
            became.push_back(faces(i));
        }

        TopoDS_Shape current = original;
        // Faces that do not join up into a closed solid are sewn together.
        bool open = !hasSolid(current);
        for (TopExp_Explorer shell(current, TopAbs_SHELL); shell.More() && !open; shell.Next()) {
            ShapeAnalysis_FreeBounds bounds(shell.Current(), Standard_False, Standard_False);
            open = TopExp_Explorer(bounds.GetClosedWires(), TopAbs_EDGE).More() ||
                   TopExp_Explorer(bounds.GetOpenWires(), TopAbs_EDGE).More();
        }
        if (open) {
            BRepBuilderAPI_Sewing sewing(gap);
            sewing.Add(current);
            sewing.Perform();
            for (TopoDS_Shape& face : became) {
                if (sewing.IsModified(face)) {
                    face = sewing.Modified(face);
                }
            }
            current = asSolid(sewing.SewedShape());
            if (!hasSolid(current)) {
                throw Error("repair",
                            "the faces do not close up into a solid: " +
                                std::to_string(sewing.NbFreeEdges()) +
                                (sewing.NbFreeEdges() == 1 ? " edge borders" : " edges border") +
                                " only one face, with gaps wider than it closes");
            }
        }

        // Edges, wires, and tolerances.
        ShapeFix_Shape fix(current);
        fix.SetPrecision(1e-7);
        fix.SetMaxTolerance(std::max(1e-3, gap));
        fix.Perform();
        current = fix.Shape();
        for (TopoDS_Shape& face : became) {
            face = fix.Context()->Value(face);
        }
        // A solid that faces inward.
        if (solidVolume(current) < 0.0) {
            current.Reverse();
        }
        if (!hasSolid(current) || !BRepCheck_Analyzer(current).IsValid()) {
            throw Error("repair", "it could not be made into a valid solid");
        }

        FaceNamer namer(current);
        std::map<const Geom_Surface*, std::string> bySurface;
        for (std::size_t i = 0; i < became.size(); ++i) {
            const std::string& name = body.faceNames.at(i);
            bySurface.emplace(
                BRep_Tool::Surface(TopoDS::Face(faces(static_cast<int>(i) + 1))).get(), name);
            if (became[i].IsNull()) {
                continue;
            }
            TopTools_ListOfShape now;
            for (TopExp_Explorer face(became[i], TopAbs_FACE); face.More(); face.Next()) {
                now.Append(face.Current());
            }
            namer.offerAll(now, name);
        }
        // A face rebuilt on the surface it had keeps its name too.
        namer.offerUnnamed([&bySurface](const TopoDS_Face& face) {
            const auto found = bySurface.find(BRep_Tool::Surface(face).get());
            return found == bySurface.end() ? std::string() : found->second;
        });
        return named(current, namer, tag);
    });
}

} // namespace lcad::kernel
