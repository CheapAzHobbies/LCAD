// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/viewport/NavigationCube.h>

#include <QApplication>
#include <QEnterEvent>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lcad::viewport {

namespace {

// The size of the widget, and half the side of the cube, in logical pixels. Turned to a corner
// the cube is then about 100 pixels across.
constexpr int widgetWidth = 140;
constexpr int widgetHeight = 168;
constexpr double halfSide = 36.0;
// How far from the middle of the widget the arrows are, and how large.
constexpr double arrowReach = 60.0;
constexpr double arrowSize = 16.0;
constexpr double buttonSize = 20.0;
// The bands along a face's sides that belong to its edges and corners, as a share of half its
// side: wide enough to hit, on a cube this small.
constexpr double band = 0.36;
// The pointer counts as on the cube from a little outside it.
constexpr double hitReach = 1.05;
// How far an arrow turns the view.
constexpr double arrowStep = std::numbers::pi / 4.0;

float along(const QVector3D& v, int axis) {
    return v[axis];
}

int along(const NavigationCube::Part& part, int axis) {
    return axis == 0 ? part.x : axis == 1 ? part.y : part.z;
}

// What a face is called, which way its words run, and which way is up on it.
struct FaceInfo {
    const char* name;
    QVector3D across;
    QVector3D upward;
};

FaceInfo faceInfo(int axis, int sign) {
    using V = QVector3D;
    if (axis == 0) {
        return sign > 0 ? FaceInfo{QT_TRANSLATE_NOOP("NavigationCube", "RIGHT"), V(0, 1, 0), V(0, 0, 1)}
                        : FaceInfo{QT_TRANSLATE_NOOP("NavigationCube", "LEFT"), V(0, -1, 0), V(0, 0, 1)};
    }
    if (axis == 1) {
        return sign > 0 ? FaceInfo{QT_TRANSLATE_NOOP("NavigationCube", "BACK"), V(-1, 0, 0), V(0, 0, 1)}
                        : FaceInfo{QT_TRANSLATE_NOOP("NavigationCube", "FRONT"), V(1, 0, 0), V(0, 0, 1)};
    }
    return sign > 0 ? FaceInfo{QT_TRANSLATE_NOOP("NavigationCube", "TOP"), V(1, 0, 0), V(0, 1, 0)}
                    : FaceInfo{QT_TRANSLATE_NOOP("NavigationCube", "BOTTOM"), V(1, 0, 0), V(0, -1, 0)};
}

// Cut a little off the corners of a face, so that they look rounded and meet cleanly.
QPolygonF softened(const QPolygonF& quad) {
    QPolygonF soft;
    for (int i = 0; i < quad.size(); ++i) {
        const QPointF at = quad[i];
        const QPointF before = quad[(i + quad.size() - 1) % quad.size()];
        const QPointF after = quad[(i + 1) % quad.size()];
        soft << at + (before - at) * 0.05 << at + (after - at) * 0.05;
    }
    return soft;
}

} // namespace

NavigationCube::NavigationCube(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("navigationCube"));
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setFixedSize(widgetWidth, widgetHeight);
}

std::vector<NavigationCube::Part> NavigationCube::parts() {
    std::vector<Part> all;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            for (int z = -1; z <= 1; ++z) {
                if (x != 0 || y != 0 || z != 0) {
                    all.push_back({x, y, z});
                }
            }
        }
    }
    return all;
}

QString NavigationCube::nameOf(Part part) {
    QStringList names;
    if (part.z != 0) {
        names << (part.z > 0 ? tr("Top") : tr("Bottom"));
    }
    if (part.y != 0) {
        names << (part.y > 0 ? tr("Back") : tr("Front"));
    }
    if (part.x != 0) {
        names << (part.x > 0 ? tr("Right") : tr("Left"));
    }
    return names.join(QStringLiteral(" + "));
}

void NavigationCube::setView(const QVector3D& right, const QVector3D& up) {
    if (qFuzzyCompare(right, m_right) && qFuzzyCompare(up, m_up)) {
        return;
    }
    m_right = right;
    m_up = up;
    update();
}

void NavigationCube::setColors(const Colors& colors) {
    m_colors = colors;
    update();
}

void NavigationCube::setPerspective(bool perspective) {
    m_perspective = perspective;
    update();
}

