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
    enum class Control { None, Cube, Home, Projection, ArrowUp, ArrowRight, ArrowDown, ArrowLeft };

    [[nodiscard]] QPointF project(const QVector3D& point) const;
    [[nodiscard]] Control controlAt(const QPointF& position) const;
    [[nodiscard]] QVector3D toward() const;
    void hover(const QPointF& position);
    void drawFace(class QPainter& painter, int axis, int sign) const;
    void drawArrow(QPainter& painter, int which, bool lit) const;
    void drawButtons(QPainter& painter) const;
    void drawAxes(QPainter& painter) const;

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
};

} // namespace lcad::viewport
