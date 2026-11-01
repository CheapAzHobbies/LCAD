// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/sketch/Edit.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace lcad::sketch {

namespace {

constexpr double pi = 3.14159265358979323846;
// Positions closer than this, in millimeters, are the same place. It is the tolerance at which
// the ends of curves are taken to be joined when outlines are found.
constexpr double joinTolerance = 1e-6;
// The shortest curve worth keeping.
constexpr double minimumLength = 1e-7;

Point2 operator+(Point2 a, Point2 b) {
    return {a.x + b.x, a.y + b.y};
}
Point2 operator-(Point2 a, Point2 b) {
    return {a.x - b.x, a.y - b.y};
}
Point2 operator*(Point2 a, double k) {
    return {a.x * k, a.y * k};
}
double dot(Point2 a, Point2 b) {
    return a.x * b.x + a.y * b.y;
}
double cross(Point2 a, Point2 b) {
    return a.x * b.y - a.y * b.x;
}
double norm(Point2 a) {
    return std::hypot(a.x, a.y);
}
double apart(Point2 a, Point2 b) {
    return norm(b - a);
}

// An angle brought into [0, 2 pi).
double turn(double angle) {
    angle = std::fmod(angle, 2.0 * pi);
    return angle < 0.0 ? angle + 2.0 * pi : angle;
}

bool isCurve(const Entity* entity) {
    return entity != nullptr && entity->kind != EntityKind::Point;
}
bool isLine(const Entity* entity) {
    return entity != nullptr && entity->kind == EntityKind::Line;
}
bool isPoint(const Entity* entity) {
    return entity != nullptr && entity->kind == EntityKind::Point;
}
bool isKind(const Entity* entity, EntityKind kind) {
    return entity != nullptr && entity->kind == kind;
}
// An ellipse or a spline: a curve the tools that work things out exactly (crossings, trim,
// offset) do not take.
bool isFreeform(const Entity* entity) {
    return entity != nullptr &&
           (entity->kind == EntityKind::Ellipse || entity->kind == EntityKind::Spline);
}

bool isRound(const Entity* entity) {
    return entity != nullptr &&
           (entity->kind == EntityKind::Circle || entity->kind == EntityKind::Arc);
}

// A curve by its numbers. A line runs from `a` to `b` with the parameter going from 0 to 1. A
// circle or an arc runs counter-clockwise from the angle `from` through `sweep` radians, with
// the parameter being the angle traveled.
struct Curve {
    EntityId id = 0;
    EntityKind kind = EntityKind::Line;
    Point2 a;
    Point2 b;
    Point2 center;
    double radius = 0.0;
    double from = 0.0;
    double sweep = 2.0 * pi;

    [[nodiscard]] bool isLine() const { return kind == EntityKind::Line; }
    [[nodiscard]] double span() const { return isLine() ? 1.0 : sweep; }
    // The length of the curve per unit of its parameter.
    [[nodiscard]] double speed() const { return isLine() ? apart(a, b) : radius; }
    [[nodiscard]] Point2 at(double parameter) const {
        if (isLine()) {
            return a + (b - a) * parameter;
        }
        const double angle = from + parameter;
        return {center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)};
    }
    // The parameter of the point of the whole line or circle nearest to `p`. For a circle or
    // an arc it is in [0, 2 pi).
    [[nodiscard]] double parameterOf(Point2 p) const {
        if (isLine()) {
            const Point2 d = b - a;
            return dot(p - a, d) / dot(d, d);
        }
        return turn(std::atan2(p.y - center.y, p.x - center.x) - from);
    }
    // Whether a parameter is within the curve itself, give or take `slack` of length.
    [[nodiscard]] bool covers(double parameter, double slack) const {
        if (kind == EntityKind::Circle) {
            return true;
        }
        const double margin = slack / std::max(speed(), minimumLength);
        if (isLine()) {
            return parameter >= -margin && parameter <= 1.0 + margin;
        }
        return parameter <= sweep + margin || parameter >= 2.0 * pi - margin;
    }
};

Curve describe(const Sketch& sketch, const Entity& entity) {
    Curve curve;
    curve.id = entity.id;
    curve.kind = entity.kind;
    if (entity.kind == EntityKind::Line) {
        curve.a = sketch.position(entity.start);
        curve.b = sketch.position(entity.end);
        return curve;
    }
    curve.center = sketch.position(entity.center);
    curve.radius = sketch.radiusOf(entity.id);
    if (entity.kind == EntityKind::Arc) {
        const Point2 start = sketch.position(entity.start);
        const Point2 end = sketch.position(entity.end);
        curve.a = start;
        curve.b = end;
        curve.from = std::atan2(start.y - curve.center.y, start.x - curve.center.x);
        curve.sweep = turn(std::atan2(end.y - curve.center.y, end.x - curve.center.x) - curve.from);
        if (curve.sweep < 1e-12) {
            curve.sweep = 2.0 * pi;
        }
    }
    return curve;
}

// Where the whole lines and circles two curves lie on meet.
std::vector<Point2> carrierIntersections(const Curve& first, const Curve& second) {
    std::vector<Point2> found;
    if (first.isLine() && second.isLine()) {
        const Point2 d1 = first.b - first.a;
        const Point2 d2 = second.b - second.a;
        const double denominator = cross(d1, d2);
        if (std::abs(denominator) > 1e-12 * norm(d1) * norm(d2)) {
            const double t = cross(second.a - first.a, d2) / denominator;
            found.push_back(first.a + d1 * t);
        }
        return found;
    }
    if (first.isLine() != second.isLine()) {
        const Curve& line = first.isLine() ? first : second;
        const Curve& round = first.isLine() ? second : first;
        const Point2 d = line.b - line.a;
        const double length = norm(d);
        const Point2 unit = d * (1.0 / length);
        // The foot of the center on the line, and how far the line passes from the center.
        const double along = dot(round.center - line.a, unit);
        const Point2 foot = line.a + unit * along;
        const double away = apart(foot, round.center);
        if (away > round.radius + joinTolerance) {
            return found;
        }
        const double half = std::sqrt(std::max(round.radius * round.radius - away * away, 0.0));
        if (half < 1e-9) {
            found.push_back(foot);
        } else {
            found.push_back(foot - unit * half);
            found.push_back(foot + unit * half);
        }
        return found;
    }
    const Point2 between = second.center - first.center;
    const double distance = norm(between);
    if (distance < minimumLength || distance > first.radius + second.radius + joinTolerance ||
        distance < std::abs(first.radius - second.radius) - joinTolerance) {
        return found;
    }
    const double along =
        (distance * distance + first.radius * first.radius - second.radius * second.radius) /
        (2.0 * distance);
    const double half = std::sqrt(std::max(first.radius * first.radius - along * along, 0.0));
    const Point2 unit = between * (1.0 / distance);
    const Point2 foot = first.center + unit * along;
    const Point2 side{-unit.y, unit.x};
    if (half < 1e-9) {
        found.push_back(foot);
    } else {
        found.push_back(foot - side * half);
        found.push_back(foot + side * half);
    }
    return found;
}

