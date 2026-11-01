// SPDX-License-Identifier: GPL-3.0-or-later

// Embeds OCCT's 3D viewer in a QOpenGLWidget: Qt owns the OpenGL context and the framebuffer,
// and OCCT adopts both. The technique follows the QOpenGLWidget sample maintained for OCCT at
// https://github.com/gkv311/occt-samples-qt (MIT license, see THIRD_PARTY_NOTICES.md).

// Before OCCT's headers, which include it with much of it switched off.
#if defined(_WIN32)
#include <windows.h>
#endif

// OCCT's OpenGL headers come first. They define every OpenGL version and function type
// themselves, and on macOS clash with Apple's gl3.h when Qt's OpenGL headers have included it
// first; the other way round, gl3.h finds the versions defined and leaves them alone.
#include <OpenGl_Context.hxx>
#include <OpenGl_FrameBuffer.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <OpenGl_View.hxx>
#include <OpenGl_Window.hxx>

#include <lcad/viewport/ViewportWidget.h>

#include <lcad/document/Document.h>
#include <lcad/document/SketchShapes.h>
#include <lcad/kernel/Error.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Stop.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>
#include <lcad/kernel/occt/PlacementAccess.h>
#include <lcad/kernel/occt/ShapeAccess.h>
#include <lcad/kernel/occt/Stoppable.h>
#include <lcad/viewport/NavigationCube.h>
#include <lcad/viewport/SketchEditor.h>

#include <AIS_AnimationCamera.hxx>
#include <AIS_InteractiveContext.hxx>
#include <AIS_InteractiveObject.hxx>
#include <AIS_Shape.hxx>
#include <AIS_ViewController.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_Grid.hxx>
#include <Aspect_NeutralWindow.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <Font_NameOfFont.hxx>
#include <Graphic3d_ArrayOfPolylines.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_CView.hxx>
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_Group.hxx>
#include <Graphic3d_MaterialAspect.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Prs3d_DatumAspect.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_PointAspect.hxx>
#include <Prs3d_Presentation.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <Prs3d_TextAspect.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <Quantity_Color.hxx>
#include <SelectMgr_IndexedMapOfOwner.hxx>
#include <SelectMgr_Selection.hxx>
#include <Standard_Failure.hxx>
#include <StdPrs_ToolTriangulatedShape.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <TColStd_IndexedDataMapOfStringString.hxx>
#include <TopoDS_Compound.hxx>
#include <V3d_AmbientLight.hxx>
#include <V3d_DirectionalLight.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <gp_Ax3.hxx>
#include <gp_Mat.hxx>
#include <gp_Quaternion.hxx>
#include <gp_QuaternionSLerp.hxx>
#include <gp_Pln.hxx>
#include <gp_XYZ.hxx>

#include <QApplication>
#include <QCursor>
#include <QEasingCurve>
#include <QPainter>
#include <QVariantAnimation>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QWheelEvent>
#include <QWindow>
#include <QtGlobal>

#include <algorithm>
#include <functional>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace lcad::viewport {

namespace {

// Points to click, drawn over the model: a point on a face is otherwise a speck in its color.
class PointsOverlay : public QWidget {
public:
    using Project = std::function<QPointF(const kernel::Point&)>;

    PointsOverlay(QWidget* parent, Project project)
        : QWidget(parent), m_project(std::move(project)) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        setObjectName(QStringLiteral("pointsOverlay"));
        hide();
    }

    void show(std::vector<PickablePoint> points, const QColor& plain, const QColor& chosen) {
        m_points = std::move(points);
        m_plain = plain;
        m_chosen = chosen;
        m_hovered = -1;
        setVisible(!m_points.empty());
        update();
    }

    [[nodiscard]] const std::vector<PickablePoint>& points() const { return m_points; }

    /// The number of the point nearest to a position, in reach of it, or -1.
    [[nodiscard]] int pointAt(const QPointF& position) const {
        int best = -1;
        double nearest = reach;
        for (std::size_t i = 0; i < m_points.size(); ++i) {
            const QPointF at = m_project(m_points[i].at);
            const double away = std::hypot(at.x() - position.x(), at.y() - position.y());
            if (away <= nearest) {
                nearest = away;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    void setHovered(int point) {
        if (point != m_hovered) {
            m_hovered = point;
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        for (std::size_t i = 0; i < m_points.size(); ++i) {
            const QPointF at = m_project(m_points[i].at);
            const bool hovered = static_cast<int>(i) == m_hovered;
            const double radius = hovered ? 7.5 : 6.0;
            // A dark ring, which shows on a light part, around a bright dot, which shows on a dark
            // one.
            painter.setPen(QPen(QColor(0, 0, 0, 170), 1.5));
            painter.setBrush(m_points[i].chosen ? m_chosen : QColor(255, 255, 255, 215));
            painter.drawEllipse(at, radius, radius);
            if (!m_points[i].chosen) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(m_plain);
                painter.drawEllipse(at, 2.2, 2.2);
            }
        }
    }

private:
    static constexpr double reach = 11.0;

    Project m_project;
    std::vector<PickablePoint> m_points;
    QColor m_plain;
    QColor m_chosen;
    int m_hovered = -1;
};

// A body. It is what AIS_Shape is, but for what can be picked of it: a round surface is one
// smooth thing, and the edge it closes on, with the corner at its end, is nothing that anyone
// aimed at. See smoothSeams().
class BodyObject final : public AIS_Shape {
public:
    using AIS_Shape::AIS_Shape;

protected:
    void ComputeSelection(const Handle(SelectMgr_Selection)& selection,
                          const Standard_Integer mode) override {
        const TopAbs_ShapeEnum type = AIS_Shape::SelectionType(mode);
        if (type != TopAbs_EDGE && type != TopAbs_VERTEX) {
            AIS_Shape::ComputeSelection(selection, mode);
            return;
        }
        // The same as ever, of a shape with those left out.
        const TopoDS_Shape whole = myshape;
        myshape = pickable(whole, type);
        AIS_Shape::ComputeSelection(selection, mode);
        myshape = whole;
    }

private:
    static TopoDS_Shape pickable(const TopoDS_Shape& whole, TopAbs_ShapeEnum type) {
        BRep_Builder builder;
        TopoDS_Compound kept;
        builder.MakeCompound(kept);
        const auto seam = [](const TopoDS_Edge& edge) {
            return BRep_Tool::Degenerated(edge) || BRep_Tool::MaxContinuity(edge) == GeomAbs_CN;
        };
        if (type == TopAbs_EDGE) {
            TopTools_IndexedMapOfShape edges;
            TopExp::MapShapes(whole, TopAbs_EDGE, edges);
            for (Standard_Integer i = 1; i <= edges.Extent(); ++i) {
                if (!seam(TopoDS::Edge(edges(i)))) {
                    builder.Add(kept, edges(i));
                }
            }
            return kept;
        }
        // A corner counts if a real edge ends there. A closed circle has no end: the corner on it
        // is where its parameter starts, as arbitrary a place as a seam.
        TopTools_IndexedDataMapOfShapeListOfShape edgesAt;
        TopExp::MapShapesAndAncestors(whole, TopAbs_VERTEX, TopAbs_EDGE, edgesAt);
        for (Standard_Integer i = 1; i <= edgesAt.Extent(); ++i) {
            bool real = edgesAt(i).IsEmpty();
            for (TopTools_ListOfShape::Iterator edge(edgesAt(i)); edge.More(); edge.Next()) {
                const TopoDS_Edge& candidate = TopoDS::Edge(edge.Value());
                TopoDS_Vertex first;
                TopoDS_Vertex last;
                TopExp::Vertices(candidate, first, last);
                real = real || (!seam(candidate) && !first.IsSame(last));
            }
            if (real) {
                builder.Add(kept, edgesAt.FindKey(i));
            }
        }
        return kept;
    }
};

// Lines in space that nothing can pick: toolpaths and the like.
class LinesObject : public AIS_InteractiveObject {
    DEFINE_STANDARD_RTTI_INLINE(LinesObject, AIS_InteractiveObject)

public:
    LinesObject(std::vector<std::vector<kernel::Point>> lines, const Quantity_Color& color,
                Aspect_TypeOfLine style, double width)
        : m_lines(std::move(lines)), m_color(color), m_style(style), m_width(width) {}

private:
    void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& shown,
                 const Standard_Integer) override {
        int vertices = 0;
        for (const auto& line : m_lines) {
            vertices += static_cast<int>(line.size());
        }
        if (vertices == 0) {
            return;
        }
        Handle(Graphic3d_ArrayOfPolylines) array =
            new Graphic3d_ArrayOfPolylines(vertices, static_cast<int>(m_lines.size()));
        for (const auto& line : m_lines) {
            array->AddBound(static_cast<int>(line.size()));
            for (const kernel::Point& p : line) {
                array->AddVertex(p.x, p.y, p.z);
            }
        }
        Handle(Graphic3d_Group) group = shown->NewGroup();
        group->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(m_color, m_style, m_width));
        group->AddPrimitiveArray(array);
    }
    void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}

    std::vector<std::vector<kernel::Point>> m_lines;
    Quantity_Color m_color;
    Aspect_TypeOfLine m_style;
    double m_width;
};

// A surface of triangles that nothing can pick: the stock as a simulation has cut it.
class SurfaceObject : public AIS_InteractiveObject {
    DEFINE_STANDARD_RTTI_INLINE(SurfaceObject, AIS_InteractiveObject)

public:
    explicit SurfaceObject(kernel::Mesh mesh) : m_mesh(std::move(mesh)) {}

private:
    void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& shown,
                 const Standard_Integer) override {
        // Smooth by what each corner's triangles say.
        std::vector<gp_XYZ> normals(m_mesh.vertices.size(), gp_XYZ(0.0, 0.0, 0.0));
        const auto vertex = [this](std::uint32_t i) {
            const kernel::Point& p = m_mesh.vertices[i];
            return gp_XYZ(p.x, p.y, p.z);
        };
        for (const auto& triangle : m_mesh.triangles) {
            const gp_XYZ normal = (vertex(triangle[1]) - vertex(triangle[0]))
                                      .Crossed(vertex(triangle[2]) - vertex(triangle[0]));
            for (const std::uint32_t corner : triangle) {
                normals[corner] += normal;
            }
        }
        Handle(Graphic3d_ArrayOfTriangles) array = new Graphic3d_ArrayOfTriangles(
            static_cast<int>(m_mesh.vertices.size()), static_cast<int>(m_mesh.triangles.size()) * 3,
            Graphic3d_ArrayFlags_VertexNormal);
        for (std::size_t i = 0; i < m_mesh.vertices.size(); ++i) {
            const kernel::Point& p = m_mesh.vertices[i];
            gp_XYZ normal = normals[i];
            if (normal.Modulus() < 1e-12) {
                normal = gp_XYZ(0.0, 0.0, 1.0);
            }
            normal.Normalize();
            array->AddVertex(gp_Pnt(p.x, p.y, p.z), gp_Dir(normal));
        }
        for (const auto& triangle : m_mesh.triangles) {
            array->AddEdges(static_cast<int>(triangle[0]) + 1, static_cast<int>(triangle[1]) + 1,
                            static_cast<int>(triangle[2]) + 1);
        }
        Handle(Graphic3d_Group) group = shown->NewGroup();
        group->SetGroupPrimitivesAspect(Attributes()->ShadingAspect()->Aspect());
        group->AddPrimitiveArray(array);
    }
    void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}

    kernel::Mesh m_mesh;
};


Quantity_Color toOcct(const QColor& color) {
    return Quantity_Color(color.redF(), color.greenF(), color.blueF(), Quantity_TOC_sRGB);
}

// Whether another thread is using OCCT, so that no viewport may. See holdDrawing().
bool drawingHeld = false;

// How long the camera takes to turn to what was picked on the cube, in milliseconds.
const int cubeTurnTime = 250;

V3d_TypeOfOrientation toOcct(StandardView view) {
    switch (view) {
    case StandardView::Home:
        return V3d_XposYnegZpos;
    case StandardView::Front:
        return V3d_Yneg;
    case StandardView::Back:
        return V3d_Ypos;
    case StandardView::Left:
        return V3d_Xneg;
    case StandardView::Right:
        return V3d_Xpos;
    case StandardView::Top:
        return V3d_Zpos;
    case StandardView::Bottom:
        return V3d_Zneg;
    }
    return V3d_XposYnegZpos;
}

// A matte finish. OCCT's default material has a strong specular highlight, which turns a flat
// face that squarely faces the camera plain white.
Graphic3d_MaterialAspect bodyMaterial() {
    return Graphic3d_MaterialAspect(Graphic3d_NameOfMaterial_Plastered);
}

// The origin planes and axes are this large on screen, in pixels, at any zoom.
const double originPlaneSize = 46.0;
const double originAxisLength = 70.0;
// And this large while one of the planes is being chosen: large enough to aim at.
const double pickPlaneSize = 120.0;

// The plane an origin item is, as a point on it, its normal, and the direction of its own X.
gp_Ax3 originPlaneAxes(OriginItem item) {
    const gp_Pnt origin(0.0, 0.0, 0.0);
    switch (item) {
    case OriginItem::PlaneXZ:
        return gp_Ax3(origin, gp::DY().Reversed(), gp::DX());
    case OriginItem::PlaneYZ:
        return gp_Ax3(origin, gp::DX(), gp::DY());
    default:
        return gp_Ax3(origin, gp::DZ(), gp::DX());
    }
}

// The selection modes of AIS_Shape that a filter switches on.
std::vector<int> selectionModes(SelectionFilter filter) {
    const int vertices = AIS_Shape::SelectionMode(TopAbs_VERTEX);
    const int edges = AIS_Shape::SelectionMode(TopAbs_EDGE);
    const int faces = AIS_Shape::SelectionMode(TopAbs_FACE);
    switch (filter) {
    case SelectionFilter::FacesEdgesVertices:
        return {vertices, edges, faces};
    case SelectionFilter::Bodies:
        return {AIS_Shape::SelectionMode(TopAbs_SHAPE)};
    case SelectionFilter::Faces:
        return {faces};
    case SelectionFilter::Edges:
        return {edges};
    case SelectionFilter::Vertices:
        return {vertices};
    }
    return {};
}

Aspect_VKeyMouse toOcctButtons(Qt::MouseButtons buttons) {
    Aspect_VKeyMouse result = Aspect_VKeyMouse_NONE;
    if (buttons.testFlag(Qt::LeftButton)) {
        result |= Aspect_VKeyMouse_LeftButton;
    }
    if (buttons.testFlag(Qt::MiddleButton)) {
        result |= Aspect_VKeyMouse_MiddleButton;
    }
    if (buttons.testFlag(Qt::RightButton)) {
        result |= Aspect_VKeyMouse_RightButton;
    }
    return result;
}

Aspect_VKeyFlags toOcctModifiers(Qt::KeyboardModifiers modifiers) {
    Aspect_VKeyFlags result = Aspect_VKeyFlags_NONE;
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        result |= Aspect_VKeyFlags_SHIFT;
    }
    if (modifiers.testFlag(Qt::ControlModifier)) {
        result |= Aspect_VKeyFlags_CTRL;
    }
    if (modifiers.testFlag(Qt::AltModifier)) {
        result |= Aspect_VKeyFlags_ALT;
    }
    if (modifiers.testFlag(Qt::MetaModifier)) {
        result |= Aspect_VKeyFlags_META;
    }
    return result;
}

Handle(OpenGl_Context) glContextOf(const Handle(V3d_View)& view) {
    Handle(OpenGl_View) glView = Handle(OpenGl_View)::DownCast(view->View());
    return glView->GlWindow()->GetGlContext();
}

QString readGraphicsInfo(const Handle(V3d_View)& view) {
    TColStd_IndexedDataMapOfStringString info;
    view->DiagnosticInformation(info, Graphic3d_DiagnosticInfo_Device);
    QStringList lines;
    for (TColStd_IndexedDataMapOfStringString::Iterator it(info); it.More(); it.Next()) {
        lines << QStringLiteral("%1: %2").arg(QString::fromUtf8(it.Key().ToCString()),
                                              QString::fromUtf8(it.Value().ToCString()));
    }
    return lines.join(QLatin1Char('\n'));
}

// OCCT's stand-in for a window it does not own. Qt reports sizes in logical pixels; OCCT uses
// the ratio to scale what it draws at a fixed on-screen size, such as the axis marker.
class ViewportWindow : public Aspect_NeutralWindow {
    DEFINE_STANDARD_RTTI_INLINE(ViewportWindow, Aspect_NeutralWindow)

public:
    Standard_Real DevicePixelRatio() const override { return m_devicePixelRatio; }
    void setDevicePixelRatio(double ratio) { m_devicePixelRatio = ratio; }

private:
    double m_devicePixelRatio = 1.0;
};

// Stands for the framebuffer QOpenGLWidget hands us to draw into. Qt allocates it as plain
// RGBA8 rather than an sRGB format, so OCCT has to apply gamma correction itself instead of
// relying on GL_FRAMEBUFFER_SRGB.
class QtFrameBuffer : public OpenGl_FrameBuffer {
    DEFINE_STANDARD_RTTI_INLINE(QtFrameBuffer, OpenGl_FrameBuffer)

public:
    void BindBuffer(const Handle(OpenGl_Context)& context) override {
        OpenGl_FrameBuffer::BindBuffer(context);
        context->SetFrameBufferSRGB(true, false);
    }

    void BindDrawBuffer(const Handle(OpenGl_Context)& context) override {
        OpenGl_FrameBuffer::BindDrawBuffer(context);
        context->SetFrameBufferSRGB(true, false);
    }
};

} // namespace

void prepareApplication() {
#if defined(Q_OS_LINUX)
    // The Open CASCADE builds that Linux distributions ship create OpenGL contexts through GLX,
    // which needs an X11 connection. In a Wayland session Qt would choose its Wayland plugin and
    // create EGL contexts, which the viewer cannot adopt. Run through XWayland instead.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "xcb");
    }
#endif
    // The top-level window's surface has to match the viewport's, and Qt creates it first.
    QSurfaceFormat::setDefaultFormat(ViewportWidget::surfaceFormat());
    // Keeps OCCT's OpenGL resources valid if the viewport later moves to another top-level
    // window, as a detached dock widget does.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
}

// AIS_ViewController turns mouse input into camera motion, hover highlighting, and selection.
class ViewportWidget::Impl : public AIS_ViewController {
public:
    explicit Impl(ViewportWidget* widget) : m_widget(widget) { applyPreset(); }

