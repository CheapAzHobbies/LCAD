// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>
#include <lcad/kernel/Shape.h>
#include <lcad/kernel/Transform.h>
#include <lcad/sketch/Edit.h>
#include <lcad/sketch/Sketch.h>

#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <memory>
#include <optional>
#include <vector>

class QKeyEvent;
class QLineEdit;
class QPainter;

namespace lcad::viewport {

class ViewportWidget;

/// What a click in a sketch being edited does.
enum class SketchTool {
    /// Selects, and drags geometry that can still move. The tool a sketch starts with.
    Select,
    Line,
    Rectangle,
    Circle,
    /// By its center, its start, and its end.
    Arc,
    Point,
    Polygon,
    Slot,
    /// By its center, the end of its major axis, and a point its minor radius reaches to.
    Ellipse,
    /// Through the first and the last of the points clicked, drawn toward the others. Enter, a
    /// double-click, or Esc ends it.
    Spline,
    /// Click a line and type its length, or click where to put it first. A circle or an arc
    /// takes one click; two points or lines, a click on each.
    Dimension,
    /// Applies one kind of geometric constraint to whatever is clicked.
    Constraint,
    Trim,
    Extend,
    Offset,
    Mirror,
    /// Click an edge or a corner of a body to bring it into the sketch as reference geometry.
    Project,
};

/// Edits one sketch of a document inside a ViewportWidget.
///
/// The viewport hands the editor the pointer and the keyboard while a sketch is open, and the
/// editor draws the sketch on top of the 3D view: its geometry colored by whether it can still
/// move, its constraints as small markers, its dimensions as values that can be clicked and
/// typed over. Every change it makes is an edit of the document, and so one undo step.
///
/// The sketch being edited lives in the document. The editor keeps only what is not part of the
/// model: the tool, the selection, and what is under the pointer.
class SketchEditor : public QObject {
    Q_OBJECT

public:
    explicit SketchEditor(ViewportWidget* viewport);
    ~SketchEditor() override;

    /// Whether a sketch is open for editing.
    [[nodiscard]] bool isActive() const;
    /// The sketch being edited. Not valid when none is.
    [[nodiscard]] document::ObjectId sketch() const;
    /// Its geometry. Only while a sketch is open.
    [[nodiscard]] const sketch::Sketch& geometry() const;
    /// What the solver says about it: how much can still move, and what conflicts.
    [[nodiscard]] const sketch::SolveResult& state() const;

    // Tools

    void setTool(SketchTool tool);
    [[nodiscard]] SketchTool tool() const;
    /// Makes clicks apply a geometric constraint: click the one, two, or three things it takes.
    void setConstraintTool(sketch::ConstraintKind kind);
    /// The kind the Constraint tool applies.
    [[nodiscard]] sketch::ConstraintKind constraintTool() const;
    /// Whether something is under way that cancel() would step back from: a curve half drawn, a
    /// drag, a value being typed, or a tool other than Select.
    [[nodiscard]] bool canCancel() const;
    /// Steps back once: closes the value being typed, or abandons the curve being drawn, or
    /// returns to the Select tool.
    void cancel();

    /// Whether new geometry picks up constraints from where it is drawn: ends that land on
    /// existing points or curves stay there, and nearly level or upright lines become exactly
    /// so. On until switched off.
    void setInference(bool on);
    [[nodiscard]] bool hasInference() const;
    /// Whether the drawing tools make construction geometry.
    void setConstructionMode(bool on);
    [[nodiscard]] bool constructionMode() const;
    /// How many sides the polygon tool gives a polygon: 3 to 64.
    void setPolygonSides(int sides);
    [[nodiscard]] int polygonSides() const;

    // The selection

    [[nodiscard]] std::vector<sketch::EntityId> selectedEntities() const;
    [[nodiscard]] std::vector<sketch::ConstraintId> selectedConstraints() const;
    void setSelection(const std::vector<sketch::EntityId>& entities,
                      const std::vector<sketch::ConstraintId>& constraints = {});
    void clearSelection();

    // Commands on the selection. Each reports why through message() when it does nothing.

