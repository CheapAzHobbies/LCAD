// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/sketch/Sketch.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace lcad::sketch {

namespace {

// Two points closer than this, in millimeters, are the same point as far as building geometry
// goes. It is the kernel's own shortest length.
constexpr double minimumLength = 1e-7;

struct KindName {
    ConstraintKind kind;
    const char* name;
};

constexpr KindName kindNames[] = {
    {ConstraintKind::Coincident, "Coincident"},
    {ConstraintKind::PointOnLine, "PointOnLine"},
    {ConstraintKind::PointOnCircle, "PointOnCircle"},
    {ConstraintKind::Horizontal, "Horizontal"},
    {ConstraintKind::Vertical, "Vertical"},
    {ConstraintKind::Parallel, "Parallel"},
    {ConstraintKind::Perpendicular, "Perpendicular"},
    {ConstraintKind::Tangent, "Tangent"},
    {ConstraintKind::Equal, "Equal"},
    {ConstraintKind::Concentric, "Concentric"},
    {ConstraintKind::Midpoint, "Midpoint"},
    {ConstraintKind::Symmetric, "Symmetric"},
    {ConstraintKind::Collinear, "Collinear"},
    {ConstraintKind::Fix, "Fix"},
    {ConstraintKind::Distance, "Distance"},
    {ConstraintKind::DistanceX, "DistanceX"},
    {ConstraintKind::DistanceY, "DistanceY"},
    {ConstraintKind::PointLineDistance, "PointLineDistance"},
    {ConstraintKind::Radius, "Radius"},
    {ConstraintKind::Diameter, "Diameter"},
    {ConstraintKind::Angle, "Angle"},
};

void requireFinite(Point2 point, const char* function) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        throw std::invalid_argument(std::string(function) + ": the position is not finite");
    }
}

double distance(Point2 a, Point2 b) {
    return std::hypot(b.x - a.x, b.y - a.y);
}

bool isRound(const Entity* entity) {
    return entity != nullptr &&
           (entity->kind == EntityKind::Circle || entity->kind == EntityKind::Arc);
}

bool isKind(const Entity* entity, EntityKind kind) {
    return entity != nullptr && entity->kind == kind;
}

// What a radius can be taken of: a circle, an arc, or an ellipse's minor axis.
bool hasRadius(const Entity* entity) {
    return isRound(entity) || isKind(entity, EntityKind::Ellipse);
}

constexpr double pi = 3.14159265358979323846;

// A point of a clamped spline with evenly spread knots, by de Boor's algorithm.
Point2 splinePoint(const std::vector<Point2>& poles, double t) {
    const int n = static_cast<int>(poles.size());
    const int p = splineDegree(poles.size());
    // Knots: p + 1 zeros, the inner ones evenly, p + 1 ones.
    const int spans = n - p;
    std::vector<double> knots;
    for (int i = 0; i <= p; ++i) {
        knots.push_back(0.0);
    }
    for (int i = 1; i < spans; ++i) {
        knots.push_back(static_cast<double>(i) / spans);
    }
    for (int i = 0; i <= p; ++i) {
        knots.push_back(1.0);
    }
    t = std::clamp(t, 0.0, 1.0);
    int k = p;
    while (k < n - 1 && knots[static_cast<std::size_t>(k + 1)] <= t) {
        ++k;
    }
    std::vector<Point2> d;
    for (int j = 0; j <= p; ++j) {
        d.push_back(poles[static_cast<std::size_t>(j + k - p)]);
    }
    for (int r = 1; r <= p; ++r) {
        for (int j = p; j >= r; --j) {
            const double left = knots[static_cast<std::size_t>(j + k - p)];
            const double right = knots[static_cast<std::size_t>(j + 1 + k - r)];
            const double alpha = right > left ? (t - left) / (right - left) : 0.0;
            d[static_cast<std::size_t>(j)] = {(1.0 - alpha) * d[static_cast<std::size_t>(j - 1)].x +
                                                  alpha * d[static_cast<std::size_t>(j)].x,
                                              (1.0 - alpha) * d[static_cast<std::size_t>(j - 1)].y +
                                                  alpha * d[static_cast<std::size_t>(j)].y};
        }
    }
    return d[static_cast<std::size_t>(p)];
}

} // namespace