    // The left button selects and never moves the camera. What the middle button does is the
    // user's choice.
    void applyPreset() {
        const unsigned int left = Aspect_VKeyMouse_LeftButton;
        const unsigned int middle = Aspect_VKeyMouse_MiddleButton;
        const unsigned int shift = Aspect_VKeyFlags_SHIFT;
        const unsigned int ctrl = Aspect_VKeyFlags_CTRL;

        AIS_MouseGestureMap& gestures = ChangeMouseGestureMap();
        gestures.Clear();
        gestures.Bind(left, AIS_MouseGesture_SelectRectangle);
        gestures.Bind(left | shift, AIS_MouseGesture_SelectRectangle);
        gestures.Bind(left | ctrl, AIS_MouseGesture_SelectRectangle);
        if (preset == NavigationPreset::MiddlePans) {
            gestures.Bind(middle, AIS_MouseGesture_Pan);
            gestures.Bind(middle | shift, AIS_MouseGesture_RotateOrbit);
            gestures.Bind(middle | ctrl | shift, AIS_MouseGesture_Zoom);
            orbitModifiers = Aspect_VKeyFlags_SHIFT;
        } else {
            gestures.Bind(middle, AIS_MouseGesture_RotateOrbit);
            gestures.Bind(middle | shift, AIS_MouseGesture_Pan);
            gestures.Bind(middle | ctrl, AIS_MouseGesture_Zoom);
            orbitModifiers = Aspect_VKeyFlags_NONE;
        }

        AIS_MouseSelectionSchemeMap& schemes = ChangeMouseSelectionSchemes();
        schemes.Clear();
        schemes.Bind(left, AIS_SelectionScheme_Replace);
        schemes.Bind(left | shift, AIS_SelectionScheme_XOR);
        schemes.Bind(left | ctrl, AIS_SelectionScheme_XOR);
    }

    Handle(V3d_Viewer) viewer;
    Handle(V3d_View) view;
    Handle(AIS_InteractiveContext) context;
    // The cube in the corner, and the turn of the camera to what was picked on it.
    NavigationCube* cube = nullptr;
    QVariantAnimation* turn = nullptr;
    gp_Quaternion turnFrom;
    gp_Quaternion turnTo;
    bool fitAfterTurn = false;
    Handle(AIS_Shape) previewObject;
    // A native window that is never shown. OCCT wants an X11 window to make the OpenGL context
    // current on, although everything it draws goes into Qt's framebuffer. This one belongs to
    // the viewport and lives exactly as long as OCCT's view does.
    std::unique_ptr<QWindow> anchorWindow;
    QString graphicsInfo;
    QString error;
    bool errorReported = false;
    bool fitPending = false;
    bool zoomReversed = false;

    document::Document* document = nullptr;
    document::Document::ListenerId listener = 0;
    // A body is drawn once for each appearance of its component: by the path to that, and the
    // body.
    struct BodyKey {
        std::vector<document::ObjectId> path;
        document::ObjectId body;
        friend bool operator<(const BodyKey& a, const BodyKey& b) {
            return std::tie(a.path, a.body) < std::tie(b.path, b.body);
        }
    };
    std::map<BodyKey, Handle(AIS_Shape)> bodyObjects;
    Handle(AIS_InteractiveObject) cutObject;
    Handle(AIS_InteractiveObject) rapidObject;
    Handle(AIS_InteractiveObject) stockObject;
    Handle(AIS_InteractiveObject) surfaceObject;
    Handle(AIS_Shape) toolObject;
    std::unordered_map<const Standard_Transient*, BodyKey> bodyOfObject;
    // The occurrence of the component being worked on, and, while joints are shown in motion,
    // where the occurrences are shown instead of where the document has them.
    std::vector<document::ObjectId> editPath;
    std::vector<document::Instance> shownInstances;
    // Where parts overlap, and the frames a joint is being placed by.
    std::vector<Handle(AIS_Shape)> highlightObjects;
    std::vector<Handle(AIS_Shape)> markerObjects;

    // An arrow dragged along a line to set a distance, the box beside it, and how it is being
    // dragged: the distance less where along the line the pointer was when it was taken.
    struct DistanceHandle {
        bool shown = false;
        kernel::Point base;
        kernel::Vector direction{0.0, 0.0, 1.0};
        double distance = 0.0;
        Handle(AIS_Shape) object;
        QLineEdit* field = nullptr;
        bool hovered = false;
        bool dragging = false;
        double grab = 0.0;
    };
    DistanceHandle handle;
    // How long the arrow is drawn, shaft and head, in pixels.
    static constexpr double handleShaft = 44.0;
    static constexpr double handleHead = 16.0;

    // The line the handle moves along, in the root's coordinates.
    [[nodiscard]] std::pair<gp_Pnt, gp_Dir> handleLine() const {
        const kernel::Placement at = editPlacement();
        const kernel::Point base = kernel::apply(at, handle.base);
        const kernel::Vector along = kernel::apply(at, handle.direction);
        return {gp_Pnt(base.x, base.y, base.z), gp_Dir(along.x, along.y, along.z)};
    }

    [[nodiscard]] gp_Pnt handleTip() const {
        const auto [base, along] = handleLine();
        return base.Translated(gp_Vec(along) * handle.distance);
    }

    // The way the arrow points: the way the distance goes.
    [[nodiscard]] gp_Dir handlePointing() const {
        const gp_Dir along = handleLine().second;
        return handle.distance < 0.0 ? along.Reversed() : along;
    }

    void showHandle() {
        if (context.IsNull()) {
            return;
        }
        if (!handle.object.IsNull()) {
            context->Remove(handle.object, false);
            handle.object.Nullify();
        }
        if (!handle.shown) {
            return;
        }
        // In pixels, from the tip, which is the anchor: a shaft and a head, the size they are
        // on screen at any zoom.
        const gp_Dir pointing = handlePointing();
        const double scale = pixelRatio;
        const gp_Pnt start(0.0, 0.0, 0.0);
        const TopoDS_Shape shaft =
            BRepPrimAPI_MakeCylinder(gp_Ax2(start, pointing), 2.8 * scale, handleShaft * scale)
                .Shape();
        const TopoDS_Shape head =
            BRepPrimAPI_MakeCone(
                gp_Ax2(start.Translated(gp_Vec(pointing) * handleShaft * scale), pointing),
                8.0 * scale, 0.0, handleHead * scale)
                .Shape();
        TopoDS_Compound arrow;
        BRep_Builder builder;
        builder.MakeCompound(arrow);
        builder.Add(arrow, shaft);
        builder.Add(arrow, head);
        handle.object = new AIS_Shape(arrow);
        handle.object->SetMaterial(bodyMaterial());
        handle.object->SetColor(
            toOcct(handle.hovered || handle.dragging ? colors.hover : colors.selection));
        handle.object->Attributes()->SetFaceBoundaryDraw(false);
        handle.object->SetTransformPersistence(
            new Graphic3d_TransformPers(Graphic3d_TMF_ZoomPers, handleTip()));
        handle.object->SetZLayer(Graphic3d_ZLayerId_Topmost);
        // Picked by its own test (overHandle()), never selected.
        context->Display(handle.object, AIS_Shaded, -1, false);
    }

    // Where the arrow is on screen: from its tip, the way it points, as long as it is drawn.
    [[nodiscard]] std::pair<QPointF, QPointF> handleOnScreen() const {
        const gp_Pnt tip = handleTip();
        const QPointF from = m_widget->project(tip.X(), tip.Y(), tip.Z());
        // How many pixels a millimeter is across the view at the tip, and how many along the
        // arrow, which is less where it points away from the viewer.
        const gp_Dir up = view->Camera()->Up();
        const gp_Pnt across = tip.Translated(gp_Vec(up));
        const gp_Pnt ahead = tip.Translated(gp_Vec(handlePointing()));
        const QPointF a = m_widget->project(across.X(), across.Y(), across.Z());
        const QPointF b = m_widget->project(ahead.X(), ahead.Y(), ahead.Z());
        const double perMillimeter = std::hypot(a.x() - from.x(), a.y() - from.y());
        if (!(perMillimeter > 1e-9)) {
            return {from, from};
        }
        const double length = handleShaft + handleHead;
        return {from, from + (b - from) * (length / perMillimeter)};
    }

    [[nodiscard]] bool overHandle(const QPointF& position) const {
        if (!handle.shown || !canRender()) {
            return false;
        }
        const auto [from, to] = handleOnScreen();
        const QPointF along = to - from;
        const double squared = QPointF::dotProduct(along, along);
        double t = 0.0;
        if (squared > 1e-9) {
            t = std::clamp(QPointF::dotProduct(position - from, along) / squared, 0.0, 1.0);
        }
        const QPointF nearest = from + along * t;
        return std::hypot(position.x() - nearest.x(), position.y() - nearest.y()) <= 9.0;
    }

    // Where along the handle's line the line of sight through a position passes nearest: the
    // distance from the base. Nothing where the line runs along the line of sight.
    [[nodiscard]] std::optional<double> handleParameterAt(const QPointF& position) const {
        int widthPixels = 0;
        int heightPixels = 0;
        view->Window()->Size(widthPixels, heightPixels);
        if (widthPixels <= 0 || heightPixels <= 0) {
            return std::nullopt;
        }
        const qreal scale = m_widget->devicePixelRatioF();
        const double deviceX = position.x() * scale / widthPixels * 2.0 - 1.0;
        const double deviceY = 1.0 - position.y() * scale / heightPixels * 2.0;
        const Handle(Graphic3d_Camera)& camera = view->Camera();
        const gp_Pnt nearer = camera->UnProject(gp_Pnt(deviceX, deviceY, -1.0));
        const gp_Pnt farther = camera->UnProject(gp_Pnt(deviceX, deviceY, 1.0));
        if (nearer.Distance(farther) < 1e-12) {
            return std::nullopt;
        }
        const gp_Dir sight(gp_Vec(nearer, farther));
        const auto [base, along] = handleLine();
        // The nearest points of two lines: base + s along, and nearer + t sight.
        const gp_Vec w(nearer, base);
        const double b = gp_Vec(along).Dot(gp_Vec(sight));
        const double d = gp_Vec(along).Dot(w);
        const double e = gp_Vec(sight).Dot(w);
        const double denominator = 1.0 - b * b;
        if (denominator < 1e-6) {
            return std::nullopt;
        }
        return (b * e - d) / denominator;
    }

    // Puts the box beside the arrow's tip, and shows the distance in it unless it is being
    // typed in.
    void placeHandleField() {
        if (handle.field == nullptr) {
            return;
        }
        if (!handle.field->hasFocus()) {
            handle.field->setText(ViewportWidget::tr("%1 mm").arg(
                QString::number(std::round(handle.distance * 1000.0) / 1000.0, 'g', 10)));
        }
        if (!canRender()) {
            return;
        }
        const auto [from, to] = handleOnScreen();
        const QSize size(std::max(84, handle.field->sizeHint().width()),
                         handle.field->sizeHint().height());
        // Beside the head of the arrow, on the side away from it.
        QPointF at = to + QPointF(14.0, -size.height() / 2.0);
        if (to.x() < from.x()) {
            at = to + QPointF(-14.0 - size.width(), -size.height() / 2.0);
        }
        QRect place(at.toPoint(), size);
        place.moveLeft(
            std::clamp(place.left(), 2, std::max(2, m_widget->width() - size.width() - 2)));
        place.moveTop(
            std::clamp(place.top(), 2, std::max(2, m_widget->height() - size.height() - 2)));
        handle.field->setGeometry(place);
    }

    [[nodiscard]] const std::vector<document::Instance>& instances() const {
        static const std::vector<document::Instance> rootOnly{document::Instance{}};
        if (!shownInstances.empty()) {
            return shownInstances;
        }
        return document != nullptr ? document->instances() : rootOnly;
    }

    [[nodiscard]] const document::Instance*
    instanceAt(const std::vector<document::ObjectId>& path) const {
        for (const document::Instance& instance : instances()) {
            if (instance.path == path) {
                return &instance;
            }
        }
        return nullptr;
    }

    // Where what is shown of a component only once is shown: at the occurrence being worked on
    // if that is one of the component's, or else at its first.
    [[nodiscard]] kernel::Placement placementFor(document::ObjectId component) const {
        if (const document::Instance* edited = instanceAt(editPath);
            edited != nullptr && edited->component == component) {
            return edited->placement;
        }
        for (const document::Instance& instance : instances()) {
            if (instance.component == component) {
                return instance.placement;
            }
        }
        return {};
    }

    // Whether a component appears, shown, anywhere: what is drawn once for it is not drawn
    // otherwise.
    [[nodiscard]] bool isShown(document::ObjectId component) const {
        return std::any_of(instances().begin(), instances().end(),
                           [component](const document::Instance& instance) {
                               return instance.component == component && instance.visible;
                           });
    }

    [[nodiscard]] kernel::Placement editPlacement() const {
        const document::Instance* edited = instanceAt(editPath);
        return edited != nullptr ? edited->placement : kernel::Placement{};
    }

    [[nodiscard]] document::ObjectId componentOfBody(document::ObjectId body) const {
        const document::Body* found = document != nullptr ? document->findBody(body) : nullptr;
        return found != nullptr ? found->component : document::ObjectId{};
    }

    static void place(const Handle(AIS_InteractiveObject)& object,
                      const kernel::Placement& placement) {
        object->SetLocalTransformation(kernel::occt::toTrsf(placement));
    }
    // A sketch that is shown but not being edited is two objects: its ordinary geometry, and
    // its construction geometry drawn dashed.
    struct SketchObjects {
        Handle(AIS_Shape) ordinary;
        Handle(AIS_Shape) construction;
    };
    std::unordered_map<document::ObjectId, SketchObjects> sketchObjects;
    std::unordered_map<const Standard_Transient*, document::ObjectId> sketchOfObject;
    SketchEditor* editor = nullptr;
    // The sketch being edited, and the camera to go back to when it is closed.
    document::ObjectId activeSketch;
    // Whether the bodies' edges and corners are switched on for the Project tool.
    bool projecting = false;
    std::optional<CameraState> cameraBeforeSketch;
    bool fitAfterSketch = false;
    SelectionFilter filter = SelectionFilter::FacesEdgesVertices;
    std::vector<SelectedItem> lastSelection;
    const Standard_Transient* lastDetected = nullptr;

    ViewportColors colors;
    DisplayStyle style = DisplayStyle::ShadedWithEdges;
    NavigationPreset preset = NavigationPreset::MiddlePans;
    // The modifier that, with the middle button, orbits in the current preset.
    Aspect_VKeyFlags orbitModifiers = Aspect_VKeyFlags_SHIFT;
    bool gridVisible = false;
    bool axesVisible = true;
    bool animated = true;
    std::array<Handle(AIS_Shape), 6> originObjects;
    // The patches shown while a plane is being chosen, and which of them the pointer is on and
    // which the button went down on (-1 for none).
    std::array<Handle(AIS_Shape), 3> pickPlanes;
    // For X, Y, and Z: +1 if the patches reach the positive way along it, -1 if the negative.
    std::array<int, 3> pickSides{1, 1, 1};
    bool planePicking = false;
    int hoveredPlane = -1;
    int pressedPlane = -1;
    // Construction planes, and which of them the pointer is on and the button went down on.
    std::unordered_map<document::ObjectId, Handle(AIS_Shape)> constructionObjects;
    // Construction axes: drawn, not picked; what turns or repeats about them names them.
    std::unordered_map<document::ObjectId, Handle(AIS_Shape)> axisObjects;
    document::ObjectId hoveredConstruction;
    document::ObjectId pressedConstruction;
    // The closed regions shown while profiles are being chosen, of which sketches, which of them
    // are picked, and which the pointer is on and the button went down on (-1 for none).
    struct ProfilePatch {
        Profile profile;
        Handle(AIS_Shape) object;
    };
    std::vector<ProfilePatch> profilePatches;
    std::vector<document::ObjectId> profileSketches;
    std::vector<Profile> pickedProfiles;
    bool profileFacesToo = false;
    int hoveredProfile = -1;
    int pressedProfile = -1;
    // What a feature would make, drawn as a body in place of the body it replaces. That body is
    // hidden, or, if `previewGhost`, left unseen but pickable under the preview.
    Handle(AIS_Shape) featureResult;
    document::ObjectId previewReplaces;
    bool previewGhost = false;

    [[nodiscard]] bool ghosted(document::ObjectId id) const {
        return previewGhost && id == previewReplaces;
    }

    // A body that cannot be seen but can be picked: its faces fully see-through, no outlines.
    static void ghost(const Handle(AIS_Shape)& object) {
        object->SetTransparency(1.0);
        object->Attributes()->SetFaceBoundaryDraw(false);
    }

    // Draws a body as it should be now: plain, or as the ghost under a preview.
    void restyle(document::ObjectId id) {
        for (const auto& [key, object] : bodyObjects) {
            if (key.body != id) {
                continue;
            }
            styleBody(object);
            if (ghosted(id)) {
                ghost(object);
                context->SetDisplayMode(object, AIS_Shaded, false);
            } else {
                context->SetDisplayMode(object, displayMode(), false);
            }
            context->Redisplay(object, false);
        }
    }
    // Physical pixels per logical pixel of the screen the viewport is on.
    double pixelRatio = 1.0;

    // OCCT measures what it draws at a fixed size on screen (the view cube, the origin guides,
    // line widths, label text) in physical pixels. On a high-DPI screen those have to grow
    // with the pixel ratio, or they come out at a fraction of their intended size.
    void applyPixelRatio(double ratio) {
        pixelRatio = ratio;
        // Scales line widths and screen-space text. 96 is OCCT's reference density.
        view->ChangeRenderingParams().Resolution =
            static_cast<unsigned int>(std::lround(96.0 * ratio));
        // How near the pointer has to come to pick something is meant in what the user sees
        // as pixels, too. A sketch is thin lines and nothing else, so it gets twice the reach.
        context->SetPixelTolerance(static_cast<int>(std::lround(2.0 * ratio)));
        for (const auto& entry : sketchObjects) {
            for (const Handle(AIS_Shape)& object :
                 {entry.second.ordinary, entry.second.construction}) {
                if (!object.IsNull()) {
                    applySketchFilter(object);
                }
            }
        }
        if (planePicking) {
            showPickPlanes();
        }
        // The origin guides are built at their pixel size; build the shown ones again.
        for (std::size_t i = 0; i < originObjects.size(); ++i) {
            if (!originObjects[i].IsNull()) {
                context->Remove(originObjects[i], false);
                originObjects[i] = makeOriginObject(static_cast<OriginItem>(i));
                context->Display(originObjects[i], AIS_Shaded, -1, false);
            }
        }
        view->Invalidate();
    }