void NavigationCube::fitTo(const QSize& view) {
    const double scale = std::min(view.width(), view.height()) < 420 ? 0.8 : 1.0;
    if (scale != m_scale) {
        m_scale = scale;
        setFixedSize(static_cast<int>(widgetWidth * scale), static_cast<int>(widgetHeight * scale));
        update();
    }
}

QPointF NavigationCube::cubeCenter() const {
    return QPointF(70.0, 68.0) * m_scale;
}

QVector3D NavigationCube::toward() const {
    return QVector3D::crossProduct(m_right, m_up);
}

QPointF NavigationCube::project(const QVector3D& point) const {
    const double size = halfSide * m_scale;
    return cubeCenter() + QPointF(QVector3D::dotProduct(point, m_right) * size,
                                  -QVector3D::dotProduct(point, m_up) * size);
}

NavigationCube::Part NavigationCube::partAt(const QPointF& position) const {
    const double size = halfSide * m_scale * hitReach;
    const QPointF off = position - cubeCenter();
    const QVector3D through =
        m_right * static_cast<float>(off.x() / size) - m_up * static_cast<float>(off.y() / size);
    const QVector3D look = toward();
    // The faces that the line of sight through the position meets, the nearest one wins.
    Part best;
    float nearest = -1e9F;
    for (int axis = 0; axis < 3; ++axis) {
        for (const int sign : {-1, 1}) {
            const float facing = along(look, axis) * static_cast<float>(sign);
            if (facing <= 1e-4F) {
                continue;
            }
            const float t = (static_cast<float>(sign) * 1.0F - along(through, axis)) /
                            along(look, axis);
            const QVector3D at = through + look * t;
            const bool on = std::abs(along(at, (axis + 1) % 3)) <= 1.0F &&
                            std::abs(along(at, (axis + 2) % 3)) <= 1.0F;
            if (!on || t <= nearest) {
                continue;
            }
            nearest = t;
            const auto part = [&](int i) {
                if (i == axis) {
                    return sign;
                }
                const float value = along(at, i);
                return value > 1.0F - band ? 1 : value < -1.0F + band ? -1 : 0;
            };
            best = {part(0), part(1), part(2)};
        }
    }
    return best;
}

QPointF NavigationCube::placeOf(Part part) const {
    const QVector3D look = toward();
    int face = -1;
    float best = 1e-3F;
    for (int axis = 0; axis < 3; ++axis) {
        const float facing = along(look, axis) * static_cast<float>(along(part, axis));
        if (facing > best) {
            best = facing;
            face = axis;
        }
    }
    if (face < 0) {
        return {-1.0, -1.0};
    }
    QVector3D at;
    for (int axis = 0; axis < 3; ++axis) {
        const auto sign = static_cast<float>(along(part, axis));
        at[axis] = axis == face ? sign : sign * static_cast<float>(1.0 - band / 2.0);
    }
    return project(at);
}

QRectF NavigationCube::arrow(int which) const {
    const double reach = arrowReach * m_scale;
    const QPointF center = cubeCenter() + QPointF(which == 1 ? reach : which == 3 ? -reach : 0.0,
                                                  which == 2 ? reach : which == 0 ? -reach : 0.0);
    const double size = arrowSize * m_scale;
    return QRectF(center.x() - size / 2.0, center.y() - size / 2.0, size, size);
}

QRectF NavigationCube::homeButton() const {
    const double size = buttonSize * m_scale;
    return QRectF(width() / 2.0 - size - 2.0, height() - size - 1.0, size, size);
}

QRectF NavigationCube::projectionButton() const {
    const double size = buttonSize * m_scale;
    return QRectF(width() / 2.0 + 2.0, height() - size - 1.0, size, size);
}

NavigationCube::Control NavigationCube::controlAt(const QPointF& position) const {
    if (homeButton().contains(position)) {
        return Control::Home;
    }
    if (projectionButton().contains(position)) {
        return Control::Projection;
    }
    if (partAt(position).isValid()) {
        return Control::Cube;
    }
    if (m_inside) {
        const std::array<Control, 4> arrows{Control::ArrowUp, Control::ArrowRight,
                                            Control::ArrowDown, Control::ArrowLeft};
        for (int i = 0; i < 4; ++i) {
            if (arrow(i).contains(position)) {
                return arrows[static_cast<std::size_t>(i)];
            }
        }
    }
    return Control::None;
}

