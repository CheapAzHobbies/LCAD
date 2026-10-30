// SPDX-License-Identifier: GPL-3.0-or-later

// The constraint solver.
//
// Every free coordinate of the sketch (the X and Y of each point, the radius of each circle) is
// an unknown. Every constraint is one or two equations in those unknowns that are zero when the
// constraint is met. Solving is Newton's method on the whole set: at each step the equations
// are linearized, and of all the changes to the unknowns that would satisfy the linearized
// equations the smallest one is taken, so that geometry which does not have to move stays put.
//
// The same linearization answers the questions a sketcher asks. Its rank says how many degrees
// of freedom are left; its null space says which unknowns can still move; and an equation that
// adds nothing to the rank is redundant, or, if it cannot be met as well, in conflict.

#include <lcad/sketch/Sketch.h>

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lcad::sketch {

// Lets this file reach the entities a Sketch keeps to itself.
struct SolverAccess {
    static std::vector<Entity>& entities(Sketch& sketch) { return sketch.m_entities; }
    static const std::vector<Entity>& entities(const Sketch& sketch) { return sketch.m_entities; }
};

namespace {

// The most unknowns one equation involves: four points and two radii.
constexpr std::size_t maxLocal = 10;
constexpr double pi = 3.14159265358979323846;

// How close to zero every equation has to come, in millimeters or radians.
constexpr double solvedTolerance = 1e-9;
// Below this share of the largest, a pivot or a remainder counts as zero when judging rank.
constexpr double rankTolerance = 1e-8;
constexpr int maximumIterations = 60;

// A number together with its derivatives with respect to an equation's unknowns. Writing the
// equations with these gives exact derivatives without working them out by hand.
struct Dual {
    double v = 0.0;
    std::array<double, maxLocal> d{};

    Dual() = default;
    Dual(double value) : v(value) {} // NOLINT: constants convert on purpose
};

Dual operator+(const Dual& a, const Dual& b) {
    Dual r(a.v + b.v);
    for (std::size_t i = 0; i < maxLocal; ++i) {
        r.d[i] = a.d[i] + b.d[i];
    }
    return r;
}

Dual operator-(const Dual& a, const Dual& b) {
    Dual r(a.v - b.v);
    for (std::size_t i = 0; i < maxLocal; ++i) {
        r.d[i] = a.d[i] - b.d[i];
    }
    return r;
}

Dual operator*(const Dual& a, const Dual& b) {
    Dual r(a.v * b.v);
    for (std::size_t i = 0; i < maxLocal; ++i) {
        r.d[i] = a.d[i] * b.v + a.v * b.d[i];
    }
    return r;
}

Dual operator/(const Dual& a, const Dual& b) {
    Dual r(a.v / b.v);
    for (std::size_t i = 0; i < maxLocal; ++i) {
        r.d[i] = (a.d[i] * b.v - a.v * b.d[i]) / (b.v * b.v);
    }
    return r;
}

Dual squareRoot(const Dual& a) {
    // A length of exactly zero has no direction to differentiate along; nudge it.
    const double root = std::sqrt(std::max(a.v, 1e-300));
    Dual r(root);
    for (std::size_t i = 0; i < maxLocal; ++i) {
        r.d[i] = a.d[i] / (2.0 * root);
    }
    return r;
}

Dual arcTangent(const Dual& y, const Dual& x) {
    Dual r(std::atan2(y.v, x.v));
    const double squared = std::max(x.v * x.v + y.v * y.v, 1e-300);
    for (std::size_t i = 0; i < maxLocal; ++i) {
        r.d[i] = (x.v * y.d[i] - y.v * x.d[i]) / squared;
    }
    return r;
}

struct Vec {
    Dual x;
    Dual y;
};

Vec operator-(const Vec& a, const Vec& b) {
    return {a.x - b.x, a.y - b.y};
}
Dual cross(const Vec& a, const Vec& b) {
    return a.x * b.y - a.y * b.x;
}
Dual dot(const Vec& a, const Vec& b) {
    return a.x * b.x + a.y * b.y;
}
Dual length(const Vec& a) {
    return squareRoot(dot(a, a));
}

// Where a point's coordinates sit among an equation's own variables.
struct PointSlots {
    int x = 0;
    int y = 0;
};
Vec at(const Dual* local, PointSlots slots) {
    return {local[slots.x], local[slots.y]};
}

// A circle or an arc: its center, and its radius either as a variable or as the distance from
// the center to the arc's start.
struct RoundSlots {
    PointSlots center;
    bool isArc = false;
    PointSlots start;
    int radius = 0;
};
Dual radiusOf(const Dual* local, const RoundSlots& round) {
    return round.isArc ? length(at(local, round.start) - at(local, round.center))
                       : local[round.radius];
}

// The equations of one constraint (or of one rule of the geometry itself): which unknowns they
// involve, and how to evaluate them.
struct Block {
    ConstraintId source = 0; // 0 for a rule that is not a constraint
    int rows = 0;
    int count = 0;
    std::array<int, maxLocal> unknown{};     // index among the unknowns, or -1 for a constant
    std::array<double, maxLocal> constant{}; // its value, when it is a constant
    std::function<void(const Dual*, Dual*)> evaluate;
};

double sign(double value) {
    return value < 0.0 ? -1.0 : 1.0;
}

double wrapAngle(double angle) {
    while (angle > pi) {
        angle -= 2.0 * pi;
    }
    while (angle <= -pi) {
        angle += 2.0 * pi;
    }
    return angle;
}

// The sketch as a system of equations.
class System {
public:
    explicit System(const Sketch& sketch) : m_sketch(sketch) {
        for (const Entity& entity : SolverAccess::entities(sketch)) {
            if (entity.fixed) {
                continue;
            }
            if (entity.kind == EntityKind::Point) {
                // A point of a fixed curve is fixed with it.
                const Entity* owner = entity.owner != 0 ? sketch.find(entity.owner) : nullptr;
                if (owner != nullptr && owner->fixed) {
                    continue;
                }
                m_index[key(entity.id, 0)] = static_cast<int>(m_values.size());
                m_values.push_back(entity.x);
                m_index[key(entity.id, 1)] = static_cast<int>(m_values.size());
                m_values.push_back(entity.y);
            } else if (entity.kind == EntityKind::Circle || entity.kind == EntityKind::Ellipse) {
                m_index[key(entity.id, 2)] = static_cast<int>(m_values.size());
                m_values.push_back(entity.radius);
            }
        }
        // Which points constraints hold together, for endOn().
        for (const Constraint& constraint : sketch.constraints()) {
            if (constraint.kind == ConstraintKind::Coincident) {
                const EntityId a = spot(constraint.first);
                const EntityId b = spot(constraint.second);
                if (a != b) {
                    m_joined[a] = b;
                }
            }
        }
        // An arc's two ends are the same distance from its center.
        for (const Entity& entity : SolverAccess::entities(sketch)) {
            if (entity.kind != EntityKind::Arc) {
                continue;
            }
            Block block;
            const PointSlots center = point(block, entity.center);
            const PointSlots start = point(block, entity.start);
            const PointSlots end = point(block, entity.end);
            block.rows = 1;
            block.evaluate = [=](const Dual* local, Dual* out) {
                out[0] = length(at(local, start) - at(local, center)) -
                         length(at(local, end) - at(local, center));
            };
            m_rules.push_back(std::move(block));
        }
    }

    [[nodiscard]] int unknowns() const { return static_cast<int>(m_values.size()); }
    [[nodiscard]] Eigen::VectorXd values() const {
        return Eigen::Map<const Eigen::VectorXd>(m_values.data(), unknowns());
    }
    [[nodiscard]] const std::vector<Block>& rules() const { return m_rules; }

    /// The index of an unknown, or -1 if that coordinate is not one.
    [[nodiscard]] int indexOf(EntityId id, int component) const {
        const auto found = m_index.find(key(id, component));
        return found == m_index.end() ? -1 : found->second;
    }

    /// The equations for a constraint. Where an equation has two mirror-image forms (which side
    /// of a line, inside or outside a circle), the form the geometry is in now is the one kept.
    [[nodiscard]] Block blockFor(const Constraint& c) const {
        Block block;
        block.source = c.id;
        const Entity* first = m_sketch.find(c.first);
        const Entity* second = m_sketch.find(c.second);
        const double value = c.value;

        switch (c.kind) {
        case ConstraintKind::Coincident: {
            const PointSlots a = point(block, c.first);
            const PointSlots b = point(block, c.second);
            block.rows = 2;
            block.evaluate = [=](const Dual* local, Dual* out) {
                out[0] = local[a.x] - local[b.x];
                out[1] = local[a.y] - local[b.y];
            };
            break;
        }
        case ConstraintKind::PointOnLine: {
            const PointSlots p = point(block, c.first);
            const auto [a, b] = line(block, c.second);
            block.rows = 1;
            block.evaluate = [=](const Dual* local, Dual* out) {
                const Vec d = at(local, b) - at(local, a);
                out[0] = cross(d, at(local, p) - at(local, a)) / length(d);
            };
            break;
        }
        case ConstraintKind::PointOnCircle: {
            const PointSlots p = point(block, c.first);
            const RoundSlots round = this->round(block, c.second);
            block.rows = 1;
            block.evaluate = [=](const Dual* local, Dual* out) {
                out[0] = length(at(local, p) - at(local, round.center)) - radiusOf(local, round);
            };
            break;
        }
        case ConstraintKind::Horizontal:
        case ConstraintKind::Vertical: {
            PointSlots a;
            PointSlots b;
            if (first->kind == EntityKind::Line) {
                std::tie(a, b) = line(block, c.first);
            } else {
                a = point(block, c.first);
                b = point(block, c.second);
            }
            const bool horizontal = c.kind == ConstraintKind::Horizontal;
            block.rows = 1;
            block.evaluate = [=](const Dual* local, Dual* out) {
                out[0] = horizontal ? local[a.y] - local[b.y] : local[a.x] - local[b.x];
            };
            break;
        }
        case ConstraintKind::Parallel:
        case ConstraintKind::Perpendicular: {
            const auto [a1, b1] = line(block, c.first);
            const auto [a2, b2] = line(block, c.second);
            const bool parallel = c.kind == ConstraintKind::Parallel;
            block.rows = 1;
            block.evaluate = [=](const Dual* local, Dual* out) {
                const Vec d1 = at(local, b1) - at(local, a1);
                const Vec d2 = at(local, b2) - at(local, a2);
                out[0] = (parallel ? cross(d1, d2) : dot(d1, d2)) / (length(d1) * length(d2));
            };
            break;
        }
        case ConstraintKind::Tangent: {
            if (first->kind == EntityKind::Line || second->kind == EntityKind::Line) {
                const bool lineFirst = first->kind == EntityKind::Line;
                const Entity& straight = lineFirst ? *first : *second;
                const Entity& curved = lineFirst ? *second : *first;
                if (const EntityId shared = endOn(straight, curved); shared != 0) {
                    // The line ends on the circle. There, "as far from the center as the
                    // radius" stops telling a tangent line from one that is slightly turned:
                    // the distance is at its largest. Ask instead for the line to be a quarter
                    // turn from the radius at that end, to the side it is on now: the angle
                    // itself, which is zero for one direction of the line only.
                    const auto [a, b] = line(block, straight.id);
                    const PointSlots center = point(block, curved.center);
                    const PointSlots touch = point(block, shared);
                    const Point2 pa = m_sketch.position(straight.start);
                    const Point2 pb = m_sketch.position(straight.end);
                    const Point2 pc = m_sketch.position(curved.center);
                    const Point2 pe = m_sketch.position(shared);
                    const double quarter =
                        sign((pe.x - pc.x) * (pb.y - pa.y) - (pe.y - pc.y) * (pb.x - pa.x)) * pi /
                        2.0;
                    block.rows = 1;
                    block.evaluate = [=](const Dual* local, Dual* out) {
                        const Vec d = at(local, b) - at(local, a);
                        const Vec radius = at(local, touch) - at(local, center);
                        Dual angle = arcTangent(cross(radius, d), dot(radius, d));
                        angle.v = wrapAngle(angle.v - quarter);
                        out[0] = angle;
                    };
                    break;
                }
                const auto [a, b] = line(block, lineFirst ? c.first : c.second);
                const RoundSlots round = this->round(block, lineFirst ? c.second : c.first);
                // The side of the line the center is on now.
                const Entity& l = lineFirst ? *first : *second;
                const Point2 pa = m_sketch.position(l.start);
                const Point2 pb = m_sketch.position(l.end);
                const Point2 pc = m_sketch.position((lineFirst ? second : first)->center);
                const double side =
                    sign((pb.x - pa.x) * (pc.y - pa.y) - (pb.y - pa.y) * (pc.x - pa.x));
                block.rows = 1;
                block.evaluate = [=](const Dual* local, Dual* out) {
                    const Vec d = at(local, b) - at(local, a);
                    out[0] =
                        Dual(side) * cross(d, at(local, round.center) - at(local, a)) / length(d) -
                        radiusOf(local, round);
                };
            } else {
                EntityId shared = endOn(*first, *second);
                if (shared == 0) {
                    shared = endOn(*second, *first);
                }
                if (shared != 0) {
                    // The curves meet at an end of one of them. They are tangent there when
                    // the two centers and that end lie on one line: seen from the end, the
                    // centers are in the same direction or in opposite ones, whichever they
                    // are nearer to now.
                    const PointSlots c1 = point(block, first->center);
                    const PointSlots c2 = point(block, second->center);
                    const PointSlots touch = point(block, shared);
                    const Point2 pe = m_sketch.position(shared);
                    const Point2 p1 = m_sketch.position(first->center);
                    const Point2 p2 = m_sketch.position(second->center);
                    const bool sameSide =
                        (pe.x - p1.x) * (pe.x - p2.x) + (pe.y - p1.y) * (pe.y - p2.y) > 0.0;
                    const double target = sameSide ? 0.0 : pi;
                    block.rows = 1;
                    block.evaluate = [=](const Dual* local, Dual* out) {
                        const Vec toFirst = at(local, touch) - at(local, c1);
                        const Vec toSecond = at(local, touch) - at(local, c2);
                        Dual angle = arcTangent(cross(toFirst, toSecond), dot(toFirst, toSecond));
                        angle.v = wrapAngle(angle.v - target);
                        out[0] = angle;
                    };
                    break;
                }
                const RoundSlots r1 = this->round(block, c.first);
                const RoundSlots r2 = this->round(block, c.second);
                // Touching from outside or from inside, whichever is nearer now.
                const Point2 c1 = m_sketch.position(first->center);
                const Point2 c2 = m_sketch.position(second->center);
                const double apart = std::hypot(c2.x - c1.x, c2.y - c1.y);
                const double radius1 = m_sketch.radiusOf(c.first);
                const double radius2 = m_sketch.radiusOf(c.second);
                const bool outside = std::abs(apart - (radius1 + radius2)) <=
                                     std::abs(apart - std::abs(radius1 - radius2));
                const double larger = sign(radius1 - radius2);
                block.rows = 1;
                block.evaluate = [=](const Dual* local, Dual* out) {
                    const Dual between = length(at(local, r2.center) - at(local, r1.center));
                    const Dual ra = radiusOf(local, r1);
                    const Dual rb = radiusOf(local, r2);
                    out[0] = outside ? between - (ra + rb) : between - Dual(larger) * (ra - rb);
                };
            }
            break;
        }
        case ConstraintKind::Equal: {
            if (first->kind == EntityKind::Line) {
                const auto [a1, b1] = line(block, c.first);
                const auto [a2, b2] = line(block, c.second);
                block.rows = 1;
                block.evaluate = [=](const Dual* local, Dual* out) {
                    out[0] = length(at(local, b1) - at(local, a1)) -
                             length(at(local, b2) - at(local, a2));
                };
            } else {
                const RoundSlots r1 = this->round(block, c.first);
                const RoundSlots r2 = this->round(block, c.second);
                block.rows = 1;
                block.evaluate = [=](const Dual* local, Dual* out) {
                    out[0] = radiusOf(local, r1) - radiusOf(local, r2);
                };
            }
            break;
        }
        case ConstraintKind::Concentric: {
            const PointSlots a = point(block, first->center);
            const PointSlots b = point(block, second->center);
            block.rows = 2;
            block.evaluate = [=](const Dual* local, Dual* out) {
                out[0] = local[a.x] - local[b.x];
                out[1] = local[a.y] - local[b.y];
            };
            break;
        }
        case ConstraintKind::Midpoint: {
            const PointSlots p = point(block, c.first);
            const auto [a, b] = line(block, c.second);
            block.rows = 2;
            block.evaluate = [=](const Dual* local, Dual* out) {
                out[0] = local[p.x] - (local[a.x] + local[b.x]) * Dual(0.5);
                out[1] = local[p.y] - (local[a.y] + local[b.y]) * Dual(0.5);
            };
            break;
        }
        case ConstraintKind::Symmetric: {
            const PointSlots p = point(block, c.first);
            const PointSlots q = point(block, c.second);
            const auto [a, b] = line(block, c.third);
            block.rows = 2;
            block.evaluate = [=](const Dual* local, Dual* out) {
                const Vec d = at(local, b) - at(local, a);
                const Vec middle{(local[p.x] + local[q.x]) * Dual(0.5),
                                 (local[p.y] + local[q.y]) * Dual(0.5)};
                // The middle of the two is on the line, and the line through them crosses it
                // squarely.
                out[0] = cross(d, middle - at(local, a)) / length(d);
                out[1] = dot(d, at(local, q) - at(local, p)) / length(d);
            };
            break;
        }
        case ConstraintKind::Collinear: {
            const auto [a1, b1] = line(block, c.first);
            const auto [a2, b2] = line(block, c.second);
            block.rows = 2;
            block.evaluate = [=](const Dual* local, Dual* out) {
                const Vec d1 = at(local, b1) - at(local, a1);
                const Vec d2 = at(local, b2) - at(local, a2);
                out[0] = cross(d1, d2) / (length(d1) * length(d2));
                out[1] = cross(d1, at(local, a2) - at(local, a1)) / length(d1);
            };
            break;
        }
        case ConstraintKind::Fix: {
            const PointSlots p = point(block, c.first);
            const double x = c.value;
            const double y = c.value2;
            block.rows = 2;
            block.evaluate = [=](const Dual* local, Dual* out) {
                out[0] = local[p.x] - Dual(x);
                out[1] = local[p.y] - Dual(y);
            };
            break;
        }
        case ConstraintKind::Distance: {
            const PointSlots p = point(block, c.first);
            const PointSlots q = point(block, c.second);
            block.rows = 1;
            block.evaluate = [=](const Dual* local, Dual* out) {
                out[0] = length(at(local, q) - at(local, p)) - Dual(value);
            };
            break;
        }
        case ConstraintKind::DistanceX:
        case ConstraintKind::DistanceY: {
            const PointSlots p = point(block, c.first);
            const PointSlots q = point(block, c.second);
            const bool alongX = c.kind == ConstraintKind::DistanceX;
            const Point2 from = m_sketch.position(c.first);
            const Point2 to = m_sketch.position(c.second);
            // The second point stays on the side of the first it is on now.
            const double side = sign(alongX ? to.x - from.x : to.y - from.y);
            block.rows = 1;
            block.evaluate = [=](const Dual* local, Dual* out) {
                const Dual apart = alongX ? local[q.x] - local[p.x] : local[q.y] - local[p.y];
                out[0] = Dual(side) * apart - Dual(value);
            };
            break;
        }
        case ConstraintKind::PointLineDistance: {
            const PointSlots p = point(block, c.first);
            const auto [a, b] = line(block, c.second);
            const Point2 pa = m_sketch.position(second->start);
            const Point2 pb = m_sketch.position(second->end);
            const Point2 pp = m_sketch.position(c.first);
            const double side = sign((pb.x - pa.x) * (pp.y - pa.y) - (pb.y - pa.y) * (pp.x - pa.x));
            block.rows = 1;
            block.evaluate = [=](const Dual* local, Dual* out) {
                const Vec d = at(local, b) - at(local, a);
                out[0] =
                    Dual(side) * cross(d, at(local, p) - at(local, a)) / length(d) - Dual(value);
            };
            break;
        }
        case ConstraintKind::Radius:
        case ConstraintKind::Diameter: {
            const RoundSlots round = this->round(block, c.first);
            const double radius = c.kind == ConstraintKind::Radius ? value : value / 2.0;
            block.rows = 1;
            block.evaluate = [=](const Dual* local, Dual* out) {
                out[0] = radiusOf(local, round) - Dual(radius);
            };
            break;
        }
        case ConstraintKind::Angle: {
            const auto [a1, b1] = line(block, c.first);
            const auto [a2, b2] = line(block, c.second);
            block.rows = 1;
            block.evaluate = [=](const Dual* local, Dual* out) {
                const Vec d1 = at(local, b1) - at(local, a1);
                const Vec d2 = at(local, b2) - at(local, a2);
                // The angle itself, not its sine: its sine is also zero half a turn away.
                Dual angle = arcTangent(cross(d1, d2), dot(d1, d2));
                angle.v = wrapAngle(angle.v - value);
                out[0] = angle;
            };
            break;
        }
        }
        return block;
    }

    /// Writes values of the unknowns back into the sketch.
    void store(const Eigen::VectorXd& x, Sketch& sketch) const {
        for (Entity& entity : SolverAccess::entities(sketch)) {
            if (entity.kind == EntityKind::Point) {
                const int ix = indexOf(entity.id, 0);
                if (ix >= 0) {
                    entity.x = x[ix];
                    entity.y = x[ix + 1];
                }
            } else if (entity.kind == EntityKind::Circle || entity.kind == EntityKind::Ellipse) {
                const int ir = indexOf(entity.id, 2);
                if (ir >= 0) {
                    entity.radius = x[ir];
                }
            }
        }
    }

private:
    static std::uint64_t key(EntityId id, int component) {
        return (static_cast<std::uint64_t>(id) << 2) | static_cast<std::uint64_t>(component);
    }

    // The place of one coordinate among a block's own variables, adding it if it is new there.
    int slot(Block& block, EntityId id, int component, double current) const {
        const int unknown = indexOf(id, component);
        for (int i = 0; i < block.count; ++i) {
            if (block.unknown[static_cast<std::size_t>(i)] == unknown && unknown >= 0) {
                return i;
            }
        }
        const auto place = static_cast<std::size_t>(block.count);
        block.unknown[place] = unknown;
        block.constant[place] = current;
        return block.count++;
    }

    // The representative of the points a point is held together with by Coincident
    // constraints.
    EntityId spot(EntityId point) const {
        for (auto found = m_joined.find(point); found != m_joined.end();
             found = m_joined.find(point)) {
            point = found->second;
        }
        return point;
    }

    // An end of `curve` (a line or an arc) that constraints keep on `round`: one that
    // coincides with an end of that arc, or has a PointOnCircle constraint to it. 0 if none.
    EntityId endOn(const Entity& curve, const Entity& round) const {
        if (curve.kind != EntityKind::Line && curve.kind != EntityKind::Arc) {
            return 0;
        }
        for (const EntityId end : {curve.start, curve.end}) {
            const EntityId here = spot(end);
            if (round.kind == EntityKind::Arc &&
                (here == spot(round.start) || here == spot(round.end))) {
                return end;
            }
            for (const Constraint& constraint : m_sketch.constraints()) {
                if (constraint.kind == ConstraintKind::PointOnCircle &&
                    constraint.second == round.id && spot(constraint.first) == here) {
                    return end;
                }
            }
        }
        return 0;
    }

    PointSlots point(Block& block, EntityId id) const {
        const Point2 at = m_sketch.position(id);
        return {slot(block, id, 0, at.x), slot(block, id, 1, at.y)};
    }

    std::pair<PointSlots, PointSlots> line(Block& block, EntityId id) const {
        const Entity* entity = m_sketch.find(id);
        return {point(block, entity->start), point(block, entity->end)};
    }

    RoundSlots round(Block& block, EntityId id) const {
        const Entity* entity = m_sketch.find(id);
        RoundSlots slots;
        slots.center = point(block, entity->center);
        slots.isArc = entity->kind == EntityKind::Arc;
        if (slots.isArc) {
            slots.start = point(block, entity->start);
        } else {
            slots.radius = slot(block, id, 2, entity->radius);
        }
        return slots;
    }

    const Sketch& m_sketch;
    std::unordered_map<EntityId, EntityId> m_joined;
    std::unordered_map<std::uint64_t, int> m_index;
    std::vector<double> m_values;
    std::vector<Block> m_rules;
};

// Evaluates the equations of `blocks` at `x`: their values in `r`, and, if asked, their
// derivatives in `jacobian`.
void evaluate(const std::vector<const Block*>& blocks, const Eigen::VectorXd& x, Eigen::VectorXd& r,
              Eigen::MatrixXd* jacobian) {
    int rows = 0;
    for (const Block* block : blocks) {
        rows += block->rows;
    }
    r.resize(rows);
    if (jacobian != nullptr) {
        jacobian->setZero(rows, x.size());
    }
    int row = 0;
    std::array<Dual, maxLocal> local;
    std::array<Dual, 2> out;
    for (const Block* block : blocks) {
        for (int i = 0; i < block->count; ++i) {
            const auto place = static_cast<std::size_t>(i);
            const int unknown = block->unknown[place];
            local[place] = Dual(unknown >= 0 ? x[unknown] : block->constant[place]);
            if (unknown >= 0) {
                local[place].d[place] = 1.0;
            }
        }
        block->evaluate(local.data(), out.data());
        for (int k = 0; k < block->rows; ++k) {
            const Dual& value = out[static_cast<std::size_t>(k)];
            r[row] = value.v;
            if (jacobian != nullptr) {
                for (int i = 0; i < block->count; ++i) {
                    const int unknown = block->unknown[static_cast<std::size_t>(i)];
                    if (unknown >= 0) {
                        (*jacobian)(row, unknown) += value.d[static_cast<std::size_t>(i)];
                    }
                }
            }
            ++row;
        }
    }
}

double largest(const Eigen::VectorXd& values) {
    return values.size() == 0 ? 0.0 : values.cwiseAbs().maxCoeff();
}

struct Attempt {
    bool converged = false;
    Eigen::VectorXd x;
};

// Newton's method. `reluctance` says, per unknown, how unwilling it is to move: of all the
// steps that satisfy the linearized equations, the one taken is the smallest when each
// unknown's share is weighted by it. Dragged points are made reluctant to leave the pointer.
Attempt newton(const std::vector<const Block*>& blocks, Eigen::VectorXd x,
               const Eigen::VectorXd& reluctance) {
    Eigen::VectorXd r;
    Eigen::MatrixXd jacobian;
    const Eigen::VectorXd scale = reluctance.cwiseSqrt().cwiseInverse();
    if (blocks.empty() || x.size() == 0) {
        evaluate(blocks, x, r, nullptr);
        return {largest(r) < solvedTolerance, x};
    }
    for (int iteration = 0; iteration < maximumIterations; ++iteration) {
        evaluate(blocks, x, r, &jacobian);
        const double error = largest(r);
        if (error < solvedTolerance) {
            return {true, x};
        }
        const Eigen::MatrixXd scaled = jacobian * scale.asDiagonal();
        Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd> decomposition(scaled);
        decomposition.setThreshold(rankTolerance);
        const Eigen::VectorXd step = scale.asDiagonal() * decomposition.solve(-r);
        if (!step.allFinite() || step.norm() < 1e-15) {
            break;
        }
        // Shorten the step until it makes things better, not worse.
        double fraction = 1.0;
        Eigen::VectorXd trial;
        Eigen::VectorXd trialValues;
        bool improved = false;
        for (int halving = 0; halving < 12; ++halving) {
            trial = x + fraction * step;
            evaluate(blocks, trial, trialValues, nullptr);
            if (trialValues.allFinite() && trialValues.norm() < r.norm()) {
                improved = true;
                break;
            }
            fraction *= 0.5;
        }
        if (!improved) {
            break;
        }
        x = trial;
    }
    evaluate(blocks, x, r, nullptr);
    return {largest(r) < solvedTolerance, x};
}

int rankOf(const Eigen::MatrixXd& matrix) {
    if (matrix.rows() == 0 || matrix.cols() == 0) {
        return 0;
    }
    Eigen::ColPivHouseholderQR<Eigen::MatrixXd> qr(matrix);
    qr.setThreshold(rankTolerance);
    return static_cast<int>(qr.rank());
}

// The whole of solving: meets what can be met, and says what could not and what is left free.
SolveResult solveSketch(Sketch& sketch, const System& system, Eigen::VectorXd start,
                        const Eigen::VectorXd& reluctance) {
    SolveResult result;
    std::vector<Block> constraintBlocks;
    for (const Constraint& constraint : sketch.constraints()) {
        constraintBlocks.push_back(system.blockFor(constraint));
    }
    std::vector<const Block*> all;
    for (const Block& rule : system.rules()) {
        all.push_back(&rule);
    }
    for (const Block& block : constraintBlocks) {
        all.push_back(&block);
    }

    std::vector<const Block*> accepted = all;
    Attempt attempt = newton(all, start, reluctance);
    if (!attempt.converged) {
        // Something contradicts something else. Take the constraints in the order they were
        // added, and leave out each one that cannot be met together with those before it.
        accepted.clear();
        for (const Block& rule : system.rules()) {
            accepted.push_back(&rule);
        }
        attempt = newton(accepted, start, reluctance);
        if (!attempt.converged) {
            attempt.x = start;
        }
        for (const Block& block : constraintBlocks) {
            std::vector<const Block*> trial = accepted;
            trial.push_back(&block);
            const Attempt next = newton(trial, attempt.x, reluctance);
            if (next.converged) {
                accepted = std::move(trial);
                attempt.x = next.x;
            } else {
                result.conflicting.push_back(block.source);
            }
        }
        result.status = SolveStatus::Conflicting;
    }
    system.store(attempt.x, sketch);

    // What the accepted equations leave free, from their derivatives at the solution.
    Eigen::VectorXd r;
    Eigen::MatrixXd jacobian;
    evaluate(accepted, attempt.x, r, &jacobian);
    const int unknowns = system.unknowns();
    const int rank = rankOf(jacobian);
    result.degreesOfFreedom = unknowns - rank;

    // A constraint whose equations all follow from those before it adds nothing.
    if (rank < jacobian.rows()) {
        std::vector<Eigen::VectorXd> basis;
        int row = 0;
        for (const Block* block : accepted) {
            bool addsSomething = false;
            for (int k = 0; k < block->rows; ++k, ++row) {
                Eigen::VectorXd remainder = jacobian.row(row).transpose();
                const double size = remainder.norm();
                // Twice, because one pass of subtraction leaves rounding behind.
                for (int pass = 0; pass < 2; ++pass) {
                    for (const Eigen::VectorXd& direction : basis) {
                        remainder -= direction.dot(remainder) * direction;
                    }
                }
                if (size > 0.0 && remainder.norm() > rankTolerance * std::max(1.0, size) * 10.0) {
                    basis.push_back(remainder.normalized());
                    addsSomething = true;
                }
            }
            if (!addsSomething && block->source != 0) {
                result.redundant.push_back(block->source);
            }
        }
        if (!result.redundant.empty() && result.status == SolveStatus::Solved) {
            result.status = SolveStatus::Redundant;
        }
    }

    // Which unknowns can still move: those with a share in the directions the equations leave
    // open.
    std::vector<bool> isFree(static_cast<std::size_t>(unknowns), result.degreesOfFreedom > 0);
    if (result.degreesOfFreedom > 0 && jacobian.rows() > 0) {
        Eigen::FullPivLU<Eigen::MatrixXd> lu(jacobian);
        lu.setThreshold(rankTolerance);
        const Eigen::MatrixXd open = lu.kernel();
        for (int i = 0; i < unknowns; ++i) {
            isFree[static_cast<std::size_t>(i)] = open.row(i).norm() > 1e-7;
        }
    }
    std::unordered_set<EntityId> free;
    for (const Entity& entity : sketch.entities()) {
        const auto canMove = [&](EntityId id, int component) {
            const int index = system.indexOf(id, component);
            return index >= 0 && isFree[static_cast<std::size_t>(index)];
        };
        const bool moves =
            entity.kind == EntityKind::Point
                ? canMove(entity.id, 0) || canMove(entity.id, 1)
                : (entity.kind == EntityKind::Circle || entity.kind == EntityKind::Ellipse) &&
                      canMove(entity.id, 2);
        if (moves) {
            // A curve's point speaks for the curve.
            free.insert(entity.owner != 0 ? entity.owner : entity.id);
        }
    }
    result.underconstrained.assign(free.begin(), free.end());
    std::sort(result.underconstrained.begin(), result.underconstrained.end());
    return result;
}

} // namespace

SolveResult Sketch::solve() {
    const System system(*this);
    return solveSketch(*this, system, system.values(), Eigen::VectorXd::Ones(system.unknowns()));
}

SolveResult Sketch::state() const {
    Sketch copy = *this;
    return copy.solve();
}

SolveResult Sketch::dragBy(EntityId id, double dx, double dy) {
    const Entity* dragged = find(id);
    if (dragged == nullptr || !std::isfinite(dx) || !std::isfinite(dy)) {
        throw std::invalid_argument("dragBy: there is nothing to drag, or nowhere to drag it");
    }
    const System system(*this);
    Eigen::VectorXd start = system.values();
    Eigen::VectorXd reluctance = Eigen::VectorXd::Ones(system.unknowns());
    const auto move = [&](EntityId point) {
        const int index = system.indexOf(point, 0);
        if (index >= 0) {
            start[index] += dx;
            start[index + 1] += dy;
            // Stay with the pointer if the constraints allow any choice in the matter.
            reluctance[index] = 1e6;
            reluctance[index + 1] = 1e6;
        }
    };
    if (dragged->kind == EntityKind::Point) {
        move(id);
    } else {
        for (const Entity& entity : m_entities) {
            if (entity.owner == id) {
                move(entity.id);
            }
        }
    }
    const Sketch before = *this;
    const SolveResult result = solveSketch(*this, system, start, reluctance);
    if (result.status == SolveStatus::Conflicting && before.state().status != result.status) {
        // The drag led somewhere the constraints cannot follow. Stay where it was.
        *this = before;
        return state();
    }
    return result;
}

SolveResult Sketch::dragRadius(EntityId circle, Point2 toward) {
    const Entity* dragged = find(circle);
    if (dragged == nullptr ||
        (dragged->kind != EntityKind::Circle && dragged->kind != EntityKind::Ellipse)) {
        throw std::invalid_argument("dragRadius: entity " + std::to_string(circle) +
                                    " is not a circle or an ellipse");
    }
    const System system(*this);
    Eigen::VectorXd start = system.values();
    Eigen::VectorXd reluctance = Eigen::VectorXd::Ones(system.unknowns());
    const int index = system.indexOf(circle, 2);
    const Point2 center = position(dragged->center);
    double wanted = std::hypot(toward.x - center.x, toward.y - center.y);
    if (dragged->kind == EntityKind::Ellipse) {
        // The minor radius: how far from the major axis the pointer is.
        const Point2 major = position(dragged->start);
        const double a = std::hypot(major.x - center.x, major.y - center.y);
        wanted = std::abs((major.x - center.x) * (toward.y - center.y) -
                          (major.y - center.y) * (toward.x - center.x)) /
                 a;
    }
    if (index >= 0 && wanted > 1e-7) {
        start[index] = wanted;
        reluctance[index] = 1e6;
    }
    const Sketch before = *this;
    const SolveResult result = solveSketch(*this, system, start, reluctance);
    if (result.status == SolveStatus::Conflicting && before.state().status != result.status) {
        *this = before;
        return state();
    }
    return result;
}

} // namespace lcad::sketch