// Where `curve` is met by `other` within the extent of `other`: the parameters on `curve`,
// which may lie outside its own extent.
std::vector<double> crossings(const Curve& curve, const Curve& other) {
    std::vector<double> parameters;
    for (const Point2 p : carrierIntersections(curve, other)) {
        if (other.covers(other.parameterOf(p), joinTolerance)) {
            parameters.push_back(curve.parameterOf(p));
        }
    }
    return parameters;
}

// A sketch taken apart, so that entities and constraints can be rewritten freely and the
// sketch put together again.
struct Parts {
    std::vector<Entity> entities;
    std::vector<Constraint> constraints;
    EntityId nextEntity;
    ConstraintId nextConstraint;

    explicit Parts(const Sketch& sketch)
        : entities(sketch.entities()), constraints(sketch.constraints()),
          nextEntity(sketch.nextEntityId()), nextConstraint(sketch.nextConstraintId()) {}

    Entity& at(EntityId id) {
        for (Entity& entity : entities) {
            if (entity.id == id) {
                return entity;
            }
        }
        throw std::invalid_argument("there is no entity " + std::to_string(id));
    }
    void place(EntityId point, Point2 where) {
        Entity& entity = at(point);
        entity.x = where.x;
        entity.y = where.y;
    }
    EntityId addPoint(Point2 where, EntityId owner) {
        Entity point;
        point.id = nextEntity++;
        point.kind = EntityKind::Point;
        point.owner = owner;
        point.x = where.x;
        point.y = where.y;
        entities.push_back(point);
        return point.id;
    }
    EntityId addLine(Point2 start, Point2 end, bool construction) {
        Entity line;
        line.id = nextEntity++;
        line.kind = EntityKind::Line;
        line.construction = construction;
        line.start = addPoint(start, line.id);
        line.end = addPoint(end, line.id);
        entities.push_back(line);
        return line.id;
    }
    EntityId addArc(Point2 center, Point2 start, Point2 end, bool construction) {
        Entity arc;
        arc.id = nextEntity++;
        arc.kind = EntityKind::Arc;
        arc.construction = construction;
        arc.center = addPoint(center, arc.id);
        arc.start = addPoint(start, arc.id);
        arc.end = addPoint(end, arc.id);
        entities.push_back(arc);
        return arc.id;
    }
    void constrain(ConstraintKind kind, EntityId first, EntityId second = 0, EntityId third = 0,
                   double value = 0.0) {
        Constraint constraint;
        constraint.id = nextConstraint++;
        constraint.kind = kind;
        constraint.first = first;
        constraint.second = second;
        constraint.third = third;
        constraint.value = value;
        constraints.push_back(constraint);
    }
    // Takes away every constraint that mentions an entity.
    void release(EntityId entity) {
        std::erase_if(constraints, [entity](const Constraint& c) {
            return c.first == entity || c.second == entity || c.third == entity;
        });
    }
    // Takes away the constraints on a curve that depend on where its ends are.
    void releaseLength(EntityId curve) {
        std::erase_if(constraints, [curve](const Constraint& c) {
            return (c.kind == ConstraintKind::Equal || c.kind == ConstraintKind::Midpoint) &&
                   (c.first == curve || c.second == curve);
        });
    }
    // Makes every constraint on one entity a constraint on another.
    void hand(EntityId from, EntityId to) {
        for (Constraint& c : constraints) {
            for (EntityId* reference : {&c.first, &c.second, &c.third}) {
                if (*reference == from) {
                    *reference = to;
                }
            }
        }
    }
    [[nodiscard]] Sketch build() const {
        return Sketch(entities, constraints, nextEntity, nextConstraint);
    }
};

// Holds a point that has just been put on a curve there: on an end of the curve if that is
// where it is, and otherwise on the curve.
void attach(Parts& parts, EntityId point, const Curve& curve, const Entity& entity, Point2 where) {
    if (entity.kind != EntityKind::Circle) {
        if (apart(where, curve.a) <= joinTolerance) {
            parts.constrain(ConstraintKind::Coincident, point, entity.start);
            return;
        }
        if (apart(where, curve.b) <= joinTolerance) {
            parts.constrain(ConstraintKind::Coincident, point, entity.end);
            return;
        }
    }
    parts.constrain(curve.isLine() ? ConstraintKind::PointOnLine : ConstraintKind::PointOnCircle,
                    point, entity.id);
}

// Where another curve meets a curve.
struct Cut {
    double parameter = 0.0;
    EntityId by = 0;
};

std::vector<Cut> cutsOf(const Sketch& sketch, const Curve& curve) {
    std::vector<Cut> cuts;
    for (const Entity& other : sketch.entities()) {
        if (other.kind == EntityKind::Point || other.id == curve.id || isFreeform(&other)) {
            continue;
        }
        for (const double parameter : crossings(curve, describe(sketch, other))) {
            cuts.push_back({parameter, other.id});
        }
    }
    std::sort(cuts.begin(), cuts.end(),
              [](const Cut& a, const Cut& b) { return a.parameter < b.parameter; });
    return cuts;
}

void requireUsable(Point2 point, const char* what) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        throw std::invalid_argument(std::string(what) + ": the position is not finite");
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Finding geometry by position

