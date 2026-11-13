// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector3D>
#include <QWidget>

#include <array>
#include <vector>

namespace lcad::viewport {

/// The cube in the corner of the 3D view, which says which way the model is turned and turns it.
///
/// It is a true cube, turned as the camera is, cut into 26 parts: the six faces give the straight
/// views, the twelve edges the views halfway between two of those, and the eight corners the
/// isometric views. It holds no view of its own: the viewport tells it where the camera is, and
/// acts on what it asks for.
class NavigationCube : public QWidget {
    Q_OBJECT

public:
    /// A part of the cube, by which way it lies along X, Y, and Z from the middle: -1, 0, or 1.
    /// One that is not zero is a face, two an edge, three a corner.
    struct Part {
        int x = 0;
        int y = 0;
        int z = 0;
        [[nodiscard]] bool isValid() const { return x != 0 || y != 0 || z != 0; }
        friend bool operator==(const Part&, const Part&) = default;
    };

    /// Which colors it is drawn in.
    struct Colors {
        QColor side;
        QColor edge;
        QColor text;
        QColor highlight;
        QColor line;
        std::array<QColor, 3> axes;
    };

    explicit NavigationCube(QWidget* parent);

    /// Every part there is: 6 faces, 12 edges, 8 corners.
    [[nodiscard]] static std::vector<Part> parts();
    /// What a part is called: "Top", "Top + Front", "Top + Front + Right".
    [[nodiscard]] static QString nameOf(Part part);
    /// The part looked straight at from this direction. Not valid if the view lies between parts.
    [[nodiscard]] static Part partFacing(const QVector3D& from);
    /// The spin around the line of sight that suits the cube from this direction, in radians. A
    /// third of a turn at a corner, where the cube looks the same again with its sides changed
    /// round, and a quarter anywhere else. At an edge that is a quarter too, not the half turn that
    /// would bring the cube back, because a half turn there just turns the view upside down.
    [[nodiscard]] static double cubeSpin(const QVector3D& from);

    /// How far the straight arrows turn the view, in radians: 45° unless set.
    void setTurnStep(double radians);
    [[nodiscard]] double turnStep() const { return m_turnStep; }
    /// How far the curved arrows spin it, in radians. 0, the default, goes as far as suits the
    /// cube: see cubeSpin().
    void setSpinStep(double radians);
    [[nodiscard]] double spinStep() const { return m_spinStep; }
    /// How far a curved arrow spins the view from this direction, in radians.
    [[nodiscard]] double spinFrom(const QVector3D& from) const;
    /// Opens a box to type how far an arrow goes: the curved ones if `spin`, else the straight.
    void askStep(bool spin);
    /// The steps the menu of an arrow offers, in degrees, besides one typed in.
    static constexpr std::array<int, 6> stepChoices{5, 10, 15, 30, 45, 90};

    /// Where the camera is: which ways the right of the screen and its top lie in the model.
    void setView(const QVector3D& right, const QVector3D& up);
    void setColors(const Colors& colors);
    void setPerspective(bool perspective);
    /// Smaller in a small view.
    void fitTo(const QSize& view);

    /// The middle of the cube, in the widget.
    [[nodiscard]] QPointF cubeCenter() const;
    /// The part at a position in the widget. Not valid for none.
    [[nodiscard]] Part partAt(const QPointF& position) const;
    /// A place on a part that can be seen from where the camera is, in the widget. A position
    /// outside the widget for a part on the far side.
    [[nodiscard]] QPointF placeOf(Part part) const;
    /// What the pointer is on. Not valid for none.
    [[nodiscard]] Part hovered() const { return m_hovered; }
    /// What is written under the cube: the name of what the pointer is on.
    [[nodiscard]] QString caption() const;
    /// Where the buttons are, in the widget.
    [[nodiscard]] QRectF homeButton() const;
    [[nodiscard]] QRectF projectionButton() const;
    /// The arrow that turns the view a step: 0 up, 1 right, 2 down, 3 left.
    [[nodiscard]] QRectF arrow(int which) const;
    /// The curved arrow that spins the view: 0 counter-clockwise, 1 clockwise.
    [[nodiscard]] QRectF spinArrow(int which) const;

signals:
    /// A part was clicked: look at the model from this direction.
    void viewChosen(const QVector3D& fromDirection);
    /// A part was double-clicked: the same, and frame the model.
    void framedViewChosen(const QVector3D& fromDirection);
    void homeChosen();
    void projectionToggled();
    /// An arrow was clicked: turn the view this far around the right of the screen and its top,
    /// in radians.
    void turnAsked(double aroundRight, double aroundUp);
    /// A curved arrow was clicked: spin what is on the screen around the line of sight.
    void spinAsked(bool clockwise);
    /// A step was chosen for the arrows, in the menu of one of them or in the box.
    void stepsChosen();
    /// The cube is being dragged, to turn the view freely. Positions are in the parent.
    void orbitStarted(const QPointF& position);
    void orbitMoved(const QPointF& position);
    void orbitEnded(const QPointF& position);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool event(QEvent* event) override;

private:
    enum class Control {
        None,
        Cube,
        Home,
        Projection,
        ArrowUp,
        ArrowRight,
        ArrowDown,
        ArrowLeft,
        SpinLeft,
        SpinRight,
    };

    [[nodiscard]] QPointF project(const QVector3D& point) const;
    [[nodiscard]] Control controlAt(const QPointF& position) const;
    [[nodiscard]] QVector3D toward() const;
    void hover(const QPointF& position);
    void drawFace(class QPainter& painter, int axis, int sign) const;
    void drawArrow(QPainter& painter, int which, bool lit) const;
    void drawSpin(QPainter& painter, int which, bool lit) const;
    void drawButtons(QPainter& painter) const;
    void drawAxes(QPainter& painter) const;
    void showSteps(bool spin, const QPoint& at);

    QVector3D m_right{1.0F, 0.0F, 0.0F};
    QVector3D m_up{0.0F, 0.0F, 1.0F};
    Colors m_colors;
    bool m_perspective = false;
    double m_scale = 1.0;
    Part m_hovered;
    Control m_control = Control::None;
    bool m_inside = false;
    // Between press and release.
    Control m_pressed = Control::None;
    Part m_pressedPart;
    QPointF m_pressPosition;
    bool m_orbiting = false;
    double m_turnStep;
    double m_spinStep = 0.0;
};

} // namespace lcad::viewport