int splineDegree(std::size_t controlPoints) {
    return static_cast<int>(std::min<std::size_t>(3, controlPoints > 0 ? controlPoints - 1 : 0));
}

bool isDimension(ConstraintKind kind) {
    switch (kind) {
    case ConstraintKind::Distance:
    case ConstraintKind::DistanceX:
    case ConstraintKind::DistanceY:
    case ConstraintKind::PointLineDistance:
    case ConstraintKind::Radius:
    case ConstraintKind::Diameter:
    case ConstraintKind::Angle:
        return true;
    default:
        return false;
    }
}

const char* toString(ConstraintKind kind) {
    for (const KindName& entry : kindNames) {
        if (entry.kind == kind) {
            return entry.name;
        }
    }
    return "Unknown";
}

std::optional<ConstraintKind> constraintKindFromString(const std::string& name) {
    for (const KindName& entry : kindNames) {
        if (name == entry.name) {
            return entry.kind;
        }
    }
    return std::nullopt;
}

Sketch::Sketch() {
    Entity origin;
    origin.id = m_nextEntity++;
    origin.kind = EntityKind::Point;
    origin.fixed = true;
    m_origin = origin.id;
    m_entities.push_back(origin);
}

Sketch::Sketch(std::vector<Entity> entities, std::vector<Constraint> constraints,
               EntityId nextEntity, ConstraintId nextConstraint)
    : m_entities(std::move(entities)), m_nextEntity(nextEntity), m_nextConstraint(nextConstraint) {
    std::unordered_set<EntityId> seen;
    for (const Entity& item : m_entities) {
        if (item.id == 0 || item.id >= m_nextEntity || !seen.insert(item.id).second) {
            throw std::invalid_argument("Sketch: entity ids are not unique or out of range");
        }
    }
    const auto isPoint = [this](EntityId id) { return isKind(find(id), EntityKind::Point); };
    for (const Entity& item : m_entities) {
        const bool ok =
            item.kind == EntityKind::Point ? std::isfinite(item.x) && std::isfinite(item.y) &&
                                                 (item.owner == 0 || find(item.owner) != nullptr)
            : item.kind == EntityKind::Line ? isPoint(item.start) && isPoint(item.end)
            : item.kind == EntityKind::Circle
                ? isPoint(item.center) && std::isfinite(item.radius) && item.radius > 0.0
            : item.kind == EntityKind::Ellipse ? isPoint(item.center) && isPoint(item.start) &&
                                                     std::isfinite(item.radius) && item.radius > 0.0
            : item.kind == EntityKind::Spline
                ? item.points.size() >= 3 &&
                      std::all_of(item.points.begin(), item.points.end(), isPoint) &&
                      item.start == item.points.front() && item.end == item.points.back()
                : isPoint(item.center) && isPoint(item.start) && isPoint(item.end);
        if (!ok) {
            throw std::invalid_argument("Sketch: entity " + std::to_string(item.id) +
                                        " is incomplete");
        }
    }
    // The origin is the fixed point that belongs to nothing and sits at (0, 0).
    for (const Entity& item : m_entities) {
        if (item.kind == EntityKind::Point && item.fixed && item.owner == 0 && item.x == 0.0 &&
            item.y == 0.0) {
            m_origin = item.id;
            break;
        }
    }
    if (m_origin == 0) {
        throw std::invalid_argument("Sketch: there is no origin point");
    }
    std::unordered_set<ConstraintId> seenConstraints;
    for (const Constraint& constraint : constraints) {
        if (constraint.id == 0 || constraint.id >= m_nextConstraint ||
            !seenConstraints.insert(constraint.id).second) {
            throw std::invalid_argument("Sketch: constraint ids are not unique or out of range");
        }
        validate(constraint);
        m_constraints.push_back(constraint);
    }
}

