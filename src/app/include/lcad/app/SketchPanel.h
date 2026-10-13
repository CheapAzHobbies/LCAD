// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/sketch/Sketch.h>
#include <lcad/viewport/SketchEditor.h>

#include <QString>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QVBoxLayout;

namespace lcad::app {

/// What the solver says about a sketch, in words: "Fully constrained", "3 degrees of freedom",
/// "Over-constrained: 2 constraints conflict".
[[nodiscard]] QString describeSketchState(const sketch::SolveResult& state);

/// The same for a sketch as a whole, which may have nothing in it yet: "Nothing drawn yet".
[[nodiscard]] QString describeSketchState(const sketch::Sketch& geometry,
                                          const sketch::SolveResult& state);

/// What one entity of a sketch is and measures: "Line · length 40 mm", "Circle · Ø20 mm".
[[nodiscard]] QString describeSketchEntity(const sketch::Sketch& geometry, sketch::EntityId id);

/// What one constraint is, with its value if it has one: "Parallel", "Distance 60 mm".
[[nodiscard]] QString describeConstraint(const sketch::Constraint& constraint);

/// What the right-hand panel shows while a sketch is open: whether the sketch is fully
/// constrained, the options of the drawing tools, what is selected with the constraints on it
/// (each of which can be removed here), and the button that finishes the sketch.
///
/// The panel mirrors the sketch editor and changes the sketch only through it.
class SketchPanel : public QWidget {
    Q_OBJECT

public:
    explicit SketchPanel(viewport::SketchEditor* editor, QWidget* parent = nullptr);

    /// Brings the panel in line with the editor. `name` is the sketch's name.
    void refresh(const QString& name);

    [[nodiscard]] QString heading() const;
    [[nodiscard]] QString stateText() const;
    /// What is said about the selection: "Line · length 40 mm", "3 items selected".
    [[nodiscard]] QString selectionText() const;
    /// The constraints listed for the selection, as the user reads them.
    [[nodiscard]] QStringList constraintRows() const { return m_rows; }
    /// Removes the constraint in a row of the list, as its button does.
    void removeConstraint(int row);

    [[nodiscard]] QSpinBox* sidesField() const { return m_sides; }
    [[nodiscard]] QCheckBox* inferenceBox() const { return m_inference; }
    [[nodiscard]] QCheckBox* constructionBox() const { return m_construction; }
    [[nodiscard]] QPushButton* finishButton() const { return m_finish; }

signals:
    void finishRequested();

private:
    void rebuildConstraints();

    viewport::SketchEditor* m_editor;
    QLabel* m_heading;
    QLabel* m_state;
    QLabel* m_profiles;
    QLabel* m_selection;
    QCheckBox* m_inference;
    QCheckBox* m_construction;
    QSpinBox* m_sides;
    QVBoxLayout* m_constraintList;
    QPushButton* m_finish;
    QStringList m_rows;
    std::vector<sketch::ConstraintId> m_listed;
    // What the list was last built from, so that it is rebuilt only when that changes.
    QString m_signature;
};

/// Asks where a new sketch goes. The answer is usually given in the 3D view, by clicking one of
/// the three planes through the origin or a flat face of a body; this panel says so, and has a
/// button for each plane for when a plane is hard to click, such as one seen edge-on.
class SketchPlanePanel : public QWidget {
    Q_OBJECT

public:
    explicit SketchPlanePanel(QWidget* parent = nullptr);

    /// The button for a plane: 0 for XY, 1 for XZ, 2 for YZ.
    [[nodiscard]] QPushButton* planeButton(int plane) const;
    /// Marks the button of the plane the pointer is on in the 3D view. -1 marks none.
    void setHoveredPlane(int plane);
    [[nodiscard]] int hoveredPlane() const { return m_hovered; }

signals:
    /// 0 for the XY plane, 1 for XZ, 2 for YZ.
    void planeChosen(int plane);
    void canceled();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    QPushButton* m_buttons[3] = {nullptr, nullptr, nullptr};
    int m_hovered = -1;
};

} // namespace lcad::app
