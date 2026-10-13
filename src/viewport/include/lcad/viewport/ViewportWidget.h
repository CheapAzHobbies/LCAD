// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/ObjectId.h>

#include <QColor>
#include <QOpenGLWidget>
#include <QPointF>
#include <QString>
#include <QSurfaceFormat>
#include <QVector3D>

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
/// orbits, Ctrl + Shift + middle button zooms, and a double-click of the middle button fits
/// the view. A left click selects, Shift or Ctrl + click adds to or removes from the selection,
/// and dragging with the left button selects what lies inside the rectangle.
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

    // The selection

    void setSelectionFilter(SelectionFilter filter);
    [[nodiscard]] SelectionFilter selectionFilter() const;
    [[nodiscard]] std::vector<SelectedItem> selection() const;
    /// Makes exactly these bodies the selection, as whole bodies. Ids of bodies that are not
    /// displayed are ignored.
    void setSelectedBodies(const std::vector<document::ObjectId>& bodies);
    void clearSelection();

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