    /// Applies a geometric constraint to the selection. If the selection is not what that
    /// constraint takes, the constraint becomes the tool instead and false is returned.
    bool constrain(sketch::ConstraintKind kind);
    /// Adds the dimension the selection calls for and opens it for typing.
    bool addDimension();
    /// Gives a dimension another value: millimeters, or degrees for an angle. Returns false,
    /// and leaves the sketch as it was, if the geometry cannot follow.
    bool setDimensionValue(sketch::ConstraintId dimension, double value);
    /// A dimension's value as it is shown and typed: millimeters, or degrees for an angle.
    [[nodiscard]] double dimensionValue(sketch::ConstraintId dimension) const;
    /// Deletes the selected curves, free-standing points, and constraints.
    bool deleteSelection();
    /// Turns the selected curves into construction geometry, or back if they all are already.
    bool toggleConstruction();
    /// Mirrors the selected geometry in a line. Returns false if `axis` is not a line.
    bool mirrorSelection(sketch::EntityId axis);
    /// Brings an edge or a vertex of a body into the sketch as reference geometry: dropped
    /// straight onto the sketch's plane, and fixed there. Returns false for a shape that cannot
    /// be projected: anything but a straight edge, a circle or arc lying flat to the sketch,
    /// or a vertex.
    bool projectShape(const kernel::Shape& edgeOrVertex);

    // Typing a value in the view

    /// Opens a dimension's value for typing, at the dimension.
    void editDimension(sketch::ConstraintId dimension);
    /// The field a value is being typed into, or nullptr.
    [[nodiscard]] QLineEdit* valueField() const;

    // Where things are in the widget

    /// Where a position of the sketch appears in the viewport.
    [[nodiscard]] QPointF toWidget(sketch::Point2 position) const;
    /// The position of the sketch under a position in the viewport, if the view is not edge-on.
    [[nodiscard]] std::optional<sketch::Point2> toSketch(const QPointF& position) const;
    /// Where a dimension's value is drawn. Empty if it is not a dimension of this sketch.
    [[nodiscard]] QRectF dimensionRect(sketch::ConstraintId dimension) const;
    /// Where a geometric constraint's marker is drawn (its first, if it has two).
    [[nodiscard]] QRectF markerRect(sketch::ConstraintId constraint) const;
    /// What is said about the tool in use and what it waits for, for the status bar.
    [[nodiscard]] QString hint() const;
    /// The curve or point under a position in the viewport, or 0.
    [[nodiscard]] sketch::EntityId entityAt(const QPointF& position) const;
    /// The constraint whose marker or value is under a position in the viewport, or 0.
    [[nodiscard]] sketch::ConstraintId constraintAt(const QPointF& position) const;
    /// The name a kind of constraint goes by in the interface: "Parallel", "Distance".
    [[nodiscard]] static QString constraintName(sketch::ConstraintKind kind);

    // For the viewport

    /// Where the sketch's component is shown: the sketch is drawn, and clicks are taken, on its
    /// plane placed so. What projectShape() is given is in the same, the root's, coordinates.
    void setPlacement(const kernel::Placement& placement);
    void begin(document::Document* document, document::ObjectId sketch);
    void end();
    void documentChanged(const document::Change& change);
    void mousePress(const QPointF& position, Qt::KeyboardModifiers modifiers);
    void mouseMove(const QPointF& position, Qt::MouseButtons buttons,
                   Qt::KeyboardModifiers modifiers);
    void mouseRelease(const QPointF& position, Qt::KeyboardModifiers modifiers);
    void mouseDoubleClick(const QPointF& position, Qt::KeyboardModifiers modifiers);
    /// Returns whether the key was the editor's to handle.
    bool keyPress(QKeyEvent* event);
    void paint(QPainter& painter);
    /// The widget the sketch is drawn on, lying over the viewport.
    [[nodiscard]] QWidget* overlay() const;

    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    /// The tool, the selection, or the sketch itself changed.
    void changed();
    /// Something the user should be told: why a constraint was not added, what a tool needs.
    void message(const QString& text);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lcad::viewport