    // A left button press on the view cube is held back until it is known to be a click (which
    // OCCT turns into a change of view) or the start of a drag (which orbits).
    std::uint64_t frames = 0;
    // Points that can be clicked, and whether the left button went down on one.
    PointsOverlay* pointsOverlay = nullptr;
    bool pressedPoint = false;
    // Where the camera was when the things drawn over the view last followed it.
    CameraState overlaidCamera;
    // The right button: a click opens the menu, held and moved it orbits.
    bool rightPressed = false;
    bool rightDragging = false;
    QPointF rightPressPosition;
    // The middle button is panning, and the pointer is a hand.
    bool panning = false;

    // Orbiting by hand: as the orbit buttons would from where it starts.
    void startOrbit(const QPointF& position) {
        stopTurn();
        AbortViewAnimation();
        UpdateMouseButtons(toFramebufferPixels(position), Aspect_VKeyMouse_MiddleButton,
                           orbitModifiers, false);
    }
    void orbitTo(const QPointF& position) {
        UpdateMousePosition(toFramebufferPixels(position), Aspect_VKeyMouse_MiddleButton,
                            orbitModifiers, false);
    }
    void endOrbit(const QPointF& position) {
        UpdateMouseButtons(toFramebufferPixels(position), Aspect_VKeyMouse_NONE,
                           Aspect_VKeyFlags_NONE, false);
    }

    void stopTurn() {
        if (turn != nullptr) {
            turn->stop();
        }
        fitAfterTurn = false;
    }

    // Which way the camera is turned, as a rotation from looking down Z with Y up.
    [[nodiscard]] gp_Quaternion orientation() const {
        const Handle(Graphic3d_Camera)& camera = view->Camera();
        const gp_Dir back = camera->Direction().Reversed();
        const gp_Dir up = camera->Up();
        const gp_Dir right = up.Crossed(back);
        return orientationOf(right, gp_Dir(back.Crossed(right)), back);
    }
    [[nodiscard]] static gp_Quaternion orientationOf(const gp_Dir& right, const gp_Dir& up,
                                                     const gp_Dir& back) {
        gp_Mat matrix;
        matrix.SetCol(1, right.XYZ());
        matrix.SetCol(2, up.XYZ());
        matrix.SetCol(3, back.XYZ());
        return gp_Quaternion(matrix);
    }

    // Puts the camera at an orientation, around the same middle and at the same distance: the
    // model stays where it is.
    void setOrientation(const gp_Quaternion& turned) {
        const Handle(Graphic3d_Camera)& camera = view->Camera();
        const gp_Pnt center = camera->Center();
        const double distance = camera->Distance();
        const gp_Vec back = turned.Multiply(gp_Vec(0.0, 0.0, 1.0));
        const gp_Vec up = turned.Multiply(gp_Vec(0.0, 1.0, 0.0));
        camera->SetUp(gp_Dir(up));
        camera->SetEyeAndCenter(center.Translated(back * distance), center);
        view->Invalidate();
    }

    // Turns smoothly to an orientation, or at once if animations are off.
    void turnToOrientation(const gp_Quaternion& target, bool thenFit) {
        stopTurn();
        AbortViewAnimation();
        fitPending = false;
        turnFrom = orientation();
        turnTo = target;
        fitAfterTurn = thenFit;
        if (!animated) {
            turn->setCurrentTime(turn->duration());
            finishTurn();
            return;
        }
        turn->start();
    }

    void stepTurn(double along) {
        gp_QuaternionSLerp slerp(turnFrom, turnTo);
        gp_Quaternion step;
        slerp.Interpolate(along, step);
        setOrientation(step);
        m_widget->update();
    }

    void finishTurn() {
        setOrientation(turnTo);
        const bool fit = fitAfterTurn;
        fitAfterTurn = false;
        if (fit) {
            m_widget->fitAll();
        }
        redraw();
    }

    // Looking at the model from a direction. Up stays up; looking straight down or up, there
    // is none to keep, and the side of the screen that is nearest to up now goes on being it.
    void turnToView(const QVector3D& from, bool thenFit) {
        const gp_Dir back(from.x(), from.y(), from.z());
        gp_Dir up(0.0, 0.0, 1.0);
        if (std::abs(back.Z()) > 0.999) {
            const gp_Dir now = view->Camera()->Up();
            double best = -2.0;
            for (const gp_Dir& candidate :
                 {gp_Dir(0, 1, 0), gp_Dir(0, -1, 0), gp_Dir(1, 0, 0), gp_Dir(-1, 0, 0)}) {
                if (candidate.Dot(now) > best) {
                    best = candidate.Dot(now);
                    up = candidate;
                }
            }
        }
        const gp_Dir right(up.Crossed(back));
        turnToOrientation(orientationOf(right, gp_Dir(back.Crossed(right)), back), thenFit);
    }

    void turnHome() {
        turnToView(QVector3D(1.0F, -1.0F, 1.0F).normalized(), true);
    }

    // Turns the view around the right of the screen and its top.
    void turnBy(double aroundRight, double aroundUp) {
        gp_Quaternion aboutRight(gp_Vec(1.0, 0.0, 0.0), aroundRight);
        gp_Quaternion aboutUp(gp_Vec(0.0, 1.0, 0.0), aroundUp);
        turnToOrientation(orientation().Multiplied(aboutRight).Multiplied(aboutUp), false);
    }

    void followCamera() const {
        const Handle(Graphic3d_Camera)& camera = view->Camera();
        const gp_Dir back = camera->Direction().Reversed();
        const gp_Dir up = camera->Up();
        const gp_Dir right = up.Crossed(back);
        const auto vector = [](const gp_Dir& d) {
            return QVector3D(static_cast<float>(d.X()), static_cast<float>(d.Y()),
                             static_cast<float>(d.Z()));
        };
        cube->setView(vector(right), vector(back.Crossed(right)));
    }

