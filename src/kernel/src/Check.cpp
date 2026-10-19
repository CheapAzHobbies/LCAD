// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Check.h>

#include <lcad/kernel/occt/Guard.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <BOPAlgo_ArgumentAnalyzer.hxx>
#include <BOPAlgo_CheckResult.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_ListOfStatus.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace lcad::kernel {

namespace {

// The kinds of problem a check reports, each with what it says about how many there are.
enum class Kind {
    Corner,
    Edge,
    Outline,
    Skin,
    Tolerance,
};

struct Found {
    std::set<int> faces;
    std::set<int> edges;
    int count = 0;
};

Kind kindOf(BRepCheck_Status status, TopAbs_ShapeEnum type) {
    switch (status) {
    case BRepCheck_InvalidPointOnCurve:
    case BRepCheck_InvalidPointOnCurveOnSurface:
    case BRepCheck_InvalidPointOnSurface:
        return Kind::Corner;
    case BRepCheck_InvalidToleranceValue:
        return Kind::Tolerance;
    default:
        break;
    }
    switch (type) {
    case TopAbs_VERTEX:
        return Kind::Corner;
    case TopAbs_EDGE:
        return Kind::Edge;
    case TopAbs_WIRE:
    case TopAbs_FACE:
        return Kind::Outline;
    default:
        return Kind::Skin;
    }
}

std::string countOf(int count, const char* one, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

std::string sentenceFor(Kind kind, const Found& found) {
    switch (kind) {
    case Kind::Corner:
        return countOf(found.count, "corner lies", "corners lie") +
               " off the edges or faces they belong to.";
    case Kind::Edge:
        return countOf(found.count, "edge does", "edges do") + " not lie on the faces they bound.";
    case Kind::Outline:
        return countOf(static_cast<int>(found.faces.size()), "face has an outline",
                       "faces have outlines") +
               " that is broken or crosses itself.";
    case Kind::Skin:
        return "Its faces are not joined into one consistent skin.";
    case Kind::Tolerance:
        return countOf(found.count, "part has a tolerance", "parts have tolerances") +
               " out of range.";
    }
    return {};
}

double signedVolume(const TopoDS_Shape& solid) {
    GProp_GProps properties;
    BRepGProp::VolumeProperties(solid, properties);
    return properties.Mass();
}

} // namespace

std::vector<GeometryProblem> checkGeometry(const Shape& shape) {
    if (shape.isNull()) {
        throw std::invalid_argument("checkGeometry: the shape is null");
    }
    return occt::guarded("check geometry", [&] {
        const TopoDS_Shape& whole = occt::unwrap(shape);
        TopTools_IndexedMapOfShape faces;
        TopTools_IndexedMapOfShape edges;
        TopExp::MapShapes(whole, TopAbs_FACE, faces);
        TopExp::MapShapes(whole, TopAbs_EDGE, edges);
        TopTools_IndexedDataMapOfShapeListOfShape facesOfEdge;
        TopTools_IndexedDataMapOfShapeListOfShape edgesOfVertex;
        TopTools_IndexedDataMapOfShapeListOfShape facesOfWire;
        TopExp::MapShapesAndUniqueAncestors(whole, TopAbs_EDGE, TopAbs_FACE, facesOfEdge);
        TopExp::MapShapesAndUniqueAncestors(whole, TopAbs_VERTEX, TopAbs_EDGE, edgesOfVertex);
        TopExp::MapShapesAndUniqueAncestors(whole, TopAbs_WIRE, TopAbs_FACE, facesOfWire);

        std::vector<GeometryProblem> problems;
        const auto add = [&problems](std::string description, const std::set<int>& onFaces,
                                     const std::set<int>& onEdges) {
            problems.push_back({std::move(description),
                                std::vector<int>(onFaces.begin(), onFaces.end()),
                                std::vector<int>(onEdges.begin(), onEdges.end())});
        };

        // What the shape's own checks find, grouped by kind.
        std::map<Kind, Found> found;
        const BRepCheck_Analyzer analyzer(whole);
        const bool valid = analyzer.IsValid();
        if (!valid) {
            const auto statusesOf = [&analyzer](const TopoDS_Shape& part) {
                std::vector<BRepCheck_Status> statuses;
                const Handle(BRepCheck_Result) result = analyzer.Result(part);
                if (result.IsNull()) {
                    return statuses;
                }
                for (BRepCheck_ListOfStatus::Iterator it(result->Status()); it.More(); it.Next()) {
                    statuses.push_back(it.Value());
                }
                for (result->InitContextIterator(); result->MoreShapeInContext();
                     result->NextShapeInContext()) {
                    for (BRepCheck_ListOfStatus::Iterator it(result->StatusOnShape()); it.More();
                         it.Next()) {
                        statuses.push_back(it.Value());
                    }
                }
                return statuses;
            };
            const auto record = [&](const TopoDS_Shape& part, TopAbs_ShapeEnum type,
                                    const std::vector<int>& onFaces,
                                    const std::vector<int>& onEdges) {
                std::set<Kind> kinds;
                for (const BRepCheck_Status status : statusesOf(part)) {
                    // A free edge is reported below, by what it means for the solid.
                    if (status != BRepCheck_NoError && status != BRepCheck_FreeEdge) {
                        kinds.insert(kindOf(status, type));
                    }
                }
                for (const Kind kind : kinds) {
                    Found& entry = found[kind];
                    ++entry.count;
                    entry.faces.insert(onFaces.begin(), onFaces.end());
                    entry.edges.insert(onEdges.begin(), onEdges.end());
                }
            };
            for (TopExp_Explorer it(whole, TopAbs_VERTEX); it.More(); it.Next()) {
                std::vector<int> around;
                if (edgesOfVertex.Contains(it.Current())) {
                    for (TopTools_ListOfShape::Iterator e(edgesOfVertex.FindFromKey(it.Current()));
                         e.More(); e.Next()) {
                        around.push_back(edges.FindIndex(e.Value()) - 1);
                    }
                }
                record(it.Current(), TopAbs_VERTEX, {}, around);
            }
            for (int i = 1; i <= edges.Extent(); ++i) {
                record(edges(i), TopAbs_EDGE, {}, {i - 1});
            }
            for (TopExp_Explorer it(whole, TopAbs_WIRE); it.More(); it.Next()) {
                std::vector<int> owners;
                if (facesOfWire.Contains(it.Current())) {
                    for (TopTools_ListOfShape::Iterator f(facesOfWire.FindFromKey(it.Current()));
                         f.More(); f.Next()) {
                        owners.push_back(faces.FindIndex(f.Value()) - 1);
                    }
                }
                record(it.Current(), TopAbs_WIRE, owners, {});
            }
            for (int i = 1; i <= faces.Extent(); ++i) {
                record(faces(i), TopAbs_FACE, {i - 1}, {});
            }
            for (const TopAbs_ShapeEnum type : {TopAbs_SHELL, TopAbs_SOLID}) {
                for (TopExp_Explorer it(whole, type); it.More(); it.Next()) {
                    record(it.Current(), type, {}, {});
                }
            }
            for (const auto& [kind, entry] : found) {
                add(sentenceFor(kind, entry), entry.faces, entry.edges);
            }
        }

        // Edges that border only one face: the faces do not close up. An edge where a closed
        // surface meets itself, such as the seam of a cylinder, borders one face twice.
        std::set<int> open;
        std::set<int> openFaces;
        for (int i = 1; i <= facesOfEdge.Extent(); ++i) {
            const TopoDS_Edge& edge = TopoDS::Edge(facesOfEdge.FindKey(i));
            const TopTools_ListOfShape& around = facesOfEdge(i);
            if (BRep_Tool::Degenerated(edge) || around.Extent() != 1 ||
                BRep_Tool::IsClosed(edge, TopoDS::Face(around.First()))) {
                continue;
            }
            open.insert(edges.FindIndex(edge) - 1);
            openFaces.insert(faces.FindIndex(around.First()) - 1);
        }
        const bool solid = TopExp_Explorer(whole, TopAbs_SOLID).More();
        if (!solid && faces.Extent() > 0) {
            add("It is surfaces, not a solid" + (open.empty()
                                                     ? std::string(".")
                                                     : ": " +
                                                           countOf(static_cast<int>(open.size()),
                                                                   "edge borders", "edges border") +
                                                           " only one face."),
                openFaces, open);
        } else if (!open.empty()) {
            add("It is not closed: " +
                    countOf(static_cast<int>(open.size()), "edge borders", "edges border") +
                    " only one face.",
                openFaces, open);
        }

        // A solid whose faces face inward encloses a negative volume.
        for (TopExp_Explorer it(whole, TopAbs_SOLID); it.More(); it.Next()) {
            if (open.empty() && signedVolume(it.Current()) < 0.0) {
                add("It is inside out: its faces face inward.", {}, {});
                break;
            }
        }

        // Faces that cross, and edges too short to work with. Only for a shape whose parts
        // are each sound: the test can fail on a broken one.
        if (valid && problems.empty()) {
            BOPAlgo_ArgumentAnalyzer analyzer2;
            analyzer2.SetShape1(whole);
            analyzer2.ArgumentTypeMode() = Standard_False;
            analyzer2.SelfInterMode() = Standard_True;
            analyzer2.SmallEdgeMode() = Standard_True;
            analyzer2.Perform();
            if (analyzer2.HasFaulty()) {
                Found crossing;
                Found small;
                for (BOPAlgo_ListOfCheckResult::Iterator it(analyzer2.GetCheckResult()); it.More();
                     it.Next()) {
                    const BOPAlgo_CheckResult& result = it.Value();
                    Found* entry = result.GetCheckStatus() == BOPAlgo_SelfIntersect  ? &crossing
                                   : result.GetCheckStatus() == BOPAlgo_TooSmallEdge ? &small
                                                                                     : nullptr;
                    if (entry == nullptr) {
                        continue;
                    }
                    for (TopTools_ListOfShape::Iterator s(result.GetFaultyShapes1()); s.More();
                         s.Next()) {
                        if (faces.Contains(s.Value())) {
                            entry->faces.insert(faces.FindIndex(s.Value()) - 1);
                        } else if (edges.Contains(s.Value())) {
                            entry->edges.insert(edges.FindIndex(s.Value()) - 1);
                        }
                    }
                    ++entry->count;
                }
                if (crossing.count > 0) {
                    const int many =
                        static_cast<int>(crossing.faces.size() + crossing.edges.size());
                    add(countOf(std::max(many, 1), "face or edge crosses", "faces or edges cross") +
                            " another part of the solid, or itself.",
                        crossing.faces, crossing.edges);
                }
                if (small.count > 0) {
                    add(countOf(static_cast<int>(std::max<std::size_t>(small.edges.size(), 1)),
                                "edge is", "edges are") +
                            " too short to work with.",
                        small.faces, small.edges);
                }
            }
        }
        return problems;
    });
}

} // namespace lcad::kernel
