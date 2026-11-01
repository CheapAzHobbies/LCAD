// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Assembly.h>
#include <lcad/document/ObjectId.h>
#include <lcad/kernel/Exchange.h>
#include <lcad/kernel/Transform.h>

#include <QColor>
#include <QOpenGLWidget>
#include <QPointF>
#include <QString>
#include <QSurfaceFormat>
#include <QVector3D>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class QLineEdit;

namespace lcad::kernel {
class Shape;
struct Plane;
} // namespace lcad::kernel
namespace lcad::document {
class Document;
}

namespace lcad::viewport {

class SketchEditor;

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
    // The view cube: its sides, the edges and corners between them, and the words on the sides.
    QColor cube{214, 218, 224};
    QColor cubeEdge{24, 26, 30};
    QColor cubeText{40, 44, 50};
    /// A sketch seen among the bodies, not being edited.
    QColor sketch{120, 180, 255};
    // A sketch being edited: geometry that can still move, geometry that is fully constrained,
    // reference geometry the sketch does not own, and whatever is part of a conflict.
    QColor sketchFree{90, 170, 255};
    QColor sketchConstrained{236, 238, 242};
    QColor sketchReference{196, 140, 240};
    QColor sketchConflict{240, 90, 90};
    /// Dimension lines and constraint markers.
    QColor sketchDimension{170, 176, 186};
    /// Behind a dimension's value and a marker.
    QColor label{38, 40, 45};

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

class NavigationCube;

/// A point of a sketch that can be clicked in the 3D view, whatever is in front of it.
struct PickablePoint {
    document::ObjectId sketch;
    std::uint32_t point = 0;
    kernel::Point at;
    bool chosen = false;
};

/// One entry of the selection: a whole body, one face, edge, or vertex of a body, or a sketch.
struct SelectedItem {
    enum class Kind { Body, Face, Edge, Vertex, Sketch };

    /// The body, or for Kind::Sketch the sketch.
    document::ObjectId body;
    Kind kind = Kind::Body;
    /// The number kernel::subShape() knows the face, edge, or vertex by. -1 for a whole body
    /// and for a sketch.
    int index = -1;
    /// Which appearance of the body it is: the occurrences from the root down to where it was
    /// picked (see document::Instance). Empty for a body of the root, and for a sketch.
    std::vector<document::ObjectId> path = {};

    friend bool operator==(const SelectedItem&, const SelectedItem&) = default;
};

/// What a machining setup shows in the 3D view: lines in the root's coordinates.
struct ToolpathLines {
    /// Where the tool cuts.
    std::vector<std::vector<kernel::Point>> cuts;
    /// Where it moves without cutting.
    std::vector<std::vector<kernel::Point>> rapids;
    /// The edges of the stock.
    std::vector<std::vector<kernel::Point>> stock;
};

/// A closed region of a sketch, which a pull or a revolve is made from: the sketch, and the
/// curves of the outline around the region (sketch::EntityId), in increasing order.
struct Profile {
    document::ObjectId sketch;
    std::vector<std::uint32_t> outline;

    friend bool operator==(const Profile&, const Profile&) = default;
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
///
/// While a sketch is open for editing (editSketch()), the left button and the keyboard belong
/// to the sketch editor instead, and bodies cannot be picked. The middle button and the wheel
/// move the camera as always.
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
    /// about to create. It cannot be picked. A null shape removes the preview. The shape is in
    /// the coordinates of the component being worked on (see setEditPath()), or, with
    /// `rootCoordinates`, of the root.
    void setPreview(const kernel::Shape& shape, bool rootCoordinates = false);
    /// Shows what a feature would make while its values are being chosen: `result` drawn as a
    /// body, in place of the body `replaces`, which is hidden meanwhile (an invalid id hides
    /// none). The preview cannot be picked. If `pickable`, the body it replaces is not hidden
    /// but left unseen under it, where its edges and faces can still be picked: for a fillet,
    /// whose edges are picked on the body as it was. A null shape ends it and shows the body
    /// again.
    void setFeaturePreview(document::ObjectId replaces, const kernel::Shape& result,
                           bool pickable = false);