    // Whether the middle button pans with these keys held, rather than orbiting or zooming.
    [[nodiscard]] bool middlePans(Qt::KeyboardModifiers modifiers) const {
        const Qt::KeyboardModifiers held =
            modifiers & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);
        return preset == NavigationPreset::MiddlePans ? held == Qt::NoModifier
                                                      : held == Qt::ShiftModifier;
    }

    // Color, finish, and outlines of a body, for the current colors and display style.
    void styleBody(const Handle(AIS_Shape)& object) const {
        const bool wires = style == DisplayStyle::Wireframe || style == DisplayStyle::HiddenLine;
        object->SetMaterial(bodyMaterial());
        // SetColor() recolors the outlines and wires too, so those are set after it.
        object->SetColor(toOcct(colors.body));
        const Handle(Prs3d_Drawer)& drawer = object->Attributes();
        drawer->SetFaceBoundaryDraw(style == DisplayStyle::ShadedWithEdges ||
                                    style == DisplayStyle::Transparent);
        // Not the edges that smoothSeams() marked: a round surface shows no line of its own.
        drawer->SetFaceBoundaryUpperContinuity(GeomAbs_C3);
        drawer->SetFaceBoundaryAspect(
            new Prs3d_LineAspect(toOcct(colors.bodyEdge), Aspect_TOL_SOLID, 1.0));
        if (wires) {
            const auto line = [this] {
                return new Prs3d_LineAspect(toOcct(colors.line), Aspect_TOL_SOLID, 1.0);
            };
            drawer->SetWireAspect(line());
            drawer->SetLineAspect(line());
            drawer->SetFreeBoundaryAspect(line());
            drawer->SetUnFreeBoundaryAspect(line());
            drawer->SetSeenLineAspect(line());
        }
        if (style == DisplayStyle::Transparent) {
            object->SetTransparency(0.6);
        } else {
            object->UnsetTransparency();
        }
    }

    [[nodiscard]] int displayMode() const {
        return style == DisplayStyle::Wireframe || style == DisplayStyle::HiddenLine ? AIS_WireFrame
                                                                                     : AIS_Shaded;
    }

    void restyleBodies() {
        std::vector<document::ObjectId> bodies;
        for (const auto& entry : bodyObjects) {
            if (std::find(bodies.begin(), bodies.end(), entry.first.body) == bodies.end()) {
                bodies.push_back(entry.first.body);
            }
        }
        for (const document::ObjectId body : bodies) {
            restyle(body);
        }
        if (!featureResult.IsNull()) {
            styleBody(featureResult);
            context->SetDisplayMode(featureResult, displayMode(), false);
            context->Redisplay(featureResult, false);
        }
        // Hidden lines are removed by OCCT's computed mode, which applies to the whole view.
        view->SetComputedMode(style == DisplayStyle::HiddenLine);
    }

    void applyColors() {
        view->SetBackgroundColor(toOcct(colors.background));
        // What is under the pointer gets an outline, which OCCT draws on top of the frame it
        // has kept, without drawing the scene again. What is selected is tinted, with the body
        // still visible through the tint.
        for (const Prs3d_TypeOfHighlight type :
             {Prs3d_TypeOfHighlight_Dynamic, Prs3d_TypeOfHighlight_LocalDynamic}) {
            context->HighlightStyle(type)->SetColor(toOcct(colors.hover));
        }
        for (const Prs3d_TypeOfHighlight type :
             {Prs3d_TypeOfHighlight_Selected, Prs3d_TypeOfHighlight_LocalSelected}) {
            const Handle(Prs3d_Drawer)& selected = context->HighlightStyle(type);
            selected->SetColor(toOcct(colors.selection));
            selected->SetDisplayMode(AIS_Shaded);
            selected->SetTransparency(0.45F);
        }
        cube->setColors({colors.cube, colors.cubeEdge, colors.cubeText, colors.selection, colors.line,
                         {ViewportWidget::originColor(OriginItem::AxisX),
                          ViewportWidget::originColor(OriginItem::AxisY),
                          ViewportWidget::originColor(OriginItem::AxisZ)}});
        if (!previewObject.IsNull()) {
            previewObject->SetColor(toOcct(colors.preview));
            context->Redisplay(previewObject, false);
        }
        restyleBodies();
        for (const ProfilePatch& patch : profilePatches) {
            styleProfile(patch.object, isPicked(patch.profile), false);
            context->Redisplay(patch.object, false);
        }
        for (const auto& entry : sketchObjects) {
            for (const Handle(AIS_Shape)& object :
                 {entry.second.ordinary, entry.second.construction}) {
                if (!object.IsNull()) {
                    styleSketch(object, object == entry.second.construction);
                    context->Redisplay(object, false);
                }
            }
        }
        applyAxes();
        applyGrid();
        showConstructionPlanes();
    }

    void applyAxes() {
        if (axesVisible) {
            view->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, toOcct(colors.line), 0.08, V3d_ZBUFFER);
        } else {
            view->TriedronErase();
        }
    }

    void applyGrid() {
        if (!gridVisible) {
            viewer->DeactivateGrid();
            return;
        }
        // Lines 10 mm apart, covering a square meter around the origin.
        viewer->SetRectangularGridValues(0.0, 0.0, 10.0, 10.0, 0.0);
        viewer->SetRectangularGridGraphicValues(500.0, 500.0, 0.0);
        viewer->ActivateGrid(Aspect_GT_Rectangular, Aspect_GDM_Lines);
        viewer->Grid()->SetColors(toOcct(colors.grid), toOcct(colors.gridMajor));
        // OCCT would otherwise leave a star on the grid point nearest a click in empty space.
        viewer->SetGridEcho(false);
    }

    Handle(AIS_Shape) makeOriginObject(OriginItem item) const {
        const gp_Pnt origin(0.0, 0.0, 0.0);
        const double planeSize = originPlaneSize * pixelRatio;
        const double axisLength = originAxisLength * pixelRatio;
        const auto plane = [&origin, planeSize](const gp_Dir& normal, const gp_Dir& along) {
            return BRepBuilderAPI_MakeFace(gp_Pln(gp_Ax3(origin, normal, along)), 0.0, planeSize,
                                           0.0, planeSize)
                .Shape();
        };
        const auto axis = [&origin, axisLength](const gp_Dir& along) {
            return BRepBuilderAPI_MakeEdge(origin, origin.Translated(gp_Vec(along) * axisLength))
                .Shape();
        };
        TopoDS_Shape shape;
        const QColor color = ViewportWidget::originColor(item);
        bool isPlane = true;
        switch (item) {
        case OriginItem::PlaneXY:
            shape = plane(gp::DZ(), gp::DX());
            break;
        case OriginItem::PlaneXZ:
            shape = plane(gp::DY().Reversed(), gp::DX());
            break;
        case OriginItem::PlaneYZ:
            shape = plane(gp::DX(), gp::DY());
            break;
        case OriginItem::AxisX:
            shape = axis(gp::DX());
            isPlane = false;
            break;
        case OriginItem::AxisY:
            shape = axis(gp::DY());
            isPlane = false;
            break;
        case OriginItem::AxisZ:
            shape = axis(gp::DZ());
            isPlane = false;
            break;
        }
        Handle(AIS_Shape) object = new AIS_Shape(shape);
        object->SetColor(toOcct(color));
        if (isPlane) {
            object->SetTransparency(0.7);
        } else {
            object->SetWidth(2.0);
        }
        // Keeps its size on screen whatever the zoom, anchored at the origin, and is left out
        // when the view is fitted to the model.
        object->SetTransformPersistence(
            new Graphic3d_TransformPers(Graphic3d_TMF_ZoomPers, origin));
        object->SetInfiniteState(true);
        return object;
    }

    // A patch of an origin plane to click on, while a plane is being chosen: larger than the
    // guide, drawn over everything else, and lit up under the pointer.
    //
    // The patches are only slightly see-through (the owner asked to see the model through
    // them, 2026-10-19). Where one lay in front of another, see-through patches would mix their
    // colors, so that the plane a click gets is drawn in a color that belongs to neither; the
    // layout below keeps them from lying in front of each other.
    //
    // Each patch reaches from the origin along its two axes toward the side the viewer was on
    // when the choosing began (pickSides). Three patches laid out that way are the inside of a
    // corner seen from within, and none of them is in front of another. All reaching the
    // positive way, as the guides do, the one in front covers half of each of the others in
    // the home view.
    Handle(AIS_Shape) makePickPlane(OriginItem item) const {
        const double size = pickPlaneSize * pixelRatio;
        // Which of X, Y, Z the patch's own two directions are.
        const std::size_t u = item == OriginItem::PlaneYZ ? 1 : 0;
        const std::size_t v = item == OriginItem::PlaneXY ? 1 : 2;
        const auto range = [this, size](std::size_t axis) {
            return pickSides[axis] > 0 ? std::pair(0.0, size) : std::pair(-size, 0.0);
        };
        // The planes of the component being worked on, where it is: turned as it is, and drawn
        // from the anchor below, which is where it is.
        const kernel::Placement at = editPlacement();
        kernel::Placement turned = at;
        turned.origin = {};
        gp_Ax3 axes = originPlaneAxes(item);
        axes.Transform(kernel::occt::toTrsf(turned));
        Handle(AIS_Shape) object =
            new AIS_Shape(BRepBuilderAPI_MakeFace(gp_Pln(axes), range(u).first, range(u).second,
                                                  range(v).first, range(v).second)
                              .Shape());
        // Not marked as infinite, as the guides are: OCCT draws an infinite shape as its
        // outline only, and these are meant to be seen as planes.
        object->SetTransformPersistence(new Graphic3d_TransformPers(
            Graphic3d_TMF_ZoomPers, gp_Pnt(at.origin.x, at.origin.y, at.origin.z)));
        object->SetZLayer(Graphic3d_ZLayerId_Topmost);
        stylePickPlane(object, item, false);
        return object;
    }

    // A patch as it is at rest, or as it is under the pointer: lighter, with a bold outline in
    // the color of whatever the pointer is on.
    void stylePickPlane(const Handle(AIS_Shape)& object, OriginItem item, bool hovered) const {
        QColor fill = ViewportWidget::originColor(item);
        if (hovered) {
            fill = fill.lighter(140);
        }
        object->SetMaterial(bodyMaterial());
        object->SetColor(toOcct(fill));
        // Slightly see-through, so that the model behind shows; less so under the pointer.
        object->SetTransparency(hovered ? 0.15 : 0.35);
        object->Attributes()->SetFaceBoundaryDraw(true);
        object->Attributes()->SetFaceBoundaryAspect(
            new Prs3d_LineAspect(toOcct(hovered ? colors.hover : colors.bodyEdge), Aspect_TOL_SOLID,
                                 hovered ? 3.0 : 1.0));
    }

    // Lights up the patch the pointer is on, and puts the one it left back.
    void showHoveredPlane(int plane) {
        for (std::size_t i = 0; i < pickPlanes.size(); ++i) {
            if (pickPlanes[i].IsNull()) {
                continue;
            }
            stylePickPlane(pickPlanes[i], static_cast<OriginItem>(i), static_cast<int>(i) == plane);
            context->Redisplay(pickPlanes[i], false);
        }
        redraw();
    }

    void showPickPlanes() {
        hidePickPlanes();
        for (std::size_t i = 0; i < pickPlanes.size(); ++i) {
            pickPlanes[i] = makePickPlane(static_cast<OriginItem>(i));
            context->Display(pickPlanes[i], AIS_Shaded, AIS_Shape::SelectionMode(TopAbs_SHAPE),
                             false);
        }
    }

    void hidePickPlanes() {
        for (Handle(AIS_Shape)& object : pickPlanes) {
            if (!object.IsNull()) {
                context->Remove(object, false);
                object.Nullify();
            }
        }
    }

    // Which plane's patch OCCT last found under the pointer: 0 to 2, or -1.
    [[nodiscard]] int detectedPickPlane() const {
        if (!planePicking || context.IsNull() || !context->HasDetected()) {
            return -1;
        }
        const Handle(AIS_InteractiveObject) under = context->DetectedInteractive();
        for (std::size_t i = 0; i < pickPlanes.size(); ++i) {
            if (!pickPlanes[i].IsNull() && under == pickPlanes[i]) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // A construction plane: see-through amber, more solid while planes are being chosen, and
    // lit up under the pointer.
    void styleConstruction(const Handle(AIS_Shape)& object, bool hovered) const {
        const QColor amber(224, 160, 48);
        object->SetMaterial(bodyMaterial());
        object->SetColor(toOcct(hovered ? amber.lighter(125) : amber));
        object->SetTransparency(planePicking ? (hovered ? 0.2 : 0.45) : 0.75);
        object->Attributes()->SetFaceBoundaryDraw(true);
        object->Attributes()->SetFaceBoundaryAspect(
            new Prs3d_LineAspect(toOcct(hovered ? colors.hover : amber.darker(140)),
                                 Aspect_TOL_SOLID, hovered ? 2.5 : 1.0));
    }

    // Shows the visible construction planes and axes where they now are.
    void showConstructionPlanes() {
        if (context.IsNull()) {
            return;
        }
        for (const auto& entry : constructionObjects) {
            context->Remove(entry.second, false);
        }
        constructionObjects.clear();
        for (const auto& entry : axisObjects) {
            context->Remove(entry.second, false);
        }
        axisObjects.clear();
        if (document == nullptr) {
            return;
        }
        for (const document::Feature& feature : document->features()) {
            if (!isShown(feature.component)) {
                continue;
            }
            if (feature.kind == document::FeatureKind::Axis && feature.visible) {
                showAxis(feature.id);
            }
            if (feature.kind != document::FeatureKind::Plane || !feature.visible) {
                continue;
            }
            const auto state = document->planeState(feature.id);
            if (!state.has_value()) {
                continue;
            }
            kernel::Shape shape;
            try {
                shape = document::planeShape(*state);
            } catch (const std::exception&) {
                continue;
            }
            Handle(AIS_Shape) object = new AIS_Shape(kernel::occt::unwrap(shape));
            place(object, placementFor(feature.component));
            styleConstruction(object, feature.id == hoveredConstruction);
            context->Display(object, AIS_Shaded,
                             planePicking ? AIS_Shape::SelectionMode(TopAbs_SHAPE) : -1, false);
            constructionObjects.emplace(feature.id, object);
        }
    }

    // A construction axis: a dashed amber line through the model.
    void showAxis(document::ObjectId id) {
        const auto state = document->axisState(id);
        if (!state.has_value()) {
            return;
        }
        kernel::Shape shape;
        try {
            shape = document::axisShape(*state);
        } catch (const std::exception&) {
            return;
        }
        Handle(AIS_Shape) object = new AIS_Shape(kernel::occt::unwrap(shape));
        if (const document::Feature* feature = document->findFeature(id)) {
            place(object, placementFor(feature->component));
        }
        const Quantity_Color amber = toOcct(QColor(224, 160, 48));
        object->SetColor(amber);
        const Handle(Prs3d_Drawer)& drawer = object->Attributes();
        drawer->SetWireAspect(new Prs3d_LineAspect(amber, Aspect_TOL_DOTDASH, 2.0));
        drawer->SetLineAspect(new Prs3d_LineAspect(amber, Aspect_TOL_DOTDASH, 2.0));
        context->Display(object, AIS_WireFrame, -1, false);
        axisObjects.emplace(id, object);
    }

    // Which construction plane OCCT last found under the pointer, while planes are chosen.
    [[nodiscard]] document::ObjectId detectedConstruction() const {
        if (!planePicking || context.IsNull() || !context->HasDetected()) {
            return {};
        }
        const Handle(AIS_InteractiveObject) under = context->DetectedInteractive();
        for (const auto& entry : constructionObjects) {
            if (under == entry.second) {
                return entry.first;
            }
        }
        return {};
    }

    [[nodiscard]] bool profilePicking() const { return !profileSketches.empty(); }

    [[nodiscard]] bool isPicked(const Profile& profile) const {
        return std::find(pickedProfiles.begin(), pickedProfiles.end(), profile) !=
               pickedProfiles.end();
    }

    // A region at rest, under the pointer, or picked. See-through, so that what lies under it
    // shows.
    void styleProfile(const Handle(AIS_Shape)& object, bool picked, bool hovered) const {
        QColor fill = picked ? colors.selection : colors.preview;
        if (hovered) {
            fill = fill.lighter(125);
        }
        object->SetMaterial(bodyMaterial());
        object->SetColor(toOcct(fill));
        // Light enough, picked, that the preview of what is made of it shows through.
        object->SetTransparency(picked ? 0.6 : hovered ? 0.5 : 0.8);
        object->Attributes()->SetFaceBoundaryDraw(true);
        object->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(
            toOcct(hovered ? colors.hover : fill), Aspect_TOL_SOLID, hovered ? 2.5 : 1.0));
    }

    void restyleProfiles() {
        for (std::size_t i = 0; i < profilePatches.size(); ++i) {
            const bool hovered = static_cast<int>(i) == hoveredProfile;
            styleProfile(profilePatches[i].object, isPicked(profilePatches[i].profile), hovered);
            // The one under the pointer is drawn over everything, so that it shows where the
            // preview of what is being made covers it.
            context->SetZLayer(profilePatches[i].object,
                               hovered ? Graphic3d_ZLayerId_Topmost : Graphic3d_ZLayerId_Top);
            context->Redisplay(profilePatches[i].object, false);
        }
        redraw();
    }

    void hideProfiles() {
        for (const ProfilePatch& patch : profilePatches) {
            context->Remove(patch.object, false);
        }
        profilePatches.clear();
    }

    // Shows the regions of the sketches that are to be picked from, as the sketches are now.
    // Picked profiles that are no longer there are let go.
    void showProfiles() {
        hideProfiles();
        hoveredProfile = -1;
        pressedProfile = -1;
        if (document == nullptr) {
            return;
        }
        for (const document::ObjectId id : profileSketches) {
            const document::SketchItem* sketch = document->findSketch(id);
            if (sketch == nullptr) {
                continue;
            }
            const std::vector<sketch::Region> regions = sketch->geometry.regions();
            const std::vector<kernel::Shape> faces = document::sketchRegionFaces(*sketch);
            for (std::size_t i = 0; i < regions.size() && i < faces.size(); ++i) {
                if (faces[i].isNull()) {
                    continue;
                }
                Profile profile{id, regions[i].key};
                Handle(AIS_Shape) object = new AIS_Shape(kernel::occt::unwrap(faces[i]));
                place(object, placementFor(sketch->component));
                // Over a body's face that lies in the sketch's plane, but behind whatever is
                // in front of it, such as the preview of what is being made of it. Picking
                // ignores what hides it: nothing else can be picked meanwhile.
                object->SetZLayer(Graphic3d_ZLayerId_Top);
                styleProfile(object, isPicked(profile), false);
                context->Display(object, AIS_Shaded, AIS_Shape::SelectionMode(TopAbs_SHAPE), false);
                profilePatches.push_back({std::move(profile), object});
            }
        }
        std::erase_if(pickedProfiles, [this](const Profile& profile) {
            return std::none_of(
                profilePatches.begin(), profilePatches.end(),
                [&profile](const ProfilePatch& patch) { return patch.profile == profile; });
        });
    }

    // Which region's patch OCCT last found under the pointer, or -1.
    [[nodiscard]] int detectedProfile() const {
        if (!profilePicking() || context.IsNull() || !context->HasDetected()) {
            return -1;
        }
        // A region wins over a face that lies in front of it, or in its plane.
        for (context->InitDetected(); context->MoreDetected(); context->NextDetected()) {
            const Handle(SelectMgr_EntityOwner) owner = context->DetectedCurrentOwner();
            if (owner.IsNull()) {
                continue;
            }
            for (std::size_t i = 0; i < profilePatches.size(); ++i) {
                if (owner->Selectable() == profilePatches[i].object) {
                    return static_cast<int>(i);
                }
            }
        }
        return -1;
    }

    // The selection entry an owner stands for, if it belongs to a body.
    [[nodiscard]] std::optional<SelectedItem>
    itemOf(const Handle(SelectMgr_EntityOwner)& owner) const {
        if (owner.IsNull()) {
            return std::nullopt;
        }
        if (const auto sketch = sketchOfObject.find(owner->Selectable().get());
            sketch != sketchOfObject.end()) {
            return SelectedItem{sketch->second, SelectedItem::Kind::Sketch, -1};
        }
        const auto body = bodyOfObject.find(owner->Selectable().get());
        if (body == bodyOfObject.end()) {
            return std::nullopt;
        }
        SelectedItem item{body->second.body, SelectedItem::Kind::Body, -1, body->second.path};
        const Handle(StdSelect_BRepOwner) shapeOwner = Handle(StdSelect_BRepOwner)::DownCast(owner);
        if (!shapeOwner.IsNull() && shapeOwner->HasShape()) {
            const TopoDS_Shape& part = shapeOwner->Shape();
            const document::Body* source =
                document != nullptr ? document->findBody(body->second.body) : nullptr;
            const int index =
                source != nullptr ? kernel::occt::subShapeIndex(source->shape, part) : -1;
            if (index >= 0) {
                item.index = index;
                item.kind = part.ShapeType() == TopAbs_FACE   ? SelectedItem::Kind::Face
                            : part.ShapeType() == TopAbs_EDGE ? SelectedItem::Kind::Edge
                                                              : SelectedItem::Kind::Vertex;
            }
        }
        return item;
    }

    [[nodiscard]] bool canRender() const { return error.isEmpty() && !view.IsNull(); }

    // Mouse events arrive in Qt's logical pixels; OCCT works in framebuffer pixels.
    [[nodiscard]] Graphic3d_Vec2i toFramebufferPixels(const QPointF& position) const {
        const qreal scale = m_widget->devicePixelRatioF();
        return Graphic3d_Vec2i(static_cast<int>(std::lround(position.x() * scale)),
                               static_cast<int>(std::lround(position.y() * scale)));
    }

    // Draws the scene again. OCCT keeps the last frame and reuses it until told that what is
    // displayed has changed.
    void redraw() {
        if (!view.IsNull()) {
            view->Invalidate();
        }
        // The controller remembers where it last looked under the pointer and does not look
        // again while the pointer stays put. What is there may just have changed.
        ResetPreviousMoveTo();
        m_widget->update();
    }

    [[nodiscard]] bool sketching() const { return activeSketch.isValid(); }

    void applyFilter(const Handle(AIS_Shape)& object) const {
        context->Deactivate(object);
        if (profilePicking()) {
            // Clicks belong to the regions being chosen from, and maybe to faces.
            if (profileFacesToo) {
                context->SetSelectionModeActive(object, AIS_Shape::SelectionMode(TopAbs_FACE), true,
                                                AIS_SelectionModesConcurrency_Multiple);
            }
            return;
        }
        if (sketching()) {
            // While a sketch is open, clicks belong to the sketch. Only the Project tool has
            // use for the bodies: their edges and corners are what it brings into the sketch.
            if (editor->tool() == SketchTool::Project) {
                for (const int mode : {AIS_Shape::SelectionMode(TopAbs_VERTEX),
                                       AIS_Shape::SelectionMode(TopAbs_EDGE)}) {
                    context->SetSelectionModeActive(object, mode, true,
                                                    AIS_SelectionModesConcurrency_Multiple);
                }
            }
            return;
        }
        for (const int mode : selectionModes(filter)) {
            context->SetSelectionModeActive(object, mode, true,
                                            AIS_SelectionModesConcurrency_Multiple);
        }
    }

    // A sketch is picked as a whole, by its curves, when the filter lets edges be picked.
    void applySketchFilter(const Handle(AIS_Shape)& object) const {
        context->Deactivate(object);
        if (!sketching() && !profilePicking() &&
            (filter == SelectionFilter::FacesEdgesVertices || filter == SelectionFilter::Edges)) {
            const int mode = AIS_Shape::SelectionMode(TopAbs_SHAPE);
            context->SetSelectionModeActive(object, mode, true,
                                            AIS_SelectionModesConcurrency_Multiple);
            // After switching the mode on: OCCT keeps count of the reaches in use, and can
            // only change that of a selection it is counting.
            context->SetSelectionSensitivity(object, mode,
                                             static_cast<int>(std::lround(4.0 * pixelRatio)));
        }
    }

    void applyFilters() {
        for (const auto& entry : bodyObjects) {
            applyFilter(entry.second);
        }
        for (const auto& entry : sketchObjects) {
            for (const Handle(AIS_Shape)& object :
                 {entry.second.ordinary, entry.second.construction}) {
                if (!object.IsNull()) {
                    applySketchFilter(object);
                }
            }
        }
    }

    void styleSketch(const Handle(AIS_Shape)& object, bool construction) const {
        const Quantity_Color color = toOcct(colors.sketch);
        object->SetColor(color);
        const Handle(Prs3d_Drawer)& drawer = object->Attributes();
        const auto line = [&color, construction] {
            return new Prs3d_LineAspect(color, construction ? Aspect_TOL_DASH : Aspect_TOL_SOLID,
                                        construction ? 1.0 : 1.6);
        };
        drawer->SetWireAspect(line());
        drawer->SetLineAspect(line());
        drawer->SetFreeBoundaryAspect(line());
        drawer->SetUnFreeBoundaryAspect(line());
        drawer->SetSeenLineAspect(line());
        // Large enough to find on a part: holes and the like go by these.
        drawer->SetPointAspect(new Prs3d_PointAspect(Aspect_TOM_O_POINT, color, 2.2));
    }

    void hideSketch(document::ObjectId id) {
        const auto found = sketchObjects.find(id);
        if (found == sketchObjects.end()) {
            return;
        }
        for (const Handle(AIS_Shape)& object :
             {found->second.ordinary, found->second.construction}) {
            if (!object.IsNull()) {
                context->Remove(object, false);
                sketchOfObject.erase(object.get());
            }
        }
        sketchObjects.erase(found);
    }

    void hideAllSketches() {
        while (!sketchObjects.empty()) {
            hideSketch(sketchObjects.begin()->first);
        }
    }

    // Shows a sketch as it is now, replacing whatever was shown of it. The sketch being edited
    // is not shown this way: the editor draws it.
    void showSketch(const document::SketchItem& sketch) {
        if (context.IsNull()) {
            return;
        }
        hideSketch(sketch.id);
        if (!sketch.visible || sketch.id == activeSketch || !isShown(sketch.component)) {
            return;
        }
        const bool wasEmpty = bodyObjects.empty() && sketchObjects.empty();
        SketchObjects objects;
        const auto make = [this, &sketch](document::SketchPart part) {
            Handle(AIS_Shape) object;
            const kernel::Shape shape = document::sketchShape(sketch, part);
            if (shape.isNull()) {
                return object;
            }
            object = new AIS_Shape(kernel::occt::unwrap(shape));
            place(object, placementFor(sketch.component));
            styleSketch(object, part == document::SketchPart::Construction);
            context->Display(object, AIS_WireFrame, AIS_Shape::SelectionMode(TopAbs_SHAPE), false);
            applySketchFilter(object);
            sketchOfObject.emplace(object.get(), sketch.id);
            return object;
        };
        objects.ordinary = make(document::SketchPart::Ordinary);
        objects.construction = make(document::SketchPart::Construction);
        if (objects.ordinary.IsNull() && objects.construction.IsNull()) {
            return;
        }
        sketchObjects.emplace(sketch.id, objects);
        if (wasEmpty && !sketching()) {
            fitPending = true;
        }
    }

    // Puts the reference grid in the plane of the sketch being edited, or back in the XY plane.
    void applyGridPlane() {
        gp_Ax3 axes;
        const document::SketchItem* sketch =
            document != nullptr && sketching() ? document->findSketch(activeSketch) : nullptr;
        if (sketch != nullptr) {
            const kernel::Plane plane =
                kernel::apply(placementFor(sketch->component), sketch->plane);
            axes = gp_Ax3(gp_Pnt(plane.origin.x, plane.origin.y, plane.origin.z),
                          gp_Dir(plane.normal.x, plane.normal.y, plane.normal.z),
                          gp_Dir(plane.xDirection.x, plane.xDirection.y, plane.xDirection.z));
        }
        viewer->SetPrivilegedPlane(axes);
        applyGrid();
    }

    // Everything that is shown, and the sketch being edited, which is not among OCCT's objects.
    [[nodiscard]] Bnd_Box sceneBounds() const {
        Bnd_Box box = view->View()->MinMaxValues();
        const document::SketchItem* sketch =
            document != nullptr && sketching() ? document->findSketch(activeSketch) : nullptr;
        if (sketch != nullptr) {
            if (const auto bounds = document::sketchBounds(*sketch)) {
                const kernel::Placement at = placementFor(sketch->component);
                for (const kernel::Point& corner : {bounds->min, bounds->max}) {
                    const kernel::Point p = kernel::apply(at, corner);
                    box.Add(gp_Pnt(p.x, p.y, p.z));
                }
            }
        }
        return box;
    }

    // `reshaped` for a body that was shown before with another shape: that one leaves the
    // camera alone even if it is the only body.
    // Shows a body at each visible appearance of its component where it is not shown yet.
    // Whether two faces lie on one round surface, as the halves of a cut cylinder do.
    [[nodiscard]] static bool onOneRoundSurface(const TopoDS_Face& a, const TopoDS_Face& b) {
        const BRepAdaptor_Surface first(a, Standard_False);
        const BRepAdaptor_Surface second(b, Standard_False);
        if (first.GetType() != second.GetType()) {
            return false;
        }
        constexpr double near = 1e-7;
        const auto sameAxis = [](const gp_Ax1& p, const gp_Ax1& q) {
            return p.IsCoaxial(q, 1e-7, near) || p.IsCoaxial(q.Reversed(), 1e-7, near);
        };
        switch (first.GetType()) {
        case GeomAbs_Cylinder:
            return std::abs(first.Cylinder().Radius() - second.Cylinder().Radius()) < near &&
                   sameAxis(first.Cylinder().Axis(), second.Cylinder().Axis());
        case GeomAbs_Sphere:
            return std::abs(first.Sphere().Radius() - second.Sphere().Radius()) < near &&
                   first.Sphere().Location().IsEqual(second.Sphere().Location(), near);
        case GeomAbs_Torus:
            return std::abs(first.Torus().MajorRadius() - second.Torus().MajorRadius()) < near &&
                   std::abs(first.Torus().MinorRadius() - second.Torus().MinorRadius()) < near &&
                   first.Torus().Location().IsEqual(second.Torus().Location(), near) &&
                   sameAxis(first.Torus().Axis(), second.Torus().Axis());
        case GeomAbs_Cone:
            return std::abs(first.Cone().SemiAngle() - second.Cone().SemiAngle()) < near &&
                   first.Cone().Apex().IsEqual(second.Cone().Apex(), near) &&
                   sameAxis(first.Cone().Axis(), second.Cone().Axis());
        default:
            return false;
        }
    }

    // A cylinder, a sphere, or a torus closes on an edge of its own, and one cut in two has
    // an edge between its halves. Those edges are what the faces are bounded by, and stay, for
    // selecting and for the edits that work on them; but nothing is to be seen there, for the
    // surface runs smoothly across. This says so of them, and styleBody() leaves out the edges
    // that are that smooth. It changes no geometry.
    static void smoothSeams(const TopoDS_Shape& shape) {
        TopTools_IndexedDataMapOfShapeListOfShape facesOfEdge;
        TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, facesOfEdge);
        BRep_Builder builder;
        for (Standard_Integer i = 1; i <= facesOfEdge.Extent(); ++i) {
            const TopoDS_Edge& edge = TopoDS::Edge(facesOfEdge.FindKey(i));
            const TopTools_ListOfShape& faces = facesOfEdge(i);
            if (faces.Extent() != 2 || BRep_Tool::Degenerated(edge) ||
                BRep_Tool::MaxContinuity(edge) == GeomAbs_CN) {
                continue;
            }
            const TopoDS_Face& a = TopoDS::Face(faces.First());
            const TopoDS_Face& b = TopoDS::Face(faces.Last());
            if (a.IsSame(b) ? BRep_Tool::IsClosed(edge, a) : onOneRoundSurface(a, b)) {
                builder.Continuity(edge, a, b, GeomAbs_CN);
            }
        }
    }

    // The triangles of a shape are worked out when it is first drawn, which on a big part takes
    // far longer than anything else. Done here first they can be stopped, and the window stays
    // alive. Stopped, the body goes undrawn, for drawing it would do the rest with no way out.
    [[nodiscard]] bool meshedForDrawing(const document::Body& body) const {
        const TopoDS_Shape& shape = kernel::occt::unwrap(body.shape);
        smoothSeams(shape);
        const Handle(Prs3d_Drawer)& drawer = context->DefaultDrawer();
        if (StdPrs_ToolTriangulatedShape::IsTessellated(shape, drawer)) {
            return true;
        }
        kernel::clearStop();
        try {
            document::Document::runWork("drawing " + body.name, [&shape, &drawer] {
                IMeshTools_Parameters parameters;
                parameters.Deflection = StdPrs_ToolTriangulatedShape::GetDeflection(shape, drawer);
                parameters.Angle = drawer->DeviationAngle();
                parameters.InParallel = Standard_True;
                const Handle(kernel::occt::Stoppable) stoppable = new kernel::occt::Stoppable();
                const BRepMesh_IncrementalMesh mesher(shape, parameters, stoppable->Start());
            });
        } catch (const kernel::Error&) {
            return false;
        }
        const bool stopped = kernel::stopRequested();
        kernel::clearStop();
        return !stopped;
    }

    void showBody(const document::Body& body, bool reshaped = false) {
        if (context.IsNull() || body.shape.isNull() ||
            (body.id == previewReplaces && !previewGhost)) {
            // A body whose feature fails, is suppressed, or lies beyond the history marker does
            // not exist at the moment. One that a feature's preview stands in for is not shown.
            return;
        }
        for (const document::Instance& instance : instances()) {
            if (instance.component != body.component || !instance.visible) {
                continue;
            }
            BodyKey key{instance.path, body.id};
            if (bodyObjects.contains(key)) {
                continue;
            }
            const bool wasEmpty = bodyObjects.empty() && !reshaped;
            if (!meshedForDrawing(body)) {
                return;
            }
            Handle(AIS_Shape) object = new BodyObject(kernel::occt::unwrap(body.shape));
            place(object, instance.placement);
            styleBody(object);
            if (ghosted(body.id)) {
                ghost(object);
            }
            // Displaying with the whole-shape mode first prepares the owner that selecting the
            // body as a whole needs, whichever filter is active afterwards.
            context->Display(object, displayMode(), AIS_Shape::SelectionMode(TopAbs_SHAPE), false);
            applyFilter(object);
            bodyOfObject.emplace(object.get(), key);
            bodyObjects.emplace(std::move(key), object);
            if (wasEmpty) {
                // The first body defines the scale of the scene. Later ones leave the camera
                // alone.
                fitPending = true;
            }
        }
    }

    void hideBody(document::ObjectId id) {
        for (auto entry = bodyObjects.begin(); entry != bodyObjects.end();) {
            if (entry->first.body != id) {
                ++entry;
                continue;
            }
            context->Remove(entry->second, false);
            bodyOfObject.erase(entry->second.get());
            entry = bodyObjects.erase(entry);
        }
    }

    // Shows every body at every visible appearance of its component, where that now is, and
    // nothing else.
    void syncBodies() {
        if (context.IsNull()) {
            return;
        }
        for (auto entry = bodyObjects.begin(); entry != bodyObjects.end();) {
            const document::Body* body =
                document != nullptr ? document->findBody(entry->first.body) : nullptr;
            const document::Instance* instance = instanceAt(entry->first.path);
            if (body == nullptr || !body->visible || body->shape.isNull() || instance == nullptr ||
                !instance->visible || instance->component != body->component) {
                context->Remove(entry->second, false);
                bodyOfObject.erase(entry->second.get());
                entry = bodyObjects.erase(entry);
                continue;
            }
            context->SetLocation(entry->second,
                                 TopLoc_Location(kernel::occt::toTrsf(instance->placement)));
            ++entry;
        }
        if (document != nullptr) {
            for (const document::Body& body : document->bodies()) {
                if (body.visible) {
                    showBody(body, true);
                }
            }
        }
    }

    // Puts what is shown once for its component (sketches, construction geometry, the sketch
    // being edited) where its component now is.
    void replaceSingles() {
        if (document == nullptr) {
            return;
        }
        for (const document::SketchItem& sketch : document->sketches()) {
            showSketch(sketch);
        }
        showConstructionPlanes();
        if (sketching()) {
            if (const document::SketchItem* sketch = document->findSketch(activeSketch)) {
                editor->setPlacement(placementFor(sketch->component));
                applyGridPlane();
            }
        }
    }

    void hideAllBodies() {
        for (const auto& entry : bodyObjects) {
            context->Remove(entry.second, false);
        }
        bodyObjects.clear();
        bodyOfObject.clear();
        hideAllSketches();
    }

    void onDocumentChanged(const document::Change& change) {
        if (context.IsNull()) {
            return;
        }
        using Kind = document::Change::Kind;
        const document::Body* body = document->findBody(change.body);
        switch (change.kind) {
        case Kind::BodyAdded:
        case Kind::BodyVisibilityChanged:
            if (body != nullptr && body->visible) {
                showBody(*body);
            } else {
                hideBody(change.body);
            }
            break;
        case Kind::BodyRemoved:
            hideBody(change.body);
            break;
        case Kind::BodyGeometryChanged:
            hideBody(change.body);
            if (body != nullptr && body->visible) {
                showBody(*body, true);
            }
            break;
        case Kind::SketchAdded:
        case Kind::SketchVisibilityChanged:
        case Kind::SketchGeometryChanged:
            if (const document::SketchItem* sketch = document->findSketch(change.body)) {
                showSketch(*sketch);
            } else {
                hideSketch(change.body);
            }
            editor->documentChanged(change);
            if (change.kind == Kind::SketchGeometryChanged &&
                std::find(profileSketches.begin(), profileSketches.end(), change.body) !=
                    profileSketches.end()) {
                showProfiles();
            }
            break;
        case Kind::SketchRemoved:
            hideSketch(change.body);
            if (std::find(profileSketches.begin(), profileSketches.end(), change.body) !=
                profileSketches.end()) {
                showProfiles();
            }
            if (change.body == activeSketch) {
                // The sketch being edited is gone: its creation was undone.
                m_widget->finishSketch();
            }
            break;
        case Kind::BodyRenamed:
        case Kind::TimelineChanged:
            // Construction planes come and go and move with the timeline.
            showConstructionPlanes();
            break;
        case Kind::AssemblyChanged:
        case Kind::PlacementsChanged:
            syncBodies();
            replaceSingles();
            break;
        case Kind::SketchRenamed:
        case Kind::ParametersChanged:
        case Kind::HistoryChanged:
        case Kind::DrawingsChanged:
        case Kind::MachiningChanged:
        case Kind::MaterialsChanged:
            return;
        }
        redraw();
        reportSelection();
    }

    [[nodiscard]] std::vector<SelectedItem> readSelection() const {
        std::vector<SelectedItem> items;
        if (context.IsNull()) {
            return items;
        }
        for (context->InitSelected(); context->MoreSelected(); context->NextSelected()) {
            if (const auto item = itemOf(context->SelectedOwner())) {
                items.push_back(*item);
            }
        }
        return items;
    }

    // Tells listeners when the selection is not what they were last told.
    void reportSelection() {
        std::vector<SelectedItem> current = readSelection();
        if (current != lastSelection) {
            lastSelection = std::move(current);
            emit m_widget->selectionChanged();
        }
    }

    void OnSelectionChanged(const Handle(AIS_InteractiveContext)& interactiveContext,
                            const Handle(V3d_View)& changedView) override {
        AIS_ViewController::OnSelectionChanged(interactiveContext, changedView);
        reportSelection();
    }

    void handleViewRedraw(const Handle(AIS_InteractiveContext)& interactiveContext,
                          const Handle(V3d_View)& redrawnView) override {
        AIS_ViewController::handleViewRedraw(interactiveContext, redrawnView);
        if (myToAskNextFrame) {
            // A camera animation is still running.
            m_widget->update();
        }
    }

