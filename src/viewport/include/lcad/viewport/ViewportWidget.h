// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/ObjectId.h>

#include <QColor>
#include <QOpenGLWidget>
#include <QPointF>
#include <QString>
#include <QSurfaceFormat>
#include <QVector3D>

#include <array>
#include <memory>
#include <vector>

namespace lcad::kernel {
class Shape;
}
namespace lcad::document {
class Document;
}

namespace lcad::viewport {

/// Process-wide setup the viewport depends on. Call it once, before constructing QApplication.
///
/// It selects the X11 platform plugin on Linux (unless QT_QPA_PLATFORM is already set), makes
/// the viewport's surface format the default, and lets OpenGL contexts share resources.
void prepareApplication();

enum class Projection { Orthographic, Perspective };

/// The named camera orientations. Z is up; Front looks along +Y, so X runs to the right. Home
/// is the isometric view from the front, right, and top.
enum class StandardView { Home, Front, Back, Left, Right, Top, Bottom };

/// What a click or a selection rectangle can pick.
enum class SelectionFilter {
    /// Faces, edges, and vertices: whichever is under the pointer. The default.
    FacesEdgesVertices,
    Bodies,
    Faces,
    Edges,
    Vertices,
};

/// How bodies are drawn.
enum class DisplayStyle {
    /// Shaded, with an outline around every face. The default.
    ShadedWithEdges,
    Shaded,
    /// Edges only, including those at the back.
    Wireframe,
    /// Edges only, without those hidden behind the body.
    HiddenLine,
    /// Shaded and see-through, with outlines.
    Transparent,
};

/// What the middle mouse button does. The wheel zooms in both.
enum class NavigationPreset {
    /// Middle button pans, Shift + middle orbits, Ctrl + Shift + middle zooms. The default.
    MiddlePans,
    /// Middle button orbits, Shift + middle pans, Ctrl + middle zooms.
    MiddleOrbits,
};

/// The reference planes and axes through the origin, which can be shown as a guide.
enum class OriginItem { PlaneXY, PlaneXZ, PlaneYZ, AxisX, AxisY, AxisZ };

/// The colors the viewport draws with. The interface's theme supplies them.
struct ViewportColors {
    QColor background{46, 49, 54};
    QColor body{168, 176, 186};
    QColor bodyEdge{30, 32, 36};
    /// Wireframe lines and the labels of the axis marker.
    QColor line{236, 238, 242};
    QColor hover{255, 255, 255};
    QColor selection{20, 120, 255};
    QColor preview{90, 170, 255};
    QColor grid{64, 68, 75};
    QColor gridMajor{84, 89, 98};
    QColor cube{214, 218, 224};
    QColor cubeText{40, 44, 50};

    friend bool operator==(const ViewportColors&, const ViewportColors&) = default;
};

/// Where the camera is and how it looks at the model, so that a view can be put back.
struct CameraState {
    std::array<double, 3> eye{};
    std::array<double, 3> center{};
    std::array<double, 3> up{};
    double scale = 1.0;
    Projection projection = Projection::Orthographic;
};

/// One entry of the selection: a whole body, or one face, edge, or vertex of a body.
struct SelectedItem {
    enum class Kind { Body, Face, Edge, Vertex };

    document::ObjectId body;
    Kind kind = Kind::Body;
    /// The number kernel::subShape() knows the face, edge, or vertex by. -1 for a whole body.
    int index = -1;

    friend bool operator==(const SelectedItem&, const SelectedItem&) = default;
};

/// The 3D view of a document.
///
/// The viewport shows the visible bodies of the document it is given and follows every change
/// to it. It holds no model state of its own. What it does own is the camera and the selection.
///
/// Mouse: the wheel zooms about the pointer, the middle button pans, Shift + middle button
/// orbits, Ctrl + Shift + middle button zooms (NavigationPreset can swap pan and orbit), and a
/// double-click of the middle button fits the view. A left click selects, Shift or Ctrl + click
/// adds to or removes from the selection, and dragging with the left button selects what lies
/// inside the rectangle. Clicking the view cube turns the camera to that side; dragging it
/// orbits.
class ViewportWidget : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit ViewportWidget(QWidget* parent = nullptr);
    ~ViewportWidget() override;