QString NavigationCube::caption() const {
    switch (m_control) {
    case Control::Cube:
        return nameOf(m_hovered);
    case Control::Home:
        return tr("Home");
    case Control::Projection:
        return m_perspective ? tr("Perspective") : tr("Orthographic");
    case Control::ArrowUp:
    case Control::ArrowRight:
    case Control::ArrowDown:
    case Control::ArrowLeft:
        return tr("Turn 45°");
    case Control::None:
        break;
    }
    return {};
}

void NavigationCube::hover(const QPointF& position) {
    const Control control = controlAt(position);
    const Part part = control == Control::Cube ? partAt(position) : Part{};
    if (control != m_control || part != m_hovered) {
        m_control = control;
        m_hovered = part;
        update();
    }
}

void NavigationCube::enterEvent(QEnterEvent* event) {
    QWidget::enterEvent(event);
    m_inside = true;
    hover(event->position());
    update();
}

void NavigationCube::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    m_inside = false;
    m_control = Control::None;
    m_hovered = {};
    update();
}

bool NavigationCube::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        QString text;
        switch (controlAt(help->pos())) {
        case Control::Cube:
            text = tr("%1\nClick for this view, double-click to frame the model too. Click the "
                      "view you are in for Home. Drag to turn freely.")
                       .arg(nameOf(partAt(help->pos())));
            break;
        case Control::Home:
            text = tr("Home: the default isometric view");
            break;
        case Control::Projection:
            text = m_perspective ? tr("Perspective. Click for orthographic.")
                                 : tr("Orthographic. Click for perspective.");
            break;
        case Control::ArrowUp:
        case Control::ArrowRight:
        case Control::ArrowDown:
        case Control::ArrowLeft:
            text = tr("Turn the view 45°");
            break;
        case Control::None:
            break;
        }
        QToolTip::showText(help->globalPos(), text, this);
        return true;
    }
    return QWidget::event(event);
}

void NavigationCube::mousePressEvent(QMouseEvent* event) {
    const Control control =
        event->button() == Qt::LeftButton ? controlAt(event->position()) : Control::None;
    if (control == Control::None) {
        // Not for the cube: what lies around it is the view, and so are the other buttons.
        event->ignore();
        return;
    }
    m_pressed = control;
    m_pressedPart = partAt(event->position());
    m_pressPosition = event->position();
    m_orbiting = false;
}

void NavigationCube::mouseMoveEvent(QMouseEvent* event) {
    if (m_pressed != Control::Cube) {
        hover(event->position());
        return;
    }
    if (!m_orbiting) {
        if ((event->position() - m_pressPosition).manhattanLength() <
            QApplication::startDragDistance()) {
            return;
        }
        m_orbiting = true;
        emit orbitStarted(mapToParent(m_pressPosition));
    }
    emit orbitMoved(mapToParent(event->position()));
}

void NavigationCube::mouseReleaseEvent(QMouseEvent* event) {
    if (m_pressed == Control::None) {
        event->ignore();
        return;
    }
    const Control pressed = m_pressed;
    m_pressed = Control::None;
    if (m_orbiting) {
        m_orbiting = false;
        emit orbitEnded(mapToParent(event->position()));
        hover(event->position());
        return;
    }
    if (controlAt(event->position()) != pressed) {
        return;
    }
    switch (pressed) {
    case Control::Cube:
        if (m_pressedPart.isValid()) {
            emit viewChosen(QVector3D(static_cast<float>(m_pressedPart.x),
                                      static_cast<float>(m_pressedPart.y),
                                      static_cast<float>(m_pressedPart.z))
                                .normalized());
        }
        break;
    case Control::Home:
        emit homeChosen();
        break;
    case Control::Projection:
        emit projectionToggled();
        break;
    // Each toward the side it points at: the arrow over the front leads to the top.
    case Control::ArrowUp:
        emit turnAsked(-arrowStep, 0.0);
        break;
    case Control::ArrowDown:
        emit turnAsked(arrowStep, 0.0);
        break;
    case Control::ArrowLeft:
        emit turnAsked(0.0, -arrowStep);
        break;
    case Control::ArrowRight:
        emit turnAsked(0.0, arrowStep);
        break;
    case Control::None:
        break;
    }
}

void NavigationCube::mouseDoubleClickEvent(QMouseEvent* event) {
    const Part part = event->button() == Qt::LeftButton ? partAt(event->position()) : Part{};
    if (!part.isValid()) {
        event->ignore();
        return;
    }
    emit framedViewChosen(QVector3D(static_cast<float>(part.x), static_cast<float>(part.y),
                                    static_cast<float>(part.z))
                              .normalized());
}

