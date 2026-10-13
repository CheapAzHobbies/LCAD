// SPDX-License-Identifier: GPL-3.0-or-later

// Embeds OCCT's 3D viewer in a QOpenGLWidget: Qt owns the OpenGL context and the framebuffer,
// and OCCT adopts both. The technique follows the QOpenGLWidget sample maintained for OCCT at
// https://github.com/gkv311/occt-samples-qt (MIT license, see THIRD_PARTY_NOTICES.md).

#include <lcad/viewport/ViewportWidget.h>

#include <lcad/document/Document.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <AIS_AnimationCamera.hxx>
#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_ViewController.hxx>
#include <AIS_ViewCube.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_NeutralWindow.hxx>
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
#include <Standard_Failure.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <TColStd_IndexedDataMapOfStringString.hxx>
#include <V3d_AmbientLight.hxx>
#include <V3d_DirectionalLight.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace lcad::viewport {

namespace {

const QColor viewportBackground(46, 49, 54);
const QColor bodyColor(168, 176, 186);
const QColor bodyEdgeColor(30, 32, 36);
const QColor previewColor(90, 170, 255);
const QColor hoverColor(255, 255, 255);
const QColor selectedColor(20, 120, 255);

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

// Dark outlines around every face. Set per object, after its color: AIS_Shape::SetColor()
// recolors the outlines too.
void outlineFaces(const Handle(AIS_Shape)& object) {
    object->Attributes()->SetFaceBoundaryDraw(true);
    object->Attributes()->SetFaceBoundaryAspect(
        new Prs3d_LineAspect(toOcct(bodyEdgeColor), Aspect_TOL_SOLID, 1.0));
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
    explicit Impl(ViewportWidget* widget) : m_widget(widget) {
        // Fusion's mouse mapping. The left button selects and never moves the camera.
        const unsigned int left = Aspect_VKeyMouse_LeftButton;
        const unsigned int middle = Aspect_VKeyMouse_MiddleButton;
        const unsigned int shift = Aspect_VKeyFlags_SHIFT;
        const unsigned int ctrl = Aspect_VKeyFlags_CTRL;

        AIS_MouseGestureMap& gestures = ChangeMouseGestureMap();
        gestures.Clear();
        gestures.Bind(left, AIS_MouseGesture_SelectRectangle);
        gestures.Bind(left | shift, AIS_MouseGesture_SelectRectangle);
        gestures.Bind(left | ctrl, AIS_MouseGesture_SelectRectangle);
        gestures.Bind(middle, AIS_MouseGesture_Pan);
        gestures.Bind(middle | shift, AIS_MouseGesture_RotateOrbit);
        gestures.Bind(middle | ctrl | shift, AIS_MouseGesture_Zoom);

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
    SelectionFilter filter = SelectionFilter::FacesEdgesVertices;
    std::vector<SelectedItem> lastSelection;
    const Standard_Transient* lastDetected = nullptr;

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

    void applyFilter(const Handle(AIS_Shape)& object) const {
        context->Deactivate(object);
        for (const int mode : selectionModes(filter)) {
            context->SetSelectionModeActive(object, mode, true,
                                            AIS_SelectionModesConcurrency_Multiple);
        }
    }

    void showBody(const document::Body& body) {
        if (context.IsNull() || bodyObjects.contains(body.id)) {
            return;
        }
        const bool wasEmpty = bodyObjects.empty();
        Handle(AIS_Shape) object = new AIS_Shape(kernel::occt::unwrap(body.shape));
        object->SetMaterial(bodyMaterial());
        object->SetColor(toOcct(bodyColor));
        outlineFaces(object);
        // Displaying with the whole-shape mode first prepares the owner that selecting the body
        // as a whole needs, whichever filter is active afterwards.
        context->Display(object, AIS_Shaded, AIS_Shape::SelectionMode(TopAbs_SHAPE), false);
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
        case Kind::BodyRenamed:
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
            const Handle(SelectMgr_EntityOwner) owner = context->SelectedOwner();
            if (owner.IsNull()) {
                continue;
            }
            const auto body = bodyOfObject.find(owner->Selectable().get());
            if (body == bodyOfObject.end()) {
                continue;
            }
            SelectedItem item{body->second, SelectedItem::Kind::Body, -1};
            const Handle(StdSelect_BRepOwner) shapeOwner =
                Handle(StdSelect_BRepOwner)::DownCast(owner);
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
            items.push_back(item);
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
        // What is under the pointer gets an outline, which OCCT draws on top of the frame it
        // has kept, without drawing the scene again. What is selected is tinted, with the body
        // still visible through the tint.
        for (const Prs3d_TypeOfHighlight type :
             {Prs3d_TypeOfHighlight_Dynamic, Prs3d_TypeOfHighlight_LocalDynamic}) {
            m_impl->context->HighlightStyle(type)->SetColor(toOcct(hoverColor));
        }
        for (const Prs3d_TypeOfHighlight type :
             {Prs3d_TypeOfHighlight_Selected, Prs3d_TypeOfHighlight_LocalSelected}) {
            const Handle(Prs3d_Drawer)& selected = m_impl->context->HighlightStyle(type);
            selected->SetColor(toOcct(selectedColor));
            selected->SetDisplayMode(AIS_Shaded);
            selected->SetTransparency(0.45F);
        }

        // The view gets its window in initializeGL(), once Qt has created the OpenGL context.
        m_impl->view = m_impl->viewer->CreateView();
        m_impl->view->SetImmediateUpdate(false);
        m_impl->view->SetBackgroundColor(
            Quantity_Color(viewportBackground.redF(), viewportBackground.greenF(),
                           viewportBackground.blueF(), Quantity_TOC_sRGB));
        m_impl->view->SetProj(V3d_XposYnegZpos); // the usual isometric CAD view
        m_impl->view->ChangeRenderingParams().NbMsaaSamples = 4;
        m_impl->view->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, Quantity_NOC_WHITE, 0.08,
                                      V3d_ZBUFFER);

        // Clicking a side, edge, or corner of the cube turns the camera to face it.
        m_impl->viewCube = new AIS_ViewCube();
        m_impl->viewCube->SetSize(viewCubeSize);
        m_impl->viewCube->SetFontHeight(viewCubeSize * 0.2);
        m_impl->viewCube->SetDrawAxes(false);
        m_impl->viewCube->SetBoxColor(toOcct(QColor(214, 218, 224)));
        m_impl->viewCube->SetTextColor(toOcct(QColor(40, 44, 50)));
        m_impl->viewCube->SetTransformPersistence(
            new Graphic3d_TransformPers(Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER,
                                        Graphic3d_Vec2i(viewCubeOffset, viewCubeOffset)));
        m_impl->viewCube->SetViewAnimation(m_impl->ViewAnimation());
        m_impl->viewCube->SetFixedAnimationLoop(false);
        m_impl->viewCube->SetAutoStartAnimation(true);
        m_impl->context->Display(m_impl->viewCube, false);
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
        m_impl->previewObject->SetColor(toOcct(previewColor));
        outlineFaces(m_impl->previewObject);
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
    int pixelX = 0;
    int pixelY = 0;
    m_impl->view->Convert(x, y, z, pixelX, pixelY);
    const qreal scale = devicePixelRatioF();
    return QPointF(pixelX / scale, pixelY / scale);
}

QPointF ViewportWidget::viewCubeCenter() const {
    return QPointF(width() - viewCubeOffset, viewCubeOffset);
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
    for (const auto& entry : m_impl->bodyObjects) {
        m_impl->applyFilter(entry.second);
    }
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

void ViewportWidget::clearSelection() {
    setSelectedBodies({});
}

QColor ViewportWidget::backgroundColor() const {
    return viewportBackground;
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

    // OCCT may have rebound the context while setting up; give Qt its framebuffer back.
    makeCurrent();
    m_impl->graphicsInfo = readGraphicsInfo(m_impl->view);
}

void ViewportWidget::paintGL() {
    QOpenGLFunctions* gl = context()->functions();
    if (!m_impl->canRender() || m_impl->view->Window().IsNull()) {
        gl->glClearColor(viewportBackground.redF(), viewportBackground.greenF(),
                         viewportBackground.blueF(), 1.0F);
        gl->glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    // The pixel ratio changes when the window moves to a screen with different scaling.
    Handle(ViewportWindow) window = Handle(ViewportWindow)::DownCast(m_impl->view->Window());
    if (window->DevicePixelRatio() != devicePixelRatioF()) {
        window->setDevicePixelRatio(devicePixelRatioF());
        m_impl->view->Invalidate();
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
        m_impl->view->FitAll(0.05, false);
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
}

void ViewportWidget::mousePressEvent(QMouseEvent* event) {
    QOpenGLWidget::mousePressEvent(event);
    if (m_impl->canRender() &&
        m_impl->UpdateMouseButtons(m_impl->toFramebufferPixels(event->position()),
                                   toOcctButtons(event->buttons()),
                                   toOcctModifiers(event->modifiers()), false)) {
        update();
    }
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* event) {
    QOpenGLWidget::mouseReleaseEvent(event);
    if (m_impl->canRender() &&
        m_impl->UpdateMouseButtons(m_impl->toFramebufferPixels(event->position()),
                                   toOcctButtons(event->buttons()),
                                   toOcctModifiers(event->modifiers()), false)) {
        update();
    }
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* event) {
    QOpenGLWidget::mouseMoveEvent(event);
    if (m_impl->canRender() &&
        m_impl->UpdateMousePosition(m_impl->toFramebufferPixels(event->position()),
                                    toOcctButtons(event->buttons()),
                                    toOcctModifiers(event->modifiers()), false)) {
        update();
    }
}

void ViewportWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        fitAll();
        return;
    }
    // The second press of a double-click is still a press.
    mousePressEvent(event);
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