Point2 nearestOn(const Sketch& sketch, EntityId id, Point2 position) {
    const Entity* entity = sketch.find(id);
    if (!isCurve(entity)) {
        throw std::invalid_argument("nearestOn: not a curve");
    }
    if (isFreeform(entity)) {
        // Close enough for aiming: the nearest point of fine chords along it.
        const std::vector<Point2> chords = sketch.samples(id, 360);
        Point2 best = chords.front();
        double bestDistance = apart(position, best);
        for (std::size_t i = 1; i < chords.size(); ++i) {
            const Point2 a = chords[i - 1];
            const Point2 d = chords[i] - a;
            const double squared = dot(d, d);
            const double t =
                squared > 0.0 ? std::clamp(dot(position - a, d) / squared, 0.0, 1.0) : 0.0;
            const Point2 on = a + d * t;
            if (apart(position, on) < bestDistance) {
                bestDistance = apart(position, on);
                best = on;
            }
        }
        return best;
    }
    const Curve curve = describe(sketch, *entity);
    if (curve.isLine()) {
        return curve.at(std::clamp(curve.parameterOf(position), 0.0, 1.0));
    }
    if (apart(position, curve.center) < minimumLength) {
        return curve.at(0.0);
    }
    const double parameter = curve.parameterOf(position);
    if (curve.kind == EntityKind::Circle || parameter <= curve.sweep) {
        return curve.at(parameter);
    }
    // Beyond the arc: the nearer of its ends.
    return apart(position, curve.a) <= apart(position, curve.b) ? curve.a : curve.b;
}

double distanceTo(const Sketch& sketch, EntityId id, Point2 position) {
    const Entity* entity = sketch.find(id);
    if (entity == nullptr) {
        throw std::invalid_argument("distanceTo: there is no entity " + std::to_string(id));
    }
    if (entity->kind == EntityKind::Point) {
        return apart(position, {entity->x, entity->y});
    }
    return apart(position, nearestOn(sketch, id, position));
}

std::vector<EntityId> entitiesAt(const Sketch& sketch, Point2 position, double tolerance) {
    std::vector<std::pair<double, EntityId>> points;
    std::vector<std::pair<double, EntityId>> curves;
    for (const Entity& entity : sketch.entities()) {
        const double distance = distanceTo(sketch, entity.id, position);
        if (distance <= tolerance) {
            (entity.kind == EntityKind::Point ? points : curves).emplace_back(distance, entity.id);
        }
    }
    std::sort(points.begin(), points.end());
    std::sort(curves.begin(), curves.end());
    std::vector<EntityId> found;
    for (const auto& entry : points) {
        found.push_back(entry.second);
    }
    for (const auto& entry : curves) {
        found.push_back(entry.second);
    }
    return found;
}

std::vector<EntityId> pointsTogether(const Sketch& sketch, EntityId point) {
    const Entity* entity = sketch.find(point);
    if (!isPoint(entity)) {
        return {};
    }
    std::vector<EntityId> together;
    for (const Entity& other : sketch.entities()) {
        if (isPoint(&other) && std::hypot(other.x - entity->x, other.y - entity->y) <= 1e-6) {
            together.push_back(other.id);
        }
    }
    return together;
}

std::vector<Point2> intersections(const Sketch& sketch, EntityId first, EntityId second) {
    const Entity* a = sketch.find(first);
    const Entity* b = sketch.find(second);
    std::vector<Point2> found;
    if (!isCurve(a) || !isCurve(b) || a == b || isFreeform(a) || isFreeform(b)) {
        return found;
    }
    const Curve one = describe(sketch, *a);
    const Curve other = describe(sketch, *b);
    for (const Point2 p : carrierIntersections(one, other)) {
        if (one.covers(one.parameterOf(p), joinTolerance) &&
            other.covers(other.parameterOf(p), joinTolerance)) {
            found.push_back(p);
        }
    }
    return found;
}

// ---------------------------------------------------------------------------------------------
// Shapes made of several curves

RectangleParts addRectangle(Sketch& sketch, Point2 corner, Point2 opposite) {
    requireUsable(corner, "addRectangle");
    requireUsable(opposite, "addRectangle");
    const double left = std::min(corner.x, opposite.x);
    const double right = std::max(corner.x, opposite.x);
    const double bottom = std::min(corner.y, opposite.y);
    const double top = std::max(corner.y, opposite.y);
    if (right - left <= minimumLength || top - bottom <= minimumLength) {
        throw std::invalid_argument("addRectangle: the rectangle has no width or no height");
    }
    Sketch work = sketch;
    RectangleParts parts;
    parts.bottom = work.addLine({left, bottom}, {right, bottom});
    parts.right = work.addLine({right, bottom}, {right, top});
    parts.top = work.addLine({right, top}, {left, top});
    parts.left = work.addLine({left, top}, {left, bottom});
    const auto join = [&work](EntityId first, EntityId second) {
        work.addConstraint(ConstraintKind::Coincident, work.find(first)->end,
                           work.find(second)->start);
    };
    join(parts.bottom, parts.right);
    join(parts.right, parts.top);
    join(parts.top, parts.left);
    join(parts.left, parts.bottom);
    work.addConstraint(ConstraintKind::Horizontal, parts.bottom);
    work.addConstraint(ConstraintKind::Horizontal, parts.top);
    work.addConstraint(ConstraintKind::Vertical, parts.right);
    work.addConstraint(ConstraintKind::Vertical, parts.left);
    sketch = std::move(work);
    return parts;
}

PolygonParts addPolygon(Sketch& sketch, Point2 center, Point2 corner, int sides) {
    requireUsable(center, "addPolygon");
    requireUsable(corner, "addPolygon");
    if (sides < 3 || sides > 64) {
        throw std::invalid_argument("addPolygon: a polygon has 3 to 64 sides");
    }
    const double radius = apart(center, corner);
    if (radius <= minimumLength) {
        throw std::invalid_argument("addPolygon: the polygon has no size");
    }
    Sketch work = sketch;
    PolygonParts parts;
    parts.circle = work.addCircle(center, radius);
    work.setConstruction(parts.circle, true);
    const double first = std::atan2(corner.y - center.y, corner.x - center.x);
    const auto cornerAt = [&](int index) {
        const double angle = first + 2.0 * pi * index / sides;
        return Point2{center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)};
    };
    for (int i = 0; i < sides; ++i) {
        parts.sides.push_back(work.addLine(cornerAt(i), cornerAt(i + 1)));
    }
    for (int i = 0; i < sides; ++i) {
        const EntityId side = parts.sides[static_cast<std::size_t>(i)];
        const EntityId next = parts.sides[static_cast<std::size_t>((i + 1) % sides)];
        work.addConstraint(ConstraintKind::Coincident, work.find(side)->end,
                           work.find(next)->start);
        work.addConstraint(ConstraintKind::PointOnCircle, work.find(side)->start, parts.circle);
        if (i > 0) {
            work.addConstraint(ConstraintKind::Equal, parts.sides.front(), side);
        }
    }
    sketch = std::move(work);
    return parts;
}

