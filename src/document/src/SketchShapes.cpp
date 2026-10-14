// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/SketchShapes.h>

#include <lcad/kernel/Error.h>
#include <lcad/kernel/Plane.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace lcad::document {

kernel::Shape sketchShape(const SketchItem& sketch, SketchPart part) {
    const sketch::Sketch& geometry = sketch.geometry;
    const kernel::Plane& plane = sketch.plane;
    const bool construction = part == SketchPart::Construction;
    std::vector<kernel::Shape> parts;
    for (const sketch::Entity& entity : geometry.entities()) {
        if (entity.construction != construction || entity.id == geometry.origin()) {
            continue;
        }
        try {
            switch (entity.kind) {
            case sketch::EntityKind::Point:
                if (entity.owner == 0) {
                    parts.push_back(kernel::makeVertex(kernel::toWorld(plane, entity.x, entity.y)));
                }
                break;
            case sketch::EntityKind::Line: {
                const sketch::Point2 a = geometry.position(entity.start);
                const sketch::Point2 b = geometry.position(entity.end);
                parts.push_back(kernel::makeSegment(kernel::toWorld(plane, a.x, a.y),
                                                    kernel::toWorld(plane, b.x, b.y)));
                break;
            }
            case sketch::EntityKind::Circle: {
                const sketch::Point2 c = geometry.position(entity.center);
                parts.push_back(kernel::makeCircleEdge(plane, c.x, c.y, entity.radius));
                break;
            }
            case sketch::EntityKind::Arc: {
                const sketch::Point2 c = geometry.position(entity.center);
                const sketch::Point2 a = geometry.position(entity.start);
                const sketch::Point2 b = geometry.position(entity.end);
                parts.push_back(kernel::makeArcEdge(plane, c.x, c.y, a.x, a.y, b.x, b.y));
                break;
            }
            }
        } catch (const std::invalid_argument&) {
            // A curve that has shrunk to nothing has no shape. It is still in the sketch.
        }
    }
    return kernel::makeCompound(parts);
}

namespace {

// One curve of a sketch as an edge in model space.
kernel::Shape curveShape(const SketchItem& sketch, const sketch::Entity& entity) {
    const sketch::Sketch& geometry = sketch.geometry;
    const kernel::Plane& plane = sketch.plane;
    switch (entity.kind) {
    case sketch::EntityKind::Line: {
        const sketch::Point2 a = geometry.position(entity.start);
        const sketch::Point2 b = geometry.position(entity.end);
        return kernel::makeSegment(kernel::toWorld(plane, a.x, a.y),
                                   kernel::toWorld(plane, b.x, b.y));
    }
    case sketch::EntityKind::Circle: {
        const sketch::Point2 c = geometry.position(entity.center);
        return kernel::makeCircleEdge(plane, c.x, c.y, entity.radius);
    }
    case sketch::EntityKind::Arc: {
        const sketch::Point2 c = geometry.position(entity.center);
        const sketch::Point2 a = geometry.position(entity.start);
        const sketch::Point2 b = geometry.position(entity.end);
        return kernel::makeArcEdge(plane, c.x, c.y, a.x, a.y, b.x, b.y);
    }
    case sketch::EntityKind::Point:
        break;
    }
    throw std::invalid_argument("a point is not a curve");
}

} // namespace

kernel::Region kernelRegion(const SketchItem& sketch, const sketch::Region& region) {
    const auto outline = [&sketch](const std::vector<sketch::EntityId>& curves) {
        std::vector<kernel::OutlineCurve> made;
        for (const sketch::EntityId id : curves) {
            const sketch::Entity* entity = sketch.geometry.find(id);
            if (entity == nullptr) {
                throw std::invalid_argument("the outline names a curve the sketch does not have");
            }
            made.push_back({curveShape(sketch, *entity), std::to_string(id)});
        }
        return made;
    };
    kernel::Region made;
    made.outer = outline(region.outer);
    for (const std::vector<sketch::EntityId>& hole : region.holes) {
        made.holes.push_back(outline(hole));
    }
    return made;
}

std::vector<kernel::Shape> sketchRegionFaces(const SketchItem& sketch) {
    std::vector<kernel::Shape> faces;
    for (const sketch::Region& region : sketch.geometry.regions()) {
        try {
            faces.push_back(kernel::makeRegionFace(sketch.plane, kernelRegion(sketch, region)));
        } catch (const std::exception&) {
            faces.emplace_back();
        }
    }
    return faces;
}

std::optional<kernel::BoundingBox> sketchBounds(const SketchItem& sketch) {
    const sketch::Sketch& geometry = sketch.geometry;
    std::optional<kernel::BoundingBox> box;
    const auto include = [&](double u, double v) {
        const kernel::Point p = kernel::toWorld(sketch.plane, u, v);
        if (!box.has_value()) {
            box = kernel::BoundingBox{p, p};
            return;
        }
        box->min = {std::min(box->min.x, p.x), std::min(box->min.y, p.y),
                    std::min(box->min.z, p.z)};
        box->max = {std::max(box->max.x, p.x), std::max(box->max.y, p.y),
                    std::max(box->max.z, p.z)};
    };
    for (const sketch::Entity& entity : geometry.entities()) {
        if (entity.id == geometry.origin()) {
            continue;
        }
        if (entity.kind == sketch::EntityKind::Point) {
            include(entity.x, entity.y);
        } else if (entity.kind != sketch::EntityKind::Line) {
            // The whole circle, also for an arc: simple, and never too small.
            const sketch::Point2 c = geometry.position(entity.center);
            const double r = geometry.radiusOf(entity.id);
            include(c.x - r, c.y - r);
            include(c.x + r, c.y + r);
            include(c.x - r, c.y + r);
            include(c.x + r, c.y - r);
        }
    }
    return box;
}

} // namespace lcad::document
