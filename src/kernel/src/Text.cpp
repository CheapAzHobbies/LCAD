// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Error.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>

#include <lcad/kernel/occt/Guard.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Font_FontAspect.hxx>
#include <Font_StrictLevel.hxx>
#include <NCollection_String.hxx>
#include <StdPrs_BRepFont.hxx>
#include <StdPrs_BRepTextBuilder.hxx>
#include <TCollection_AsciiString.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopExp.hxx>
#include <gp_Ax3.hxx>
#include <gp_Trsf.hxx>

#include <cmath>
#include <stdexcept>

namespace lcad::kernel {

NamedShape textSolid(const Plane& plane, double u, double v, const std::string& text,
                     double height, double from, double to, const std::string& font,
                     const std::string& tag) {
    if (text.find_first_not_of(" \t\n") == std::string::npos) {
        throw std::invalid_argument("textSolid: there is no text");
    }
    if (!std::isfinite(height) || height <= 1e-6) {
        throw std::invalid_argument("textSolid: the height has to be more than zero");
    }
    if (!std::isfinite(from) || !std::isfinite(to) || std::abs(to - from) < 1e-7) {
        throw std::invalid_argument("textSolid: the depth has to be more than zero");
    }
    return occt::guarded("write text", [&] {
        StdPrs_BRepFont letters;
        if (!letters.FindAndInit(TCollection_AsciiString(font.c_str()), Font_FontAspect_Regular,
                                 height, Font_StrictLevel_Any)) {
            throw Error("write text", "there is no font to write it in");
        }
        const Point at = toWorld(plane, u, v);
        const gp_Ax3 pen(gp_Pnt(at.x, at.y, at.z),
                         gp_Dir(plane.normal.x, plane.normal.y, plane.normal.z),
                         gp_Dir(plane.xDirection.x, plane.xDirection.y, plane.xDirection.z));
        StdPrs_BRepTextBuilder builder;
        TopoDS_Shape faces = builder.Perform(letters, NCollection_String(text.c_str()), pen);
        if (faces.IsNull() || !TopExp_Explorer(faces, TopAbs_FACE).More()) {
            throw Error("write text", "the text makes no letters in that font");
        }
        const gp_Vec n(plane.normal.x, plane.normal.y, plane.normal.z);
        gp_Trsf start;
        start.SetTranslation(n * from);
        faces = BRepBuilderAPI_Transform(faces, start, Standard_True).Shape();
        BRepPrimAPI_MakePrism prism(faces, n * (to - from));
        if (!prism.IsDone()) {
            throw Error("write text", "the letters could not be made into a solid");
        }
        // Named in the order of where the faces are: text has no other names to keep.
        return nameFaces(occt::wrap(prism.Shape()), tag + "/text/");
    });
}

} // namespace lcad::kernel