SlotParts addSlot(Sketch& sketch, Point2 first, Point2 second, double radius) {
    requireUsable(first, "addSlot");
    requireUsable(second, "addSlot");
    const double length = apart(first, second);
    if (length <= minimumLength || !std::isfinite(radius) || radius <= minimumLength) {
        throw std::invalid_argument("addSlot: the slot has no length or no width");
    }
    const Point2 along = (second - first) * (1.0 / length);
    const Point2 up = Point2{-along.y, along.x} * radius;
    Sketch work = sketch;
    SlotParts parts;
    // Counter-clockwise: around the first center from the top to the bottom, along the
    // bottom, around the second center back up, and along the top.
    parts.firstArc = work.addArc(first, first + up, first - up);
    parts.firstLine = work.addLine(first - up, second - up);
    parts.secondArc = work.addArc(second, second - up, second + up);
    parts.secondLine = work.addLine(second + up, first + up);
    const auto join = [&work](EntityId a, EntityId b) {
        work.addConstraint(ConstraintKind::Coincident, work.find(a)->end, work.find(b)->start);
    };
    join(parts.firstArc, parts.firstLine);
    join(parts.firstLine, parts.secondArc);
    join(parts.secondArc, parts.secondLine);
    join(parts.secondLine, parts.firstArc);
    for (const EntityId line : {parts.firstLine, parts.secondLine}) {
        work.addConstraint(ConstraintKind::Tangent, line, parts.firstArc);
        work.addConstraint(ConstraintKind::Tangent, line, parts.secondArc);
    }
    work.addConstraint(ConstraintKind::Equal, parts.firstArc, parts.secondArc);
    sketch = std::move(work);
    return parts;
}

// ---------------------------------------------------------------------------------------------
// Constraints for a selection

std::optional<ConstraintChoice> constraintFor(const Sketch& sketch, ConstraintKind tool,
                                              const std::vector<EntityId>& selection) {
    std::vector<const Entity*> picked;
    for (const EntityId id : selection) {
        const Entity* entity = sketch.find(id);
        if (entity == nullptr) {
            return std::nullopt;
        }
        picked.push_back(entity);
    }
    const auto choice = [](ConstraintKind kind, const Entity* a, const Entity* b = nullptr,
                           const Entity* c = nullptr) {
        ConstraintChoice made;
        made.kind = kind;
        made.first = a->id;
        made.second = b != nullptr ? b->id : 0;
        made.third = c != nullptr ? c->id : 0;
        return std::optional<ConstraintChoice>(made);
    };
    // The two entities, with the one `wanted` accepts first.
    const auto ordered = [&picked](bool (*wanted)(const Entity*)) {
        return wanted(picked[0]) ? std::pair(picked[0], picked[1])
                                 : std::pair(picked[1], picked[0]);
    };
    const std::size_t count = picked.size();
    switch (tool) {
    case ConstraintKind::Coincident:
    case ConstraintKind::PointOnLine:
    case ConstraintKind::PointOnCircle: {
        if (count != 2 || picked[0] == picked[1]) {
            return std::nullopt;
        }
        if (isPoint(picked[0]) && isPoint(picked[1])) {
            return choice(ConstraintKind::Coincident, picked[0], picked[1]);
        }
        const auto [point, other] = ordered(isPoint);
        if (!isPoint(point) || other->id == point->owner) {
            return std::nullopt;
        }
        if (isLine(other)) {
            return choice(ConstraintKind::PointOnLine, point, other);
        }
        if (isRound(other)) {
            return choice(ConstraintKind::PointOnCircle, point, other);
        }
        return std::nullopt;
    }
    case ConstraintKind::Horizontal:
    case ConstraintKind::Vertical:
        if (count == 1 && isLine(picked[0])) {
            return choice(tool, picked[0]);
        }
        if (count == 2 && isPoint(picked[0]) && isPoint(picked[1]) && picked[0] != picked[1]) {
            return choice(tool, picked[0], picked[1]);
        }
        return std::nullopt;
    case ConstraintKind::Parallel:
    case ConstraintKind::Perpendicular:
    case ConstraintKind::Collinear:
        if (count == 2 && isLine(picked[0]) && isLine(picked[1]) && picked[0] != picked[1]) {
            return choice(tool, picked[0], picked[1]);
        }
        return std::nullopt;
    case ConstraintKind::Tangent:
        if (count == 2 && picked[0] != picked[1] &&
            ((isLine(picked[0]) && isRound(picked[1])) ||
             (isRound(picked[0]) && isLine(picked[1])) ||
             (isRound(picked[0]) && isRound(picked[1])))) {
            return choice(tool, picked[0], picked[1]);
        }
        return std::nullopt;
    case ConstraintKind::Equal:
        if (count == 2 && picked[0] != picked[1] &&
            ((isLine(picked[0]) && isLine(picked[1])) ||
             (isRound(picked[0]) && isRound(picked[1])))) {
            return choice(tool, picked[0], picked[1]);
        }
        return std::nullopt;
    case ConstraintKind::Concentric:
        if (count == 2 && picked[0] != picked[1] && isRound(picked[0]) && isRound(picked[1])) {
            return choice(tool, picked[0], picked[1]);
        }
        return std::nullopt;
    case ConstraintKind::Midpoint: {
        if (count != 2) {
            return std::nullopt;
        }
        const auto [point, curve] = ordered(isPoint);
        if (isPoint(point) && (isLine(curve) || isKind(curve, EntityKind::Arc)) &&
            point->owner != curve->id) {
            return choice(tool, point, curve);
        }
        return std::nullopt;
    }
    case ConstraintKind::Symmetric: {
        if (count != 3) {
            return std::nullopt;
        }
        std::vector<const Entity*> points;
        const Entity* axis = nullptr;
        for (const Entity* entity : picked) {
            if (isPoint(entity)) {
                points.push_back(entity);
            } else if (isLine(entity) && axis == nullptr) {
                axis = entity;
            } else {
                return std::nullopt;
            }
        }
        if (points.size() == 2 && axis != nullptr && points[0] != points[1]) {
            return choice(tool, points[0], points[1], axis);
        }
        return std::nullopt;
    }
    case ConstraintKind::Fix:
        if (count == 1 && isPoint(picked[0]) && !picked[0]->fixed) {
            return choice(tool, picked[0]);
        }
        return std::nullopt;
    case ConstraintKind::Distance:
    case ConstraintKind::DistanceX:
    case ConstraintKind::DistanceY:
    case ConstraintKind::PointLineDistance:
    case ConstraintKind::Radius:
    case ConstraintKind::Diameter:
    case ConstraintKind::Angle:
        return dimensionFor(sketch, selection);
    }
    return std::nullopt;
}

