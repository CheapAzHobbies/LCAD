// SPDX-License-Identifier: GPL-3.0-or-later

// Says what is wrong with the solids made from a mesh.

#include <lcad/kernel/Check.h>
#include <lcad/kernel/Exchange.h>
#include <lcad/kernel/MeshSolid.h>
#include <lcad/kernel/MeshSurfaces.h>

#include <lcad/kernel/occt/ShapeAccess.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <gp_Circ.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Face.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepCheck_Status.hxx>
#include <BRepCheck_Wire.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <cstdio>
#include <map>

using namespace lcad::kernel;

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const Mesh mesh = readMesh(argv[i]);
        const MeshSurfaces found = findSurfaces(mesh);
        const MeshSolids made = solidsFromMesh(mesh, found);
        for (const Shape& solid : made.solids) {
            std::map<std::string, int> problems;
            for (const GeometryProblem& p : checkGeometry(solid)) {
                ++problems[p.description];
            }
            for (const auto& [text, count] : problems) {
                std::printf("  %3d x %s\n", count, text.c_str());
            }
            const TopoDS_Shape& shape = occt::unwrap(solid);
            const BRepCheck_Analyzer analyzer(shape);
            TopTools_IndexedMapOfShape faces;
            TopExp::MapShapes(shape, TopAbs_FACE, faces);
            std::printf("  faces %d\n", faces.Extent());
            for (int f = 1; f <= faces.Extent(); ++f) {
                const TopoDS_Face face = TopoDS::Face(faces(f));
                if (analyzer.IsValid(face)) {
                    continue;
                }
                const BRepAdaptor_Surface surface(face, Standard_False);
                int wires = 0;
                int edges = 0;
                for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
                    ++wires;
                    for (TopExp_Explorer e(w.Current(), TopAbs_EDGE); e.More(); e.Next()) {
                        ++edges;
                    }
                }
                double u0, u1, v0, v1;
                BRepTools::UVBounds(face, u0, u1, v0, v1);
                std::printf("  bad face %d: type %d wires %d edges %d orient %d uv [%.3f %.3f]x[%.3f %.3f] status:",
                            f, static_cast<int>(surface.GetType()), wires, edges,
                            static_cast<int>(face.Orientation()), u0, u1, v0, v1);
                for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
                    const Handle(BRepCheck_Result) wr = analyzer.Result(w.Current());
                    if (!wr.IsNull()) {
                        for (BRepCheck_ListOfStatus::Iterator it(wr->Status()); it.More(); it.Next()) {
                            std::printf(" W%d", static_cast<int>(it.Value()));
                        }
                        for (wr->InitContextIterator(); wr->MoreShapeInContext(); wr->NextShapeInContext()) {
                            for (BRepCheck_ListOfStatus::Iterator it(wr->StatusOnShape()); it.More(); it.Next()) {
                                std::printf(" w%d", static_cast<int>(it.Value()));
                            }
                        }
                    }
                }
                const Handle(BRepCheck_Result) fr = analyzer.Result(face);
                if (!fr.IsNull()) {
                    for (BRepCheck_ListOfStatus::Iterator it(fr->Status()); it.More(); it.Next()) {
                        std::printf(" F%d", static_cast<int>(it.Value()));
                    }
                }
                std::printf("\n");
                for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
                    std::printf("     wire:");
                    for (TopExp_Explorer e(w.Current(), TopAbs_EDGE); e.More(); e.Next()) {
                        const BRepAdaptor_Curve curve(TopoDS::Edge(e.Current()));
                        const gp_Pnt a = curve.Value(curve.FirstParameter());
                        const gp_Pnt b = curve.Value(curve.LastParameter());
                        std::printf(" [t%d o%d %.3f..%.3f (%.3f %.3f %.3f)->(%.3f %.3f %.3f)", static_cast<int>(curve.GetType()),
                                    static_cast<int>(e.Current().Orientation()), curve.FirstParameter(), curve.LastParameter(),
                                    a.X(), a.Y(), a.Z(), b.X(), b.Y(), b.Z());
                        if (curve.GetType() == GeomAbs_Circle) {
                            const gp_Circ c = curve.Circle();
                            std::printf(" r=%.4f c=(%.3f %.3f %.3f) n=(%.2f %.2f %.2f)", c.Radius(), c.Location().X(), c.Location().Y(), c.Location().Z(), c.Axis().Direction().X(), c.Axis().Direction().Y(), c.Axis().Direction().Z());
                        }
                        std::printf("]");
                    }
                    std::printf("\n");
                }
            }
        }
    }
    return 0;
}