const Entity* Sketch::find(EntityId id) const {
    for (const Entity& item : m_entities) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

Entity& Sketch::entity(EntityId id) {
    return const_cast<Entity&>(static_cast<const Sketch*>(this)->entity(id));
}

const Entity& Sketch::entity(EntityId id) const {
    const Entity* found = find(id);
    if (found == nullptr) {
        throw std::invalid_argument("Sketch: there is no entity " + std::to_string(id));
    }
    return *found;
}

Point2 Sketch::position(EntityId point) const {
    const Entity& item = entity(point);
    if (item.kind != EntityKind::Point) {
        throw std::invalid_argument("position: entity " + std::to_string(point) +
                                    " is not a point");
    }
    return {item.x, item.y};
}

double Sketch::radiusOf(EntityId curve) const {
    const Entity& item = entity(curve);
    if (item.kind == EntityKind::Circle) {
        return item.radius;
    }
    if (item.kind == EntityKind::Arc) {
        return distance(position(item.center), position(item.start));
    }
    if (item.kind == EntityKind::Ellipse) {
        return item.radius;
    }
    throw std::invalid_argument("radiusOf: entity " + std::to_string(curve) +
                                " is not a circle, an arc, or an ellipse");
}

Point2 Sketch::pointAt(EntityId curve, double t) const {
    const Entity& item = entity(curve);
    switch (item.kind) {
    case EntityKind::Point:
        return {item.x, item.y};
    case EntityKind::Line: {
        const Point2 a = position(item.start);
        const Point2 b = position(item.end);
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    }
    case EntityKind::Circle: {
        const Point2 c = position(item.center);
        return {c.x + item.radius * std::cos(2.0 * pi * t),
                c.y + item.radius * std::sin(2.0 * pi * t)};
    }
    case EntityKind::Arc: {
        const Point2 c = position(item.center);
        const Point2 s = position(item.start);
        const Point2 e = position(item.end);
        const double r = distance(c, s);
        const double from = std::atan2(s.y - c.y, s.x - c.x);
        double sweep = std::atan2(e.y - c.y, e.x - c.x) - from;
        while (sweep <= 0.0) {
            sweep += 2.0 * pi;
        }
        const double angle = from + sweep * t;
        return {c.x + r * std::cos(angle), c.y + r * std::sin(angle)};
    }
    case EntityKind::Ellipse: {
        const Point2 c = position(item.center);
        const Point2 m = position(item.start);
        const double a = distance(c, m);
        const Point2 u{(m.x - c.x) / a, (m.y - c.y) / a};
        const Point2 v{-u.y, u.x};
        const double angle = 2.0 * pi * t;
        const double along = a * std::cos(angle);
        const double across = item.radius * std::sin(angle);
        return {c.x + u.x * along + v.x * across, c.y + u.y * along + v.y * across};
    }
    case EntityKind::Spline: {
        std::vector<Point2> poles;
        for (const EntityId id : item.points) {
            poles.push_back(position(id));
        }
        return splinePoint(poles, t);
    }
    }
    return {};
}

std::vector<Point2> Sketch::samples(EntityId curve, int pieces) const {
    std::vector<Point2> points;
    pieces = std::max(pieces, 1);
    for (int i = 0; i <= pieces; ++i) {
        points.push_back(pointAt(curve, static_cast<double>(i) / pieces));
    }
    return points;
}

EntityId Sketch::addOwnedPoint(Point2 at, EntityId owner) {
    Entity point;
    point.id = m_nextEntity++;
    point.kind = EntityKind::Point;
    point.owner = owner;
    point.x = at.x;
    point.y = at.y;
    m_entities.push_back(point);
    return point.id;
}

EntityId Sketch::addPoint(Point2 at) {
    requireFinite(at, "addPoint");
    return addOwnedPoint(at, 0);
}

EntityId Sketch::addLine(Point2 start, Point2 end) {
    requireFinite(start, "addLine");
    requireFinite(end, "addLine");
    if (distance(start, end) <= minimumLength) {
        throw std::invalid_argument("addLine: the line has no length");
    }
    Entity line;
    line.id = m_nextEntity++;
    line.kind = EntityKind::Line;
    line.start = addOwnedPoint(start, line.id);
    line.end = addOwnedPoint(end, line.id);
    m_entities.push_back(line);
    return line.id;
}

EntityId Sketch::addCircle(Point2 center, double radius) {
    requireFinite(center, "addCircle");
    if (!std::isfinite(radius) || radius <= minimumLength) {
        throw std::invalid_argument("addCircle: the radius must be greater than zero");
    }
    Entity circle;
    circle.id = m_nextEntity++;
    circle.kind = EntityKind::Circle;
    circle.center = addOwnedPoint(center, circle.id);
    circle.radius = radius;
    m_entities.push_back(circle);
    return circle.id;
}

EntityId Sketch::addArc(Point2 center, Point2 start, Point2 end) {
    requireFinite(center, "addArc");
    requireFinite(start, "addArc");
    requireFinite(end, "addArc");
    const double radius = distance(center, start);
    const double toEnd = distance(center, end);
    if (radius <= minimumLength || toEnd <= minimumLength) {
        throw std::invalid_argument("addArc: the start and the end must be away from the center");
    }
    // The end goes on the circle through the start.
    const Point2 onCircle{center.x + (end.x - center.x) * radius / toEnd,
                          center.y + (end.y - center.y) * radius / toEnd};
    if (distance(start, onCircle) <= minimumLength) {
        throw std::invalid_argument("addArc: the arc has no length");
    }
    Entity arc;
    arc.id = m_nextEntity++;
    arc.kind = EntityKind::Arc;
    arc.center = addOwnedPoint(center, arc.id);
    arc.start = addOwnedPoint(start, arc.id);
    arc.end = addOwnedPoint(onCircle, arc.id);
    m_entities.push_back(arc);
    return arc.id;
}

EntityId Sketch::addEllipse(Point2 center, Point2 majorEnd, double minorRadius) {
    requireFinite(center, "addEllipse");
    requireFinite(majorEnd, "addEllipse");
    const double major = distance(center, majorEnd);
    if (!std::isfinite(minorRadius) || minorRadius <= minimumLength ||
        major <= minorRadius + minimumLength) {
        throw std::invalid_argument(
            "addEllipse: both radii must be greater than zero, the major one the larger");
    }
    Entity ellipse;
    ellipse.id = m_nextEntity++;
    ellipse.kind = EntityKind::Ellipse;
    ellipse.center = addOwnedPoint(center, ellipse.id);
    ellipse.start = addOwnedPoint(majorEnd, ellipse.id);
    ellipse.radius = minorRadius;
    m_entities.push_back(ellipse);
    return ellipse.id;
}

EntityId Sketch::addSpline(const std::vector<Point2>& controlPoints) {
    if (controlPoints.size() < 3) {
        throw std::invalid_argument("addSpline: a spline needs three control points or more");
    }
    for (std::size_t i = 0; i < controlPoints.size(); ++i) {
        requireFinite(controlPoints[i], "addSpline");
        if (i > 0 && distance(controlPoints[i - 1], controlPoints[i]) <= minimumLength) {
            throw std::invalid_argument("addSpline: two control points in a row are in one place");
        }
    }
    Entity spline;
    spline.id = m_nextEntity++;
    spline.kind = EntityKind::Spline;
    for (const Point2 at : controlPoints) {
        spline.points.push_back(addOwnedPoint(at, spline.id));
    }
    spline.start = spline.points.front();
    spline.end = spline.points.back();
    m_entities.push_back(spline);
    return spline.id;
}

void Sketch::remove(EntityId id) {
    const Entity* found = find(id);
    if (found == nullptr) {
        throw std::invalid_argument("remove: there is no entity " + std::to_string(id));
    }
    // A curve's point stands for the curve.
    const EntityId top = found->owner != 0 ? found->owner : found->id;
    if (top == m_origin) {
        throw std::invalid_argument("remove: the origin cannot be removed");
    }
    std::unordered_set<EntityId> doomed{top};
    for (const Entity& item : m_entities) {
        if (item.owner == top) {
            doomed.insert(item.id);
        }
    }
    std::erase_if(m_constraints, [&doomed](const Constraint& constraint) {
        return doomed.contains(constraint.first) || doomed.contains(constraint.second) ||
               doomed.contains(constraint.third);
    });
    std::erase_if(m_entities, [&doomed](const Entity& item) { return doomed.contains(item.id); });
}

void Sketch::setConstruction(EntityId id, bool construction) {
    Entity& item = entity(id);
    if (item.owner != 0 || item.id == m_origin) {
        throw std::invalid_argument("setConstruction: choose a curve or a free-standing point");
    }
    item.construction = construction;
}

void Sketch::setFixed(EntityId id, bool fixed) {
    Entity& item = entity(id);
    if (item.owner != 0 || item.id == m_origin) {
        throw std::invalid_argument("setFixed: choose a curve or a free-standing point");
    }
    item.fixed = fixed;
}

void Sketch::validate(const Constraint& c) const {
    const Entity* a = find(c.first);
    const Entity* b = find(c.second);
    const Entity* third = find(c.third);
    const auto point = [](const Entity* e) { return isKind(e, EntityKind::Point); };
    const auto line = [](const Entity* e) { return isKind(e, EntityKind::Line); };
    bool ok = false;
    switch (c.kind) {
    case ConstraintKind::Coincident:
    case ConstraintKind::Distance:
    case ConstraintKind::DistanceX:
    case ConstraintKind::DistanceY:
        ok = point(a) && point(b) && a != b;
        break;
    case ConstraintKind::PointOnLine:
    case ConstraintKind::Midpoint:
    case ConstraintKind::PointLineDistance:
        ok = point(a) && line(b);
        break;
    case ConstraintKind::PointOnCircle:
        ok = point(a) && isRound(b);
        break;
    case ConstraintKind::Horizontal:
    case ConstraintKind::Vertical:
        ok = (line(a) && c.second == 0) || (point(a) && point(b) && a != b);
        break;
    case ConstraintKind::Parallel:
    case ConstraintKind::Perpendicular:
    case ConstraintKind::Collinear:
    case ConstraintKind::Angle:
        ok = line(a) && line(b) && a != b;
        break;
    case ConstraintKind::Tangent:
        ok = (line(a) && isRound(b)) || (isRound(a) && line(b)) ||
             (isRound(a) && isRound(b) && a != b);
        break;
    case ConstraintKind::Equal:
        ok = a != b && ((line(a) && line(b)) || (isRound(a) && isRound(b)) ||
                        (isKind(a, EntityKind::Ellipse) && isKind(b, EntityKind::Ellipse)));
        break;
    case ConstraintKind::Concentric:
        ok = hasRadius(a) && hasRadius(b) && a != b;
        break;
    case ConstraintKind::Symmetric:
        ok = point(a) && point(b) && a != b && line(third);
        break;
    case ConstraintKind::Fix:
        ok = point(a);
        break;
    case ConstraintKind::Radius:
    case ConstraintKind::Diameter:
        // Of an ellipse: its minor radius. The major one is the distance from its center to
        // the end of its major axis.
        ok = hasRadius(a);
        break;
    }
    if (!ok) {
        throw std::invalid_argument(std::string("addConstraint: ") + toString(c.kind) +
                                    " cannot be applied to these entities");
    }
    if (!std::isfinite(c.value) || !std::isfinite(c.value2)) {
        throw std::invalid_argument(std::string("addConstraint: the value of ") + toString(c.kind) +
                                    " is not finite");
    }
    const bool needsPositive = c.kind == ConstraintKind::Distance ||
                               c.kind == ConstraintKind::Radius ||
                               c.kind == ConstraintKind::Diameter;
    const bool needsNonNegative = c.kind == ConstraintKind::DistanceX ||
                                  c.kind == ConstraintKind::DistanceY ||
                                  c.kind == ConstraintKind::PointLineDistance;
    if ((needsPositive && c.value <= minimumLength) || (needsNonNegative && c.value < 0.0)) {
        throw std::invalid_argument(std::string("addConstraint: the value of ") + toString(c.kind) +
                                    " must be greater than zero");
    }
}

ConstraintId Sketch::addConstraint(ConstraintKind kind, EntityId first, EntityId second,
                                   EntityId third, double value) {
    Constraint constraint;
    constraint.kind = kind;
    constraint.first = first;
    constraint.second = second;
    constraint.third = third;
    constraint.value = value;
    if (kind == ConstraintKind::Fix && find(first) != nullptr &&
        find(first)->kind == EntityKind::Point) {
        constraint.value = find(first)->x;
        constraint.value2 = find(first)->y;
    }
    validate(constraint);
    constraint.id = m_nextConstraint++;
    m_constraints.push_back(constraint);
    return constraint.id;
}

void Sketch::removeConstraint(ConstraintId id) {
    const auto before = m_constraints.size();
    std::erase_if(m_constraints, [id](const Constraint& c) { return c.id == id; });
    if (m_constraints.size() == before) {
        throw std::invalid_argument("removeConstraint: there is no constraint " +
                                    std::to_string(id));
    }
}

void Sketch::setConstraintValue(ConstraintId id, double value) {
    for (Constraint& constraint : m_constraints) {
        if (constraint.id != id) {
            continue;
        }
        if (!isDimension(constraint.kind)) {
            throw std::invalid_argument("setConstraintValue: this constraint has no value");
        }
        Constraint changed = constraint;
        changed.value = value;
        validate(changed);
        constraint = changed;
        return;
    }
    throw std::invalid_argument("setConstraintValue: there is no constraint " + std::to_string(id));
}

const Constraint* Sketch::findConstraint(ConstraintId id) const {
    for (const Constraint& constraint : m_constraints) {
        if (constraint.id == id) {
            return &constraint;
        }
    }
    return nullptr;
}

double Sketch::measure(ConstraintKind kind, EntityId first, EntityId second) const {
    const auto direction = [this](EntityId lineId) {
        const Entity& line = entity(lineId);
        const Point2 a = position(line.start);
        const Point2 b = position(line.end);
        return Point2{b.x - a.x, b.y - a.y};
    };
    switch (kind) {
    case ConstraintKind::Distance:
        return distance(position(first), position(second));
    case ConstraintKind::DistanceX:
        return std::abs(position(second).x - position(first).x);
    case ConstraintKind::DistanceY:
        return std::abs(position(second).y - position(first).y);
    case ConstraintKind::PointLineDistance: {
        const Entity& line = entity(second);
        const Point2 a = position(line.start);
        const Point2 d = direction(second);
        const Point2 p = position(first);
        return std::abs(d.x * (p.y - a.y) - d.y * (p.x - a.x)) / std::hypot(d.x, d.y);
    }
    case ConstraintKind::Radius:
        return radiusOf(first);
    case ConstraintKind::Diameter:
        return 2.0 * radiusOf(first);
    case ConstraintKind::Angle: {
        const Point2 d1 = direction(first);
        const Point2 d2 = direction(second);
        return std::atan2(d1.x * d2.y - d1.y * d2.x, d1.x * d2.x + d1.y * d2.y);
    }
    default:
        throw std::invalid_argument(std::string("measure: ") + toString(kind) +
                                    " is not a dimension");
    }
}

} // namespace lcad::sketch
