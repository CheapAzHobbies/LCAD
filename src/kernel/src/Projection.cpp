// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Projection.h>

#include <lcad/kernel/Error.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/occt/Guard.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepLib.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <GeomAbs_CurveType.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <Precision.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lcad::kernel {

namespace {

Vector crossOf(const Vector& a, const Vector& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double dotOf(const Vector& a, const Vector& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector toward(const ViewFrame& frame) {
    return crossOf(frame.right, frame.up);
}

gp_Dir toDir(const Vector& v) {
    return {v.x, v.y, v.z};
}

gp_Ax2 axesOf(const ViewFrame& frame) {
    return {gp_Pnt(0.0, 0.0, 0.0), toDir(toward(frame)), toDir(frame.right)};
}

// The points along an edge, in order from its first end to its last, at most `tolerance` off.
std::vector<gp_Pnt> pointsAlong(const TopoDS_Edge& edge, double tolerance) {
    std::vector<gp_Pnt> points;
    const BRepAdaptor_Curve curve(edge);
    if (curve.GetType() == GeomAbs_Line) {
        points.push_back(curve.Value(curve.FirstParameter()));
        points.push_back(curve.Value(curve.LastParameter()));
    } else {
        GCPnts_QuasiUniformDeflection sampler(curve, tolerance);
        if (!sampler.IsDone()) {
            throw Error("flatten a curve", "It could not be divided into points.");
        }
        for (int i = 1; i <= sampler.NbPoints(); ++i) {
            points.push_back(sampler.Value(i));
        }
    }
    return points;
}

// The curves of the flat result of hidden line removal, whose edges lie in the plane z = 0 of
// the view.
void gather(const TopoDS_Shape& edges, double tolerance, std::vector<ProjectedCurve>& curves) {
    if (edges.IsNull()) {
        return;
    }
    BRepLib::BuildCurves3d(edges);
    for (TopExp_Explorer it(edges, TopAbs_EDGE); it.More(); it.Next()) {
        const TopoDS_Edge& edge = TopoDS::Edge(it.Current());
        if (BRep_Tool::Degenerated(edge)) {
            continue;
        }
        const BRepAdaptor_Curve curve(edge);
        ProjectedCurve flat;
        for (const gp_Pnt& point : pointsAlong(edge, tolerance)) {
            flat.points.push_back({point.X(), point.Y()});
        }
        if (flat.points.size() < 2) {
            continue;
        }
        if (curve.GetType() == GeomAbs_Circle) {
            const gp_Circ circle = curve.Circle();
            flat.isArc = true;
            flat.center = {circle.Location().X(), circle.Location().Y()};
            flat.radius = circle.Radius();
            const auto angleOf = [&flat](const gp_Pnt& p) {
                return std::atan2(p.Y() - flat.center.y, p.X() - flat.center.x);
            };
            // A circle that runs clockwise as seen is, turned around, one that starts at its
            // last point.
            const bool clockwise = circle.Axis().Direction().Z() < 0.0;
            flat.startAngle =
                angleOf(curve.Value(clockwise ? curve.LastParameter() : curve.FirstParameter()));
            flat.endAngle = flat.startAngle + (curve.LastParameter() - curve.FirstParameter());
        }
        curves.push_back(std::move(flat));
    }
}

double distanceToSegment(const Point2D& p, const Point2D& a, const Point2D& b) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double squared = dx * dx + dy * dy;
    const double along =
        squared > 0.0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / squared, 0.0, 1.0) : 0.0;
    return std::hypot(p.x - a.x - along * dx, p.y - a.y - along * dy);
}

bool liesOn(const Point2D& p, const ProjectedCurve& curve, double tolerance) {
    for (std::size_t i = 1; i < curve.points.size(); ++i) {
        if (distanceToSegment(p, curve.points[i - 1], curve.points[i]) <= tolerance) {
            return true;
        }
    }
    return false;
}

// Whether the whole of a curve lies on those of `others`: its ends, and its points in between,
// each on one of them.
bool isCovered(const ProjectedCurve& curve, const std::vector<ProjectedCurve>& others,
               double tolerance) {
    std::vector<Point2D> samples = curve.points;
    if (samples.size() == 2) {
        samples.insert(samples.begin() + 1,
                       {(samples[0].x + samples[1].x) / 2.0, (samples[0].y + samples[1].y) / 2.0});
    }
    return std::all_of(samples.begin(), samples.end(), [&](const Point2D& p) {
        return std::any_of(others.begin(), others.end(), [&](const ProjectedCurve& other) {
            return liesOn(p, other, tolerance);
        });
    });
}

// What is left of hidden curves once those that lie on a visible one, or on another hidden one,
// are gone: a drawing has one line there, and the visible one wins.
std::vector<ProjectedCurve> withoutCovered(std::vector<ProjectedCurve> hidden,
                                           const std::vector<ProjectedCurve>& visible,
                                           double tolerance) {
    std::vector<ProjectedCurve> kept;
    for (ProjectedCurve& curve : hidden) {
        if (!isCovered(curve, visible, tolerance) && !isCovered(curve, kept, tolerance)) {
            kept.push_back(std::move(curve));
        }
    }
    return kept;
}

std::vector<Point2D> flatPoints(const TopoDS_Wire& wire, const gp_Pln& plane, double tolerance) {
    std::vector<Point2D> points;
    const gp_Ax3 frame = plane.Position();
    const gp_Pnt origin = frame.Location();
    const gp_Dir xAxis = frame.XDirection();
    const gp_Dir yAxis = frame.YDirection();
    const auto add = [&](const gp_Pnt& p) {
        const gp_Vec from(origin, p);
        const Point2D flat{from.Dot(gp_Vec(xAxis)), from.Dot(gp_Vec(yAxis))};
        if (points.empty() || std::hypot(points.back().x - flat.x, points.back().y - flat.y) >
                                  Precision::Confusion()) {
            points.push_back(flat);
        }
    };
    for (BRepTools_WireExplorer it(wire); it.More(); it.Next()) {
        std::vector<gp_Pnt> along = pointsAlong(it.Current(), tolerance);
        if (it.Current().Orientation() == TopAbs_REVERSED) {
            std::reverse(along.begin(), along.end());
        }
        for (const gp_Pnt& p : along) {
            add(p);
        }
    }
    if (points.size() > 1 &&
        std::hypot(points.front().x - points.back().x, points.front().y - points.back().y) <
            Precision::Confusion()) {
        points.pop_back();
    }
    return points;
}

// A face's outlines in the coordinates of a frame lying in its plane.
FlatRegion regionOf(const TopoDS_Face& face, const gp_Pln& plane, double tolerance) {
    FlatRegion region;
    const TopoDS_Wire outer = BRepTools::OuterWire(face);
    region.outer = flatPoints(outer, plane, tolerance);
    for (TopExp_Explorer it(face, TopAbs_WIRE); it.More(); it.Next()) {
        const TopoDS_Wire& wire = TopoDS::Wire(it.Current());
        if (!wire.IsSame(outer)) {
            region.holes.push_back(flatPoints(wire, plane, tolerance));
        }
    }
    return region;
}

} // namespace