std::optional<ConstraintChoice> dimensionFor(const Sketch& sketch,
                                             const std::vector<EntityId>& selection) {
    std::vector<const Entity*> picked;
    for (const EntityId id : selection) {
        const Entity* entity = sketch.find(id);
        if (entity == nullptr) {
            return std::nullopt;
        }
        picked.push_back(entity);
    }
    ConstraintChoice made;
    if (picked.size() == 1) {
        const Entity* only = picked[0];
        if (isLine(only)) {
            made.kind = ConstraintKind::Distance;
            made.first = only->start;
            made.second = only->end;
        } else if (only->kind == EntityKind::Circle) {
            made.kind = ConstraintKind::Diameter;
            made.first = only->id;
        } else if (only->kind == EntityKind::Arc || only->kind == EntityKind::Ellipse) {
            // Of an ellipse, its minor radius.
            made.kind = ConstraintKind::Radius;
            made.first = only->id;
        } else {
            return std::nullopt;
        }
    } else if (picked.size() == 2 && picked[0] != picked[1]) {
        const Entity* a = picked[0];
        const Entity* b = picked[1];
        if (isPoint(a) && isPoint(b)) {
            made.kind = ConstraintKind::Distance;
            made.first = a->id;
            made.second = b->id;
        } else if (isLine(a) && isLine(b)) {
            const Curve one = describe(sketch, *a);
            const Curve other = describe(sketch, *b);
            const Point2 d1 = one.b - one.a;
            const Point2 d2 = other.b - other.a;
            if (std::abs(cross(d1, d2)) <= 1e-9 * norm(d1) * norm(d2)) {
                // Parallel lines have no angle between them worth a dimension; they have a
                // distance.
                made.kind = ConstraintKind::PointLineDistance;
                made.first = a->start;
                made.second = b->id;
            } else {
                made.kind = ConstraintKind::Angle;
                made.first = a->id;
                made.second = b->id;
            }
        } else if ((isPoint(a) && isLine(b)) || (isLine(a) && isPoint(b))) {
            const Entity* point = isPoint(a) ? a : b;
            const Entity* line = isPoint(a) ? b : a;
            if (point->owner == line->id) {
                return std::nullopt;
            }
            made.kind = ConstraintKind::PointLineDistance;
            made.first = point->id;
            made.second = line->id;
        } else {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    made.value = sketch.measure(made.kind, made.first, made.second);
    return made;
}

namespace {

bool contains(const std::vector<ConstraintId>& list, ConstraintId id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}

// Whether solving `work` turned up a conflict that the sketch did not have before.
bool hasNewConflict(const SolveResult& before, const SolveResult& after) {
    return std::any_of(after.conflicting.begin(), after.conflicting.end(),
                       [&before](ConstraintId id) { return !contains(before.conflicting, id); });
}

} // namespace

Outcome tryConstraint(Sketch& sketch, ConstraintKind kind, EntityId first, EntityId second,
                      EntityId third, double value) {
    Outcome outcome;
    const SolveResult before = sketch.state();
    outcome.result = before;
    Sketch work = sketch;
    ConstraintId added = 0;
    try {
        // The middle of an arc: the solver looks near where a point is, and from just anywhere
        // it could as well settle on the opposite side of the circle.
        if (kind == ConstraintKind::Midpoint && isKind(sketch.find(second), EntityKind::Arc) &&
            isPoint(sketch.find(first))) {
            const Point2 here = sketch.position(first);
            const Point2 middle = sketch.pointAt(second, 0.5);
            work.dragBy(first, middle.x - here.x, middle.y - here.y);
        }
        added = work.addConstraint(kind, first, second, third, value);
    } catch (const std::invalid_argument&) {
        outcome.refusal = Refusal::Invalid;
        return outcome;
    }
    const SolveResult after = work.solve();
    if (contains(after.conflicting, added) || hasNewConflict(before, after)) {
        outcome.refusal = Refusal::Conflicting;
        return outcome;
    }
    if (contains(after.redundant, added)) {
        outcome.refusal = Refusal::Redundant;
        return outcome;
    }
    sketch = std::move(work);
    outcome.constraint = added;
    outcome.result = after;
    return outcome;
}

std::vector<ConstraintId> blockedBy(const Sketch& sketch, ConstraintKind kind, EntityId first,
                                    EntityId second, EntityId third, double value) {
    const auto entitiesOf = [](EntityId a, EntityId b, EntityId c) {
        std::array<EntityId, 3> sorted{a, b, c};
        std::sort(sorted.begin(), sorted.end());
        return sorted;
    };
    const auto wanted = entitiesOf(first, second, third);
    for (const Constraint& c : sketch.constraints()) {
        if (c.kind == kind && entitiesOf(c.first, c.second, c.third) == wanted) {
            return {c.id};
        }
    }
    std::vector<ConstraintId> blockers;
    for (const Constraint& c : sketch.constraints()) {
        Sketch without = sketch;
        without.removeConstraint(c.id);
        without.solve();
        if (tryConstraint(without, kind, first, second, third, value).ok()) {
            blockers.push_back(c.id);
        }
    }
    if (isDimension(kind)) {
        std::vector<ConstraintId> dimensions;
        for (const ConstraintId id : blockers) {
            if (isDimension(sketch.findConstraint(id)->kind)) {
                dimensions.push_back(id);
            }
        }
        if (!dimensions.empty()) {
            return dimensions;
        }
    }
    return blockers;
}

Outcome tryDimensionValue(Sketch& sketch, ConstraintId dimension, double value) {
    Outcome outcome;
    const SolveResult before = sketch.state();
    outcome.result = before;
    Sketch work = sketch;
    try {
        work.setConstraintValue(dimension, value);
    } catch (const std::invalid_argument&) {
        outcome.refusal = Refusal::Invalid;
        return outcome;
    }
    const SolveResult after = work.solve();
    if (hasNewConflict(before, after)) {
        outcome.refusal = Refusal::Conflicting;
        return outcome;
    }
    sketch = std::move(work);
    outcome.constraint = dimension;
    outcome.result = after;
    return outcome;
}

// ---------------------------------------------------------------------------------------------
// Mirror

std::vector<EntityId> mirror(Sketch& sketch, const std::vector<EntityId>& entities, EntityId axis) {
    const Entity* axisLine = sketch.find(axis);
    if (!isLine(axisLine)) {
        throw std::invalid_argument("mirror: the axis has to be a line");
    }
    const Point2 a = sketch.position(axisLine->start);
    const Point2 d = sketch.position(axisLine->end) - a;
    const double squared = dot(d, d);
    const auto image = [&](Point2 p) {
        const Point2 foot = a + d * (dot(p - a, d) / squared);
        return foot + (foot - p);
    };

    Sketch work = sketch;
    std::vector<EntityId> made;
    const auto pair = [&work, axis](EntityId original, EntityId copy) {
        work.addConstraint(ConstraintKind::Symmetric, original, copy, axis);
    };
    for (const EntityId id : entities) {
        const Entity* found = sketch.find(id);
        if (found == nullptr || id == axis || found->owner != 0 || id == sketch.origin()) {
            throw std::invalid_argument(
                "mirror: choose curves and free-standing points other than the axis");
        }
        const Entity entity = *found;
        EntityId copy = 0;
        switch (entity.kind) {
        case EntityKind::Point:
            copy = work.addPoint(image({entity.x, entity.y}));
            pair(entity.id, copy);
            break;
        case EntityKind::Line:
            copy = work.addLine(image(sketch.position(entity.start)),
                                image(sketch.position(entity.end)));
            pair(entity.start, work.find(copy)->start);
            pair(entity.end, work.find(copy)->end);
            break;
        case EntityKind::Circle:
            copy = work.addCircle(image(sketch.position(entity.center)), entity.radius);
            pair(entity.center, work.find(copy)->center);
            work.addConstraint(ConstraintKind::Equal, entity.id, copy);
            break;
        case EntityKind::Arc:
            // A mirror image turns the other way, so the image of the end is the copy's start.
            copy = work.addArc(image(sketch.position(entity.center)),
                               image(sketch.position(entity.end)),
                               image(sketch.position(entity.start)));
            pair(entity.center, work.find(copy)->center);
            pair(entity.end, work.find(copy)->start);
            pair(entity.start, work.find(copy)->end);
            break;
        case EntityKind::Ellipse:
            copy = work.addEllipse(image(sketch.position(entity.center)),
                                   image(sketch.position(entity.start)), entity.radius);
            pair(entity.center, work.find(copy)->center);
            pair(entity.start, work.find(copy)->start);
            work.addConstraint(ConstraintKind::Equal, entity.id, copy);
            break;
        case EntityKind::Spline: {
            std::vector<Point2> images;
            for (const EntityId point : entity.points) {
                images.push_back(image(sketch.position(point)));
            }
            copy = work.addSpline(images);
            const std::vector<EntityId> copied = work.find(copy)->points;
            for (std::size_t i = 0; i < copied.size(); ++i) {
                pair(entity.points[i], copied[i]);
            }
            break;
        }
        }
        work.setConstruction(copy, entity.construction);
        made.push_back(copy);
    }
    work.solve();
    sketch = std::move(work);
    return made;
}

// ---------------------------------------------------------------------------------------------
// Offset

namespace {

// A curve taking part in an offset: which way the outline runs along it, and what it becomes.
struct Link {
    Curve curve;
    bool construction = false;
    // Whether the outline runs from the curve's start to its end.
    bool forward = true;
    // The offset curve: a line through `through` along the original's direction, or a circle
    // around the original's center with this radius.
    Point2 through;
    double radius = 0.0;
    // Where the offset curve's start and end come to lie.
    Point2 start;
    Point2 end;
    EntityId made = 0;
};

Point2 startOf(const Link& link) {
    return link.curve.a;
}
Point2 endOf(const Link& link) {
    return link.curve.b;
}
Point2 entryOf(const Link& link) {
    return link.forward ? startOf(link) : endOf(link);
}
Point2 exitOf(const Link& link) {
    return link.forward ? endOf(link) : startOf(link);
}

// Whether `side` is to the left of a curve when traveling from its start to its end.
bool isLeftOf(const Curve& curve, Point2 side) {
    if (curve.isLine()) {
        return cross(curve.b - curve.a, side - curve.a) > 0.0;
    }
    // Counter-clockwise travel has the center on its left.
    return apart(side, curve.center) < curve.radius;
}

// Moves a link's curve `shift` to the left of the way the outline runs.
void shiftLink(Link& link, double shift) {
    const double toLeft = link.forward ? shift : -shift;
    if (link.curve.isLine()) {
        const Point2 d = link.curve.b - link.curve.a;
        const Point2 left = Point2{-d.y, d.x} * (1.0 / norm(d));
        link.through = link.curve.a + left * toLeft;
        link.start = link.through;
        link.end = link.curve.b + left * toLeft;
        return;
    }
    link.radius = link.curve.radius - toLeft;
    if (link.radius <= minimumLength) {
        throw std::invalid_argument("offset: the distance leaves nothing of an arc or circle");
    }
    const double scale = link.radius / link.curve.radius;
    link.start = link.curve.center + (link.curve.a - link.curve.center) * scale;
    link.end = link.curve.center + (link.curve.b - link.curve.center) * scale;
}

// The offset curve of a link as a whole line or circle.
Curve carrierOf(const Link& link) {
    Curve carrier = link.curve;
    if (carrier.isLine()) {
        carrier.b = link.through + (link.curve.b - link.curve.a);
        carrier.a = link.through;
    } else {
        carrier.kind = EntityKind::Circle;
        carrier.radius = link.radius;
    }
    return carrier;
}

// Where the offsets of two consecutive links meet: the crossing of their carriers nearest to
// where the corner would be if both had simply been moved.
Point2 corner(const Link& before, const Link& after) {
    const Point2 moved = before.forward ? before.end : before.start;
    const std::vector<Point2> candidates =
        carrierIntersections(carrierOf(before), carrierOf(after));
    if (candidates.empty()) {
        // Parallel lines that continue each other, or curves that touch without crossing.
        return moved;
    }
    return *std::min_element(candidates.begin(), candidates.end(), [moved](Point2 a, Point2 b) {
        return apart(a, moved) < apart(b, moved);
    });
}

} // namespace

std::vector<EntityId> offset(Sketch& sketch, const std::vector<EntityId>& curves, double distance,
                             Point2 side) {
    if (!std::isfinite(distance) || distance <= minimumLength) {
        throw std::invalid_argument("offset: the distance must be greater than zero");
    }
    requireUsable(side, "offset");
    std::vector<Link> pool;
    for (const EntityId id : curves) {
        const Entity* entity = sketch.find(id);
        if (!isCurve(entity) || isFreeform(entity)) {
            throw std::invalid_argument("offset: choose lines, arcs, and circles");
        }
        if (std::any_of(pool.begin(), pool.end(),
                        [id](const Link& link) { return link.curve.id == id; })) {
            continue;
        }
        Link link;
        link.curve = describe(sketch, *entity);
        link.construction = entity->construction;
        pool.push_back(link);
    }

    Sketch work = sketch;
    std::vector<EntityId> made;
    std::vector<bool> used(pool.size(), false);
    const auto meets = [](Point2 a, Point2 b) { return apart(a, b) <= joinTolerance; };

    for (std::size_t seed = 0; seed < pool.size(); ++seed) {
        if (used[seed]) {
            continue;
        }
        // Gather the outline this curve is part of: follow joined ends forward from its end,
        // then backward from its start.
        std::vector<Link> chain;
        used[seed] = true;
        chain.push_back(pool[seed]);
        bool closed = false;
        if (pool[seed].curve.kind != EntityKind::Circle) {
            const auto grow = [&](bool atBack) {
                for (;;) {
                    const Point2 open = atBack ? exitOf(chain.back()) : entryOf(chain.front());
                    std::size_t next = pool.size();
                    bool forward = true;
                    for (std::size_t i = 0; i < pool.size(); ++i) {
                        if (used[i] || pool[i].curve.kind == EntityKind::Circle) {
                            continue;
                        }
                        const bool atStart = meets(startOf(pool[i]), open);
                        const bool atEnd = meets(endOf(pool[i]), open);
                        if (atStart || atEnd) {
                            next = i;
                            // Joined by its start, a curve continues an outline that is being
                            // followed forward; joined by its end, one followed backward.
                            forward = atBack ? atStart : atEnd;
                            break;
                        }
                    }
                    if (next == pool.size()) {
                        return;
                    }
                    used[next] = true;
                    Link link = pool[next];
                    link.forward = forward;
                    if (atBack) {
                        chain.push_back(link);
                    } else {
                        chain.insert(chain.begin(), link);
                    }
                }
            };
            grow(true);
            grow(false);
            closed = chain.size() > 1 && meets(exitOf(chain.back()), entryOf(chain.front()));
        }

        // Which way to move: decided by the curve of the outline nearest to `side`.
        const Link* nearest = &chain.front();
        double best = distanceTo(sketch, nearest->curve.id, side);
        for (const Link& link : chain) {
            const double away = distanceTo(sketch, link.curve.id, side);
            if (away < best) {
                best = away;
                nearest = &link;
            }
        }
        const bool left = isLeftOf(nearest->curve, side) == nearest->forward;
        for (Link& link : chain) {
            shiftLink(link, left ? distance : -distance);
        }
        // Corners: where neighbors' offsets cross.
        const std::size_t count = chain.size();
        for (std::size_t i = 0; i + 1 < count || (closed && i < count); ++i) {
            Link& before = chain[i];
            Link& after = chain[(i + 1) % count];
            const Point2 at = corner(before, after);
            (before.forward ? before.end : before.start) = at;
            (after.forward ? after.start : after.end) = at;
        }

        bool dimensioned = false;
        for (Link& link : chain) {
            const Entity original = *sketch.find(link.curve.id);
            if (link.curve.isLine()) {
                link.made = work.addLine(link.start, link.end);
                work.addConstraint(ConstraintKind::Parallel, original.id, link.made);
                if (!dimensioned) {
                    work.addConstraint(ConstraintKind::PointLineDistance,
                                       work.find(link.made)->start, original.id, 0, distance);
                    dimensioned = true;
                }
            } else if (link.curve.kind == EntityKind::Circle) {
                link.made = work.addCircle(link.curve.center, link.radius);
                work.addConstraint(ConstraintKind::Concentric, original.id, link.made);
            } else {
                link.made = work.addArc(link.curve.center, link.start, link.end);
                work.addConstraint(ConstraintKind::Concentric, original.id, link.made);
            }
            work.setConstruction(link.made, link.construction);
            made.push_back(link.made);
        }
        for (std::size_t i = 0; i + 1 < count || (closed && i < count); ++i) {
            const Entity before = *work.find(chain[i].made);
            const Entity after = *work.find(chain[(i + 1) % count].made);
            work.addConstraint(ConstraintKind::Coincident,
                               chain[i].forward ? before.end : before.start,
                               chain[(i + 1) % count].forward ? after.start : after.end);
        }
    }
    work.solve();
    sketch = std::move(work);
    return made;
}

// ---------------------------------------------------------------------------------------------
// Trim and extend

bool trim(Sketch& sketch, EntityId id, Point2 nearPoint) {
    const Entity* found = sketch.find(id);
    // Ellipses and splines are not trimmed: where they cross other curves is not worked out.
    if (!isCurve(found) || found->fixed || isFreeform(found)) {
        return false;
    }
    const Entity entity = *found;
    const Curve curve = describe(sketch, entity);
    const double speed = std::max(curve.speed(), minimumLength);
    const double margin = 10.0 * joinTolerance / speed;
    const std::vector<Cut> cuts = cutsOf(sketch, curve);
    const auto cutter = [&sketch](const Cut& cut) { return *sketch.find(cut.by); };

    if (curve.kind == EntityKind::Circle) {
        // Distinct places around the circle where something crosses it.
        std::vector<Cut> around;
        for (const Cut& cut : cuts) {
            if (around.empty() || cut.parameter - around.back().parameter > margin) {
                around.push_back(cut);
            }
        }
        if (around.size() > 1 &&
            around.front().parameter + 2.0 * pi - around.back().parameter <= margin) {
            around.pop_back();
        }
        if (around.empty()) {
            sketch.remove(id);
            return true;
        }
        if (around.size() < 2) {
            return false;
        }
        // The part to go lies between the crossings on either side of `near`.
        const double where = curve.parameterOf(nearPoint);
        std::size_t after = 0;
        while (after < around.size() && around[after].parameter <= where) {
            ++after;
        }
        const Cut& upper = around[after % around.size()];
        const Cut& lower = around[(after + around.size() - 1) % around.size()];
        // What stays runs counter-clockwise from the upper crossing round to the lower one.
        Parts parts(sketch);
        const Point2 startAt = curve.at(upper.parameter);
        const Point2 endAt = curve.at(lower.parameter);
        Entity& round = parts.at(id);
        round.kind = EntityKind::Arc;
        round.radius = 0.0;
        const EntityId start = parts.addPoint(startAt, id);
        const EntityId end = parts.addPoint(endAt, id);
        parts.at(id).start = start;
        parts.at(id).end = end;
        const Entity upperCutter = cutter(upper);
        const Entity lowerCutter = cutter(lower);
        attach(parts, start, describe(sketch, upperCutter), upperCutter, startAt);
        attach(parts, end, describe(sketch, lowerCutter), lowerCutter, endAt);
        Sketch work = parts.build();
        work.solve();
        sketch = std::move(work);
        return true;
    }

    // A line or an arc: the crossings strictly between its ends.
    const double span = curve.span();
    std::vector<Cut> inside;
    for (const Cut& cut : cuts) {
        if (cut.parameter > margin && cut.parameter < span - margin) {
            inside.push_back(cut);
        }
    }
    if (inside.empty()) {
        sketch.remove(id);
        return true;
    }
    double where = curve.parameterOf(nearPoint);
    if (!curve.isLine() && where > span) {
        // Beyond the arc: nearer to its start or to its end?
        where = (where - span) < (2.0 * pi - where) ? span : 0.0;
    }
    where = std::clamp(where, 0.0, span);
    const Cut* lower = nullptr;
    const Cut* upper = nullptr;
    for (const Cut& cut : inside) {
        if (cut.parameter <= where) {
            lower = &cut;
        } else if (upper == nullptr) {
            upper = &cut;
        }
    }

    Parts parts(sketch);
    const auto hold = [&](EntityId point, const Cut& cut) {
        const Entity by = cutter(cut);
        attach(parts, point, describe(sketch, by), by, curve.at(cut.parameter));
    };
    if (lower != nullptr && upper != nullptr) {
        // The middle goes. The curve keeps its start and ends at the lower crossing; a new
        // curve runs from the upper crossing to where the end was, and takes over whatever
        // was attached to that end.
        const Point2 oldEnd = curve.b;
        EntityId rest = 0;
        EntityId restStart = 0;
        EntityId restEnd = 0;
        if (curve.isLine()) {
            rest = parts.addLine(curve.at(upper->parameter), oldEnd, entity.construction);
        } else {
            rest =
                parts.addArc(curve.center, curve.at(upper->parameter), oldEnd, entity.construction);
        }
        restStart = parts.at(rest).start;
        restEnd = parts.at(rest).end;
        parts.releaseLength(id);
        parts.hand(entity.end, restEnd);
        parts.place(entity.end, curve.at(lower->parameter));
        hold(entity.end, *lower);
        hold(restStart, *upper);
        if (curve.isLine()) {
            parts.constrain(ConstraintKind::Collinear, id, rest);
        } else {
            parts.constrain(ConstraintKind::Concentric, id, rest);
            parts.constrain(ConstraintKind::Equal, id, rest);
        }
    } else {
        // One end goes, back to the crossing next to it.
        const Cut& cut = lower != nullptr ? *lower : *upper;
        const EntityId moved = lower != nullptr ? entity.end : entity.start;
        parts.releaseLength(id);
        parts.release(moved);
        parts.place(moved, curve.at(cut.parameter));
        hold(moved, cut);
    }
    Sketch work = parts.build();
    work.solve();
    sketch = std::move(work);
    return true;
}

bool extend(Sketch& sketch, EntityId id, Point2 nearPoint) {
    const Entity* found = sketch.find(id);
    if (found == nullptr || found->fixed ||
        (found->kind != EntityKind::Line && found->kind != EntityKind::Arc)) {
        return false;
    }
    const Entity entity = *found;
    const Curve curve = describe(sketch, entity);
    const double speed = std::max(curve.speed(), minimumLength);
    const double margin = 10.0 * joinTolerance / speed;
    const double span = curve.span();
    const bool atEnd = apart(nearPoint, curve.b) <= apart(nearPoint, curve.a);

    // The nearest crossing beyond the chosen end.
    const Cut* target = nullptr;
    double reach = 0.0;
    const std::vector<Cut> cuts = cutsOf(sketch, curve);
    for (const Cut& cut : cuts) {
        double beyond = 0.0;
        if (curve.isLine()) {
            beyond = atEnd ? cut.parameter - 1.0 : -cut.parameter;
        } else {
            // Around the rest of the circle, counter-clockwise from the end or clockwise
            // from the start.
            beyond = atEnd ? cut.parameter - span : 2.0 * pi - cut.parameter;
            if (beyond >= 2.0 * pi - span - margin) {
                continue;
            }
        }
        if (beyond > margin && (target == nullptr || beyond < reach)) {
            target = &cut;
            reach = beyond;
        }
    }
    if (target == nullptr) {
        return false;
    }
    const Entity by = *sketch.find(target->by);
    const Point2 where = curve.at(target->parameter);
    const EntityId moved = atEnd ? entity.end : entity.start;
    Parts parts(sketch);
    parts.releaseLength(id);
    parts.release(moved);
    parts.place(moved, where);
    attach(parts, moved, describe(sketch, by), by, where);
    Sketch work = parts.build();
    work.solve();
    sketch = std::move(work);
    return true;
}

} // namespace lcad::sketch