    /// Where new things go and are previewed, and sketches are edited: the occurrence (the path
    /// to it from the root) of the component that is being worked on. Empty for the root. A
    /// preview, a sketch being edited, and the planes a new sketch is chosen on are shown where
    /// that occurrence is.
    void setEditPath(const std::vector<document::ObjectId>& path);
    [[nodiscard]] std::vector<document::ObjectId> editPath() const;
    /// Shows the occurrences where these say instead of where the document has them: for
    /// joints in motion. An empty list shows them where the document has them again.
    void showInstances(const std::vector<document::Instance>& instances);
    /// Shows shapes, placed in the root's coordinates, in the conflict color over everything:
    /// where parts overlap. An empty list removes them.
    void setHighlights(const std::vector<kernel::Shape>& shapes);
    /// Small X, Y, Z arrows at these frames, in the root's coordinates: where a joint is being
    /// placed. An empty list removes them.
    void setFrameMarkers(const std::vector<kernel::Placement>& frames);
    /// Shows toolpaths and the stock's edges. They cannot be picked. Empty lists remove them.
    void setToolpaths(const ToolpathLines& lines);
    /// Shows the stock as a simulation has cut it, in place of the bodies. A mesh without
    /// triangles removes it and shows the bodies again.
    void setStockSurface(const kernel::Mesh& mesh);
    /// Shows a tool, in the root's coordinates. A null shape removes it.
    void setToolShape(const kernel::Shape& shape);

    // A handle that sets a distance

    /// Shows an arrow that is dragged to set a distance, as a pull's: it stands `distance` from
    /// `base` along `direction` (of unit length), in the coordinates of the component being
    /// worked on, and points the way the distance goes. Dragging it along that line reports
    /// distanceDragged(); a box beside it shows the distance, and what is typed there is
    /// reported by distanceEntered(). Called again while it is dragged, it moves and the drag
    /// goes on.
    void setDistanceHandle(const kernel::Point& base, const kernel::Vector& direction,
                           double distance);
    void clearDistanceHandle();
    [[nodiscard]] bool hasDistanceHandle() const;
    /// Where the arrow's tip is, in the widget's coordinates. Meaningful once a frame is drawn.
    [[nodiscard]] QPointF distanceHandlePosition() const;
    /// A point along the arrow, on screen: where to press to drag it.
    [[nodiscard]] QPointF distanceHandleGrip() const;
    /// The box beside the arrow, or nullptr while there is no arrow.
    [[nodiscard]] QLineEdit* distanceField() const;

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
    [[nodiscard]] NavigationCube* navigationCube() const;
    /// Where the line of sight through a position in the widget meets a plane, in the plane's
    /// own coordinates (u, v). Nothing if the plane is seen edge-on.
    [[nodiscard]] std::optional<QPointF> positionOnPlane(const kernel::Plane& plane,
                                                         const QPointF& position) const;
    /// Turns the camera to look straight at a plane from the side its normal points to, with
    /// the plane's u to the right and its v up. Keeps looking at the same place.
    void lookAt(const kernel::Plane& plane);

    // Sketching

    /// Opens a sketch of the document for editing in place: turns the camera to face it, draws
    /// it with its constraints and dimensions, and gives the left button to the sketch tools.
    /// Returns false if there is no such sketch.
    bool editSketch(document::ObjectId sketch);
    /// Closes the sketch being edited and puts the camera back where it was.
    void finishSketch();
    /// The sketch being edited. Not valid when none is.
    [[nodiscard]] document::ObjectId editedSketch() const;
    /// The editor of the sketch. It exists always, and is active while a sketch is open.
    [[nodiscard]] SketchEditor* sketchEditor() const;

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
    /// The color an origin plane or axis is drawn in: red, green, and blue for X, Y, and Z, and
    /// a plane in the color of the axis it is square to.
    [[nodiscard]] static QColor originColor(OriginItem item);

    // Choosing a plane

    /// While on, the three planes through the origin are shown as patches large enough to click,
    /// drawn over the model so that a body at the origin does not hide them. Each lights up
    /// under the pointer, and a click on one is reported by planePicked() and selects nothing.
    /// This is how the plane of a new sketch is chosen. Off until switched on.
    void setPlanePicking(bool on);
    [[nodiscard]] bool isPlanePicking() const;
    /// The plane whose patch is at a position in the widget, while plane picking is on.
    [[nodiscard]] std::optional<OriginItem> pickPlaneAt(const QPoint& position);
    /// The construction plane at a position in the widget, while plane picking is on, or an
    /// invalid id. Construction planes are drawn as see-through squares whenever they are
    /// visible, and can be clicked only while a plane is being chosen.
    [[nodiscard]] document::ObjectId constructionPlaneAt(const QPoint& position);