Point2D projected(const ViewFrame& frame, const Point& point) {
    const Vector p{point.x, point.y, point.z};
    return {dotOf(p, frame.right), dotOf(p, frame.up)};
}

double depthOf(const ViewFrame& frame, const Point& point) {
    return dotOf({point.x, point.y, point.z}, toward(frame));
}

Projection project(const Shape& shape, const ViewFrame& frame, double tolerance) {
    if (shape.isNull()) {
        throw Error("project a shape", "There is nothing to project.");
    }
    return occt::guarded("project a shape", [&] {
        Handle(HLRBRep_Algo) algorithm = new HLRBRep_Algo();
        algorithm->Add(occt::unwrap(shape));
        algorithm->Projector(HLRAlgo_Projector(axesOf(frame)));
        algorithm->Update();
        algorithm->Hide();
        HLRBRep_HLRToShape result(algorithm);
        Projection projection;
        gather(result.VCompound(), tolerance, projection.visible);
        gather(result.OutLineVCompound(), tolerance, projection.visible);
        gather(result.Rg1LineVCompound(), tolerance, projection.smooth);
        gather(result.HCompound(), tolerance, projection.hidden);
        gather(result.OutLineHCompound(), tolerance, projection.hidden);
        projection.hidden =
            withoutCovered(std::move(projection.hidden), projection.visible, tolerance * 2.0);
        return projection;
    });
}

SectionCut sectionBy(const Shape& shape, const Plane& plane, const ViewFrame& frame,
                     double tolerance) {
    if (shape.isNull()) {
        throw Error("cut a section", "There is nothing to cut.");
    }
    return occt::guarded("cut a section", [&] {
        const TopoDS_Shape& solid = occt::unwrap(shape);
        Bnd_Box box;
        BRepBndLib::Add(solid, box);
        const double size = std::sqrt(box.SquareExtent()) * 2.0 + 10.0;
        const gp_Pnt origin(plane.origin.x, plane.origin.y, plane.origin.z);
        const gp_Dir normal = toDir(plane.normal);
        const gp_Dir xAxis = toDir(plane.xDirection);
        const gp_Pln flat(gp_Ax3(origin, normal, xAxis));

        // What is on the normal's side goes: a box that stands on the plane.
        const gp_Pnt corner = origin.Translated(gp_Vec(xAxis) * (-size / 2.0) +
                                                gp_Vec(normal.Crossed(xAxis)) * (-size / 2.0));
        const gp_Ax2 boxAxes(corner, normal, xAxis);
        const TopoDS_Shape cutter = BRepPrimAPI_MakeBox(boxAxes, size, size, size).Shape();
        BRepAlgoAPI_Cut cut(solid, cutter);
        if (!cut.IsDone() || cut.Shape().IsNull()) {
            throw Error("cut a section", "The cut did not work.");
        }

        // What of the solid lies in the plane: the cut faces.
        const TopoDS_Face sheet = BRepBuilderAPI_MakeFace(flat, -size, size, -size, size).Face();
        BRepAlgoAPI_Common common(solid, sheet);
        if (!common.IsDone()) {
            throw Error("cut a section", "The plane could not be laid through the solid.");
        }
        SectionCut section;
        section.behind = occt::wrap(cut.Shape());

        // The plane of the regions' coordinates has the view's own frame.
        const gp_Ax3 viewFrame(gp_Pnt(0.0, 0.0, 0.0), toDir(toward(frame)), toDir(frame.right));
        const gp_Pln viewPlane(viewFrame);
        for (TopExp_Explorer it(common.Shape(), TopAbs_FACE); it.More(); it.Next()) {
            section.regions.push_back(regionOf(TopoDS::Face(it.Current()), viewPlane, tolerance));
        }
        if (section.regions.empty()) {
            throw Error("cut a section", "The plane does not pass through the solid.");
        }
        return section;
    });
}

