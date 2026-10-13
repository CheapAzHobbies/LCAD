// SPDX-License-Identifier: GPL-3.0-or-later

// Embeds OCCT's 3D viewer in a QOpenGLWidget: Qt owns the OpenGL context and the framebuffer,
// and OCCT adopts both. The technique follows the QOpenGLWidget sample maintained for OCCT at
// https://github.com/gkv311/occt-samples-qt (MIT license, see THIRD_PARTY_NOTICES.md).

#include <lcad/viewport/ViewportWidget.h>

#include <lcad/document/Document.h>
#include <lcad/document/SketchShapes.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/occt/ShapeAccess.h>
#include <lcad/viewport/SketchEditor.h>

#include <AIS_AnimationCamera.hxx>
#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_ViewController.hxx>
#include <AIS_ViewCube.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_Grid.hxx>
#include <Aspect_NeutralWindow.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <Bnd_Box.hxx>
#include <Graphic3d_CView.hxx>
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_MaterialAspect.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <OpenGl_Context.hxx>
#include <OpenGl_FrameBuffer.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <OpenGl_View.hxx>
#include <OpenGl_Window.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Quantity_Color.hxx>
#include <SelectMgr_IndexedMapOfOwner.hxx>
#include <Standard_Failure.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <TColStd_IndexedDataMapOfStringString.hxx>
#include <V3d_AmbientLight.hxx>
#include <V3d_DirectionalLight.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <unordered_map>

namespace lcad::viewport {

namespace {

// The view cube sits this far in from the top right corner and is this large, in logical pixels.
const int viewCubeOffset = 70;
const double viewCubeSize = 46.0;

Quantity_Color toOcct(const QColor& color) {
    return Quantity_Color(color.redF(), color.greenF(), color.blueF(), Quantity_TOC_sRGB);
}

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
    Handle(AIS_ViewCube) viewCube;
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
    std::unordered_map<document::ObjectId, Handle(AIS_Shape)> bodyObjects;
    std::unordered_map<const Standard_Transient*, document::ObjectId> bodyOfObject;
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
        viewCube->SetSize(viewCubeSize * ratio);
        // Not multiplied: the resolution above already scales text.
        viewCube->SetFontHeight(viewCubeSize * 0.2);
        const int offset = static_cast<int>(std::lround(viewCubeOffset * ratio));
        viewCube->SetTransformPersistence(new Graphic3d_TransformPers(
            Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER, Graphic3d_Vec2i(offset, offset)));
        if (context->IsDisplayed(viewCube)) {
            context->Redisplay(viewCube, false);
        }
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
    bool cubePressed = false;
    bool cubeDragging = false;
    QPointF cubePressPosition;
    Qt::KeyboardModifiers cubePressModifiers;