    // Choosing profiles

    /// Shows the closed regions of these sketches as patches that can be clicked, drawn over
    /// the model, hidden sketches too. Each lights up under the pointer. A click on one adds it
    /// to the picked profiles, or takes it out if it is picked, and is reported by
    /// profilesPicked(); it selects nothing. Bodies and sketches cannot be picked meanwhile,
    /// except, with `facesToo`, the faces of bodies, where no region is under the pointer: for a
    /// pull up to a face. An empty list ends it.
    void setProfileSketches(const std::vector<document::ObjectId>& sketches, bool facesToo = false);
    [[nodiscard]] bool isProfilePicking() const;
    /// Makes these the picked profiles, which are drawn in the selection color.
    void setPickedProfiles(const std::vector<Profile>& profiles);
    [[nodiscard]] std::vector<Profile> pickedProfiles() const;
    /// Every profile of the sketches given to setProfileSketches(), which can be picked.
    [[nodiscard]] std::vector<Profile> pickableProfiles() const;
    /// The profile whose patch is at a position in the widget, while profiles are being chosen.
    [[nodiscard]] std::optional<Profile> profileAt(const QPoint& position);

    // The selection

    void setSelectionFilter(SelectionFilter filter);
    [[nodiscard]] SelectionFilter selectionFilter() const;
    [[nodiscard]] std::vector<SelectedItem> selection() const;
    /// Makes exactly these bodies the selection, as whole bodies. Ids of bodies that are not
    /// displayed are ignored.
    void setSelectedBodies(const std::vector<document::ObjectId>& bodies);
    /// Makes exactly these items the selection. Faces, edges, and vertices can only be
    /// selected while the filter lets them be picked; others are ignored. A sketch is picked
    /// as a whole, while the filter lets edges be picked.
    void setSelection(const std::vector<SelectedItem>& items);
    void clearSelection();
    /// Everything the filter lets be picked at a position in the widget, nearest first: also
    /// what lies behind the item a click there would select.
    [[nodiscard]] std::vector<SelectedItem> itemsAt(const QPoint& position);
    /// Shows these points over everything, and makes each a thing to click: pointPicked() says
    /// which. None ends that.
    void setPickablePoints(std::vector<PickablePoint> points);
    [[nodiscard]] const std::vector<PickablePoint>& pickablePoints() const;

    // Diagnostics

    [[nodiscard]] QColor backgroundColor() const;
    /// The OpenGL vendor, device, and version the viewport renders with, one per line. Empty
    /// until the viewport has been shown.
    [[nodiscard]] QString graphicsInfo() const;
    /// Why the viewport cannot render, or an empty string when it is working.
    [[nodiscard]] QString errorString() const;

signals:
    void selectionChanged();
    /// Perspective was switched on or off, by whatever means.
    void projectionChanged();
    /// One of the points of setPickablePoints() was clicked.
    void pointPicked(lcad::document::ObjectId sketch, std::uint32_t point);
    /// A sketch was opened for editing, or closed.
    void sketchEditingChanged();
    /// The user double-clicked something that can be picked.
    void itemActivated(const lcad::viewport::SelectedItem& item);
    /// While plane picking is on: the user clicked a plane's patch.
    void planePicked(lcad::viewport::OriginItem plane);
    /// While plane picking is on: the user clicked a construction plane (a feature of the
    /// document).
    void constructionPlanePicked(lcad::document::ObjectId plane);
    /// While plane picking is on: the pointer came onto a plane's patch (0 for XY, 1 for XZ, 2
    /// for YZ) or left the patches (-1).
    void planeHovered(int plane);
    /// While profiles are being chosen: the user clicked one, and the picked ones changed.
    void profilesPicked();
    /// Emitted once if the viewport cannot start rendering.
    void initializationFailed(const QString& reason);
    /// The distance handle was dragged to this distance, rounded to a tenth of a millimeter.
    void distanceDragged(double distance);
    /// Text was typed into the box beside the distance handle, and Enter pressed.
    void distanceEntered(const QString& text);

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    class Impl;
    void fail(const QString& reason);

    std::unique_ptr<Impl> m_impl;
};

} // namespace lcad::viewport