FlatRegion flatOutline(const Shape& face, const Plane& plane, double tolerance) {
    if (face.isNull() || occt::unwrap(face).ShapeType() != TopAbs_FACE) {
        throw Error("outline a face", "That is not a face.");
    }
    return occt::guarded("outline a face", [&] {
        const gp_Pln flat(gp_Ax3(gp_Pnt(plane.origin.x, plane.origin.y, plane.origin.z),
                                 toDir(plane.normal), toDir(plane.xDirection)));
        return regionOf(TopoDS::Face(occt::unwrap(face)), flat, tolerance);
    });
}

std::optional<RoundFace> roundFaceAlongZ(const Shape& face) {
    if (face.isNull() || occt::unwrap(face).ShapeType() != TopAbs_FACE) {
        return std::nullopt;
    }
    return occt::guarded("look at a round face", [&]() -> std::optional<RoundFace> {
        const TopoDS_Face& topoFace = TopoDS::Face(occt::unwrap(face));
        const BRepAdaptor_Surface surface(topoFace);
        if (surface.GetType() != GeomAbs_Cylinder) {
            return std::nullopt;
        }
        const gp_Cylinder cylinder = surface.Cylinder();
        const gp_Dir axis = cylinder.Axis().Direction();
        if (std::abs(std::abs(axis.Z()) - 1.0) > 1e-9) {
            return std::nullopt;
        }
        Bnd_Box box;
        BRepBndLib::AddOptimal(topoFace, box, false, false);
        double xMin = 0.0;
        double yMin = 0.0;
        double zMin = 0.0;
        double xMax = 0.0;
        double yMax = 0.0;
        double zMax = 0.0;
        box.Get(xMin, yMin, zMin, xMax, yMax, zMax);
        RoundFace round;
        round.center = {cylinder.Location().X(), cylinder.Location().Y()};
        round.radius = cylinder.Radius();
        round.bottom = zMin;
        round.top = zMax;
        return round;
    });
}

Plane levelPlane(double z) {
    return makePlane({0.0, 0.0, z}, {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0});
}

std::vector<EdgeSummary> edgeSummaries(const NamedShape& shape) {
    return occt::guarded("describe the edges of a shape", [&] {
        const std::vector<std::string> names = edgeNames(shape);
        std::vector<EdgeSummary> summaries;
        summaries.reserve(names.size());
        for (std::size_t i = 0; i < names.size(); ++i) {
            const TopoDS_Edge edge = TopoDS::Edge(
                occt::unwrap(subShape(shape.shape, SubShapeKind::Edge, static_cast<int>(i))));
            EdgeSummary summary;
            summary.name = names[i];
            if (!BRep_Tool::Degenerated(edge)) {
                const BRepAdaptor_Curve curve(edge);
                const auto point = [&curve](double at) {
                    const gp_Pnt p = curve.Value(at);
                    return Point{p.X(), p.Y(), p.Z()};
                };
                const double first = curve.FirstParameter();
                const double last = curve.LastParameter();
                summary.start = point(first);
                summary.end = point(last);
                summary.middle = point((first + last) / 2.0);
                summary.isLine = curve.GetType() == GeomAbs_Line;
                if (curve.GetType() == GeomAbs_Circle) {
                    const gp_Circ circle = curve.Circle();
                    summary.isCircle = true;
                    summary.center = {circle.Location().X(), circle.Location().Y(),
                                      circle.Location().Z()};
                    summary.radius = circle.Radius();
                    const gp_Dir axis = circle.Axis().Direction();
                    summary.axis = {axis.X(), axis.Y(), axis.Z()};
                }
            }
            summaries.push_back(std::move(summary));
        }
        return summaries;
    });
}

} // namespace lcad::kernel