private:
    ViewportWidget* m_widget;
};

ViewportWidget::ViewportWidget(QWidget* parent)
    : QOpenGLWidget(parent), m_impl(std::make_unique<Impl>(this)) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    setFormat(surfaceFormat());
    m_impl->editor = new SketchEditor(this);
    m_impl->pointsOverlay = new PointsOverlay(this, [this](const kernel::Point& p) {
        return project(p.x, p.y, p.z);
    });
    m_impl->cube = new NavigationCube(this);
    m_impl->turn = new QVariantAnimation(this);
    m_impl->turn->setDuration(cubeTurnTime);
    m_impl->turn->setStartValue(0.0);
    m_impl->turn->setEndValue(1.0);
    m_impl->turn->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_impl->turn, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) { m_impl->stepTurn(value.toDouble()); });
    connect(m_impl->turn, &QVariantAnimation::finished, this, [this] { m_impl->finishTurn(); });
    connect(m_impl->cube, &NavigationCube::viewChosen, this, [this](const QVector3D& from) {
        // The view one is in already: there is nowhere to go but home.
        const gp_Dir back = m_impl->view->Camera()->Direction().Reversed();
        if (std::abs(back.X() - from.x()) + std::abs(back.Y() - from.y()) +
                std::abs(back.Z() - from.z()) <
            1e-3) {
            m_impl->turnHome();
        } else {
            m_impl->turnToView(from, false);
        }
    });
    connect(m_impl->cube, &NavigationCube::framedViewChosen, this,
            [this](const QVector3D& from) { m_impl->turnToView(from, true); });
    connect(m_impl->cube, &NavigationCube::homeChosen, this, [this] { m_impl->turnHome(); });
    connect(m_impl->cube, &NavigationCube::turnAsked, this,
            [this](double aroundRight, double aroundUp) { m_impl->turnBy(aroundRight, aroundUp); });
    connect(m_impl->cube, &NavigationCube::projectionToggled, this, [this] {
        setProjection(projection() == Projection::Perspective ? Projection::Orthographic
                                                              : Projection::Perspective);
    });
    connect(m_impl->cube, &NavigationCube::orbitStarted, this,
            [this](const QPointF& position) { m_impl->startOrbit(position); });
    connect(m_impl->cube, &NavigationCube::orbitMoved, this, [this](const QPointF& position) {
        m_impl->orbitTo(position);
        update();
    });
    connect(m_impl->cube, &NavigationCube::orbitEnded, this, [this](const QPointF& position) {
        m_impl->endOrbit(position);
        update();
    });
    // Whether bodies can be pointed at while a sketch is open depends on the sketch tool.
    connect(m_impl->editor, &SketchEditor::changed, this, [this] {
        const bool projecting =
            m_impl->sketching() && m_impl->editor->tool() == SketchTool::Project;
        if (projecting != m_impl->projecting && !m_impl->context.IsNull()) {
            m_impl->projecting = projecting;
            m_impl->context->ClearDetected(false);
            m_impl->applyFilters();
            m_impl->redraw();
        }
    });

    try {
        Handle(Aspect_DisplayConnection) display = new Aspect_DisplayConnection();
        Handle(OpenGl_GraphicDriver) driver = new OpenGl_GraphicDriver(display, false);
        // Qt owns the OpenGL context and the buffer swap. OCCT renders into buffers of its own
        // and copies the finished frame into the framebuffer Qt provides.
        driver->ChangeOptions().buffersNoSwap = true;
        driver->ChangeOptions().buffersOpaqueAlpha = true;
        driver->ChangeOptions().useSystemBuffer = false;

        m_impl->viewer = new V3d_Viewer(driver);
        // Soft light from everywhere, plus two lights that travel with the camera: the main one
        // from above the viewer's left shoulder and a weaker one from the right. The sides of
        // a body then differ in brightness from whichever direction it is looked at, and none
        // of them goes black. A light pointing straight ahead would shade all three visible
        // sides of a box alike in the isometric view.
        Handle(V3d_AmbientLight) ambient = new V3d_AmbientLight(Quantity_NOC_WHITE);
        ambient->SetIntensity(0.8F);
        Handle(V3d_DirectionalLight) key =
            new V3d_DirectionalLight(gp_Dir(0.35, -0.55, -0.75), Quantity_NOC_WHITE);
        key->SetHeadlight(true);
        key->SetIntensity(0.9F);
        Handle(V3d_DirectionalLight) fill =
            new V3d_DirectionalLight(gp_Dir(-0.6, -0.1, -0.8), Quantity_NOC_WHITE);
        fill->SetHeadlight(true);
        fill->SetIntensity(0.4F);
        m_impl->viewer->AddLight(ambient);
        m_impl->viewer->AddLight(key);
        m_impl->viewer->AddLight(fill);
        m_impl->viewer->SetLightOn();

        m_impl->context = new AIS_InteractiveContext(m_impl->viewer);
        const Handle(Prs3d_Drawer)& drawer = m_impl->context->DefaultDrawer();
        // Finer than OCCT's default of 20 degrees, so that round shapes have round outlines.
        drawer->SetDeviationAngle(10.0 * M_PI / 180.0);
        // The polygonal algorithm for removing hidden lines: fast, and exact enough to look at.
        drawer->SetTypeOfHLR(Prs3d_TOH_PolyAlgo);

        // The view gets its window in initializeGL(), once Qt has created the OpenGL context.
        m_impl->view = m_impl->viewer->CreateView();
        m_impl->view->SetImmediateUpdate(false);
        m_impl->view->SetProj(V3d_XposYnegZpos); // the usual isometric CAD view
        m_impl->view->ChangeRenderingParams().NbMsaaSamples = 4;

        m_impl->applyColors();
    } catch (const Standard_Failure& failure) {
        m_impl->view.Nullify();
        m_impl->error = tr("The 3D viewer could not connect to the display: %1")
                            .arg(QString::fromUtf8(failure.GetMessageString()));
        qWarning("%s", qPrintable(m_impl->error));
    }
}

ViewportWidget::~ViewportWidget() {
    setDocument(nullptr);
    if (m_impl->viewer.IsNull()) {
        return;
    }
    // OCCT frees its OpenGL resources as the viewer goes away, so Qt's context has to be
    // current while that happens. OCCT may make the context current through its own X
    // connection along the way; that connection must stay open until Qt has taken the context
    // back.
    Handle(Aspect_DisplayConnection) display = m_impl->viewer->Driver()->GetDisplayConnection();
    makeCurrent();
    m_impl->context->RemoveAll(false);
    m_impl->previewObject.Nullify();
    m_impl->featureResult.Nullify();
    m_impl->profilePatches.clear();
    m_impl->constructionObjects.clear();
    m_impl->axisObjects.clear();
    for (Handle(AIS_Shape)& object : m_impl->originObjects) {
        object.Nullify();
    }
    for (Handle(AIS_Shape)& object : m_impl->pickPlanes) {
        object.Nullify();
    }
    m_impl->context.Nullify();
    if (!m_impl->view.IsNull()) {
        m_impl->view->Remove();
        m_impl->view.Nullify();
    }
    m_impl->viewer.Nullify();
    makeCurrent();
    display.Nullify();
    doneCurrent();
    m_impl->anchorWindow.reset();
}

QSurfaceFormat ViewportWidget::surfaceFormat() {
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
#if defined(Q_OS_MACOS)
    // macOS gives OpenGL 2.1 to a compatibility context, and 4.1 only to a core one, which OCCT
    // draws with as well.
    format.setVersion(4, 1);
    format.setProfile(QSurfaceFormat::CoreProfile);
#else
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
#endif
    return format;
}

void ViewportWidget::setDocument(document::Document* document) {
    if (document == m_impl->document) {
        return;
    }
    // The sketch being edited belongs to the document that is leaving. The camera is the next
    // document's business.
    m_impl->cameraBeforeSketch.reset();
    m_impl->fitAfterSketch = false;
    finishSketch();
    // So are the previews of a command, and the profiles it was picking from.
    setProfileSketches({});
    setFeaturePreview({}, kernel::Shape(), false);
    if (m_impl->document != nullptr) {
        m_impl->document->removeListener(m_impl->listener);
        m_impl->listener = 0;
    }
    m_impl->document = document;
    m_impl->editPath.clear();
    m_impl->shownInstances.clear();
    if (m_impl->context.IsNull()) {
        return;
    }
    setHighlights({});
    setFrameMarkers({});
    clearDistanceHandle();
    m_impl->hideAllBodies();
    setPreview(kernel::Shape());
    if (document != nullptr) {
        m_impl->listener = document->addListener(
            [this](const document::Change& change) { m_impl->onDocumentChanged(change); });
        for (const document::Body& body : document->bodies()) {
            if (body.visible) {
                m_impl->showBody(body);
            }
        }
        for (const document::SketchItem& sketch : document->sketches()) {
            m_impl->showSketch(sketch);
        }
    }
    m_impl->showConstructionPlanes();
    fitAll();
    m_impl->reportSelection();
}

document::Document* ViewportWidget::document() const {
    return m_impl->document;
}

void ViewportWidget::setPreview(const kernel::Shape& shape, bool rootCoordinates) {
    if (m_impl->context.IsNull()) {
        return;
    }
    if (!m_impl->previewObject.IsNull()) {
        m_impl->context->Remove(m_impl->previewObject, false);
        m_impl->previewObject.Nullify();
    }
    if (!shape.isNull()) {
        m_impl->previewObject = new AIS_Shape(kernel::occt::unwrap(shape));
        // What a command makes goes into the component being worked on.
        Impl::place(m_impl->previewObject,
                    rootCoordinates ? kernel::Placement{} : m_impl->editPlacement());
        m_impl->previewObject->SetMaterial(bodyMaterial());
        m_impl->previewObject->SetColor(toOcct(m_impl->colors.preview));
        m_impl->previewObject->Attributes()->SetFaceBoundaryDraw(true);
        m_impl->previewObject->Attributes()->SetFaceBoundaryAspect(
            new Prs3d_LineAspect(toOcct(m_impl->colors.bodyEdge), Aspect_TOL_SOLID, 1.0));
        m_impl->previewObject->SetTransparency(0.6);
        // Selection mode -1: displayed, but nothing of it can be picked.
        m_impl->context->Display(m_impl->previewObject, AIS_Shaded, -1, false);
        if (m_impl->bodyObjects.empty()) {
            // Nothing else gives the scene a scale yet.
            m_impl->fitPending = true;
        }
    }
    m_impl->redraw();
}