void NavigationCube::drawFace(QPainter& painter, int axis, int sign) const {
    const int first = (axis + 1) % 3;
    const int second = (axis + 2) % 3;
    // A piece of the face, from -1 to 1 along its two sides, as it appears.
    const auto piece = [&](double a0, double a1, double b0, double b1) {
        const auto corner = [&](double a, double b) {
            QVector3D at;
            at[axis] = static_cast<float>(sign);
            at[first] = static_cast<float>(a);
            at[second] = static_cast<float>(b);
            return project(at);
        };
        return QPolygonF({corner(a0, b0), corner(a1, b0), corner(a1, b1), corner(a0, b1)});
    };

    // Shaded by which way it faces, so that three sides in view are three shades.
    QVector3D normal;
    normal[axis] = static_cast<float>(sign);
    const QVector3D light = (toward() * 0.75F + m_up * 0.6F - m_right * 0.28F).normalized();
    const float lit = std::clamp(QVector3D::dotProduct(normal, light), 0.0F, 1.0F);
    QColor side = m_colors.side;
    const int shade = 74 + static_cast<int>(lit * 26.0F);
    side = QColor(side.red() * shade / 100, side.green() * shade / 100, side.blue() * shade / 100);
    // A little of the model shows through.
    side.setAlpha(205);

    const QPolygonF whole = softened(piece(-1.0, 1.0, -1.0, 1.0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(side);
    painter.drawPolygon(whole);

    // What the pointer is on, as far as it lies on this face.
    if (m_hovered.isValid() && along(m_hovered, axis) == sign) {
        const auto range = [](int value) {
            return value > 0   ? std::pair(1.0 - band, 1.0)
                   : value < 0 ? std::pair(-1.0, -1.0 + band)
                               : std::pair(-1.0 + band, 1.0 - band);
        };
        const bool isFace = along(m_hovered, first) == 0 && along(m_hovered, second) == 0;
        const auto [a0, a1] = isFace ? std::pair(-1.0, 1.0) : range(along(m_hovered, first));
        const auto [b0, b1] = isFace ? std::pair(-1.0, 1.0) : range(along(m_hovered, second));
        QColor glow = m_colors.highlight;
        glow.setAlpha(isFace ? 95 : 165);
        painter.setBrush(glow);
        painter.drawPolygon(isFace ? whole : piece(a0, a1, b0, b1));
    }

    // Its word, lying on the face, unless the face is seen too nearly edge-on to read.
    const float facing = QVector3D::dotProduct(normal, toward());
    if (facing > 0.22F) {
        const FaceInfo info = faceInfo(axis, sign);
        const QVector3D middle = normal;
        const auto at = [&](float a, float b) {
            return project(middle + info.across * a + info.upward * b);
        };
        const QPolygonF square({{0.0, 0.0}, {100.0, 0.0}, {100.0, 100.0}, {0.0, 100.0}});
        const QPolygonF laid({at(-1.0F, 1.0F), at(1.0F, 1.0F), at(1.0F, -1.0F), at(-1.0F, -1.0F)});
        QTransform transform;
        if (QTransform::quadToQuad(square, laid, transform)) {
            painter.save();
            painter.setTransform(transform, true);
            QFont font = painter.font();
            font.setPixelSize(19);
            font.setBold(true);
            painter.setFont(font);
            painter.setPen(m_colors.text);
            painter.drawText(QRectF(0.0, 0.0, 100.0, 100.0), Qt::AlignCenter, tr(info.name));
            painter.restore();
        }
    }

    // Its outline, thin and dark: edges are lines, and a corner is where lines meet.
    painter.setBrush(Qt::NoBrush);
    QPen outline(m_colors.edge, 1.1);
    outline.setJoinStyle(Qt::RoundJoin);
    painter.setPen(outline);
    painter.drawPolygon(whole);
}

void NavigationCube::drawArrow(QPainter& painter, int which, bool lit) const {
    const QRectF box = arrow(which);
    const QPointF c = box.center();
    const double r = box.width() * 0.3;
    const std::array<QPointF, 4> ways{QPointF(0, -1), QPointF(1, 0), QPointF(0, 1), QPointF(-1, 0)};
    const QPointF way = ways[static_cast<std::size_t>(which)];
    const QPointF side(-way.y(), way.x());
    QColor ink = lit ? m_colors.highlight : m_colors.line;
    ink.setAlpha(lit ? 255 : 150);
    QPen pen(ink, 2.0);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(
        QPolygonF({c - way * r * 0.6 + side * r, c + way * r * 0.7, c - way * r * 0.6 - side * r}));
}

void NavigationCube::drawButtons(QPainter& painter) const {
    const auto backing = [&](const QRectF& box, bool lit) {
        QColor fill = lit ? m_colors.highlight : m_colors.side;
        fill.setAlpha(lit ? 120 : 34);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(box, 3.0, 3.0);
    };
    QPen pen(m_colors.line, 1.3);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);

    const QRectF home = homeButton();
    backing(home, m_control == Control::Home);
    const QPointF c = home.center();
    const double r = home.width() * 0.3;
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(QPolygonF({c + QPointF(-r * 1.15, 0.1 * r), c + QPointF(0.0, -r),
                                    c + QPointF(r * 1.15, 0.1 * r)}));
    painter.drawPolyline(QPolygonF({c + QPointF(-r * 0.7, -0.15 * r), c + QPointF(-r * 0.7, r),
                                    c + QPointF(r * 0.7, r), c + QPointF(r * 0.7, -0.15 * r)}));

    const QRectF projection = projectionButton();
    backing(projection, m_control == Control::Projection);
    const QPointF p = projection.center();
    const double s = projection.width() * 0.3;
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    // A box as it looks: straight sides if orthographic, drawing together if not.
    const double narrow = m_perspective ? 0.55 : 1.0;
    painter.drawPolygon(QPolygonF({p + QPointF(-s, s), p + QPointF(s, s),
                                   p + QPointF(s * narrow, -s), p + QPointF(-s * narrow, -s)}));
}

void NavigationCube::drawAxes(QPainter& painter) const {
    const QPointF at(16.0 * m_scale, height() - 22.0 * m_scale);
    const double reach = 15.0 * m_scale;
    const std::array<QVector3D, 3> axes{QVector3D(1, 0, 0), QVector3D(0, 1, 0), QVector3D(0, 0, 1)};
    const std::array<QString, 3> names{QStringLiteral("X"), QStringLiteral("Y"),
                                       QStringLiteral("Z")};
    // The one that points away first, so that the nearest lies on top.
    std::array<int, 3> order{0, 1, 2};
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return QVector3D::dotProduct(axes[static_cast<std::size_t>(a)], toward()) <
               QVector3D::dotProduct(axes[static_cast<std::size_t>(b)], toward());
    });
    QFont font = painter.font();
    font.setPixelSize(static_cast<int>(9.0 * m_scale));
    font.setBold(true);
    painter.setFont(font);
    for (const int i : order) {
        const auto index = static_cast<std::size_t>(i);
        const QPointF way(QVector3D::dotProduct(axes[index], m_right) * reach,
                          -QVector3D::dotProduct(axes[index], m_up) * reach);
        QPen pen(m_colors.axes[index], 1.8);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.drawLine(QLineF(at, at + way));
        painter.drawText(QRectF(at + way * 1.32 - QPointF(6.0, 6.0), QSizeF(12.0, 12.0)),
                         Qt::AlignCenter, names[index]);
    }
}

void NavigationCube::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const QVector3D look = toward();
    for (int axis = 0; axis < 3; ++axis) {
        for (const int sign : {-1, 1}) {
            if (along(look, axis) * static_cast<float>(sign) > 1e-3F) {
                drawFace(painter, axis, sign);
            }
        }
    }
    drawAxes(painter);
    // Only while they might be wanted: the cube stands alone otherwise.
    if (m_inside) {
        const std::array<Control, 4> arrows{Control::ArrowUp, Control::ArrowRight,
                                            Control::ArrowDown, Control::ArrowLeft};
        for (int i = 0; i < 4; ++i) {
            drawArrow(painter, i, m_control == arrows[static_cast<std::size_t>(i)]);
        }
    }
    drawButtons(painter);
    const QString words = caption();
    if (!words.isEmpty()) {
        QFont font = painter.font();
        font.setPixelSize(static_cast<int>(11.0 * m_scale));
        font.setBold(false);
        painter.setFont(font);
        const QRectF box(0.0, homeButton().top() - 15.0 * m_scale, width(), 13.0 * m_scale);
        painter.setPen(m_colors.line);
        painter.drawText(box, Qt::AlignCenter, words);
    }
}

} // namespace lcad::viewport