    /// The OpenGL surface format the viewport needs.
    [[nodiscard]] static QSurfaceFormat surfaceFormat();

    // The document

    /// Shows `document` in place of whatever was shown, and frames it. The document has to stay
    /// alive until another one, or nullptr, is set.
    void setDocument(document::Document* document);
    [[nodiscard]] document::Document* document() const;

    /// Shows `shape` see-through on top of the document, as a preview of what a command is
    /// about to create. It cannot be picked. A null shape removes the preview.
    void setPreview(const kernel::Shape& shape);

    // The camera

    /// Moves the camera so that everything displayed is in view.
    void fitAll();
    /// Turns the camera to a named orientation and frames everything.
    void setStandardView(StandardView view);
    void setProjection(Projection projection);
    [[nodiscard]] Projection projection() const;
    /// Moves the camera so that the selection fills the view, or everything if nothing is
    /// selected.
    void fitSelection();
    void setNavigationPreset(NavigationPreset preset);
    [[nodiscard]] NavigationPreset navigationPreset() const;
    /// Whether the camera turns to a side of the view cube in a short animation or at once.
    void setAnimated(bool animated);
    [[nodiscard]] bool isAnimated() const;
    [[nodiscard]] CameraState cameraState() const;
    /// Puts the camera where cameraState() found it. Cancels a pending fit.
    void setCameraState(const CameraState& state);
    /// When set, rolling the wheel away from the user zooms out instead of in.
    void setZoomReversed(bool reversed);
    [[nodiscard]] bool isZoomReversed() const;

    /// The direction the camera looks in, as a unit vector in model space.
    [[nodiscard]] QVector3D viewDirection() const;
    /// Where a point in model space (millimeters) appears in the widget, in the widget's
    /// coordinates. Meaningful once the viewport has drawn a frame.
    [[nodiscard]] QPointF project(double x, double y, double z) const;
    /// The middle of the view cube, in the widget's coordinates.
    [[nodiscard]] QPointF viewCubeCenter() const;

    // What is drawn besides the document

    void setColors(const ViewportColors& colors);
    [[nodiscard]] ViewportColors colors() const;
    void setDisplayStyle(DisplayStyle style);
    [[nodiscard]] DisplayStyle displayStyle() const;
    /// A reference grid on the XY plane, with lines 10 mm apart. Off until switched on.
    void setGridVisible(bool visible);
    [[nodiscard]] bool isGridVisible() const;
    /// The X, Y, Z marker in the corner. On until switched off.
    void setAxesVisible(bool visible);
    [[nodiscard]] bool areAxesVisible() const;
    /// The origin planes and axes keep their size on screen at any zoom, and cannot be picked.
    void setOriginItemVisible(OriginItem item, bool visible);
    [[nodiscard]] bool isOriginItemVisible(OriginItem item) const;

    // The selection

    void setSelectionFilter(SelectionFilter filter);
    [[nodiscard]] SelectionFilter selectionFilter() const;
    [[nodiscard]] std::vector<SelectedItem> selection() const;
    /// Makes exactly these bodies the selection, as whole bodies. Ids of bodies that are not
    /// displayed are ignored.
    void setSelectedBodies(const std::vector<document::ObjectId>& bodies);
    /// Makes exactly these items the selection. Faces, edges, and vertices can only be
    /// selected while the filter lets them be picked; others are ignored.
    void setSelection(const std::vector<SelectedItem>& items);
    void clearSelection();
    /// Everything the filter lets be picked at a position in the widget, nearest first: also
    /// what lies behind the item a click there would select.
    [[nodiscard]] std::vector<SelectedItem> itemsAt(const QPoint& position);

    // Diagnostics

    [[nodiscard]] QColor backgroundColor() const;
    /// The OpenGL vendor, device, and version the viewport renders with, one per line. Empty
    /// until the viewport has been shown.
    [[nodiscard]] QString graphicsInfo() const;
    /// Why the viewport cannot render, or an empty string when it is working.
    [[nodiscard]] QString errorString() const;

signals:
    void selectionChanged();
    /// Emitted once if the viewport cannot start rendering.
    void initializationFailed(const QString& reason);

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    class Impl;
    void fail(const QString& reason);

    std::unique_ptr<Impl> m_impl;
};

} // namespace lcad::viewport