void ViewportWidget::setFeaturePreview(document::ObjectId replaces, const kernel::Shape& result,
                                       bool pickable) {
    if (m_impl->context.IsNull()) {
        return;
    }
    if (!m_impl->featureResult.IsNull()) {
        m_impl->context->Remove(m_impl->featureResult, false);
        m_impl->featureResult.Nullify();
    }
    const document::ObjectId was = m_impl->previewReplaces;
    const bool wasGhost = m_impl->previewGhost;
    m_impl->previewReplaces = result.isNull() ? document::ObjectId{} : replaces;
    m_impl->previewGhost = m_impl->previewReplaces.isValid() && pickable;
    if (was != m_impl->previewReplaces || wasGhost != m_impl->previewGhost) {
        // The body the preview stood in for is drawn as it was, and the one it stands in for
        // now hidden or made a ghost. A ghost is changed in place, which keeps what of it is
        // selected.
        if (was.isValid() && m_impl->document != nullptr) {
            if (wasGhost) {
                m_impl->restyle(was);
            } else if (const document::Body* body = m_impl->document->findBody(was);
                       body != nullptr && body->visible) {
                m_impl->showBody(*body, true);
            }
        }
        if (m_impl->previewGhost) {
            m_impl->restyle(m_impl->previewReplaces);
        } else if (m_impl->previewReplaces.isValid()) {
            m_impl->hideBody(m_impl->previewReplaces);
        }
    }
    if (!result.isNull()) {
        m_impl->featureResult = new AIS_Shape(kernel::occt::unwrap(result));
        Impl::place(m_impl->featureResult,
                    replaces.isValid() && m_impl->document != nullptr &&
                            m_impl->document->findBody(replaces) != nullptr
                        ? m_impl->placementFor(m_impl->componentOfBody(replaces))
                        : m_impl->editPlacement());
        m_impl->styleBody(m_impl->featureResult);
        // Selection mode -1: displayed, but nothing of it can be picked.
        m_impl->context->Display(m_impl->featureResult, m_impl->displayMode(), -1, false);
    }
    m_impl->redraw();
    m_impl->reportSelection();
}

void ViewportWidget::setEditPath(const std::vector<document::ObjectId>& path) {
    if (m_impl->editPath == path) {
        return;
    }
    m_impl->editPath = path;
    if (m_impl->context.IsNull()) {
        return;
    }
    if (m_impl->planePicking) {
        m_impl->showPickPlanes();
    }
    m_impl->replaceSingles();
    m_impl->redraw();
}

std::vector<document::ObjectId> ViewportWidget::editPath() const {
    return m_impl->editPath;
}

void ViewportWidget::showInstances(const std::vector<document::Instance>& instances) {
    m_impl->shownInstances = instances;
    if (m_impl->context.IsNull()) {
        return;
    }
    m_impl->syncBodies();
    m_impl->replaceSingles();
    m_impl->redraw();
}

void ViewportWidget::setToolpaths(const ToolpathLines& lines) {
    if (m_impl->context.IsNull()) {
        return;
    }
    const auto replace = [this](Handle(AIS_InteractiveObject)& object,
                                const std::vector<std::vector<kernel::Point>>& paths,
                                const QColor& color, Aspect_TypeOfLine style, double width) {
        if (!object.IsNull()) {
            m_impl->context->Remove(object, false);
            object.Nullify();
        }
        if (!paths.empty()) {
            object = new LinesObject(paths, toOcct(color), style, width);
            // Over the bodies: what runs inside them is what is looked at here.
            object->SetZLayer(Graphic3d_ZLayerId_Topmost);
            m_impl->context->Display(object, 0, -1, false);
        }
    };
    replace(m_impl->cutObject, lines.cuts, m_impl->colors.selection, Aspect_TOL_SOLID, 1.0);
    replace(m_impl->rapidObject, lines.rapids, m_impl->colors.sketchConflict, Aspect_TOL_DOT, 1.0);
    replace(m_impl->stockObject, lines.stock, m_impl->colors.line, Aspect_TOL_DASH, 1.0);
    m_impl->redraw();
}

void ViewportWidget::setStockSurface(const kernel::Mesh& mesh) {
    if (m_impl->context.IsNull()) {
        return;
    }
    const bool wasShown = !m_impl->surfaceObject.IsNull();
    if (wasShown) {
        m_impl->context->Remove(m_impl->surfaceObject, false);
        m_impl->surfaceObject.Nullify();
    }
    const bool showing = !mesh.triangles.empty();
    if (showing) {
        Handle(SurfaceObject) surface = new SurfaceObject(mesh);
        surface->Attributes()->SetShadingAspect(new Prs3d_ShadingAspect());
        surface->Attributes()->ShadingAspect()->SetMaterial(bodyMaterial());
        surface->Attributes()->ShadingAspect()->SetColor(toOcct(m_impl->colors.body));
        m_impl->surfaceObject = surface;
        m_impl->context->Display(m_impl->surfaceObject, 0, -1, false);
    }
    if (showing != wasShown) {
        for (const auto& entry : m_impl->bodyObjects) {
            if (showing) {
                m_impl->context->Erase(entry.second, false);
            } else {
                m_impl->context->Display(entry.second, false);
            }
        }
    }
    m_impl->redraw();
}

void ViewportWidget::setToolShape(const kernel::Shape& shape) {
    if (m_impl->context.IsNull()) {
        return;
    }
    if (!m_impl->toolObject.IsNull()) {
        m_impl->context->Remove(m_impl->toolObject, false);
        m_impl->toolObject.Nullify();
    }
    if (!shape.isNull()) {
        m_impl->toolObject = new AIS_Shape(kernel::occt::unwrap(shape));
        m_impl->toolObject->SetMaterial(bodyMaterial());
        m_impl->toolObject->SetColor(toOcct(m_impl->colors.preview));
        m_impl->context->Display(m_impl->toolObject, AIS_Shaded, -1, false);
    }
    m_impl->redraw();
}

void ViewportWidget::setHighlights(const std::vector<kernel::Shape>& shapes) {
    if (m_impl->context.IsNull()) {
        return;
    }
    for (const Handle(AIS_Shape)& object : m_impl->highlightObjects) {
        m_impl->context->Remove(object, false);
    }
    m_impl->highlightObjects.clear();
    for (const kernel::Shape& shape : shapes) {
        if (shape.isNull()) {
            continue;
        }
        Handle(AIS_Shape) object = new AIS_Shape(kernel::occt::unwrap(shape));
        object->SetMaterial(bodyMaterial());
        object->SetColor(toOcct(m_impl->colors.sketchConflict));
        object->SetTransparency(0.25);
        object->Attributes()->SetFaceBoundaryDraw(true);
        object->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(
            toOcct(m_impl->colors.sketchConflict.darker(150)), Aspect_TOL_SOLID, 1.5));
        // Inside the bodies it is where they overlap: drawn over them.
        object->SetZLayer(Graphic3d_ZLayerId_Topmost);
        m_impl->context->Display(object, AIS_Shaded, -1, false);
        m_impl->highlightObjects.push_back(object);
    }
    m_impl->redraw();
}

void ViewportWidget::setFrameMarkers(const std::vector<kernel::Placement>& frames) {
    if (m_impl->context.IsNull()) {
        return;
    }
    for (const Handle(AIS_Shape)& object : m_impl->markerObjects) {
        m_impl->context->Remove(object, false);
    }
    m_impl->markerObjects.clear();
    // As long on screen at any zoom as the origin guides.
    const double length = 40.0 * m_impl->pixelRatio;
    for (const kernel::Placement& frame : frames) {
        // Drawn from the anchor, which is where the frame is, in pixels.
        const gp_Pnt origin(frame.origin.x, frame.origin.y, frame.origin.z);
        const std::array<std::pair<kernel::Vector, OriginItem>, 3> arrows{{
            {frame.xAxis, OriginItem::AxisX},
            {frame.yAxis, OriginItem::AxisY},
            {frame.zAxis, OriginItem::AxisZ},
        }};
        for (const auto& [direction, item] : arrows) {
            const gp_Pnt start(0.0, 0.0, 0.0);
            const gp_Pnt tip =
                start.Translated(gp_Vec(direction.x, direction.y, direction.z) * length);
            Handle(AIS_Shape) object = new AIS_Shape(BRepBuilderAPI_MakeEdge(start, tip).Shape());
            object->SetColor(toOcct(originColor(item)));
            object->SetWidth(item == OriginItem::AxisZ ? 3.5 : 2.5);
            object->SetTransformPersistence(
                new Graphic3d_TransformPers(Graphic3d_TMF_ZoomPers, origin));
            object->SetZLayer(Graphic3d_ZLayerId_Topmost);
            m_impl->context->Display(object, AIS_WireFrame, -1, false);
            m_impl->markerObjects.push_back(object);
        }
    }
    m_impl->redraw();
}

void ViewportWidget::setDistanceHandle(const kernel::Point& base, const kernel::Vector& direction,
                                       double distance) {
    const double length = std::hypot(direction.x, direction.y, direction.z);
    if (!(length > 1e-12) || !std::isfinite(distance)) {
        clearDistanceHandle();
        return;
    }
    Impl::DistanceHandle& handle = m_impl->handle;
    handle.shown = true;
    handle.base = base;
    handle.direction = {direction.x / length, direction.y / length, direction.z / length};
    handle.distance = distance;
    if (handle.field == nullptr) {
        handle.field = new QLineEdit(this);
        handle.field->setObjectName(QStringLiteral("distanceValue"));
        handle.field->setAlignment(Qt::AlignCenter);
        handle.field->setToolTip(tr("How far: type a distance, negative for the other way, and "
                                    "press Enter. Or drag the arrow."));
        connect(handle.field, &QLineEdit::returnPressed, this, [this] {
            const QString text = m_impl->handle.field->text();
            m_impl->handle.field->clearFocus();
            setFocus();
            emit distanceEntered(text);
        });
        handle.field->show();
    }
    m_impl->showHandle();
    m_impl->placeHandleField();
    m_impl->redraw();
}

void ViewportWidget::clearDistanceHandle() {
    Impl::DistanceHandle& handle = m_impl->handle;
    if (!handle.shown && handle.field == nullptr) {
        return;
    }
    handle.shown = false;
    handle.dragging = false;
    handle.hovered = false;
    if (handle.field != nullptr) {
        handle.field->hide();
        handle.field->deleteLater();
        handle.field = nullptr;
    }
    m_impl->showHandle();
    unsetCursor();
    m_impl->redraw();
}

bool ViewportWidget::hasDistanceHandle() const {
    return m_impl->handle.shown;
}

QPointF ViewportWidget::distanceHandlePosition() const {
    if (!m_impl->handle.shown || !m_impl->canRender()) {
        return {};
    }
    return m_impl->handleOnScreen().first;
}

QPointF ViewportWidget::distanceHandleGrip() const {
    if (!m_impl->handle.shown || !m_impl->canRender()) {
        return {};
    }
    const auto [from, to] = m_impl->handleOnScreen();
    return from + (to - from) * 0.4;
}

QLineEdit* ViewportWidget::distanceField() const {
    return m_impl->handle.field;
}

void ViewportWidget::fitAll() {
    // Framing needs the view's size, which is only known while painting.
    m_impl->fitPending = true;
    m_impl->ResetPreviousMoveTo();
    update();
}

void ViewportWidget::setStandardView(StandardView view) {
    if (!m_impl->canRender()) {
        return;
    }
    m_impl->AbortViewAnimation();
    m_impl->view->SetProj(toOcct(view));
    m_impl->ResetPreviousMoveTo();
    fitAll();
}

void ViewportWidget::setProjection(Projection projection) {
    if (!m_impl->canRender()) {
        return;
    }
    m_impl->view->Camera()->SetProjectionType(projection == Projection::Perspective
                                                  ? Graphic3d_Camera::Projection_Perspective
                                                  : Graphic3d_Camera::Projection_Orthographic);
    m_impl->view->Invalidate();
    m_impl->cube->setPerspective(projection == Projection::Perspective);
    update();
    emit projectionChanged();
}

Projection ViewportWidget::projection() const {
    if (m_impl->canRender() && !m_impl->view->Camera()->IsOrthographic()) {
        return Projection::Perspective;
    }
    return Projection::Orthographic;
}

void ViewportWidget::setZoomReversed(bool reversed) {
    m_impl->zoomReversed = reversed;
}

bool ViewportWidget::isZoomReversed() const {
    return m_impl->zoomReversed;
}

QVector3D ViewportWidget::viewDirection() const {
    if (!m_impl->canRender()) {
        return {};
    }
    const gp_Dir direction = m_impl->view->Camera()->Direction();
    return QVector3D(static_cast<float>(direction.X()), static_cast<float>(direction.Y()),
                     static_cast<float>(direction.Z()));
}

QPointF ViewportWidget::project(double x, double y, double z) const {
    if (!m_impl->canRender() || m_impl->view->Window().IsNull()) {
        return {};
    }
    // Through the camera, not V3d_View::Convert(), which rounds to whole pixels: what is drawn
    // over the view has to sit exactly on what the view draws.
    const gp_Pnt device = m_impl->view->Camera()->Project(gp_Pnt(x, y, z));
    int widthPixels = 0;
    int heightPixels = 0;
    m_impl->view->Window()->Size(widthPixels, heightPixels);
    const qreal scale = devicePixelRatioF();
    return QPointF((device.X() + 1.0) / 2.0 * widthPixels / scale,
                   (1.0 - device.Y()) / 2.0 * heightPixels / scale);
}

QPointF ViewportWidget::viewCubeCenter() const {
    return m_impl->cube->pos() + m_impl->cube->cubeCenter();
}

NavigationCube* ViewportWidget::navigationCube() const {
    return m_impl->cube;
}

std::optional<QPointF> ViewportWidget::positionOnPlane(const kernel::Plane& plane,
                                                       const QPointF& position) const {
    if (!m_impl->canRender() || m_impl->view->Window().IsNull()) {
        return std::nullopt;
    }
    // The line of sight through that position, from the camera: the exact inverse of
    // project(), with no rounding to whole pixels.
    int widthPixels = 0;
    int heightPixels = 0;
    m_impl->view->Window()->Size(widthPixels, heightPixels);
    if (widthPixels <= 0 || heightPixels <= 0) {
        return std::nullopt;
    }
    const qreal scale = devicePixelRatioF();
    const double deviceX = position.x() * scale / widthPixels * 2.0 - 1.0;
    const double deviceY = 1.0 - position.y() * scale / heightPixels * 2.0;
    const Handle(Graphic3d_Camera)& camera = m_impl->view->Camera();
    const gp_Pnt nearer = camera->UnProject(gp_Pnt(deviceX, deviceY, -1.0));
    const gp_Pnt farther = camera->UnProject(gp_Pnt(deviceX, deviceY, 1.0));
    const double reach = nearer.Distance(farther);
    if (!(reach > 1e-12)) {
        return std::nullopt;
    }
    const double x = nearer.X();
    const double y = nearer.Y();
    const double z = nearer.Z();
    const double dx = (farther.X() - x) / reach;
    const double dy = (farther.Y() - y) / reach;
    const double dz = (farther.Z() - z) / reach;
    // Where the line through (x, y, z) along (dx, dy, dz) meets the plane.
    const double facing = dx * plane.normal.x + dy * plane.normal.y + dz * plane.normal.z;
    if (std::abs(facing) < 1e-6) {
        return std::nullopt;
    }
    const double along =
        ((plane.origin.x - x) * plane.normal.x + (plane.origin.y - y) * plane.normal.y +
         (plane.origin.z - z) * plane.normal.z) /
        facing;
    const kernel::PlaneCoordinates local =
        kernel::toLocal(plane, {x + along * dx, y + along * dy, z + along * dz});
    if (!std::isfinite(local.u) || !std::isfinite(local.v)) {
        return std::nullopt;
    }
    return QPointF(local.u, local.v);
}

void ViewportWidget::lookAt(const kernel::Plane& plane) {
    if (!m_impl->canRender()) {
        return;
    }
    m_impl->AbortViewAnimation();
    const kernel::Vector v = kernel::yDirection(plane);
    const gp_Dir normal(plane.normal.x, plane.normal.y, plane.normal.z);
    const Handle(Graphic3d_Camera)& camera = m_impl->view->Camera();
    const gp_Pnt center = camera->Center();
    const double distance = camera->Distance();
    camera->SetEyeAndCenter(center.Translated(gp_Vec(normal) * distance), center);
    camera->SetUp(gp_Dir(v.x, v.y, v.z));
    m_impl->redraw();
}

bool ViewportWidget::editSketch(document::ObjectId sketch) {
    if (!m_impl->canRender() || m_impl->document == nullptr) {
        return false;
    }
    const document::SketchItem* found = m_impl->document->findSketch(sketch);
    if (found == nullptr) {
        return false;
    }
    if (m_impl->activeSketch == sketch) {
        return true;
    }
    finishSketch();
    // Before anything changes: where the camera was, and whether it was framing anything.
    m_impl->cameraBeforeSketch = cameraState();
    m_impl->fitAfterSketch = m_impl->view->View()->MinMaxValues().IsVoid();
    setPreview(kernel::Shape());

    m_impl->activeSketch = sketch;
    m_impl->context->ClearSelected(false);
    m_impl->context->ClearDetected(false);
    m_impl->applyFilters();
    // The editor draws the sketch from here on.
    m_impl->hideSketch(sketch);
    setProjection(Projection::Orthographic);
    // A sketch of a component is edited where the component is.
    const kernel::Placement at = m_impl->placementFor(found->component);
    m_impl->editor->setPlacement(at);
    lookAt(kernel::apply(at, found->plane));
    m_impl->applyGridPlane();
    m_impl->editor->begin(m_impl->document, sketch);
    fitAll();
    m_impl->reportSelection();
    emit sketchEditingChanged();
    return true;
}