    [[nodiscard]] bool isOverViewCube() const {
        return !context.IsNull() && context->HasDetected() &&
               context->DetectedInteractive() == viewCube;
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
        for (const auto& entry : bodyObjects) {
            styleBody(entry.second);
            context->SetDisplayMode(entry.second, displayMode(), false);
            context->Redisplay(entry.second, false);
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
        viewCube->SetBoxColor(toOcct(colors.cube));
        viewCube->SetTextColor(toOcct(colors.cubeText));
        if (context->IsDisplayed(viewCube)) {
            context->Redisplay(viewCube, false);
        }
        if (!previewObject.IsNull()) {
            previewObject->SetColor(toOcct(colors.preview));
            context->Redisplay(previewObject, false);
        }
        restyleBodies();
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
        // X is red, Y green, Z blue, as on the axis marker. A plane takes the color of the
        // axis it is square to.
        const QColor red(214, 72, 72);
        const QColor green(72, 170, 86);
        const QColor blue(66, 120, 220);
        TopoDS_Shape shape;
        QColor color;
        bool isPlane = true;
        switch (item) {
        case OriginItem::PlaneXY:
            shape = plane(gp::DZ(), gp::DX());
            color = blue;
            break;
        case OriginItem::PlaneXZ:
            shape = plane(gp::DY().Reversed(), gp::DX());
            color = green;
            break;
        case OriginItem::PlaneYZ:
            shape = plane(gp::DX(), gp::DY());
            color = red;
            break;
        case OriginItem::AxisX:
            shape = axis(gp::DX());
            color = red;
            isPlane = false;
            break;
        case OriginItem::AxisY:
            shape = axis(gp::DY());
            color = green;
            isPlane = false;
            break;
        case OriginItem::AxisZ:
            shape = axis(gp::DZ());
            color = blue;
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
        SelectedItem item{body->second, SelectedItem::Kind::Body, -1};
        const Handle(StdSelect_BRepOwner) shapeOwner = Handle(StdSelect_BRepOwner)::DownCast(owner);
        if (!shapeOwner.IsNull() && shapeOwner->HasShape()) {
            const TopoDS_Shape& part = shapeOwner->Shape();
            const document::Body* source = document->findBody(body->second);
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
        if (sketching()) {
            // While a sketch is open, clicks belong to the sketch.
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
        if (!sketching() &&
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
        if (!sketch.visible || sketch.id == activeSketch) {
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
            const kernel::Plane& plane = sketch->plane;
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
                box.Add(gp_Pnt(bounds->min.x, bounds->min.y, bounds->min.z));
                box.Add(gp_Pnt(bounds->max.x, bounds->max.y, bounds->max.z));
            }
        }
        return box;
    }

    void showBody(const document::Body& body) {
        if (context.IsNull() || bodyObjects.contains(body.id)) {
            return;
        }
        const bool wasEmpty = bodyObjects.empty();
        Handle(AIS_Shape) object = new AIS_Shape(kernel::occt::unwrap(body.shape));
        styleBody(object);
        // Displaying with the whole-shape mode first prepares the owner that selecting the body
        // as a whole needs, whichever filter is active afterwards.
        context->Display(object, displayMode(), AIS_Shape::SelectionMode(TopAbs_SHAPE), false);
        applyFilter(object);
        bodyObjects.emplace(body.id, object);
        bodyOfObject.emplace(object.get(), body.id);
        if (wasEmpty) {
            // The first body defines the scale of the scene. Later ones leave the camera alone.
            fitPending = true;
        }
    }

    void hideBody(document::ObjectId id) {
        const auto found = bodyObjects.find(id);
        if (found == bodyObjects.end()) {
            return;
        }
        context->Remove(found->second, false);
        bodyOfObject.erase(found->second.get());
        bodyObjects.erase(found);
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
        case Kind::SketchAdded:
        case Kind::SketchVisibilityChanged:
        case Kind::SketchGeometryChanged:
            if (const document::SketchItem* sketch = document->findSketch(change.body)) {
                showSketch(*sketch);
            } else {
                hideSketch(change.body);
            }
            editor->documentChanged(change);
            break;
        case Kind::SketchRemoved:
            hideSketch(change.body);
            if (change.body == activeSketch) {
                // The sketch being edited is gone: its creation was undone.
                m_widget->finishSketch();
            }
            break;
        case Kind::BodyRenamed:
        case Kind::SketchRenamed:
        case Kind::HistoryChanged:
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

        // Clicking a side, edge, or corner of the cube turns the camera to face it.
        m_impl->viewCube = new AIS_ViewCube();
        m_impl->viewCube->SetSize(viewCubeSize);
        m_impl->viewCube->SetFontHeight(viewCubeSize * 0.2);
        m_impl->viewCube->SetDrawAxes(false);
        m_impl->viewCube->SetTransformPersistence(
            new Graphic3d_TransformPers(Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER,
                                        Graphic3d_Vec2i(viewCubeOffset, viewCubeOffset)));
        m_impl->viewCube->SetViewAnimation(m_impl->ViewAnimation());
        m_impl->viewCube->SetFixedAnimationLoop(false);
        m_impl->viewCube->SetAutoStartAnimation(true);
        m_impl->context->Display(m_impl->viewCube, false);
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
    m_impl->viewCube.Nullify();
    for (Handle(AIS_Shape)& object : m_impl->originObjects) {
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
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
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
    if (m_impl->document != nullptr) {
        m_impl->document->removeListener(m_impl->listener);
        m_impl->listener = 0;
    }
    m_impl->document = document;
    if (m_impl->context.IsNull()) {
        return;
    }
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
    fitAll();
    m_impl->reportSelection();
}

document::Document* ViewportWidget::document() const {
    return m_impl->document;
}

void ViewportWidget::setPreview(const kernel::Shape& shape) {
    if (m_impl->context.IsNull()) {
        return;
    }
    if (!m_impl->previewObject.IsNull()) {
        m_impl->context->Remove(m_impl->previewObject, false);
        m_impl->previewObject.Nullify();
    }
    if (!shape.isNull()) {
        m_impl->previewObject = new AIS_Shape(kernel::occt::unwrap(shape));
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
    update();
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
    return QPointF(width() - viewCubeOffset, viewCubeOffset);
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
    lookAt(found->plane);
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
    if (!m_impl->viewCube.IsNull()) {
        // Not zero: the animation still has to run for one frame to move the camera.
        m_impl->viewCube->SetDuration(animated ? 0.5 : 0.001);
    }
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
    for (const document::ObjectId id : bodies) {
        const auto found = m_impl->bodyObjects.find(id);
        if (found != m_impl->bodyObjects.end()) {
            m_impl->context->AddOrRemoveSelected(found->second, false);
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
        const auto found = m_impl->bodyObjects.find(wanted.body);
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
            const auto item = m_impl->itemOf(it.Value());
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
        fail(tr("The 3D viewer could not use the OpenGL context created by Qt (platform \"%1\"). "
                "LCAD needs an X11 or XWayland connection with GLX; try starting it with "
                "QT_QPA_PLATFORM=xcb.")
                 .arg(QGuiApplication::platformName()));
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
    if (!m_impl->anchorWindow) {
        m_impl->anchorWindow = std::make_unique<QWindow>();
        m_impl->anchorWindow->setSurfaceType(QSurface::OpenGLSurface);
        m_impl->anchorWindow->setFormat(context()->format());
        m_impl->anchorWindow->create();
    }
    window->SetNativeHandle(static_cast<Aspect_Drawable>(m_impl->anchorWindow->winId()));
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
    if (!m_impl->canRender() || m_impl->view->Window().IsNull()) {
        const QColor& background = m_impl->colors.background;
        gl->glClearColor(background.redF(), background.greenF(), background.blueF(), 1.0F);
        gl->glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

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

    // When the pointer comes to rest on something new, draw once more. OCCT draws the hover
    // highlight on top of the frame it has kept, and the first time after the scene changed
    // that does not always show; a second pass does.
    const Standard_Transient* detected =
        m_impl->context->HasDetected() ? m_impl->context->DetectedOwner().get() : nullptr;
    if (detected != m_impl->lastDetected) {
        m_impl->lastDetected = detected;
        update();
    }

    // And restore what Qt expects, or it draws text and other textures corrupted.
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl->glActiveTexture(GL_TEXTURE0);

    if (m_impl->sketching()) {
        // The sketch is drawn over this frame, and has to follow the camera.
        m_impl->editor->overlay()->update();
    }
}

void ViewportWidget::mousePressEvent(QMouseEvent* event) {
    QOpenGLWidget::mousePressEvent(event);
    if (!m_impl->canRender()) {
        return;
    }
    if (event->button() == Qt::LeftButton && m_impl->isOverViewCube()) {
        // Wait and see whether this becomes a click or a drag.
        m_impl->cubePressed = true;
        m_impl->cubeDragging = false;
        m_impl->cubePressPosition = event->position();
        m_impl->cubePressModifiers = event->modifiers();
        return;
    }
    if (m_impl->sketching() && event->button() == Qt::LeftButton) {
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
    if (m_impl->cubePressed && event->button() == Qt::LeftButton) {
        m_impl->cubePressed = false;
        if (m_impl->cubeDragging) {
            m_impl->cubeDragging = false;
            m_impl->UpdateMouseButtons(m_impl->toFramebufferPixels(event->position()),
                                       Aspect_VKeyMouse_NONE, Aspect_VKeyFlags_NONE, false);
            update();
            return;
        }
        // It was a click. Give OCCT the press it has not seen yet; the release follows below.
        m_impl->UpdateMouseButtons(m_impl->toFramebufferPixels(m_impl->cubePressPosition),
                                   Aspect_VKeyMouse_LeftButton,
                                   toOcctModifiers(m_impl->cubePressModifiers), false);
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
    if (m_impl->cubePressed) {
        // Dragging the view cube orbits, as the orbit buttons would from the same spot.
        const Aspect_VKeyMouse orbitButton = Aspect_VKeyMouse_MiddleButton;
        if (!m_impl->cubeDragging) {
            const QPointF moved = event->position() - m_impl->cubePressPosition;
            if (moved.manhattanLength() < QApplication::startDragDistance()) {
                return;
            }
            m_impl->cubeDragging = true;
            m_impl->AbortViewAnimation();
            m_impl->UpdateMouseButtons(m_impl->toFramebufferPixels(m_impl->cubePressPosition),
                                       orbitButton, m_impl->orbitModifiers, false);
        }
        m_impl->UpdateMousePosition(m_impl->toFramebufferPixels(event->position()), orbitButton,
                                    m_impl->orbitModifiers, false);
        update();
        return;
    }
    Qt::MouseButtons buttons = event->buttons();
    if (m_impl->sketching()) {
        m_impl->editor->mouseMove(event->position(), buttons, event->modifiers());
        // OCCT never heard of the left button going down, and is not to act on it.
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
    if (event->button() == Qt::LeftButton && m_impl->canRender() && !m_impl->isOverViewCube()) {
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
    QOpenGLWidget::keyPressEvent(event);
}

void ViewportWidget::resizeEvent(QResizeEvent* event) {
    QOpenGLWidget::resizeEvent(event);
    m_impl->editor->overlay()->setGeometry(rect());
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
