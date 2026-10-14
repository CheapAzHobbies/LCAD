// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/viewport/SketchEditor.h>

#include <lcad/kernel/Plane.h>
#include <lcad/viewport/ViewportWidget.h>

#include <QApplication>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QStyle>

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace lcad::viewport {

namespace {

using sketch::Constraint;
using sketch::ConstraintId;
using sketch::ConstraintKind;
using sketch::Entity;
using sketch::EntityId;
using sketch::EntityKind;
using sketch::Point2;

constexpr double pi = 3.14159265358979323846;

// How near the pointer has to come, in logical pixels.
constexpr double pointReach = 9.0;
constexpr double curveReach = 7.0;
// How far a dimension's line stands off from what it measures, and how large its arrowheads
// and the constraint markers are.
constexpr double dimensionOffset = 38.0;
constexpr double arrowLength = 8.0;
constexpr double markerSize = 14.0;

Point2 operator+(Point2 a, Point2 b) {
    return {a.x + b.x, a.y + b.y};
}
Point2 operator-(Point2 a, Point2 b) {
    return {a.x - b.x, a.y - b.y};
}
Point2 operator*(Point2 a, double k) {
    return {a.x * k, a.y * k};
}
double norm(Point2 a) {
    return std::hypot(a.x, a.y);
}
double apart(Point2 a, Point2 b) {
    return norm(b - a);
}
Point2 unit(Point2 a) {
    const double length = norm(a);
    return length > 1e-12 ? a * (1.0 / length) : Point2{1.0, 0.0};
}
Point2 direction(double angle) {
    return {std::cos(angle), std::sin(angle)};
}
double turn(double angle) {
    angle = std::fmod(angle, 2.0 * pi);
    return angle < 0.0 ? angle + 2.0 * pi : angle;
}
double wrap(double angle) {
    angle = turn(angle);
    return angle > pi ? angle - 2.0 * pi : angle;
}

template <typename T>
bool contains(const std::vector<T>& list, const T& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}
template <typename T>
void toggle(std::vector<T>& list, const T& value) {
    const auto found = std::find(list.begin(), list.end(), value);
    if (found == list.end()) {
        list.push_back(value);
    } else {
        list.erase(found);
    }
}

// A number as it is shown: three decimals at most, without trailing zeros.
QString formatValue(double value) {
    QString text = QString::number(value, 'f', 3);
    while (text.contains(QLatin1Char('.')) &&
           (text.endsWith(QLatin1Char('0')) || text.endsWith(QLatin1Char('.')))) {
        const bool wasPoint = text.endsWith(QLatin1Char('.'));
        text.chop(1);
        if (wasPoint) {
            break;
        }
    }
    return text == QLatin1String("-0") ? QStringLiteral("0") : text;
}

// A number as it is typed: with a point or the locale's decimal mark, and with or without the
// unit after it.
std::optional<double> parseValue(QString text) {
    text = text.trimmed();
    for (const QString& suffix :
         {QStringLiteral("mm"), QStringLiteral("deg"), QStringLiteral("°")}) {
        if (text.endsWith(suffix, Qt::CaseInsensitive)) {
            text.chop(suffix.size());
            text = text.trimmed();
        }
    }
    if (text.isEmpty()) {
        return std::nullopt;
    }
    bool ok = false;
    double value = QLocale::c().toDouble(text, &ok);
    if (!ok) {
        value = QLocale().toDouble(text, &ok);
    }
    if (!ok) {
        value =
            QLocale::c().toDouble(QString(text).replace(QLatin1Char(','), QLatin1Char('.')), &ok);
    }
    if (!ok || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

// Something to draw: a straight piece, or a circle or part of one.
struct Stroke {
    bool round = false;
    Point2 a;
    Point2 b;
    Point2 center;
    double radius = 0.0;
    double from = 0.0;
    double sweep = 2.0 * pi;
};

Stroke segment(Point2 a, Point2 b) {
    Stroke stroke;
    stroke.a = a;
    stroke.b = b;
    return stroke;
}
Stroke circle(Point2 center, double radius) {
    Stroke stroke;
    stroke.round = true;
    stroke.center = center;
    stroke.radius = radius;
    return stroke;
}
// Counter-clockwise from `start` to the direction of `end`.
Stroke arc(Point2 center, Point2 start, Point2 end) {
    Stroke stroke = circle(center, apart(center, start));
    stroke.from = std::atan2(start.y - center.y, start.x - center.x);
    stroke.sweep = turn(std::atan2(end.y - center.y, end.x - center.x) - stroke.from);
    if (stroke.sweep < 1e-12) {
        stroke.sweep = 2.0 * pi;
    }
    return stroke;
}

Stroke strokeOf(const sketch::Sketch& geometry, const Entity& entity) {
    switch (entity.kind) {
    case EntityKind::Line:
        return segment(geometry.position(entity.start), geometry.position(entity.end));
    case EntityKind::Circle:
        return circle(geometry.position(entity.center), entity.radius);
    case EntityKind::Arc:
        return arc(geometry.position(entity.center), geometry.position(entity.start),
                   geometry.position(entity.end));
    case EntityKind::Point:
        break;
    }
    return {};
}

// What a tool's click landed on.
struct Snap {
    enum class Kind { None, Point, Curve, Midpoint };
    Point2 position;
    Kind kind = Kind::None;
    EntityId entity = 0;
    // For the end of a line: level with, or straight above or below, where the line starts.
    bool horizontal = false;
    bool vertical = false;
};

// What is under the pointer.
struct Hit {
    EntityId entity = 0;
    ConstraintId constraint = 0;
};

// How a constraint is drawn: a dimension with its lines and its value, or a small marker.
struct Annotation {
    ConstraintId id = 0;
    ConstraintKind kind = ConstraintKind::Coincident;
    bool dimension = false;
    QRectF rect;
    QString text;
    QPainterPath lines;
    std::vector<QPolygonF> arrows;
};

// The sketch is drawn on a widget of its own that lies over the 3D view and lets the pointer
// through. Qt puts ordinary widgets on top of an OpenGL widget's picture, so this draws with
// the same painter, fonts, and colors as the rest of the interface, and leaves OCCT's OpenGL
// state alone.
class Overlay : public QWidget {
public:
    Overlay(QWidget* parent, SketchEditor* editor) : QWidget(parent), m_editor(editor) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        setObjectName(QStringLiteral("sketchOverlay"));
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        m_editor->paint(painter);
    }

private:
    SketchEditor* m_editor;
};

} // namespace

class SketchEditor::Impl {
public:
    Impl(SketchEditor* editor, ViewportWidget* viewport)
        : q(editor), view(viewport), overlay(new Overlay(viewport, editor)) {
        overlay->hide();
    }

    SketchEditor* q;
    ViewportWidget* view;
    Overlay* overlay;

    static QString tr(const char* text) { return SketchEditor::tr(text); }

    document::Document* document = nullptr;
    document::ObjectId sketchId;
    sketch::SolveResult state;

    SketchTool tool = SketchTool::Select;
    ConstraintKind constraintKind = ConstraintKind::Coincident;
    bool inference = true;
    bool constructionMode = false;
    int polygonSides = 6;

    std::vector<EntityId> selected;
    std::vector<ConstraintId> selectedConstraints;
    EntityId hover = 0;
    ConstraintId hoverConstraint = 0;

    // A tool at work: the places clicked so far, and where the pointer is.
    std::vector<Snap> clicks;
    Snap cursor;
    bool cursorValid = false;
    QPointF lastPosition;
    // Line: the end of the segment before, which the next one starts at, and the start of the
    // first, which closes the outline when it is reached again.
    EntityId chainEnd = 0;
    EntityId chainFirst = 0;
    // Arc: how far round the pointer has gone since the start was clicked.
    double arcSweep = 0.0;
    double arcLastAngle = 0.0;
    // Dimension and Constraint: what has been clicked so far.
    std::vector<EntityId> pending;

    // The Select tool, between press and release.
    bool pressed = false;
    QPointF pressPosition;
    Hit pressHit;
    bool dragging = false;
    bool boxing = false;
    QPointF boxEnd;
    EntityId dragEntity = 0;
    Point2 dragLast;
    std::optional<sketch::Sketch> dragBefore;
    std::unique_ptr<document::Transaction> dragTransaction;

    // Typing a value.
    QLineEdit* field = nullptr;
    ConstraintId editing = 0; // 0 while the value is for the tool at work

    [[nodiscard]] bool active() const { return document != nullptr; }
    [[nodiscard]] const document::SketchItem* item() const {
        return document != nullptr ? document->findSketch(sketchId) : nullptr;
    }
    [[nodiscard]] const sketch::Sketch& geometry() const {
        const document::SketchItem* found = item();
        if (found == nullptr) {
            throw std::logic_error("SketchEditor: no sketch is being edited");
        }
        return found->geometry;
    }

    void update() const { overlay->update(); }

    void commit(sketch::Sketch next, const QString& label) const {
        document->setSketchGeometry(sketchId, std::move(next), label.toStdString());
    }

    void refresh() {
        const sketch::Sketch& g = geometry();
        state = g.state();
        std::erase_if(selected, [&g](EntityId id) { return g.find(id) == nullptr; });
        std::erase_if(selectedConstraints,
                      [&g](ConstraintId id) { return g.findConstraint(id) == nullptr; });
        std::erase_if(pending, [&g](EntityId id) { return g.find(id) == nullptr; });
        if (g.find(hover) == nullptr) {
            hover = 0;
        }
        if (g.findConstraint(hoverConstraint) == nullptr) {
            hoverConstraint = 0;
        }
        if (g.find(chainEnd) == nullptr) {
            chainEnd = 0;
        }
        if (editing != 0 && g.findConstraint(editing) == nullptr) {
            closeField();
        }
        update();
        emit q->changed();
    }

    // -----------------------------------------------------------------------------------------
    // Where things are

    [[nodiscard]] QPointF toWidget(Point2 p) const {
        const document::SketchItem* found = item();
        if (found == nullptr) {
            return {};
        }
        const kernel::Point world = kernel::toWorld(found->plane, p.x, p.y);
        return view->project(world.x, world.y, world.z);
    }

    [[nodiscard]] std::optional<Point2> toSketch(const QPointF& position) const {
        const document::SketchItem* found = item();
        if (found == nullptr) {
            return std::nullopt;
        }
        const auto at = view->positionOnPlane(found->plane, position);
        if (!at.has_value()) {
            return std::nullopt;
        }
        return Point2{at->x(), at->y()};
    }

    // How many millimeters of the sketch a pixel covers around a position in the widget.
    [[nodiscard]] double millimetersPerPixel(const QPointF& position) const {
        const auto here = toSketch(position);
        const auto right = toSketch(position + QPointF(1.0, 0.0));
        const auto below = toSketch(position + QPointF(0.0, 1.0));
        if (!here || !right || !below) {
            return 1.0;
        }
        const double size = std::max(apart(*here, *right), apart(*here, *below));
        return size > 1e-12 && std::isfinite(size) ? size : 1.0;
    }

    [[nodiscard]] double millimetersPerPixel() const {
        return millimetersPerPixel(QPointF(view->width() / 2.0, view->height() / 2.0));
    }

    [[nodiscard]] QPainterPath pathOf(const Stroke& stroke) const {
        QPainterPath path;
        if (!stroke.round) {
            path.moveTo(toWidget(stroke.a));
            path.lineTo(toWidget(stroke.b));
            return path;
        }
        // Enough pieces that the corners between them cannot be seen.
        const double pixels = stroke.radius / millimetersPerPixel();
        const double step =
            2.0 * std::acos(std::clamp(1.0 - 0.2 / std::max(pixels, 0.3), -1.0, 1.0));
        const int pieces =
            std::clamp(static_cast<int>(std::ceil(stroke.sweep / std::max(step, 1e-3))), 8, 720);
        for (int i = 0; i <= pieces; ++i) {
            const double angle = stroke.from + stroke.sweep * i / pieces;
            const QPointF at = toWidget(stroke.center + direction(angle) * stroke.radius);
            if (i == 0) {
                path.moveTo(at);
            } else {
                path.lineTo(at);
            }
        }
        return path;
    }

    // -----------------------------------------------------------------------------------------
    // Constraints as they are drawn

    [[nodiscard]] QFont labelFont() const {
        QFont font = view->font();
        font.setPointSizeF(font.pointSizeF() * 0.92);
        return font;
    }

    // Where the marker of a constraint on an entity hangs.
    [[nodiscard]] Point2 anchorOf(const sketch::Sketch& g, const Entity& entity) const {
        switch (entity.kind) {
        case EntityKind::Point:
            return {entity.x, entity.y};
        case EntityKind::Line:
            return (g.position(entity.start) + g.position(entity.end)) * 0.5;
        case EntityKind::Circle:
            return g.position(entity.center) + direction(0.75 * pi) * entity.radius;
        case EntityKind::Arc: {
            const Stroke stroke = strokeOf(g, entity);
            return stroke.center + direction(stroke.from + stroke.sweep / 2.0) * stroke.radius;
        }
        }
        return {};
    }

    [[nodiscard]] static QPolygonF arrowAt(const QPointF& tip, const QPointF& from) {
        const QPointF along = tip - from;
        const double length = std::hypot(along.x(), along.y());
        if (length < 1e-6) {
            return {};
        }
        const QPointF d = along / length;
        const QPointF n(-d.y(), d.x());
        return QPolygonF({tip, tip - d * arrowLength + n * 3.0, tip - d * arrowLength - n * 3.0});
    }

    [[nodiscard]] std::vector<Annotation> layout() const {
        std::vector<Annotation> annotations;
        if (!active() || item() == nullptr) {
            return annotations;
        }
        const sketch::Sketch& g = geometry();
        const double mm = millimetersPerPixel();
        const QFontMetricsF metrics(labelFont());
        const auto labelRect = [&metrics](const QPointF& center, const QString& text) {
            const QSizeF size(metrics.horizontalAdvance(text) + 12.0, metrics.height() + 4.0);
            return QRectF(center.x() - size.width() / 2.0, center.y() - size.height() / 2.0,
                          size.width(), size.height());
        };
        const auto line = [this](QPainterPath& path, Point2 from, Point2 to) {
            path.moveTo(toWidget(from));
            path.lineTo(toWidget(to));
        };
        // How many markers already hang on an entity, so that the next goes beside them.
        std::map<EntityId, int> hung;

        for (const Constraint& c : g.constraints()) {
            const Entity* first = g.find(c.first);
            const Entity* second = g.find(c.second);
            if (first == nullptr) {
                continue;
            }
            Annotation a;
            a.id = c.id;
            a.kind = c.kind;
            a.dimension = sketch::isDimension(c.kind);
            if (!a.dimension) {
                // Two ends joined is the commonest constraint by far, and a marker at every
                // corner would bury the sketch. Those show when their point is pointed at or
                // selected, which is when one wants to know what holds it.
                if (c.kind == ConstraintKind::Coincident) {
                    const auto concerns = [this](EntityId id) {
                        return id != 0 &&
                               (id == hover || contains(selected, id) || contains(pending, id));
                    };
                    const bool wanted =
                        concerns(c.first) || concerns(c.second) || concerns(first->owner) ||
                        (second != nullptr && concerns(second->owner)) || c.id == hoverConstraint ||
                        contains(selectedConstraints, c.id);
                    if (!wanted) {
                        continue;
                    }
                }
                std::vector<const Entity*> marked{first};
                const bool both =
                    c.kind == ConstraintKind::Parallel || c.kind == ConstraintKind::Perpendicular ||
                    c.kind == ConstraintKind::Equal || c.kind == ConstraintKind::Collinear ||
                    c.kind == ConstraintKind::Tangent || c.kind == ConstraintKind::Concentric ||
                    c.kind == ConstraintKind::Symmetric ||
                    ((c.kind == ConstraintKind::Horizontal || c.kind == ConstraintKind::Vertical) &&
                     second != nullptr);
                if (both && second != nullptr) {
                    marked.push_back(second);
                }
                for (const Entity* entity : marked) {
                    const int place = hung[entity->id]++;
                    const QPointF at = toWidget(anchorOf(g, *entity)) +
                                       QPointF(9.0 + place * (markerSize + 2.0), -markerSize - 6.0);
                    Annotation marker = a;
                    marker.rect = QRectF(at, QSizeF(markerSize, markerSize));
                    annotations.push_back(marker);
                }
                continue;
            }

            const double off = dimensionOffset * mm;
            QPointF labelAt;
            switch (c.kind) {
            case ConstraintKind::Distance:
            case ConstraintKind::DistanceX:
            case ConstraintKind::DistanceY: {
                if (second == nullptr) {
                    continue;
                }
                const Point2 p1 = g.position(c.first);
                const Point2 p2 = g.position(c.second);
                Point2 q1;
                Point2 q2;
                if (c.kind == ConstraintKind::Distance) {
                    // To the right of the way from the first point to the second, which is the
                    // outside of an outline drawn counter-clockwise.
                    const Point2 d = unit(p2 - p1);
                    const Point2 n{d.y, -d.x};
                    q1 = p1 + n * off;
                    q2 = p2 + n * off;
                } else if (c.kind == ConstraintKind::DistanceX) {
                    const double level = std::min(p1.y, p2.y) - off;
                    q1 = {p1.x, level};
                    q2 = {p2.x, level};
                } else {
                    const double level = std::max(p1.x, p2.x) + off;
                    q1 = {level, p1.y};
                    q2 = {level, p2.y};
                }
                line(a.lines, p1, q1 + unit(q1 - p1) * (5.0 * mm));
                line(a.lines, p2, q2 + unit(q2 - p2) * (5.0 * mm));
                line(a.lines, q1, q2);
                a.arrows.push_back(arrowAt(toWidget(q1), toWidget(q2)));
                a.arrows.push_back(arrowAt(toWidget(q2), toWidget(q1)));
                labelAt = (toWidget(q1) + toWidget(q2)) / 2.0;
                a.text = formatValue(c.value);
                break;
            }
            case ConstraintKind::PointLineDistance: {
                if (second == nullptr || second->kind != EntityKind::Line) {
                    continue;
                }
                const Point2 p = g.position(c.first);
                const Point2 foot = sketch::nearestOn(g, c.second, p);
                // The foot on the whole line, which may be beyond the line's ends.
                const Point2 s = g.position(second->start);
                const Point2 d = unit(g.position(second->end) - s);
                const Point2 onLine = s + d * ((p.x - s.x) * d.x + (p.y - s.y) * d.y);
                if (apart(onLine, foot) > 1e-9) {
                    line(a.lines, foot, onLine);
                }
                line(a.lines, p, onLine);
                a.arrows.push_back(arrowAt(toWidget(p), toWidget(onLine)));
                a.arrows.push_back(arrowAt(toWidget(onLine), toWidget(p)));
                labelAt = (toWidget(p) + toWidget(onLine)) / 2.0;
                a.text = formatValue(c.value);
                break;
            }
            case ConstraintKind::Radius:
            case ConstraintKind::Diameter: {
                if (first->kind == EntityKind::Point || first->kind == EntityKind::Line) {
                    continue;
                }
                const Stroke stroke = strokeOf(g, *first);
                const double angle =
                    first->kind == EntityKind::Arc ? stroke.from + stroke.sweep / 2.0 : 0.25 * pi;
                const Point2 out = direction(angle);
                const Point2 rim = stroke.center + out * stroke.radius;
                const Point2 from = c.kind == ConstraintKind::Radius
                                        ? stroke.center
                                        : stroke.center - out * stroke.radius;
                line(a.lines, from, rim + out * (10.0 * mm));
                a.arrows.push_back(arrowAt(toWidget(rim), toWidget(from)));
                if (c.kind == ConstraintKind::Diameter) {
                    a.arrows.push_back(arrowAt(toWidget(from), toWidget(rim)));
                }
                a.text =
                    (c.kind == ConstraintKind::Radius ? QStringLiteral("R") : QStringLiteral("Ø")) +
                    formatValue(c.value);
                const QPointF rimAt = toWidget(rim);
                const QPointF beyond = toWidget(rim + out * (10.0 * mm));
                const QPointF away = beyond - rimAt;
                const double length = std::max(std::hypot(away.x(), away.y()), 1e-6);
                labelAt = beyond + away / length * (metrics.horizontalAdvance(a.text) / 2.0 + 8.0);
                break;
            }
            case ConstraintKind::Angle: {
                if (second == nullptr || first->kind != EntityKind::Line ||
                    second->kind != EntityKind::Line) {
                    continue;
                }
                const Point2 a1 = g.position(first->start);
                const Point2 d1 = g.position(first->end) - a1;
                const Point2 a2 = g.position(second->start);
                const Point2 d2 = g.position(second->end) - a2;
                const double denominator = d1.x * d2.y - d1.y * d2.x;
                Point2 corner = (a1 + a2) * 0.5;
                if (std::abs(denominator) > 1e-12) {
                    const Point2 between = a2 - a1;
                    corner = a1 + d1 * ((between.x * d2.y - between.y * d2.x) / denominator);
                }
                const double start = std::atan2(d1.y, d1.x);
                const double radius = 34.0 * mm;
                const int pieces = 24;
                for (int i = 0; i <= pieces; ++i) {
                    const QPointF at =
                        toWidget(corner + direction(start + c.value * i / pieces) * radius);
                    if (i == 0) {
                        a.lines.moveTo(at);
                    } else {
                        a.lines.lineTo(at);
                    }
                }
                labelAt =
                    toWidget(corner + direction(start + c.value / 2.0) * (radius + 18.0 * mm));
                a.text = formatValue(std::abs(c.value) * 180.0 / pi) + QStringLiteral("°");
                break;
            }
            default:
                continue;
            }
            // A value worked out from parameters is marked so.
            if (const document::SketchItem* sketch = item();
                sketch != nullptr && sketch->expressions.contains(c.id)) {
                a.text = QStringLiteral("ƒ ") + a.text;
            }
            a.rect = labelRect(labelAt, a.text);
            annotations.push_back(a);
        }
        return annotations;
    }

    // -----------------------------------------------------------------------------------------
    // What is under the pointer

    [[nodiscard]] EntityId pointAt(const QPointF& position, double reach,
                                   std::optional<Point2> except = std::nullopt) const {
        const sketch::Sketch& g = geometry();
        EntityId best = 0;
        double nearest = reach;
        for (const Entity& entity : g.entities()) {
            if (entity.kind != EntityKind::Point) {
                continue;
            }
            if (except.has_value() && apart(*except, {entity.x, entity.y}) < 1e-9) {
                continue;
            }
            const QPointF at = toWidget({entity.x, entity.y});
            const double away = std::hypot(at.x() - position.x(), at.y() - position.y());
            if (away <= nearest) {
                nearest = away;
                best = entity.id;
            }
        }
        return best;
    }

    [[nodiscard]] EntityId curveAt(const QPointF& position, double reach) const {
        const auto at = toSketch(position);
        if (!at.has_value()) {
            return 0;
        }
        const sketch::Sketch& g = geometry();
        const double tolerance = reach * millimetersPerPixel(position);
        EntityId best = 0;
        double nearest = tolerance;
        for (const Entity& entity : g.entities()) {
            if (entity.kind == EntityKind::Point) {
                continue;
            }
            const double away = sketch::distanceTo(g, entity.id, *at);
            if (away <= nearest) {
                nearest = away;
                best = entity.id;
            }
        }
        return best;
    }

    [[nodiscard]] Hit hitAt(const QPointF& position, bool entitiesOnly = false) const {
        Hit hit;
        if (!active() || item() == nullptr) {
            return hit;
        }
        if (!entitiesOnly) {
            // Values and markers lie on top of the geometry.
            const std::vector<Annotation> annotations = layout();
            for (auto it = annotations.rbegin(); it != annotations.rend(); ++it) {
                if (it->rect.adjusted(-2.0, -2.0, 2.0, 2.0).contains(position)) {
                    hit.constraint = it->id;
                    return hit;
                }
            }
        }
        hit.entity = pointAt(position, pointReach);
        if (hit.entity == 0) {
            hit.entity = curveAt(position, curveReach);
        }
        return hit;
    }

    [[nodiscard]] Snap snapAt(const QPointF& position, Point2 at,
                              std::optional<Point2> anchor) const {
        Snap snap;
        snap.position = at;
        if (!inference) {
            return snap;
        }
        const sketch::Sketch& g = geometry();
        if (const EntityId point = pointAt(position, pointReach, anchor); point != 0) {
            snap.kind = Snap::Kind::Point;
            snap.entity = point;
            snap.position = g.position(point);
            return snap;
        }
        for (const Entity& entity : g.entities()) {
            if (entity.kind != EntityKind::Line) {
                continue;
            }
            const Point2 middle = (g.position(entity.start) + g.position(entity.end)) * 0.5;
            const QPointF there = toWidget(middle);
            if (std::hypot(there.x() - position.x(), there.y() - position.y()) <= pointReach) {
                snap.kind = Snap::Kind::Midpoint;
                snap.entity = entity.id;
                snap.position = middle;
                return snap;
            }
        }
        if (const EntityId curve = curveAt(position, curveReach); curve != 0) {
            snap.kind = Snap::Kind::Curve;
            snap.entity = curve;
            snap.position = sketch::nearestOn(g, curve, at);
            return snap;
        }
        if (anchor.has_value() && tool == SketchTool::Line) {
            const double reach = pointReach * millimetersPerPixel(position);
            const double dx = std::abs(at.x - anchor->x);
            const double dy = std::abs(at.y - anchor->y);
            if (dy < reach && dx > reach) {
                snap.position.y = anchor->y;
                snap.horizontal = true;
            } else if (dx < reach && dy > reach) {
                snap.position.x = anchor->x;
                snap.vertical = true;
            }
        }
        return snap;
    }

    // Keeps a new point where the click that made it landed.
    void attach(sketch::Sketch& g, EntityId point, const Snap& snap) const {
        if (!inference || snap.entity == 0 || g.find(snap.entity) == nullptr) {
            return;
        }
        switch (snap.kind) {
        case Snap::Kind::Point:
            sketch::tryConstraint(g, ConstraintKind::Coincident, point, snap.entity);
            break;
        case Snap::Kind::Midpoint:
            sketch::tryConstraint(g, ConstraintKind::Midpoint, point, snap.entity);
            break;
        case Snap::Kind::Curve:
            sketch::tryConstraint(g,
                                  g.find(snap.entity)->kind == EntityKind::Line
                                      ? ConstraintKind::PointOnLine
                                      : ConstraintKind::PointOnCircle,
                                  point, snap.entity);
            break;
        case Snap::Kind::None:
            break;
        }
    }

    void finishCurve(sketch::Sketch& g, EntityId curve) const {
        if (constructionMode) {
            g.setConstruction(curve, true);
        }
    }

    // -----------------------------------------------------------------------------------------
    // The drawing tools

    void resetTool() {
        clicks.clear();
        pending.clear();
        chainEnd = 0;
        chainFirst = 0;
        arcSweep = 0.0;
        cursorValid = false;
        hover = 0;
        hoverConstraint = 0;
    }

    // Returns the line's end point, or 0 if no line was added.
    EntityId addSegment(const Snap& from, const Snap& to, std::optional<double> length) {
        sketch::Sketch g = geometry();
        EntityId lineId = 0;
        try {
            lineId = g.addLine(from.position, to.position);
        } catch (const std::invalid_argument&) {
            emit q->message(tr("A line needs two different points."));
            return 0;
        }
        finishCurve(g, lineId);
        const Entity made = *g.find(lineId);
        if (chainEnd != 0 && g.find(chainEnd) != nullptr) {
            sketch::tryConstraint(g, ConstraintKind::Coincident, made.start, chainEnd);
        } else {
            attach(g, made.start, from);
        }
        if (inference && to.horizontal) {
            sketch::tryConstraint(g, ConstraintKind::Horizontal, lineId);
        }
        if (inference && to.vertical) {
            sketch::tryConstraint(g, ConstraintKind::Vertical, lineId);
        }
        attach(g, made.end, to);
        if (length.has_value()) {
            sketch::tryConstraint(g, ConstraintKind::Distance, made.start, made.end, 0, *length);
        }
        const EntityId first = chainFirst != 0 ? chainFirst : made.start;
        const bool closes = to.kind == Snap::Kind::Point && to.entity == chainFirst;
        commit(std::move(g), tr("Line"));
        if (closes) {
            // Back where the outline started: it is finished.
            clicks.clear();
            chainEnd = 0;
            chainFirst = 0;
        } else {
            Snap next;
            next.position = geometry().position(made.end);
            clicks = {next};
            chainEnd = made.end;
            chainFirst = first;
        }
        return made.end;
    }

    void addRectangle(const Snap& corner, const Snap& opposite,
                      std::optional<std::pair<double, double>> size) {
        sketch::Sketch g = geometry();
        sketch::RectangleParts parts;
        try {
            parts = sketch::addRectangle(g, corner.position, opposite.position);
        } catch (const std::invalid_argument&) {
            emit q->message(tr("A rectangle needs a width and a height."));
            return;
        }
        for (const EntityId side : {parts.bottom, parts.right, parts.top, parts.left}) {
            finishCurve(g, side);
        }
        // Each click landed on one of the corners.
        const auto cornerAt = [&g, &parts](Point2 where) {
            EntityId best = 0;
            double nearest = 0.0;
            for (const EntityId side : {parts.bottom, parts.right, parts.top, parts.left}) {
                const EntityId start = g.find(side)->start;
                const double away = apart(g.position(start), where);
                if (best == 0 || away < nearest) {
                    best = start;
                    nearest = away;
                }
            }
            return best;
        };
        attach(g, cornerAt(corner.position), corner);
        attach(g, cornerAt(opposite.position), opposite);
        if (size.has_value()) {
            sketch::tryConstraint(g, ConstraintKind::Distance, g.find(parts.bottom)->start,
                                  g.find(parts.bottom)->end, 0, size->first);
            sketch::tryConstraint(g, ConstraintKind::Distance, g.find(parts.right)->start,
                                  g.find(parts.right)->end, 0, size->second);
        }
        commit(std::move(g), tr("Rectangle"));
        clicks.clear();
    }

    void addCircle(const Snap& center, double radius, const Snap* rim, bool dimensioned) {
        sketch::Sketch g = geometry();
        EntityId made = 0;
        try {
            made = g.addCircle(center.position, radius);
        } catch (const std::invalid_argument&) {
            emit q->message(tr("A circle needs a radius."));
            return;
        }
        finishCurve(g, made);
        attach(g, g.find(made)->center, center);
        if (rim != nullptr && inference && rim->kind == Snap::Kind::Point) {
            sketch::tryConstraint(g, ConstraintKind::PointOnCircle, rim->entity, made);
        }
        if (dimensioned) {
            sketch::tryConstraint(g, ConstraintKind::Diameter, made, 0, 0, 2.0 * radius);
        }
        commit(std::move(g), tr("Circle"));
        clicks.clear();
    }

    void addArc(const Snap& center, const Snap& start, const Snap& end) {
        sketch::Sketch g = geometry();
        const double radius = apart(center.position, start.position);
        const Point2 endAt = center.position + unit(end.position - center.position) * radius;
        // The pointer went round counter-clockwise or clockwise; the arc runs counter-clockwise
        // either way, so going clockwise it starts where the pointer ended.
        const bool clockwise = arcSweep < 0.0;
        EntityId made = 0;
        try {
            made = clockwise ? g.addArc(center.position, endAt, start.position)
                             : g.addArc(center.position, start.position, endAt);
        } catch (const std::invalid_argument&) {
            emit q->message(tr("An arc needs a radius, and a start and an end that differ."));
            return;
        }
        finishCurve(g, made);
        const Entity arc = *g.find(made);
        attach(g, arc.center, center);
        attach(g, clockwise ? arc.end : arc.start, start);
        attach(g, clockwise ? arc.start : arc.end, end);
        commit(std::move(g), tr("Arc"));
        clicks.clear();
    }

    void addPoint(const Snap& at) {
        sketch::Sketch g = geometry();
        const EntityId made = g.addPoint(at.position);
        finishCurve(g, made);
        attach(g, made, at);
        commit(std::move(g), tr("Point"));
    }

    void addPolygon(const Snap& center, const Snap& corner) {
        sketch::Sketch g = geometry();
        sketch::PolygonParts parts;
        try {
            parts = sketch::addPolygon(g, center.position, corner.position, polygonSides);
        } catch (const std::invalid_argument&) {
            emit q->message(tr("A polygon needs a size."));
            return;
        }
        for (const EntityId side : parts.sides) {
            finishCurve(g, side);
        }
        attach(g, g.find(parts.circle)->center, center);
        attach(g, g.find(parts.sides.front())->start, corner);
        commit(std::move(g), tr("Polygon"));
        clicks.clear();
    }

    [[nodiscard]] static double slotRadius(Point2 first, Point2 second, Point2 at) {
        const Point2 d = unit(second - first);
        return std::abs(d.x * (at.y - first.y) - d.y * (at.x - first.x));
    }

    void addSlot(const Snap& first, const Snap& second, Point2 width) {
        sketch::Sketch g = geometry();
        sketch::SlotParts parts;
        try {
            parts = sketch::addSlot(g, first.position, second.position,
                                    slotRadius(first.position, second.position, width));
        } catch (const std::invalid_argument&) {
            emit q->message(tr("A slot needs a length and a width."));
            return;
        }
        for (const EntityId part :
             {parts.firstArc, parts.secondArc, parts.firstLine, parts.secondLine}) {
            finishCurve(g, part);
        }
        attach(g, g.find(parts.firstArc)->center, first);
        attach(g, g.find(parts.secondArc)->center, second);
        commit(std::move(g), tr("Slot"));
        clicks.clear();
    }

    // What the tool at work would add if the pointer were clicked where it is.
    [[nodiscard]] std::vector<Stroke> preview() const {
        std::vector<Stroke> strokes;
        if (cursorValid && tool == SketchTool::Offset) {
            // The copy an offset would make if the pointer were clicked where it is.
            const sketch::Sketch& g = geometry();
            std::vector<EntityId> curves;
            double distance = 0.0;
            for (const EntityId id : selected) {
                const Entity* entity = g.find(id);
                if (entity != nullptr && entity->kind != EntityKind::Point) {
                    const double away = sketch::distanceTo(g, id, cursor.position);
                    distance = curves.empty() ? away : std::min(distance, away);
                    curves.push_back(id);
                }
            }
            if (!curves.empty()) {
                sketch::Sketch copy = g;
                try {
                    for (const EntityId made :
                         sketch::offset(copy, curves, distance, cursor.position)) {
                        strokes.push_back(strokeOf(copy, *copy.find(made)));
                    }
                } catch (const std::invalid_argument&) {
                    strokes.clear();
                }
            }
            return strokes;
        }
        if (!cursorValid || clicks.empty()) {
            return strokes;
        }
        const Point2 at = cursor.position;
        const Point2 first = clicks.front().position;
        switch (tool) {
        case SketchTool::Line:
            strokes.push_back(segment(clicks.back().position, at));
            break;
        case SketchTool::Rectangle:
            strokes.push_back(segment(first, {at.x, first.y}));
            strokes.push_back(segment({at.x, first.y}, at));
            strokes.push_back(segment(at, {first.x, at.y}));
            strokes.push_back(segment({first.x, at.y}, first));
            break;
        case SketchTool::Circle:
            if (apart(first, at) > 1e-9) {
                strokes.push_back(circle(first, apart(first, at)));
            }
            break;
        case SketchTool::Arc:
            if (clicks.size() == 1) {
                strokes.push_back(segment(first, at));
            } else {
                const Point2 start = clicks[1].position;
                strokes.push_back(arcSweep < 0.0 ? arc(first, at, start) : arc(first, start, at));
                strokes.push_back(segment(first, start));
            }
            break;
        case SketchTool::Polygon: {
            const double radius = apart(first, at);
            const double start = std::atan2(at.y - first.y, at.x - first.x);
            for (int i = 0; radius > 1e-9 && i < polygonSides; ++i) {
                strokes.push_back(
                    segment(first + direction(start + 2.0 * pi * i / polygonSides) * radius,
                            first + direction(start + 2.0 * pi * (i + 1) / polygonSides) * radius));
            }
            break;
        }
        case SketchTool::Slot:
            if (clicks.size() == 1) {
                strokes.push_back(segment(first, at));
            } else {
                const Point2 second = clicks[1].position;
                const double radius = slotRadius(first, second, at);
                if (radius > 1e-9 && apart(first, second) > 1e-9) {
                    const Point2 along = unit(second - first);
                    const Point2 up = Point2{-along.y, along.x} * radius;
                    strokes.push_back(arc(first, first + up, first - up));
                    strokes.push_back(segment(first - up, second - up));
                    strokes.push_back(arc(second, second - up, second + up));
                    strokes.push_back(segment(second + up, first + up));
                }
            }
            break;
        default:
            break;
        }
        return strokes;
    }

    // What the tool at work measures at the moment, to show beside the pointer.
    [[nodiscard]] QString readout() const {
        if (!cursorValid || clicks.empty()) {
            return {};
        }
        const Point2 at = cursor.position;
        const Point2 first = clicks.front().position;
        switch (tool) {
        case SketchTool::Line: {
            const Point2 d = at - clicks.back().position;
            return tr("%1 mm  %2°")
                .arg(formatValue(norm(d)), formatValue(turn(std::atan2(d.y, d.x)) * 180.0 / pi));
        }
        case SketchTool::Rectangle:
            return tr("%1 × %2 mm")
                .arg(formatValue(std::abs(at.x - first.x)), formatValue(std::abs(at.y - first.y)));
        case SketchTool::Circle:
            return tr("Ø%1 mm").arg(formatValue(2.0 * apart(first, at)));
        case SketchTool::Arc:
            return clicks.size() == 1 ? tr("R%1 mm").arg(formatValue(apart(first, at)))
                                      : tr("%1°").arg(formatValue(std::abs(arcSweep) * 180.0 / pi));
        case SketchTool::Polygon:
            return tr("R%1 mm").arg(formatValue(apart(first, at)));
        case SketchTool::Slot:
            return clicks.size() == 1
                       ? tr("%1 mm").arg(formatValue(apart(first, at)))
                       : tr("width %1 mm")
                             .arg(formatValue(2.0 * slotRadius(first, clicks[1].position, at)));
        default:
            return {};
        }
    }

    void moveCursor(const QPointF& position) {
        cursorValid = false;
        const auto at = toSketch(position);
        if (!at.has_value()) {
            return;
        }
        const bool draws = tool == SketchTool::Line || tool == SketchTool::Rectangle ||
                           tool == SketchTool::Circle || tool == SketchTool::Arc ||
                           tool == SketchTool::Point || tool == SketchTool::Polygon ||
                           tool == SketchTool::Slot;
        if (draws) {
            const std::optional<Point2> anchor =
                clicks.empty() ? std::nullopt : std::optional<Point2>(clicks.back().position);
            cursor = snapAt(position, *at, anchor);
            hover = 0;
            if (tool == SketchTool::Arc && clicks.size() == 2) {
                const Point2 center = clicks.front().position;
                const double angle =
                    std::atan2(cursor.position.y - center.y, cursor.position.x - center.x);
                arcSweep = std::clamp(arcSweep + wrap(angle - arcLastAngle), -2.0 * pi + 0.01,
                                      2.0 * pi - 0.01);
                arcLastAngle = angle;
            }
        } else {
            cursor = Snap();
            cursor.position = *at;
            // Dimensions and constraints are about points as much as curves; the tools that
            // change curves are not.
            const bool takesPoints =
                tool == SketchTool::Dimension || tool == SketchTool::Constraint;
            hover = takesPoints ? hitAt(position, true).entity : curveAt(position, curveReach);
        }
        cursorValid = true;
    }

    void toolClick(const QPointF& position) {
        moveCursor(position);
        if (!cursorValid) {
            emit q->message(tr("The sketch is seen edge-on here. Turn the view to face it."));
            return;
        }
        const sketch::Sketch& g = geometry();
        switch (tool) {
        case SketchTool::Select:
            break;
        case SketchTool::Line:
            if (clicks.empty()) {
                clicks.push_back(cursor);
            } else {
                addSegment(clicks.back(), cursor, std::nullopt);
            }
            break;
        case SketchTool::Rectangle:
            if (clicks.empty()) {
                clicks.push_back(cursor);
            } else {
                addRectangle(clicks.front(), cursor, std::nullopt);
            }
            break;
        case SketchTool::Circle:
            if (clicks.empty()) {
                clicks.push_back(cursor);
            } else {
                addCircle(clicks.front(), apart(clicks.front().position, cursor.position), &cursor,
                          false);
            }
            break;
        case SketchTool::Arc:
            if (clicks.size() < 2) {
                clicks.push_back(cursor);
                if (clicks.size() == 2) {
                    const Point2 center = clicks.front().position;
                    arcSweep = 0.0;
                    arcLastAngle =
                        std::atan2(cursor.position.y - center.y, cursor.position.x - center.x);
                }
            } else {
                addArc(clicks[0], clicks[1], cursor);
            }
            break;
        case SketchTool::Point:
            addPoint(cursor);
            break;
        case SketchTool::Polygon:
            if (clicks.empty()) {
                clicks.push_back(cursor);
            } else {
                addPolygon(clicks.front(), cursor);
            }
            break;
        case SketchTool::Slot:
            if (clicks.size() < 2) {
                clicks.push_back(cursor);
            } else {
                addSlot(clicks[0], clicks[1], cursor.position);
            }
            break;
        case SketchTool::Dimension: {
            const EntityId under = hover;
            if (under != 0 && !contains(pending, under)) {
                pending.push_back(under);
                const Entity* entity = g.find(under);
                const bool complete =
                    pending.size() == 2 ||
                    (pending.size() == 1 && entity != nullptr &&
                     (entity->kind == EntityKind::Circle || entity->kind == EntityKind::Arc));
                if (complete) {
                    placeDimension();
                }
            } else if (!pending.empty()) {
                placeDimension(cursor.position);
            } else {
                emit q->message(tr("Click a line, a circle, an arc, or a point to measure."));
            }
            break;
        }
        case SketchTool::Constraint: {
            if (hover == 0) {
                pending.clear();
                break;
            }
            if (!contains(pending, hover)) {
                pending.push_back(hover);
            }
            if (const auto choice = sketch::constraintFor(g, constraintKind, pending)) {
                applyChoices({*choice}, nameOf(constraintKind));
                pending.clear();
            } else if (pending.size() >= (constraintKind == ConstraintKind::Symmetric ? 3U : 2U)) {
                emit q->message(needsOf(constraintKind));
                pending.clear();
            }
            break;
        }
        case SketchTool::Trim:
        case SketchTool::Extend: {
            if (hover == 0 || g.find(hover)->kind == EntityKind::Point) {
                emit q->message(tool == SketchTool::Trim
                                    ? tr("Click the part of a curve to remove.")
                                    : tr("Click near the end to lengthen."));
                break;
            }
            sketch::Sketch next = g;
            const bool done = tool == SketchTool::Trim
                                  ? sketch::trim(next, hover, cursor.position)
                                  : sketch::extend(next, hover, cursor.position);
            if (done) {
                hover = 0;
                commit(std::move(next), tool == SketchTool::Trim ? tr("Trim") : tr("Extend"));
            } else {
                emit q->message(tool == SketchTool::Trim
                                    ? tr("Nothing crosses that curve, so there is nothing to trim.")
                                    : tr("That end meets no other curve if it is lengthened."));
            }
            break;
        }
        case SketchTool::Offset: {
            std::vector<EntityId> curves;
            for (const EntityId id : selected) {
                const Entity* entity = g.find(id);
                if (entity != nullptr && entity->kind != EntityKind::Point) {
                    curves.push_back(id);
                }
            }
            if (curves.empty()) {
                if (hover != 0 && g.find(hover)->kind != EntityKind::Point) {
                    selected = outlineOf(hover);
                    emit q->changed();
                } else {
                    emit q->message(
                        tr("Click the curve to offset, then where the copy should go."));
                }
                break;
            }
            double distance = 0.0;
            for (std::size_t i = 0; i < curves.size(); ++i) {
                const double away = sketch::distanceTo(g, curves[i], cursor.position);
                distance = i == 0 ? away : std::min(distance, away);
            }
            sketch::Sketch next = g;
            try {
                sketch::offset(next, curves, distance, cursor.position);
            } catch (const std::invalid_argument&) {
                emit q->message(tr("An offset that far leaves nothing of the curve."));
                break;
            }
            selected.clear();
            commit(std::move(next), tr("Offset"));
            break;
        }
        case SketchTool::Mirror:
            if (hover == 0 || g.find(hover)->kind != EntityKind::Line) {
                emit q->message(tr("Click the line to mirror in."));
            } else {
                q->mirrorSelection(hover);
            }
            break;
        case SketchTool::Project:
            // The viewport knows what of a body is under the pointer, and hands it over.
            break;
        }
        update();
        emit q->changed();
    }

    // The curves joined end to end with a curve, itself included.
    [[nodiscard]] std::vector<EntityId> outlineOf(EntityId curve) const {
        const sketch::Sketch& g = geometry();
        std::vector<EntityId> outline{curve};
        const auto ends = [&g](const Entity& entity) {
            std::vector<Point2> at;
            if (entity.kind == EntityKind::Line || entity.kind == EntityKind::Arc) {
                at.push_back(g.position(entity.start));
                at.push_back(g.position(entity.end));
            }
            return at;
        };
        for (std::size_t next = 0; next < outline.size(); ++next) {
            const std::vector<Point2> mine = ends(*g.find(outline[next]));
            for (const Entity& other : g.entities()) {
                if (other.kind == EntityKind::Point || contains(outline, other.id)) {
                    continue;
                }
                for (const Point2 theirs : ends(other)) {
                    if (std::any_of(mine.begin(), mine.end(),
                                    [theirs](Point2 p) { return apart(p, theirs) <= 1e-6; })) {
                        outline.push_back(other.id);
                        break;
                    }
                }
            }
        }
        return outline;
    }

    // -----------------------------------------------------------------------------------------
    // Constraints and dimensions

    [[nodiscard]] static QString nameOf(ConstraintKind kind) {
        switch (kind) {
        case ConstraintKind::Coincident:
        case ConstraintKind::PointOnLine:
        case ConstraintKind::PointOnCircle:
            return tr("Coincident");
        case ConstraintKind::Horizontal:
            return tr("Horizontal");
        case ConstraintKind::Vertical:
            return tr("Vertical");
        case ConstraintKind::Parallel:
            return tr("Parallel");
        case ConstraintKind::Perpendicular:
            return tr("Perpendicular");
        case ConstraintKind::Tangent:
            return tr("Tangent");
        case ConstraintKind::Equal:
            return tr("Equal");
        case ConstraintKind::Concentric:
            return tr("Concentric");
        case ConstraintKind::Midpoint:
            return tr("Midpoint");
        case ConstraintKind::Symmetric:
            return tr("Symmetric");
        case ConstraintKind::Collinear:
            return tr("Collinear");
        case ConstraintKind::Fix:
            return tr("Fix");
        default:
            return tr("Dimension");
        }
    }

    [[nodiscard]] static QString needsOf(ConstraintKind kind) {
        switch (kind) {
        case ConstraintKind::Coincident:
        case ConstraintKind::PointOnLine:
        case ConstraintKind::PointOnCircle:
            return tr("Coincident takes two points, or a point and the curve it should lie on.");
        case ConstraintKind::Horizontal:
            return tr("Horizontal takes a line, or two points.");
        case ConstraintKind::Vertical:
            return tr("Vertical takes a line, or two points.");
        case ConstraintKind::Parallel:
            return tr("Parallel takes two lines.");
        case ConstraintKind::Perpendicular:
            return tr("Perpendicular takes two lines.");
        case ConstraintKind::Collinear:
            return tr("Collinear takes two lines.");
        case ConstraintKind::Tangent:
            return tr("Tangent takes a line and a circle or arc, or two circles or arcs.");
        case ConstraintKind::Equal:
            return tr("Equal takes two lines, or two circles or arcs.");
        case ConstraintKind::Concentric:
            return tr("Concentric takes two circles or arcs.");
        case ConstraintKind::Midpoint:
            return tr("Midpoint takes a point and a line.");
        case ConstraintKind::Symmetric:
            return tr("Symmetric takes two points and the line between them.");
        case ConstraintKind::Fix:
            return tr("Fix takes a point.");
        default:
            return tr("A dimension takes a line, a circle, an arc, two points, a point and a "
                      "line, or two lines.");
        }
    }

    [[nodiscard]] static QString refusalText(sketch::Refusal refusal, const QString& name) {
        switch (refusal) {
        case sketch::Refusal::Redundant:
            return tr("%1 was not added: the sketch already holds that, and adding it again "
                      "would over-constrain the sketch.")
                .arg(name);
        case sketch::Refusal::Conflicting:
            return tr("%1 was not added: it contradicts the constraints that are already there.")
                .arg(name);
        case sketch::Refusal::Invalid:
            return tr("%1 does not apply to that.").arg(name);
        case sketch::Refusal::None:
            break;
        }
        return {};
    }

    // Adds the constraints that can be added, as one undo step, and says why about the rest.
    bool applyChoices(const std::vector<sketch::ConstraintChoice>& choices, const QString& name) {
        sketch::Sketch g = geometry();
        int added = 0;
        sketch::Refusal refused = sketch::Refusal::None;
        for (const sketch::ConstraintChoice& choice : choices) {
            const sketch::Outcome outcome = sketch::tryConstraint(
                g, choice.kind, choice.first, choice.second, choice.third, choice.value);
            if (outcome.ok()) {
                ++added;
            } else {
                refused = outcome.refusal;
            }
        }
        if (refused != sketch::Refusal::None) {
            emit q->message(refusalText(refused, name));
        }
        if (added == 0) {
            return false;
        }
        commit(std::move(g), name);
        return true;
    }

    void placeDimension(std::optional<Point2> placedAt = std::nullopt) {
        const std::vector<EntityId> measured = pending;
        pending.clear();
        addDimensionFor(measured, placedAt);
    }

    // `placedAt` is where the value was put, if it was put anywhere. Between the two points
    // sideways, and above or below both, it asks for their horizontal distance; beside them,
    // for the vertical one; anywhere else, for the distance straight across.
    bool addDimensionFor(const std::vector<EntityId>& entities,
                         std::optional<Point2> placedAt = std::nullopt) {
        sketch::Sketch g = geometry();
        auto choice = sketch::dimensionFor(g, entities);
        if (!choice.has_value()) {
            emit q->message(needsOf(ConstraintKind::Distance));
            return false;
        }
        if (choice->kind == ConstraintKind::Distance && placedAt.has_value()) {
            const Point2 a = g.position(choice->first);
            const Point2 b = g.position(choice->second);
            const double least = 1e-6;
            if (std::abs(a.x - b.x) > least && std::abs(a.y - b.y) > least) {
                const bool betweenX =
                    placedAt->x > std::min(a.x, b.x) && placedAt->x < std::max(a.x, b.x);
                const bool betweenY =
                    placedAt->y > std::min(a.y, b.y) && placedAt->y < std::max(a.y, b.y);
                if (betweenX && !betweenY) {
                    choice->kind = ConstraintKind::DistanceX;
                    choice->value = std::abs(a.x - b.x);
                } else if (betweenY && !betweenX) {
                    choice->kind = ConstraintKind::DistanceY;
                    choice->value = std::abs(a.y - b.y);
                }
            }
        }
        const sketch::Outcome outcome = sketch::tryConstraint(
            g, choice->kind, choice->first, choice->second, choice->third, choice->value);
        if (!outcome.ok()) {
            emit q->message(outcome.refusal == sketch::Refusal::Redundant
                                ? tr("That is already fixed by the other dimensions and "
                                     "constraints. A dimension there would over-constrain the "
                                     "sketch.")
                                : refusalText(outcome.refusal, tr("The dimension")));
            return false;
        }
        commit(std::move(g), tr("Dimension"));
        selected.clear();
        selectedConstraints = {outcome.constraint};
        q->editDimension(outcome.constraint);
        return true;
    }

    // -----------------------------------------------------------------------------------------
    // Typing a value

    void openField(const QRectF& where, const QString& text, bool selectAll) {
        closeField();
        field = new QLineEdit(view);
        field->setObjectName(QStringLiteral("sketchValue"));
        field->setAlignment(Qt::AlignCenter);
        field->setText(text);
        const int width = std::max(96, static_cast<int>(where.width()) + 40);
        const int height = field->sizeHint().height();
        QRect place(static_cast<int>(where.center().x()) - width / 2,
                    static_cast<int>(where.center().y()) - height / 2, width, height);
        // Wholly inside the view.
        place.moveLeft(std::clamp(place.left(), 2, std::max(2, view->width() - width - 2)));
        place.moveTop(std::clamp(place.top(), 2, std::max(2, view->height() - height - 2)));
        field->setGeometry(place);
        field->show();
        field->setFocus();
        if (selectAll) {
            field->selectAll();
        } else {
            field->end(false);
        }
        QObject::connect(field, &QLineEdit::returnPressed, q, [this] { acceptField(); });
        QObject::connect(field, &QLineEdit::textEdited, q, [this] { markField(QString()); });
        // Clicking elsewhere gives the value up, as Esc does: see eventFilter().
        field->installEventFilter(q);
    }

    void closeField() {
        if (field == nullptr) {
            return;
        }

        QLineEdit* closing = field;
        field = nullptr;
        editing = 0;
        closing->hide();
        closing->deleteLater();
        view->setFocus();
        update();
    }

    // Says what is wrong with what was typed, and leaves it there to be corrected.
    void markField(const QString& problem, bool announce = true) {
        if (field == nullptr) {
            return;
        }
        field->setProperty("invalid", !problem.isEmpty());
        field->setToolTip(problem);
        field->style()->unpolish(field);
        field->style()->polish(field);
        if (announce && !problem.isEmpty()) {
            emit q->message(problem);
        }
    }

    void acceptField() {
        if (field == nullptr) {
            return;
        }
        const QString text = field->text();
        if (editing != 0) {
            auto value = parseValue(text);
            // Not a number: an expression of parameters, such as "width / 2".
            QString expression;
            if (!value.has_value() && document != nullptr) {
                try {
                    value = document->evaluate(text.trimmed().toStdString());
                    expression = text.trimmed();
                } catch (const std::invalid_argument& error) {
                    markField(tr("\"%1\" is neither a number nor an expression that can be "
                                 "worked out: %2")
                                  .arg(text, QString::fromUtf8(error.what())));
                    return;
                }
            }
            if (!value.has_value()) {
                markField(tr("\"%1\" is not a number.").arg(text));
                return;
            }
            const ConstraintId dimension = editing;
            // One undo step: the value, and whether it follows an expression.
            document::Transaction transaction(*document, tr("Change Dimension").toStdString());
            // The field stays open, with what was typed, if the value is refused.
            if (q->setDimensionValue(dimension, *value)) {
                document->setDimensionExpression(sketchId, dimension, expression.toStdString());
                transaction.commit();
                closeField();
            } else {
                // setDimensionValue() has said why.
                markField(lastProblem, false);
            }
            return;
        }
        acceptToolValue(text);
    }

    QString lastProblem;

    void acceptToolValue(const QString& text) {
        if (clicks.empty()) {
            closeField();
            return;
        }
        const Snap first = clicks.front();
        if (tool == SketchTool::Rectangle) {
            const QStringList parts =
                text.split(QRegularExpression(QStringLiteral("[\\sx×;]+")), Qt::SkipEmptyParts);
            const auto width = parts.size() == 2 ? parseValue(parts[0]) : std::nullopt;
            const auto height = parts.size() == 2 ? parseValue(parts[1]) : std::nullopt;
            if (!width || !height || *width <= 0.0 || *height <= 0.0) {
                markField(tr("Type the width and the height, for example \"40 25\"."));
                return;
            }
            const Point2 toward = cursorValid ? cursor.position : first.position + Point2{1.0, 1.0};
            Snap opposite;
            opposite.position = {
                first.position.x + (toward.x < first.position.x ? -*width : *width),
                first.position.y + (toward.y < first.position.y ? -*height : *height)};
            closeField();
            addRectangle(first, opposite, std::pair(*width, *height));
            return;
        }
        const auto value = parseValue(text);
        if (!value.has_value() || *value <= 0.0) {
            markField(tr("\"%1\" is not a length.").arg(text));
            return;
        }
        if (tool == SketchTool::Line) {
            const Snap from = clicks.back();
            Snap to = cursorValid ? cursor : Snap();
            const Point2 toward = cursorValid && apart(cursor.position, from.position) > 1e-9
                                      ? unit(cursor.position - from.position)
                                      : Point2{1.0, 0.0};
            to.position = from.position + toward * *value;
            to.kind = Snap::Kind::None;
            to.entity = 0;
            closeField();
            addSegment(from, to, *value);
        } else if (tool == SketchTool::Circle) {
            closeField();
            addCircle(first, *value / 2.0, nullptr, true);
        } else {
            closeField();
        }
        update();
        emit q->changed();
    }

    // -----------------------------------------------------------------------------------------
    // The Select tool

    void startDrag() {
        const auto at = toSketch(pressPosition);
        if (!at.has_value()) {
            return;
        }
        dragEntity = pressHit.entity;
        dragLast = *at;
        dragBefore = geometry();
        dragTransaction =
            std::make_unique<document::Transaction>(*document, tr("Drag").toStdString());
        dragging = true;
    }

    void dragTo(const QPointF& position) {
        const auto at = toSketch(position);
        if (!at.has_value()) {
            return;
        }
        sketch::Sketch g = geometry();
        const Entity* entity = g.find(dragEntity);
        if (entity == nullptr) {
            return;
        }
        if (entity->kind == EntityKind::Circle) {
            g.dragRadius(dragEntity, *at);
        } else if (entity->kind == EntityKind::Point) {
            // Toward the pointer from wherever the point has got to, so that a point held back
            // by a constraint catches up when the pointer comes back.
            g.dragBy(dragEntity, at->x - entity->x, at->y - entity->y);
        } else {
            g.dragBy(dragEntity, at->x - dragLast.x, at->y - dragLast.y);
        }
        dragLast = *at;
        document->setSketchGeometry(sketchId, std::move(g), tr("Drag").toStdString());
    }

    void finishDrag(bool keep) {
        dragging = false;
        if (dragTransaction == nullptr) {
            return;
        }
        bool moved = false;
        if (keep && dragBefore.has_value()) {
            const sketch::Sketch& now = geometry();
            const auto& before = dragBefore->entities();
            const auto& after = now.entities();
            moved = before.size() != after.size();
            // By more than the solver's own rounding.
            const double least = 1e-7;
            for (std::size_t i = 0; !moved && i < before.size(); ++i) {
                moved = std::abs(before[i].x - after[i].x) > least ||
                        std::abs(before[i].y - after[i].y) > least ||
                        std::abs(before[i].radius - after[i].radius) > least;
            }
        }
        if (moved) {
            dragTransaction->commit();
        }
        // Without commit(), this puts the sketch back and records nothing.
        dragTransaction.reset();
        dragBefore.reset();
    }

    void selectInBox(const QRectF& box, bool add) {
        const sketch::Sketch& g = geometry();
        if (!add) {
            selected.clear();
            selectedConstraints.clear();
        }
        for (const Entity& entity : g.entities()) {
            if (entity.id == g.origin() || entity.owner != 0) {
                continue;
            }
            bool inside = true;
            if (entity.kind == EntityKind::Point) {
                inside = box.contains(toWidget({entity.x, entity.y}));
            } else {
                // By its corners: a level line's box has no height, and is in no rectangle.
                const QRectF bounds = pathOf(strokeOf(g, entity)).boundingRect();
                inside = box.contains(bounds.topLeft()) && box.contains(bounds.bottomRight());
            }
            if (inside && !contains(selected, entity.id)) {
                selected.push_back(entity.id);
            }
        }
    }

    // -----------------------------------------------------------------------------------------
    // Drawing

    static void drawMarker(QPainter& painter, const QRectF& rect, ConstraintKind kind) {
        const QPointF c = rect.center();
        const double r = rect.width() / 2.0 - 3.0;
        const auto stroke = [&painter](const QPointF& a, const QPointF& b) {
            painter.drawLine(QLineF(a, b));
        };
        switch (kind) {
        case ConstraintKind::Horizontal:
            stroke(c + QPointF(-r, 0), c + QPointF(r, 0));
            break;
        case ConstraintKind::Vertical:
            stroke(c + QPointF(0, -r), c + QPointF(0, r));
            break;
        case ConstraintKind::Parallel:
            stroke(c + QPointF(-r, r), c + QPointF(-r * 0.2, -r));
            stroke(c + QPointF(r * 0.2, r), c + QPointF(r, -r));
            break;
        case ConstraintKind::Perpendicular:
            stroke(c + QPointF(-r, r), c + QPointF(r, r));
            stroke(c + QPointF(0, r), c + QPointF(0, -r));
            break;
        case ConstraintKind::Tangent:
            painter.drawEllipse(c + QPointF(0, r * 0.35), r * 0.65, r * 0.65);
            stroke(c + QPointF(-r, -r * 0.3), c + QPointF(r, -r * 0.3));
            break;
        case ConstraintKind::Equal:
            stroke(c + QPointF(-r, -r * 0.4), c + QPointF(r, -r * 0.4));
            stroke(c + QPointF(-r, r * 0.4), c + QPointF(r, r * 0.4));
            break;
        case ConstraintKind::Concentric:
            painter.drawEllipse(c, r, r);
            painter.drawEllipse(c, r * 0.4, r * 0.4);
            break;
        case ConstraintKind::Midpoint:
            stroke(c + QPointF(-r, r * 0.6), c + QPointF(r, r * 0.6));
            painter.drawPolygon(QPolygonF({c + QPointF(0, r * 0.6), c + QPointF(-r * 0.6, -r * 0.7),
                                           c + QPointF(r * 0.6, -r * 0.7)}));
            break;
        case ConstraintKind::Symmetric:
            stroke(c + QPointF(0, -r), c + QPointF(0, r));
            stroke(c + QPointF(-r, 0), c + QPointF(-r * 0.35, 0));
            stroke(c + QPointF(r * 0.35, 0), c + QPointF(r, 0));
            break;
        case ConstraintKind::Collinear:
            stroke(c + QPointF(-r, r), c + QPointF(-r * 0.25, r * 0.25));
            stroke(c + QPointF(r * 0.25, -r * 0.25), c + QPointF(r, -r));
            break;
        case ConstraintKind::Fix:
            stroke(c + QPointF(-r, -r), c + QPointF(r, r));
            stroke(c + QPointF(-r, r), c + QPointF(r, -r));
            painter.drawRect(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r));
            break;
        default:
            // Coincident, and a point held on a curve: a ring, as around the point itself.
            painter.drawEllipse(c, r * 0.75, r * 0.75);
            break;
        }
    }

    void paint(QPainter& painter) const {
        if (!active() || item() == nullptr) {
            return;
        }
        const sketch::Sketch& g = geometry();
        const ViewportColors colors = view->colors();
        painter.setRenderHint(QPainter::Antialiasing);

        // What a constraint under the pointer, or a selected one, is about.
        std::vector<EntityId> involved;
        for (const Constraint& c : g.constraints()) {
            if (c.id == hoverConstraint || contains(selectedConstraints, c.id)) {
                involved.insert(involved.end(), {c.first, c.second, c.third});
            }
        }
        const auto colorOf = [&](const Entity& entity) {
            const EntityId top = entity.owner != 0 && entity.kind == EntityKind::Point &&
                                         !contains(selected, entity.id) && entity.id != hover &&
                                         !contains(pending, entity.id) &&
                                         !contains(involved, entity.id)
                                     ? entity.owner
                                     : entity.id;
            if (contains(selected, top) || contains(pending, top)) {
                return colors.selection;
            }
            if (top == hover || contains(involved, top)) {
                return colors.hover;
            }
            const Entity* owner = g.find(top);
            if (entity.fixed || (owner != nullptr && owner->fixed)) {
                return colors.sketchReference;
            }
            return contains(state.underconstrained, top) ? colors.sketchFree
                                                         : colors.sketchConstrained;
        };

        // Closed outlines are tinted, which is how a profile that will make a solid is told
        // from an outline that only looks closed.
        QColor tint = colors.sketchFree;
        tint.setAlpha(34);
        for (const sketch::Profile& profile : g.profiles()) {
            QPainterPath outline;
            std::optional<QPointF> last;
            for (std::size_t i = 0; i < profile.curves.size(); ++i) {
                const Entity* entity = g.find(profile.curves[i]);
                if (entity == nullptr) {
                    continue;
                }
                QPolygonF points = pathOf(strokeOf(g, *entity)).toSubpathPolygons().value(0);
                if (points.isEmpty()) {
                    continue;
                }
                if (!last.has_value() && profile.curves.size() > 1) {
                    // Start the first curve at the end the second does not share.
                    const Entity* next = g.find(profile.curves[1]);
                    const QPolygonF following =
                        next != nullptr ? pathOf(strokeOf(g, *next)).toSubpathPolygons().value(0)
                                        : QPolygonF();
                    const auto near = [&following](const QPointF& p) {
                        return !following.isEmpty() &&
                               (QLineF(p, following.first()).length() < 0.5 ||
                                QLineF(p, following.last()).length() < 0.5);
                    };
                    if (near(points.first()) && !near(points.last())) {
                        std::reverse(points.begin(), points.end());
                    }
                } else if (last.has_value() && QLineF(*last, points.last()).length() <
                                                   QLineF(*last, points.first()).length()) {
                    std::reverse(points.begin(), points.end());
                }
                if (!last.has_value()) {
                    outline.moveTo(points.first());
                }
                for (const QPointF& point : points) {
                    outline.lineTo(point);
                }
                last = points.last();
            }
            outline.closeSubpath();
            painter.fillPath(outline, tint);
        }

        // Curves.
        for (const Entity& entity : g.entities()) {
            if (entity.kind == EntityKind::Point) {
                continue;
            }
            const bool strong = contains(selected, entity.id) || entity.id == hover ||
                                contains(pending, entity.id) || contains(involved, entity.id);
            QPen pen(colorOf(entity), strong ? 2.6 : 1.7);
            if (entity.construction) {
                pen.setStyle(Qt::CustomDashLine);
                pen.setDashPattern({6.0, 4.0});
            }
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(pathOf(strokeOf(g, entity)));
        }

        // Points.
        for (const Entity& entity : g.entities()) {
            if (entity.kind != EntityKind::Point) {
                continue;
            }
            const QPointF at = toWidget({entity.x, entity.y});
            const bool strong = contains(selected, entity.id) || entity.id == hover ||
                                contains(pending, entity.id) || contains(involved, entity.id);
            const QColor color = colorOf(entity);
            if (entity.id == g.origin()) {
                painter.setPen(QPen(strong ? color : colors.sketchReference, 1.6));
                painter.setBrush(colors.background);
                painter.drawEllipse(at, 4.5, 4.5);
                painter.drawLine(QLineF(at + QPointF(-8, 0), at + QPointF(8, 0)));
                painter.drawLine(QLineF(at + QPointF(0, -8), at + QPointF(0, 8)));
                continue;
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            const double radius = strong ? 4.2 : 2.8;
            painter.drawEllipse(at, radius, radius);
        }

        // Constraints.
        painter.setFont(labelFont());
        for (const Annotation& a : layout()) {
            const bool conflicting = contains(state.conflicting, a.id);
            const bool isSelected = contains(selectedConstraints, a.id);
            const bool isHovered = a.id == hoverConstraint;
            QColor ink = colors.sketchDimension;
            if (conflicting) {
                ink = colors.sketchConflict;
            } else if (isSelected) {
                ink = colors.selection;
            } else if (isHovered) {
                ink = colors.hover;
            }
            if (!a.dimension) {
                painter.setPen(QPen(ink, 1.0));
                QColor fill = colors.label;
                fill.setAlpha(isSelected || isHovered ? 255 : 215);
                painter.setBrush(fill);
                painter.drawRoundedRect(a.rect, 3.0, 3.0);
                painter.setPen(QPen(ink, 1.3));
                painter.setBrush(Qt::NoBrush);
                drawMarker(painter, a.rect, a.kind);
                continue;
            }
            painter.setPen(QPen(ink, 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(a.lines);
            painter.setBrush(ink);
            for (const QPolygonF& arrow : a.arrows) {
                painter.drawPolygon(arrow);
            }
            if (a.id == editing && field != nullptr) {
                continue;
            }
            painter.setPen(QPen(isSelected || isHovered || conflicting ? ink : colors.label, 1.0));
            painter.setBrush(colors.label);
            painter.drawRoundedRect(a.rect, 3.0, 3.0);
            painter.setPen(conflicting ? colors.sketchConflict : colors.line);
            painter.drawText(a.rect, Qt::AlignCenter, a.text);
        }

        // The tool at work.
        const std::vector<Stroke> strokes = preview();
        if (!strokes.empty()) {
            QPen pen(colors.preview, 1.6);
            pen.setStyle(Qt::CustomDashLine);
            pen.setDashPattern({4.0, 3.0});
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            for (const Stroke& stroke : strokes) {
                painter.drawPath(pathOf(stroke));
            }
        }
        const bool draws = tool != SketchTool::Select && tool != SketchTool::Dimension &&
                           tool != SketchTool::Constraint && tool != SketchTool::Trim &&
                           tool != SketchTool::Extend && tool != SketchTool::Offset &&
                           tool != SketchTool::Mirror && tool != SketchTool::Project;
        if (cursorValid && draws) {
            const QPointF at = toWidget(cursor.position);
            painter.setPen(QPen(colors.preview, 1.4));
            painter.setBrush(Qt::NoBrush);
            switch (cursor.kind) {
            case Snap::Kind::Point:
                painter.drawRect(QRectF(at.x() - 5.5, at.y() - 5.5, 11.0, 11.0));
                break;
            case Snap::Kind::Midpoint:
                painter.drawPolygon(
                    QPolygonF({at + QPointF(0, -7), at + QPointF(6.5, 5), at + QPointF(-6.5, 5)}));
                break;
            case Snap::Kind::Curve:
                painter.drawLine(QLineF(at + QPointF(-5, -5), at + QPointF(5, 5)));
                painter.drawLine(QLineF(at + QPointF(-5, 5), at + QPointF(5, -5)));
                break;
            case Snap::Kind::None:
                break;
            }
            if ((cursor.horizontal || cursor.vertical) && !clicks.empty()) {
                // A guide through the start of the line, to say what it lined up with.
                QPen guide(colors.preview, 1.0, Qt::DotLine);
                painter.setPen(guide);
                const QPointF from = toWidget(clicks.back().position);
                painter.drawLine(QLineF(from + (from - at) * 0.25, at + (at - from) * 0.25));
            }
            const QString text = readout();
            if (!text.isEmpty() && field == nullptr) {
                const QFontMetricsF metrics(labelFont());
                const QRectF box(at.x() + 16.0, at.y() + 14.0,
                                 metrics.horizontalAdvance(text) + 12.0, metrics.height() + 4.0);
                painter.setPen(Qt::NoPen);
                painter.setBrush(colors.label);
                painter.drawRoundedRect(box, 3.0, 3.0);
                painter.setPen(colors.line);
                painter.drawText(box, Qt::AlignCenter, text);
            }
        }

        if (boxing) {
            QColor fill = colors.selection;
            fill.setAlpha(36);
            painter.setPen(QPen(colors.selection, 1.0, Qt::DashLine));
            painter.setBrush(fill);
            painter.drawRect(QRectF(pressPosition, boxEnd).normalized());
        }
    }
};

SketchEditor::SketchEditor(ViewportWidget* viewport)
    : QObject(viewport), m_impl(std::make_unique<Impl>(this, viewport)) {}

SketchEditor::~SketchEditor() = default;

bool SketchEditor::isActive() const {
    return m_impl->active() && m_impl->item() != nullptr;
}

document::ObjectId SketchEditor::sketch() const {
    return m_impl->active() ? m_impl->sketchId : document::ObjectId{};
}

const sketch::Sketch& SketchEditor::geometry() const {
    return m_impl->geometry();
}

const sketch::SolveResult& SketchEditor::state() const {
    return m_impl->state;
}

void SketchEditor::setTool(SketchTool tool) {
    if (!isActive()) {
        return;
    }
    m_impl->closeField();
    if (m_impl->dragging) {
        m_impl->finishDrag(false);
    }
    m_impl->pressed = false;
    m_impl->boxing = false;
    m_impl->resetTool();
    m_impl->tool = tool;
    if (tool == SketchTool::Mirror && m_impl->selected.empty()) {
        emit message(tr("Select what to mirror first, then choose Mirror and click the line to "
                        "mirror in."));
        m_impl->tool = SketchTool::Select;
    }
    m_impl->update();
    emit changed();
}

SketchTool SketchEditor::tool() const {
    return m_impl->tool;
}

void SketchEditor::setConstraintTool(sketch::ConstraintKind kind) {
    if (!isActive()) {
        return;
    }
    setTool(SketchTool::Constraint);
    m_impl->constraintKind = kind;
    m_impl->selected.clear();
    m_impl->selectedConstraints.clear();
    emit changed();
}

sketch::ConstraintKind SketchEditor::constraintTool() const {
    return m_impl->constraintKind;
}

bool SketchEditor::canCancel() const {
    return isActive() && (m_impl->field != nullptr || m_impl->dragging || !m_impl->clicks.empty() ||
                          !m_impl->pending.empty() || m_impl->tool != SketchTool::Select);
}

void SketchEditor::cancel() {
    if (!isActive()) {
        return;
    }
    if (m_impl->field != nullptr) {
        m_impl->closeField();
    } else if (m_impl->dragging) {
        m_impl->finishDrag(false);
        m_impl->pressed = false;
    } else if (!m_impl->clicks.empty() || !m_impl->pending.empty()) {
        m_impl->resetTool();
    } else if (m_impl->tool != SketchTool::Select) {
        m_impl->tool = SketchTool::Select;
        m_impl->resetTool();
    }
    m_impl->update();
    emit changed();
}

void SketchEditor::setInference(bool on) {
    m_impl->inference = on;
    emit changed();
}

bool SketchEditor::hasInference() const {
    return m_impl->inference;
}

void SketchEditor::setConstructionMode(bool on) {
    m_impl->constructionMode = on;
    emit changed();
}

bool SketchEditor::constructionMode() const {
    return m_impl->constructionMode;
}

void SketchEditor::setPolygonSides(int sides) {
    m_impl->polygonSides = std::clamp(sides, 3, 64);
    m_impl->update();
    emit changed();
}

int SketchEditor::polygonSides() const {
    return m_impl->polygonSides;
}

std::vector<sketch::EntityId> SketchEditor::selectedEntities() const {
    return m_impl->selected;
}

std::vector<sketch::ConstraintId> SketchEditor::selectedConstraints() const {
    return m_impl->selectedConstraints;
}

void SketchEditor::setSelection(const std::vector<sketch::EntityId>& entities,
                                const std::vector<sketch::ConstraintId>& constraints) {
    if (!isActive()) {
        return;
    }
    const sketch::Sketch& g = geometry();
    m_impl->selected.clear();
    m_impl->selectedConstraints.clear();
    for (const sketch::EntityId id : entities) {
        if (g.find(id) != nullptr && !contains(m_impl->selected, id)) {
            m_impl->selected.push_back(id);
        }
    }
    for (const sketch::ConstraintId id : constraints) {
        if (g.findConstraint(id) != nullptr && !contains(m_impl->selectedConstraints, id)) {
            m_impl->selectedConstraints.push_back(id);
        }
    }
    m_impl->update();
    emit changed();
}

void SketchEditor::clearSelection() {
    setSelection({});
}

bool SketchEditor::constrain(sketch::ConstraintKind kind) {
    if (!isActive()) {
        return false;
    }
    const sketch::Sketch& g = geometry();
    const std::vector<sketch::EntityId>& selected = m_impl->selected;
    std::vector<sketch::ConstraintChoice> choices;
    const auto all = [&](sketch::EntityKind wanted) {
        return !selected.empty() &&
               std::all_of(selected.begin(), selected.end(), [&](sketch::EntityId id) {
                   return g.find(id) != nullptr && g.find(id)->kind == wanted;
               });
    };
    if ((kind == ConstraintKind::Horizontal || kind == ConstraintKind::Vertical) &&
        all(EntityKind::Line)) {
        // Several lines at once: each on its own.
        for (const sketch::EntityId id : selected) {
            choices.push_back({kind, id, 0, 0, 0.0});
        }
    } else if (kind == ConstraintKind::Fix && !selected.empty()) {
        // A curve is fixed by fixing the points it is made of.
        for (const sketch::EntityId id : selected) {
            const Entity* entity = g.find(id);
            if (entity == nullptr) {
                continue;
            }
            for (const sketch::EntityId point :
                 entity->kind == EntityKind::Point
                     ? std::vector<sketch::EntityId>{id}
                     : std::vector<sketch::EntityId>{entity->center, entity->start, entity->end}) {
                const Entity* found = g.find(point);
                if (found != nullptr && !found->fixed) {
                    choices.push_back({ConstraintKind::Fix, point, 0, 0, 0.0});
                }
            }
        }
    } else if (const auto choice = sketch::constraintFor(g, kind, selected)) {
        choices.push_back(*choice);
    }
    if (choices.empty()) {
        // Not a selection this constraint takes: wait for clicks instead.
        setConstraintTool(kind);
        emit message(Impl::needsOf(kind) + QLatin1Char(' ') + tr("Click them now."));
        return false;
    }
    const bool added = m_impl->applyChoices(choices, Impl::nameOf(kind));
    if (added) {
        m_impl->selected.clear();
        m_impl->update();
        emit changed();
    }
    return added;
}

bool SketchEditor::addDimension() {
    if (!isActive()) {
        return false;
    }
    if (m_impl->selected.empty()) {
        setTool(SketchTool::Dimension);
        emit message(tr("Click what to measure, then empty space, and type the value."));
        return false;
    }
    return m_impl->addDimensionFor(m_impl->selected);
}

double SketchEditor::dimensionValue(sketch::ConstraintId dimension) const {
    const Constraint* constraint = isActive() ? geometry().findConstraint(dimension) : nullptr;
    if (constraint == nullptr) {
        return 0.0;
    }
    return constraint->kind == ConstraintKind::Angle ? std::abs(constraint->value) * 180.0 / pi
                                                     : constraint->value;
}

bool SketchEditor::setDimensionValue(sketch::ConstraintId dimension, double value) {
    if (!isActive()) {
        return false;
    }
    sketch::Sketch g = geometry();
    const Constraint* constraint = g.findConstraint(dimension);
    if (constraint == nullptr || !sketch::isDimension(constraint->kind)) {
        return false;
    }
    double stored = value;
    if (constraint->kind == ConstraintKind::Angle) {
        // Shown without a sign; kept turning the way it turned.
        stored = (constraint->value < 0.0 ? -1.0 : 1.0) * value * pi / 180.0;
    }
    if (stored == constraint->value) {
        return true;
    }
    const sketch::Outcome outcome = sketch::tryDimensionValue(g, dimension, stored);
    if (!outcome.ok()) {
        m_impl->lastProblem =
            outcome.refusal == sketch::Refusal::Invalid
                ? tr("%1 is not a value this dimension can have.").arg(formatValue(value))
                : tr("The sketch cannot take %1 there: it contradicts the other dimensions and "
                     "constraints. The value was left as it was.")
                      .arg(formatValue(value));
        emit message(m_impl->lastProblem);
        return false;
    }
    m_impl->commit(std::move(g), tr("Change Dimension"));
    return true;
}

bool SketchEditor::deleteSelection() {
    if (!isActive() || (m_impl->selected.empty() && m_impl->selectedConstraints.empty())) {
        return false;
    }
    sketch::Sketch g = geometry();
    bool removed = false;
    for (const sketch::ConstraintId id : m_impl->selectedConstraints) {
        if (g.findConstraint(id) != nullptr) {
            g.removeConstraint(id);
            removed = true;
        }
    }
    bool onlyEnds = !m_impl->selected.empty();
    for (const sketch::EntityId id : m_impl->selected) {
        const Entity* entity = g.find(id);
        if (entity == nullptr) {
            continue;
        }
        if (entity->owner != 0 || id == g.origin()) {
            // The end of a curve goes with its curve, not on its own.
            continue;
        }
        onlyEnds = false;
        g.remove(id);
        removed = true;
    }
    if (!removed) {
        if (onlyEnds) {
            emit message(tr("The end of a curve cannot be deleted on its own. Select the curve."));
        }
        return false;
    }
    g.solve();
    m_impl->selected.clear();
    m_impl->selectedConstraints.clear();
    m_impl->commit(std::move(g), tr("Delete"));
    return true;
}

bool SketchEditor::toggleConstruction() {
    if (!isActive()) {
        return false;
    }
    sketch::Sketch g = geometry();
    std::vector<sketch::EntityId> targets;
    for (const sketch::EntityId id : m_impl->selected) {
        const Entity* entity = g.find(id);
        if (entity != nullptr && entity->owner == 0 && id != g.origin()) {
            targets.push_back(id);
        }
    }
    if (targets.empty()) {
        // Nothing to change: switch what the drawing tools make instead.
        setConstructionMode(!m_impl->constructionMode);
        emit message(m_impl->constructionMode ? tr("New geometry will be construction geometry.")
                                              : tr("New geometry will be ordinary geometry."));
        return false;
    }
    const bool allAlready = std::all_of(targets.begin(), targets.end(), [&g](sketch::EntityId id) {
        return g.find(id)->construction;
    });
    for (const sketch::EntityId id : targets) {
        g.setConstruction(id, !allAlready);
    }
    m_impl->commit(std::move(g), allAlready ? tr("Ordinary Geometry") : tr("Construction"));
    return true;
}

bool SketchEditor::mirrorSelection(sketch::EntityId axis) {
    if (!isActive()) {
        return false;
    }
    sketch::Sketch g = geometry();
    std::vector<sketch::EntityId> sources;
    for (const sketch::EntityId id : m_impl->selected) {
        const Entity* entity = g.find(id);
        if (entity != nullptr && entity->owner == 0 && id != g.origin() && id != axis) {
            sources.push_back(id);
        }
    }
    if (sources.empty()) {
        emit message(tr("Select what to mirror first, then choose Mirror and click the line to "
                        "mirror in."));
        return false;
    }
    try {
        sketch::mirror(g, sources, axis);
    } catch (const std::invalid_argument&) {
        emit message(tr("Click the line to mirror in."));
        return false;
    }
    m_impl->selected.clear();
    m_impl->tool = SketchTool::Select;
    m_impl->resetTool();
    m_impl->commit(std::move(g), tr("Mirror"));
    return true;
}

bool SketchEditor::projectShape(const kernel::Shape& edgeOrVertex) {
    if (!isActive()) {
        return false;
    }
    if (edgeOrVertex.isNull()) {
        emit message(tr("Click an edge or a corner of a body to project it into the sketch."));
        return false;
    }
    const document::SketchItem* item = m_impl->item();
    const std::optional<kernel::PlaneCurve> curve = kernel::projectOnto(item->plane, edgeOrVertex);
    if (!curve.has_value()) {
        emit message(tr("That cannot be projected yet. Straight edges, circles and arcs that lie "
                        "flat to the sketch, and corners can."));
        return false;
    }
    sketch::Sketch g = geometry();
    sketch::EntityId made = 0;
    try {
        switch (curve->kind) {
        case kernel::PlaneCurve::Kind::Point:
            made = g.addPoint({curve->u1, curve->v1});
            break;
        case kernel::PlaneCurve::Kind::Segment:
            made = g.addLine({curve->u1, curve->v1}, {curve->u2, curve->v2});
            break;
        case kernel::PlaneCurve::Kind::Circle:
            made = g.addCircle({curve->u1, curve->v1}, curve->radius);
            break;
        case kernel::PlaneCurve::Kind::Arc:
            made = g.addArc({curve->u1, curve->v1}, {curve->u2, curve->v2}, {curve->u3, curve->v3});
            break;
        }
    } catch (const std::invalid_argument&) {
        emit message(tr("That edge has no size when seen from the sketch."));
        return false;
    }
    // Reference geometry: it stays where the body put it.
    g.setFixed(made, true);
    m_impl->commit(std::move(g), tr("Project"));
    return true;
}

void SketchEditor::editDimension(sketch::ConstraintId dimension) {
    if (!isActive()) {
        return;
    }
    const Constraint* constraint = geometry().findConstraint(dimension);
    if (constraint == nullptr || !sketch::isDimension(constraint->kind)) {
        return;
    }
    QRectF where = dimensionRect(dimension);
    if (where.isEmpty()) {
        where = QRectF(m_impl->view->width() / 2.0 - 40.0, m_impl->view->height() / 2.0 - 12.0,
                       80.0, 24.0);
    }
    // A value worked out from parameters is edited as the expression it comes from.
    const document::SketchItem* sketch = m_impl->item();
    const auto expression = sketch != nullptr ? sketch->expressions.find(dimension)
                                              : decltype(sketch->expressions)::const_iterator{};
    m_impl->openField(where,
                      sketch != nullptr && expression != sketch->expressions.end()
                          ? QString::fromStdString(expression->second)
                          : formatValue(dimensionValue(dimension)),
                      true);
    m_impl->editing = dimension;
    m_impl->update();
}

QLineEdit* SketchEditor::valueField() const {
    return m_impl->field;
}

QPointF SketchEditor::toWidget(sketch::Point2 position) const {
    return m_impl->toWidget(position);
}

std::optional<sketch::Point2> SketchEditor::toSketch(const QPointF& position) const {
    return m_impl->toSketch(position);
}

QRectF SketchEditor::dimensionRect(sketch::ConstraintId dimension) const {
    for (const Annotation& annotation : m_impl->layout()) {
        if (annotation.id == dimension && annotation.dimension) {
            return annotation.rect;
        }
    }
    return {};
}

QRectF SketchEditor::markerRect(sketch::ConstraintId constraint) const {
    for (const Annotation& annotation : m_impl->layout()) {
        if (annotation.id == constraint && !annotation.dimension) {
            return annotation.rect;
        }
    }
    return {};
}

QString SketchEditor::hint() const {
    if (!isActive()) {
        return {};
    }
    const std::size_t placed = m_impl->clicks.size();
    switch (m_impl->tool) {
    case SketchTool::Select:
        return {};
    case SketchTool::Line:
        return placed == 0 ? tr("Line: click where it starts")
                           : tr("Line: click the next point, or type a length. Esc ends the line");
    case SketchTool::Rectangle:
        return placed == 0 ? tr("Rectangle: click one corner")
                           : tr("Rectangle: click the opposite corner, or type width and height");
    case SketchTool::Circle:
        return placed == 0 ? tr("Circle: click the center")
                           : tr("Circle: click a point on the circle, or type a diameter");
    case SketchTool::Arc:
        return placed == 0   ? tr("Arc: click the center")
               : placed == 1 ? tr("Arc: click where it starts")
                             : tr("Arc: move round the center and click where it ends");
    case SketchTool::Point:
        return tr("Point: click where to put it");
    case SketchTool::Polygon:
        return placed == 0 ? tr("Polygon: click the center") : tr("Polygon: click a corner");
    case SketchTool::Slot:
        return placed == 0   ? tr("Slot: click the first center")
               : placed == 1 ? tr("Slot: click the second center")
                             : tr("Slot: click to set the width");
    case SketchTool::Dimension:
        return m_impl->pending.empty()
                   ? tr("Dimension: click what to measure")
                   : tr("Dimension: click a second line or point, or empty space to place it");
    case SketchTool::Constraint:
        return tr("%1: %2").arg(Impl::nameOf(m_impl->constraintKind),
                                Impl::needsOf(m_impl->constraintKind));
    case SketchTool::Trim:
        return tr("Trim: click the part of a curve to remove");
    case SketchTool::Extend:
        return tr("Extend: click a line or an arc near the end to lengthen");
    case SketchTool::Offset:
        return m_impl->selected.empty() ? tr("Offset: click the curve to offset")
                                        : tr("Offset: click where the copy should go");
    case SketchTool::Mirror:
        return tr("Mirror: click the line to mirror in");
    case SketchTool::Project:
        return tr("Project: click an edge or a corner of a body");
    }
    return {};
}

sketch::EntityId SketchEditor::entityAt(const QPointF& position) const {
    return isActive() ? m_impl->hitAt(position, true).entity : 0;
}

sketch::ConstraintId SketchEditor::constraintAt(const QPointF& position) const {
    return isActive() ? m_impl->hitAt(position).constraint : 0;
}

QString SketchEditor::constraintName(sketch::ConstraintKind kind) {
    switch (kind) {
    case ConstraintKind::Distance:
    case ConstraintKind::PointLineDistance:
        return tr("Distance");
    case ConstraintKind::DistanceX:
        return tr("Horizontal Distance");
    case ConstraintKind::DistanceY:
        return tr("Vertical Distance");
    case ConstraintKind::Radius:
        return tr("Radius");
    case ConstraintKind::Diameter:
        return tr("Diameter");
    case ConstraintKind::Angle:
        return tr("Angle");
    default:
        return Impl::nameOf(kind);
    }
}

void SketchEditor::begin(document::Document* document, document::ObjectId sketch) {
    end();
    if (document == nullptr || document->findSketch(sketch) == nullptr) {
        return;
    }
    m_impl->document = document;
    m_impl->sketchId = sketch;
    m_impl->tool = SketchTool::Select;
    m_impl->resetTool();
    m_impl->selected.clear();
    m_impl->selectedConstraints.clear();
    m_impl->overlay->setGeometry(m_impl->view->rect());
    m_impl->overlay->show();
    m_impl->overlay->raise();
    m_impl->refresh();
}

void SketchEditor::end() {
    if (m_impl->document == nullptr) {
        return;
    }
    m_impl->closeField();
    if (m_impl->dragging) {
        m_impl->finishDrag(true);
    }
    m_impl->dragTransaction.reset();
    m_impl->pressed = false;
    m_impl->boxing = false;
    m_impl->resetTool();
    m_impl->selected.clear();
    m_impl->selectedConstraints.clear();
    m_impl->tool = SketchTool::Select;
    m_impl->document = nullptr;
    m_impl->sketchId = {};
    m_impl->state = {};
    m_impl->overlay->hide();
    emit changed();
}

void SketchEditor::documentChanged(const document::Change& change) {
    if (!m_impl->active() || change.body != m_impl->sketchId) {
        return;
    }
    if (change.kind == document::Change::Kind::SketchGeometryChanged && m_impl->item() != nullptr) {
        m_impl->refresh();
    }
}

void SketchEditor::mousePress(const QPointF& position, Qt::KeyboardModifiers modifiers) {
    Q_UNUSED(modifiers)
    if (!isActive()) {
        return;
    }
    m_impl->lastPosition = position;
    if (m_impl->field != nullptr && m_impl->editing != 0) {
        m_impl->closeField();
    }
    if (m_impl->tool == SketchTool::Select) {
        m_impl->pressed = true;
        m_impl->dragging = false;
        m_impl->boxing = false;
        m_impl->pressPosition = position;
        m_impl->pressHit = m_impl->hitAt(position);
        return;
    }
    m_impl->closeField();
    m_impl->toolClick(position);
}

void SketchEditor::mouseMove(const QPointF& position, Qt::MouseButtons buttons,
                             Qt::KeyboardModifiers modifiers) {
    Q_UNUSED(modifiers)
    if (!isActive()) {
        return;
    }
    m_impl->lastPosition = position;
    if (m_impl->tool != SketchTool::Select) {
        m_impl->moveCursor(position);
        m_impl->update();
        return;
    }
    if (m_impl->pressed && buttons.testFlag(Qt::LeftButton)) {
        if (!m_impl->dragging && !m_impl->boxing &&
            (position - m_impl->pressPosition).manhattanLength() >=
                QApplication::startDragDistance()) {
            if (m_impl->pressHit.entity != 0) {
                m_impl->startDrag();
            } else if (m_impl->pressHit.constraint == 0) {
                m_impl->boxing = true;
            }
        }
        if (m_impl->dragging) {
            m_impl->dragTo(position);
        }
        if (m_impl->boxing) {
            m_impl->boxEnd = position;
            m_impl->update();
        }
        return;
    }
    const Hit hit = m_impl->hitAt(position);
    if (hit.entity != m_impl->hover || hit.constraint != m_impl->hoverConstraint) {
        m_impl->hover = hit.entity;
        m_impl->hoverConstraint = hit.constraint;
        m_impl->update();
    }
}

void SketchEditor::mouseRelease(const QPointF& position, Qt::KeyboardModifiers modifiers) {
    if (!isActive() || m_impl->tool != SketchTool::Select || !m_impl->pressed) {
        return;
    }
    m_impl->pressed = false;
    const bool add =
        modifiers.testFlag(Qt::ShiftModifier) || modifiers.testFlag(Qt::ControlModifier);
    if (m_impl->dragging) {
        m_impl->finishDrag(true);
        m_impl->update();
        emit changed();
        return;
    }
    if (m_impl->boxing) {
        m_impl->boxing = false;
        m_impl->selectInBox(QRectF(m_impl->pressPosition, position).normalized(), add);
        m_impl->update();
        emit changed();
        return;
    }
    const Hit& hit = m_impl->pressHit;
    if (hit.constraint != 0) {
        if (add) {
            toggle(m_impl->selectedConstraints, hit.constraint);
        } else {
            m_impl->selected.clear();
            m_impl->selectedConstraints = {hit.constraint};
        }
    } else if (hit.entity != 0) {
        if (add) {
            toggle(m_impl->selected, hit.entity);
        } else {
            m_impl->selectedConstraints.clear();
            m_impl->selected = {hit.entity};
        }
    } else if (!add) {
        m_impl->selected.clear();
        m_impl->selectedConstraints.clear();
    }
    m_impl->update();
    emit changed();
}

void SketchEditor::mouseDoubleClick(const QPointF& position, Qt::KeyboardModifiers modifiers) {
    Q_UNUSED(modifiers)
    if (!isActive()) {
        return;
    }
    if (m_impl->tool == SketchTool::Line) {
        // The first click of the pair has placed a point; the second ends the line there.
        m_impl->clicks.clear();
        m_impl->chainEnd = 0;
        m_impl->chainFirst = 0;
        m_impl->update();
        emit changed();
        return;
    }
    if (m_impl->tool != SketchTool::Select) {
        m_impl->toolClick(position);
        return;
    }
    m_impl->pressed = false;
    const Hit hit = m_impl->hitAt(position);
    if (hit.constraint != 0) {
        const Constraint* constraint = geometry().findConstraint(hit.constraint);
        if (constraint != nullptr && sketch::isDimension(constraint->kind)) {
            m_impl->selected.clear();
            m_impl->selectedConstraints = {hit.constraint};
            editDimension(hit.constraint);
            emit changed();
        }
    }
}

bool SketchEditor::keyPress(QKeyEvent* event) {
    if (!isActive() || m_impl->field != nullptr) {
        return false;
    }
    // Typing a number while a line, a rectangle, or a circle is being drawn gives it its size.
    const bool sizing = (m_impl->tool == SketchTool::Line || m_impl->tool == SketchTool::Circle ||
                         m_impl->tool == SketchTool::Rectangle) &&
                        !m_impl->clicks.empty();
    const QString typed = event->text();
    if (sizing && typed.size() == 1 &&
        (typed.at(0).isDigit() || typed.at(0) == QLatin1Char('.') ||
         typed.at(0) == QLatin1Char(','))) {
        const QPointF at =
            m_impl->cursorValid ? m_impl->toWidget(m_impl->cursor.position) : m_impl->lastPosition;
        m_impl->openField(QRectF(at.x() + 16.0, at.y() + 14.0, 70.0, 24.0), typed, false);
        m_impl->editing = 0;
        return true;
    }
    return false;
}

bool SketchEditor::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_impl->field && event->type() == QEvent::FocusOut) {
        const Qt::FocusReason reason = static_cast<QFocusEvent*>(event)->reason();
        // Not when the whole window loses the keyboard, or a menu opens over it.
        if (reason != Qt::ActiveWindowFocusReason && reason != Qt::PopupFocusReason) {
            m_impl->closeField();
        }
    }
    return QObject::eventFilter(watched, event);
}

void SketchEditor::paint(QPainter& painter) {
    m_impl->paint(painter);
}

QWidget* SketchEditor::overlay() const {
    return m_impl->overlay;
}

} // namespace lcad::viewport