void ViewportWidget::finishSketch() {
    if (!m_impl->sketching()) {
        return;
    }
    // No sketch is open from here on, for whoever asks while the editor is winding up.
    const document::ObjectId closed = m_impl->activeSketch;
    m_impl->activeSketch = {};
    m_impl->editor->end();
    if (!m_impl->context.IsNull()) {
        m_impl->applyGridPlane();
        if (m_impl->document != nullptr) {
            if (const document::SketchItem* sketch = m_impl->document->findSketch(closed)) {
                m_impl->showSketch(*sketch);
            }
        }
        m_impl->applyFilters();
        if (m_impl->cameraBeforeSketch.has_value()) {
            setCameraState(*m_impl->cameraBeforeSketch);
            if (m_impl->fitAfterSketch) {
                // The view was of an empty scene; there is something to look at now.
                fitAll();
            }
        }
        m_impl->redraw();
    }
    m_impl->cameraBeforeSketch.reset();
    m_impl->fitAfterSketch = false;
    emit sketchEditingChanged();
}

document::ObjectId ViewportWidget::editedSketch() const {
    return m_impl->activeSketch;
}

SketchEditor* ViewportWidget::sketchEditor() const {
    return m_impl->editor;
}

void ViewportWidget::fitSelection() {
    if (!m_impl->canRender() || m_impl->view->Window().IsNull() ||
        m_impl->context->NbSelected() == 0) {
        fitAll();
        return;
    }
    m_impl->AbortViewAnimation();
    m_impl->context->FitSelected(m_impl->view, 0.05, false);
    m_impl->redraw();
}

void ViewportWidget::setNavigationPreset(NavigationPreset preset) {
    m_impl->preset = preset;
    m_impl->applyPreset();
}

NavigationPreset ViewportWidget::navigationPreset() const {
    return m_impl->preset;
}

void ViewportWidget::setAnimated(bool animated) {
    m_impl->animated = animated;
}

bool ViewportWidget::isAnimated() const {
    return m_impl->animated;
}

CameraState ViewportWidget::cameraState() const {
    CameraState state;
    if (!m_impl->canRender()) {
        return state;
    }
    const Handle(Graphic3d_Camera)& camera = m_impl->view->Camera();
    const gp_Pnt eye = camera->Eye();
    const gp_Pnt center = camera->Center();
    const gp_Dir up = camera->Up();
    state.eye = {eye.X(), eye.Y(), eye.Z()};
    state.center = {center.X(), center.Y(), center.Z()};
    state.up = {up.X(), up.Y(), up.Z()};
    state.scale = camera->Scale();
    state.projection = projection();
    return state;
}

void ViewportWidget::setCameraState(const CameraState& state) {
    if (!m_impl->canRender()) {
        return;
    }
    m_impl->AbortViewAnimation();
    setProjection(state.projection);
    const Handle(Graphic3d_Camera)& camera = m_impl->view->Camera();
    camera->SetUp(gp_Dir(state.up[0], state.up[1], state.up[2]));
    camera->SetEyeAndCenter(gp_Pnt(state.eye[0], state.eye[1], state.eye[2]),
                            gp_Pnt(state.center[0], state.center[1], state.center[2]));
    camera->SetScale(state.scale);
    m_impl->fitPending = false;
    m_impl->redraw();
}

void ViewportWidget::setColors(const ViewportColors& colors) {
    m_impl->colors = colors;
    if (m_impl->canRender()) {
        m_impl->applyColors();
        m_impl->redraw();
    }
}

ViewportColors ViewportWidget::colors() const {
    return m_impl->colors;
}

void ViewportWidget::setDisplayStyle(DisplayStyle style) {
    if (style == m_impl->style) {
        return;
    }
    m_impl->style = style;
    if (m_impl->canRender()) {
        m_impl->restyleBodies();
        m_impl->redraw();
    }
}

DisplayStyle ViewportWidget::displayStyle() const {
    return m_impl->style;
}

void ViewportWidget::setGridVisible(bool visible) {
    m_impl->gridVisible = visible;
    if (m_impl->canRender()) {
        m_impl->applyGrid();
        m_impl->redraw();
    }
}

bool ViewportWidget::isGridVisible() const {
    return m_impl->gridVisible;
}

void ViewportWidget::setAxesVisible(bool visible) {
    m_impl->axesVisible = visible;
    if (m_impl->canRender()) {
        m_impl->applyAxes();
        m_impl->redraw();
    }
}

bool ViewportWidget::areAxesVisible() const {
    return m_impl->axesVisible;
}

void ViewportWidget::setOriginItemVisible(OriginItem item, bool visible) {
    if (!m_impl->canRender()) {
        return;
    }
    Handle(AIS_Shape)& object = m_impl->originObjects[static_cast<std::size_t>(item)];
    if (visible == !object.IsNull()) {
        return;
    }
    if (visible) {
        object = m_impl->makeOriginObject(item);
        // Selection mode -1: displayed, but nothing of it can be picked.
        m_impl->context->Display(object, AIS_Shaded, -1, false);
    } else {
        m_impl->context->Remove(object, false);
        object.Nullify();
    }
    m_impl->redraw();
}

bool ViewportWidget::isOriginItemVisible(OriginItem item) const {
    return !m_impl->originObjects[static_cast<std::size_t>(item)].IsNull();
}

QColor ViewportWidget::originColor(OriginItem item) {
    // X is red, Y green, Z blue, as on the axis marker. A plane takes the color of the axis it
    // is square to.
    switch (item) {
    case OriginItem::PlaneYZ:
    case OriginItem::AxisX:
        return QColor(214, 72, 72);
    case OriginItem::PlaneXZ:
    case OriginItem::AxisY:
        return QColor(72, 170, 86);
    case OriginItem::PlaneXY:
    case OriginItem::AxisZ:
        break;
    }
    return QColor(66, 120, 220);
}

void ViewportWidget::setPlanePicking(bool on) {
    if (on == m_impl->planePicking || !m_impl->canRender()) {
        return;
    }
    m_impl->planePicking = on;
    m_impl->pressedPlane = -1;
    if (on) {
        // Toward the viewer, along each axis, as the view is now. Decided once: patches that
        // jumped about as the view was turned would be hard to aim at.
        const gp_Dir looking = m_impl->view->Camera()->Direction();
        m_impl->pickSides = {looking.X() > 1e-6 ? -1 : 1, looking.Y() > 1e-6 ? -1 : 1,
                             looking.Z() > 1e-6 ? -1 : 1};
        m_impl->showPickPlanes();
        m_impl->showConstructionPlanes();
    } else {
        m_impl->context->ClearDetected(false);
        m_impl->hidePickPlanes();
        m_impl->hoveredConstruction = {};
        m_impl->pressedConstruction = {};
        m_impl->showConstructionPlanes();
        unsetCursor();
        if (m_impl->hoveredPlane != -1) {
            m_impl->hoveredPlane = -1;
            emit planeHovered(-1);
        }
    }
    m_impl->redraw();
}

bool ViewportWidget::isPlanePicking() const {
    return m_impl->planePicking;
}

std::optional<OriginItem> ViewportWidget::pickPlaneAt(const QPoint& position) {
    if (!m_impl->planePicking || !m_impl->canRender() || m_impl->view->Window().IsNull()) {
        return std::nullopt;
    }
    const Graphic3d_Vec2i pixel = m_impl->toFramebufferPixels(position);
    m_impl->context->MoveTo(pixel.x(), pixel.y(), m_impl->view, false);
    update();
    const int plane = m_impl->detectedPickPlane();
    if (plane < 0) {
        return std::nullopt;
    }
    return static_cast<OriginItem>(plane);
}

void ViewportWidget::setProfileSketches(const std::vector<document::ObjectId>& sketches,
                                        bool facesToo) {
    if (!m_impl->canRender()) {
        return;
    }
    if (sketches == m_impl->profileSketches) {
        if (facesToo != m_impl->profileFacesToo) {
            m_impl->profileFacesToo = facesToo;
            m_impl->applyFilters();
            m_impl->redraw();
        }
        return;
    }
    const bool was = m_impl->profilePicking();
    const bool facesWere = m_impl->profileFacesToo;
    m_impl->profileFacesToo = facesToo && !sketches.empty();
    m_impl->profileSketches = sketches;
    m_impl->context->ClearDetected(false);
    if (sketches.empty()) {
        m_impl->hideProfiles();
        m_impl->pickedProfiles.clear();
        m_impl->hoveredProfile = -1;
        m_impl->pressedProfile = -1;
        unsetCursor();
    } else {
        // Nothing is selected while profiles are chosen: a click means a profile.
        m_impl->context->ClearSelected(false);
        m_impl->showProfiles();
    }
    if (was != m_impl->profilePicking() || facesWere != m_impl->profileFacesToo) {
        m_impl->applyFilters();
    }
    m_impl->redraw();
    m_impl->reportSelection();
}

bool ViewportWidget::isProfilePicking() const {
    return m_impl->profilePicking();
}

void ViewportWidget::setPickedProfiles(const std::vector<Profile>& profiles) {
    m_impl->pickedProfiles.clear();
    for (const Profile& profile : profiles) {
        const bool exists = std::any_of(
            m_impl->profilePatches.begin(), m_impl->profilePatches.end(),
            [&profile](const Impl::ProfilePatch& patch) { return patch.profile == profile; });
        if (exists && !m_impl->isPicked(profile)) {
            m_impl->pickedProfiles.push_back(profile);
        }
    }
    if (!m_impl->context.IsNull()) {
        m_impl->restyleProfiles();
    }
}

std::vector<Profile> ViewportWidget::pickedProfiles() const {
    return m_impl->pickedProfiles;
}

std::vector<Profile> ViewportWidget::pickableProfiles() const {
    std::vector<Profile> profiles;
    for (const Impl::ProfilePatch& patch : m_impl->profilePatches) {
        profiles.push_back(patch.profile);
    }
    return profiles;
}

std::optional<Profile> ViewportWidget::profileAt(const QPoint& position) {
    if (!m_impl->profilePicking() || !m_impl->canRender() || m_impl->view->Window().IsNull()) {
        return std::nullopt;
    }
    const Graphic3d_Vec2i pixel = m_impl->toFramebufferPixels(position);
    m_impl->context->MoveTo(pixel.x(), pixel.y(), m_impl->view, false);
    update();
    const int index = m_impl->detectedProfile();
    if (index < 0) {
        return std::nullopt;
    }
    return m_impl->profilePatches[static_cast<std::size_t>(index)].profile;
}

document::ObjectId ViewportWidget::constructionPlaneAt(const QPoint& position) {
    if (!m_impl->planePicking || !m_impl->canRender() || m_impl->view->Window().IsNull()) {
        return {};
    }
    const Graphic3d_Vec2i pixel = m_impl->toFramebufferPixels(position);
    m_impl->context->MoveTo(pixel.x(), pixel.y(), m_impl->view, false);
    update();
    return m_impl->detectedConstruction();
}

void ViewportWidget::setSelectionFilter(SelectionFilter filter) {
    if (filter == m_impl->filter) {
        return;
    }
    m_impl->filter = filter;
    if (m_impl->context.IsNull()) {
        return;
    }
    // What was selected, or is under the pointer, may be something the new filter cannot pick.
    m_impl->context->ClearSelected(false);
    m_impl->context->ClearDetected(false);
    m_impl->applyFilters();
    m_impl->redraw();
    m_impl->reportSelection();
}

SelectionFilter ViewportWidget::selectionFilter() const {
    return m_impl->filter;
}

std::vector<SelectedItem> ViewportWidget::selection() const {
    return m_impl->readSelection();
}

void ViewportWidget::setSelectedBodies(const std::vector<document::ObjectId>& bodies) {
    if (m_impl->context.IsNull()) {
        return;
    }
    m_impl->context->ClearSelected(false);
    for (const auto& [key, object] : m_impl->bodyObjects) {
        if (std::find(bodies.begin(), bodies.end(), key.body) != bodies.end()) {
            m_impl->context->AddOrRemoveSelected(object, false);
        }
    }
    m_impl->redraw();
    m_impl->reportSelection();
}

void ViewportWidget::setSelection(const std::vector<SelectedItem>& items) {
    if (m_impl->context.IsNull()) {
        return;
    }
    m_impl->context->ClearSelected(false);
    for (const SelectedItem& wanted : items) {
        if (wanted.kind == SelectedItem::Kind::Sketch) {
            const auto sketch = m_impl->sketchObjects.find(wanted.body);
            if (sketch != m_impl->sketchObjects.end()) {
                // One of the two stands for the sketch; either reports it as selected.
                const Handle(AIS_Shape)& object = !sketch->second.ordinary.IsNull()
                                                      ? sketch->second.ordinary
                                                      : sketch->second.construction;
                m_impl->context->AddOrRemoveSelected(object, false);
            }
            continue;
        }
        auto found = m_impl->bodyObjects.find(Impl::BodyKey{wanted.path, wanted.body});
        if (found == m_impl->bodyObjects.end() && wanted.path.empty()) {
            // Where the path is not said: the first appearance of the body.
            found = std::find_if(
                m_impl->bodyObjects.begin(), m_impl->bodyObjects.end(),
                [&wanted](const auto& entry) { return entry.first.body == wanted.body; });
        }
        if (found == m_impl->bodyObjects.end()) {
            continue;
        }
        if (wanted.kind == SelectedItem::Kind::Body) {
            m_impl->context->AddOrRemoveSelected(found->second, false);
            continue;
        }
        // Find the owner that stands for this face, edge, or vertex among those the filter
        // has switched on.
        Handle(SelectMgr_IndexedMapOfOwner) owners;
        m_impl->context->EntityOwners(owners, found->second, -1);
        if (owners.IsNull()) {
            continue;
        }
        for (SelectMgr_IndexedMapOfOwner::Iterator it(*owners); it.More(); it.Next()) {
            auto item = m_impl->itemOf(it.Value());
            if (item.has_value() && wanted.path.empty()) {
                item->path.clear();
            }
            if (item.has_value() && *item == wanted) {
                m_impl->context->AddOrRemoveSelected(it.Value(), false);
                break;
            }
        }
    }
    m_impl->redraw();
    m_impl->reportSelection();
}

void ViewportWidget::clearSelection() {
    setSelectedBodies({});
}

std::vector<SelectedItem> ViewportWidget::itemsAt(const QPoint& position) {
    std::vector<SelectedItem> items;
    if (!m_impl->canRender() || m_impl->view->Window().IsNull()) {
        return items;
    }
    const Graphic3d_Vec2i pixel = m_impl->toFramebufferPixels(position);
    m_impl->context->MoveTo(pixel.x(), pixel.y(), m_impl->view, false);
    for (m_impl->context->InitDetected(); m_impl->context->MoreDetected();
         m_impl->context->NextDetected()) {
        const auto item = m_impl->itemOf(m_impl->context->DetectedCurrentOwner());
        if (item.has_value() && std::find(items.begin(), items.end(), *item) == items.end()) {
            items.push_back(*item);
        }
    }
    update();
    return items;
}

QColor ViewportWidget::backgroundColor() const {
    return m_impl->colors.background;
}

QString ViewportWidget::graphicsInfo() const {
    return m_impl->graphicsInfo;
}

QString ViewportWidget::errorString() const {
    return m_impl->error;
}

void ViewportWidget::fail(const QString& reason) {
    if (m_impl->error.isEmpty()) {
        m_impl->error = reason;
        qWarning("%s", qPrintable(reason));
    }
    if (!m_impl->errorReported) {
        m_impl->errorReported = true;
        emit initializationFailed(m_impl->error);
    }
}

void ViewportWidget::initializeGL() {
    if (!m_impl->canRender()) {
        // The constructor could not reach the display. Nobody was listening then; say so now.
        fail(m_impl->error);
        return;
    }

    Handle(OpenGl_GraphicDriver) driver =
        Handle(OpenGl_GraphicDriver)::DownCast(m_impl->viewer->Driver());
    driver->ChangeOptions().contextCompatible =
        context()->format().profile() != QSurfaceFormat::CoreProfile;

    // Let OCCT adopt the context Qt has just made current instead of creating its own.
    Handle(OpenGl_Context) glContext = new OpenGl_Context();
    if (!glContext->Init(!driver->Options().contextCompatible)) {
#if defined(Q_OS_LINUX)
        fail(tr("The 3D viewer could not use the OpenGL context created by Qt (platform \"%1\"). "
                "LCAD needs an X11 or XWayland connection with GLX; try starting it with "
                "QT_QPA_PLATFORM=xcb.")
                 .arg(QGuiApplication::platformName()));
#else
        fail(tr("The 3D viewer could not use the OpenGL context created by Qt (platform \"%1\"). "
                "LCAD needs OpenGL 3.3 or newer from the graphics driver.")
                 .arg(QGuiApplication::platformName()));
#endif
        return;
    }

    Handle(ViewportWindow) window = Handle(ViewportWindow)::DownCast(m_impl->view->Window());
    if (window.IsNull()) {
        window = new ViewportWindow();
        window->SetVirtual(true);
    }
    // Not the window the widget is shown in: that one belongs to the top-level widget, which
    // destroys and replaces it on its own schedule (when it is closed, or moves to another
    // screen), leaving OCCT holding a window that no longer exists. And never a native window
    // of this widget's own (winId()), which breaks how Qt composes QOpenGLWidget with the
    // widgets around it.
#if defined(Q_OS_WIN)
    // On Windows, the window whose device context Qt made current: the widget's own offscreen
    // surface, which has the pixel format the context needs and lives as long as the context.
    // OCCT's own Qt sample does the same. A window of LCAD's own would have no pixel format
    // until a context had been made current on it.
    window->SetNativeHandle((Aspect_Drawable)WindowFromDC(wglGetCurrentDC()));
#else
    if (!m_impl->anchorWindow) {
        m_impl->anchorWindow = std::make_unique<QWindow>();
        m_impl->anchorWindow->setSurfaceType(QSurface::OpenGLSurface);
        m_impl->anchorWindow->setFormat(context()->format());
        m_impl->anchorWindow->create();
    }
    window->SetNativeHandle((Aspect_Drawable)m_impl->anchorWindow->winId());
#endif
    window->SetSize(width(), height());
    window->setDevicePixelRatio(devicePixelRatioF());
    m_impl->view->SetWindow(window, glContext->RenderingContext());
    m_impl->view->MustBeResized();
    m_impl->view->Invalidate();

    m_impl->applyPixelRatio(devicePixelRatioF());

    // OCCT may have rebound the context while setting up; give Qt its framebuffer back.
    makeCurrent();
    m_impl->graphicsInfo = readGraphicsInfo(m_impl->view);
}

void ViewportWidget::paintGL() {
    QOpenGLFunctions* gl = context()->functions();
    if (drawingHeld || !m_impl->canRender() || m_impl->view->Window().IsNull()) {
        const QColor& background = m_impl->colors.background;
        gl->glClearColor(background.redF(), background.greenF(), background.blueF(), 1.0F);
        gl->glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    ++m_impl->frames;

    // The pixel ratio changes when the window moves to a screen with different scaling.
    Handle(ViewportWindow) window = Handle(ViewportWindow)::DownCast(m_impl->view->Window());
    if (window->DevicePixelRatio() != devicePixelRatioF()) {
        window->setDevicePixelRatio(devicePixelRatioF());
        m_impl->applyPixelRatio(devicePixelRatioF());
    }

    // Point OCCT at the framebuffer Qt wants this frame drawn into.
    Handle(OpenGl_Context) glContext = glContextOf(m_impl->view);
    Handle(QtFrameBuffer) target = Handle(QtFrameBuffer)::DownCast(glContext->DefaultFrameBuffer());
    if (target.IsNull()) {
        target = new QtFrameBuffer();
    }
    if (!target->InitWrapper(glContext)) {
        fail(tr("The 3D viewer could not draw into the framebuffer provided by Qt."));
        return;
    }
    glContext->SetDefaultFrameBuffer(target);

    // Follow the size of Qt's framebuffer, which is in device pixels.
    const Graphic3d_Vec2i newSize = target->GetVPSize();
    Graphic3d_Vec2i oldSize;
    window->Size(oldSize.x(), oldSize.y());
    if (newSize != oldSize) {
        window->SetSize(newSize.x(), newSize.y());
        m_impl->view->MustBeResized();
        m_impl->view->Invalidate();
    }

    if (m_impl->fitPending) {
        m_impl->fitPending = false;
        if (m_impl->sketching()) {
            const Bnd_Box bounds = m_impl->sceneBounds();
            if (bounds.IsVoid()) {
                // Nothing to frame yet: a view some 200 mm tall, centered on the sketch.
                const document::SketchItem* sketch =
                    m_impl->document->findSketch(m_impl->activeSketch);
                const Handle(Graphic3d_Camera)& camera = m_impl->view->Camera();
                const gp_Pnt origin(sketch->plane.origin.x, sketch->plane.origin.y,
                                    sketch->plane.origin.z);
                const gp_Vec back = gp_Vec(camera->Direction()).Reversed() * camera->Distance();
                camera->SetEyeAndCenter(origin.Translated(back), origin);
                camera->SetScale(200.0);
            } else {
                m_impl->view->FitAll(bounds, 0.12, false);
            }
        } else {
            m_impl->view->FitAll(0.05, false);
        }
        m_impl->view->Invalidate();
    }

    // Qt and OCCT share one context and each assumes OpenGL state the other changes. Undo what
    // Qt leaves behind before OCCT draws.
    const bool hasFixedFunction = context()->format().profile() != QSurfaceFormat::CoreProfile;
    gl->glUseProgram(0);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    gl->glDisable(GL_BLEND);
    if (hasFixedFunction) {
        gl->glDisable(GL_ALPHA_TEST);
        gl->glDisable(GL_TEXTURE_2D);
    }

    // Apply the mouse input queued since the last frame, then redraw.
    m_impl->view->InvalidateImmediate();
    m_impl->FlushViewEvents(m_impl->context, m_impl->view, true);
    // The box beside the distance handle follows it as the camera moves.
    m_impl->placeHandleField();

    // When the pointer comes to rest on something new, draw once more. OCCT draws the hover
    // highlight on top of the frame it has kept, and the first time after the scene changed
    // that does not always show; a second pass does.
    const Standard_Transient* detected =
        m_impl->context->HasDetected() ? m_impl->context->DetectedOwner().get() : nullptr;
    if (detected != m_impl->lastDetected) {
        m_impl->lastDetected = detected;
        update();
    }
    if (const int plane = m_impl->detectedPickPlane(); plane != m_impl->hoveredPlane) {
        m_impl->hoveredPlane = plane;
        // Told once painting is over: whoever listens may want to change what is on screen.
        QMetaObject::invokeMethod(
            this,
            [this, plane] {
                if (m_impl->planePicking && m_impl->hoveredPlane == plane) {
                    if (plane >= 0) {
                        setCursor(Qt::PointingHandCursor);
                    } else {
                        unsetCursor();
                    }
                    m_impl->showHoveredPlane(plane);
                    emit planeHovered(plane);
                }
            },
            Qt::QueuedConnection);
    }

    if (const document::ObjectId plane = m_impl->detectedConstruction();
        plane != m_impl->hoveredConstruction) {
        m_impl->hoveredConstruction = plane;
        QMetaObject::invokeMethod(
            this,
            [this, plane] {
                if (m_impl->planePicking && m_impl->hoveredConstruction == plane) {
                    if (plane.isValid()) {
                        setCursor(Qt::PointingHandCursor);
                    } else if (m_impl->hoveredPlane < 0) {
                        unsetCursor();
                    }
                    for (const auto& entry : m_impl->constructionObjects) {
                        m_impl->styleConstruction(entry.second, entry.first == plane);
                        m_impl->context->Redisplay(entry.second, false);
                    }
                    m_impl->redraw();
                }
            },
            Qt::QueuedConnection);
    }
    if (const int profile = m_impl->detectedProfile(); profile != m_impl->hoveredProfile) {
        m_impl->hoveredProfile = profile;
        QMetaObject::invokeMethod(
            this,
            [this, profile] {
                if (m_impl->profilePicking() && m_impl->hoveredProfile == profile) {
                    if (profile >= 0) {
                        setCursor(Qt::PointingHandCursor);
                    } else {
                        unsetCursor();
                    }
                    m_impl->restyleProfiles();
                }
            },
            Qt::QueuedConnection);
    }

    // And restore what Qt expects, or it draws text and other textures corrupted.
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl->glActiveTexture(GL_TEXTURE0);

    if (m_impl->sketching()) {
        // The sketch is drawn over this frame, and has to follow the camera.
        m_impl->editor->overlay()->update();
    }
    if (m_impl->pointsOverlay->isVisible()) {
        m_impl->pointsOverlay->update();
    }
    m_impl->followCamera();
}

void ViewportWidget::mousePressEvent(QMouseEvent* event) {
    QOpenGLWidget::mousePressEvent(event);
    if (!m_impl->canRender()) {
        return;
    }
    if (event->button() == Qt::RightButton) {
        // Wait and see: held and moved it turns the view, let go it opens the menu.
        m_impl->rightPressed = true;
        m_impl->rightDragging = false;
        m_impl->rightPressPosition = event->position();
        return;
    }
    if (event->button() == Qt::MiddleButton && m_impl->middlePans(event->modifiers())) {
        m_impl->panning = true;
        setCursor(Qt::ClosedHandCursor);
    }
    if (event->button() == Qt::LeftButton && m_impl->pointsOverlay->isVisible()) {
        if (const int point = m_impl->pointsOverlay->pointAt(event->position()); point >= 0) {
            m_impl->pressedPoint = true;
            const PickablePoint picked =
                m_impl->pointsOverlay->points()[static_cast<std::size_t>(point)];
            emit pointPicked(picked.sketch, picked.point);
            return;
        }
    }
    if (event->button() == Qt::LeftButton && !m_impl->sketching() &&
        m_impl->overHandle(event->position())) {
        // The distance handle is taken: dragged, it reports distances, and selects nothing.
        if (const auto along = m_impl->handleParameterAt(event->position())) {
            m_impl->handle.dragging = true;
            m_impl->handle.grab = m_impl->handle.distance - *along;
            setCursor(Qt::ClosedHandCursor);
            m_impl->showHandle();
            m_impl->redraw();
        }
        return;
    }
    if (event->button() == Qt::LeftButton) {
        // What the press is on, rather than what the pointer last moved over: a press can come
        // with no move before it, as the click that brings the window forward does, and on
        // macOS a move can arrive after the press it came before.
        const Graphic3d_Vec2i pixel = m_impl->toFramebufferPixels(event->position());
        m_impl->context->MoveTo(pixel.x(), pixel.y(), m_impl->view, false);
    }
    if (m_impl->planePicking && event->button() == Qt::LeftButton) {
        if (const auto plane = pickPlaneAt(event->position().toPoint())) {
            // A plane is being chosen, and this is one. OCCT is not to select anything.
            m_impl->pressedPlane = static_cast<int>(*plane);
            return;
        }
        if (const document::ObjectId plane = m_impl->detectedConstruction(); plane.isValid()) {
            m_impl->pressedConstruction = plane;
            return;
        }
    }
    if (m_impl->profilePicking() && event->button() == Qt::LeftButton) {
        // Profiles are being chosen. A press elsewhere selects nothing either, unless faces
        // may be picked too.
        const auto profile = profileAt(event->position().toPoint());
        m_impl->pressedProfile = profile.has_value() ? m_impl->detectedProfile() : -1;
        if (profile.has_value() || !m_impl->profileFacesToo) {
            return;
        }
    }
    if (m_impl->sketching() && event->button() == Qt::LeftButton) {
        if (m_impl->editor->tool() == SketchTool::Project) {
            // What of a body is under the pointer goes into the sketch: a corner if there is
            // one there, which is what was aimed at where edges meet, and otherwise an edge.
            const std::vector<SelectedItem> under = itemsAt(event->position().toPoint());
            for (const SelectedItem::Kind wanted :
                 {SelectedItem::Kind::Vertex, SelectedItem::Kind::Edge}) {
                for (const SelectedItem& item : under) {
                    const document::Body* body = m_impl->document->findBody(item.body);
                    if (body == nullptr || item.kind != wanted) {
                        continue;
                    }
                    try {
                        // Where it is in the root's coordinates, as the sketch is drawn.
                        const document::Instance* instance = m_impl->instanceAt(item.path);
                        m_impl->editor->projectShape(kernel::placed(
                            kernel::subShape(body->shape,
                                             wanted == SelectedItem::Kind::Edge
                                                 ? kernel::SubShapeKind::Edge
                                                 : kernel::SubShapeKind::Vertex,
                                             item.index),
                            instance != nullptr ? instance->placement : kernel::Placement{}));
                        return;
                    } catch (const std::out_of_range&) {
                        // The body changed under the pointer. Try what lies behind.
                    }
                }
            }
            m_impl->editor->projectShape(kernel::Shape());
            return;
        }
        m_impl->editor->mousePress(event->position(), event->modifiers());
        return;
    }
    if (m_impl->UpdateMouseButtons(m_impl->toFramebufferPixels(event->position()),
                                   toOcctButtons(event->buttons()),
                                   toOcctModifiers(event->modifiers()), false)) {
        update();
    }
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* event) {
    QOpenGLWidget::mouseReleaseEvent(event);
    if (!m_impl->canRender()) {
        return;
    }
    if (event->button() == Qt::RightButton && m_impl->rightPressed) {
        m_impl->rightPressed = false;
        if (m_impl->rightDragging) {
            m_impl->rightDragging = false;
            m_impl->UpdateMouseButtons(m_impl->toFramebufferPixels(event->position()),
                                       Aspect_VKeyMouse_NONE, Aspect_VKeyFlags_NONE, false);
            update();
        } else {
            emit customContextMenuRequested(event->position().toPoint());
        }
        return;
    }
    if (event->button() == Qt::MiddleButton && m_impl->panning) {
        m_impl->panning = false;
        unsetCursor();
    }
    if (event->button() == Qt::LeftButton && m_impl->pressedPoint) {
        m_impl->pressedPoint = false;
        return;
    }
    if (m_impl->handle.dragging && event->button() == Qt::LeftButton) {
        m_impl->handle.dragging = false;
        setCursor(m_impl->overHandle(event->position()) ? Qt::OpenHandCursor : Qt::ArrowCursor);
        m_impl->showHandle();
        m_impl->redraw();
        return;
    }
    if (m_impl->pressedConstruction.isValid() && event->button() == Qt::LeftButton) {
        const document::ObjectId pressed = m_impl->pressedConstruction;
        m_impl->pressedConstruction = {};
        if (constructionPlaneAt(event->position().toPoint()) == pressed) {
            emit constructionPlanePicked(pressed);
        }
        return;
    } else if (m_impl->pressedPlane >= 0 && event->button() == Qt::LeftButton) {
        // Chosen if the button comes up on the patch it went down on.
        const int pressed = m_impl->pressedPlane;
        m_impl->pressedPlane = -1;
        const auto plane = pickPlaneAt(event->position().toPoint());
        if (plane.has_value() && static_cast<int>(*plane) == pressed) {
            emit planePicked(*plane);
        }
        return;
    } else if (m_impl->profilePicking() && event->button() == Qt::LeftButton &&
               (m_impl->pressedProfile >= 0 || !m_impl->profileFacesToo)) {
        // Picked, or let go, if the button comes up on the region it went down on.
        const int pressed = m_impl->pressedProfile;
        m_impl->pressedProfile = -1;
        const auto profile = profileAt(event->position().toPoint());
        if (pressed >= 0 && profile.has_value() && m_impl->detectedProfile() == pressed) {
            auto& picked = m_impl->pickedProfiles;
            const auto found = std::find(picked.begin(), picked.end(), *profile);
            if (found != picked.end()) {
                picked.erase(found);
            } else {
                picked.push_back(*profile);
            }
            m_impl->restyleProfiles();
            emit profilesPicked();
        }
        return;
    } else if (m_impl->sketching() && event->button() == Qt::LeftButton) {
        m_impl->editor->mouseRelease(event->position(), event->modifiers());
        return;
    }
    if (m_impl->UpdateMouseButtons(m_impl->toFramebufferPixels(event->position()),
                                   toOcctButtons(event->buttons()),
                                   toOcctModifiers(event->modifiers()), false)) {
        update();
    }
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* event) {
    QOpenGLWidget::mouseMoveEvent(event);
    if (!m_impl->canRender()) {
        return;
    }
    if (m_impl->pointsOverlay->isVisible()) {
        const int point = m_impl->pointsOverlay->pointAt(event->position());
        m_impl->pointsOverlay->setHovered(point);
        if (point >= 0) {
            setCursor(Qt::PointingHandCursor);
        } else {
            unsetCursor();
        }
    }
    if (m_impl->handle.dragging) {
        if (const auto along = m_impl->handleParameterAt(event->position())) {
            // To a tenth of a millimeter: the box beside it takes any value.
            const double distance = std::round((*along + m_impl->handle.grab) * 10.0) / 10.0;
            if (distance != m_impl->handle.distance) {
                emit distanceDragged(distance);
            }
        }
        return;
    }
    if (m_impl->handle.shown && !m_impl->sketching()) {
        const bool over = m_impl->overHandle(event->position());
        if (over != m_impl->handle.hovered) {
            m_impl->handle.hovered = over;
            setCursor(over ? Qt::OpenHandCursor : Qt::ArrowCursor);
            m_impl->showHandle();
            m_impl->redraw();
        }
    }
    if (m_impl->rightPressed) {
        // Held and moved, the right button orbits, as the orbit buttons would from the same spot.
        if (!m_impl->rightDragging) {
            const QPointF moved = event->position() - m_impl->rightPressPosition;
            if (moved.manhattanLength() < QApplication::startDragDistance()) {
                return;
            }
            m_impl->rightDragging = true;
            m_impl->startOrbit(m_impl->rightPressPosition);
        }
        m_impl->orbitTo(event->position());
        update();
        return;
    }
    Qt::MouseButtons buttons = event->buttons();
    if (m_impl->sketching()) {
        m_impl->editor->mouseMove(event->position(), buttons, event->modifiers());
        // OCCT never heard of the left button going down, and is not to act on it.
        buttons.setFlag(Qt::LeftButton, false);
    }
    if (m_impl->pressedPlane >= 0 || m_impl->pressedConstruction.isValid() ||
        m_impl->pressedProfile >= 0 || (m_impl->profilePicking() && !m_impl->profileFacesToo)) {
        // Nor of a press on a plane or a profile that is being chosen.
        buttons.setFlag(Qt::LeftButton, false);
    }
    if (m_impl->UpdateMousePosition(m_impl->toFramebufferPixels(event->position()),
                                    toOcctButtons(buttons), toOcctModifiers(event->modifiers()),
                                    false)) {
        update();
    }
}

void ViewportWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        fitAll();
        return;
    }
    if (event->button() == Qt::LeftButton && m_impl->canRender()) {
        if (m_impl->sketching()) {
            m_impl->editor->mouseDoubleClick(event->position(), event->modifiers());
            return;
        }
        const std::vector<SelectedItem> under = itemsAt(event->position().toPoint());
        if (!under.empty()) {
            emit itemActivated(under.front());
        }
    }
    // The second press of a double-click is still a press.
    mousePressEvent(event);
}

void ViewportWidget::keyPressEvent(QKeyEvent* event) {
    if (m_impl->sketching() && m_impl->editor->keyPress(event)) {
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Menu) {
        emit customContextMenuRequested(mapFromGlobal(QCursor::pos()));
        event->accept();
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void ViewportWidget::leaveEvent(QEvent* event) {
    QOpenGLWidget::leaveEvent(event);
    m_impl->editor->pointerLeft();
}

void ViewportWidget::resizeEvent(QResizeEvent* event) {
    QOpenGLWidget::resizeEvent(event);
    m_impl->editor->overlay()->setGeometry(rect());
    m_impl->pointsOverlay->setGeometry(rect());
    m_impl->cube->fitTo(size());
    m_impl->cube->move(width() - m_impl->cube->width() - 4, 4);
}

std::uint64_t ViewportWidget::framesDrawn() const {
    return m_impl->frames;
}

void ViewportWidget::holdDrawing(bool held) {
    drawingHeld = held;
    if (held) {
        return;
    }
    for (QWidget* widget : QApplication::allWidgets()) {
        if (auto* view = qobject_cast<ViewportWidget*>(widget)) {
            view->update();
        }
    }
}

void ViewportWidget::setPickablePoints(std::vector<PickablePoint> points) {
    m_impl->pointsOverlay->setGeometry(rect());
    m_impl->pointsOverlay->show(std::move(points), m_impl->colors.selection,
                                m_impl->colors.selection);
    m_impl->pointsOverlay->raise();
}

const std::vector<PickablePoint>& ViewportWidget::pickablePoints() const {
    return m_impl->pointsOverlay->points();
}

void ViewportWidget::wheelEvent(QWheelEvent* event) {
    QOpenGLWidget::wheelEvent(event);
    if (!m_impl->canRender()) {
        return;
    }
    // One notch of a conventional mouse wheel is 120 eighths of a degree.
    const double notches = event->angleDelta().y() / 120.0 * (m_impl->zoomReversed ? -1.0 : 1.0);
    if (notches != 0.0 && m_impl->UpdateMouseScroll(Aspect_ScrollDelta(
                              m_impl->toFramebufferPixels(event->position()), notches))) {
        update();
    }
}

} // namespace lcad::viewport
