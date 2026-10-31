// SPDX-License-Identifier: GPL-3.0-or-later

#include "Areas.h"

#include <clipper2/clipper.h>

#include <cmath>

namespace lcad::cam {

namespace {

// To a ten thousandth of a millimeter.
constexpr int precision = 4;
// How far a round corner of an offset may lie from the true circle, in millimeters.
constexpr double arcTolerance = 0.002;

Clipper2Lib::PathsD toPaths(const Area& area) {
    Clipper2Lib::PathsD paths;
    paths.reserve(area.size());
    for (const Loop& loop : area) {
        Clipper2Lib::PathD path;
        path.reserve(loop.size());
        for (const kernel::Point2D& p : loop) {
            path.emplace_back(p.x, p.y);
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

Area toArea(const Clipper2Lib::PathsD& paths) {
    Area area;
    area.reserve(paths.size());
    for (const Clipper2Lib::PathD& path : paths) {
        Loop loop;
        loop.reserve(path.size());
        for (const Clipper2Lib::PointD& p : path) {
            loop.push_back({p.x, p.y});
        }
        area.push_back(std::move(loop));
    }
    return area;
}

} // namespace

Area areaOf(const kernel::FlatRegion& region) {
    Clipper2Lib::PathsD outer = toPaths({region.outer});
    Clipper2Lib::PathsD holes;
    for (const auto& hole : region.holes) {
        holes.push_back(toPaths({hole}).front());
    }
    // However the loops run, what comes back runs as this file says it does.
    return toArea(Clipper2Lib::Difference(outer, holes, Clipper2Lib::FillRule::NonZero, precision));
}

Area offset(const Area& area, double distance) {
    return toArea(Clipper2Lib::InflatePaths(toPaths(area), distance, Clipper2Lib::JoinType::Round,
                                            Clipper2Lib::EndType::Polygon, 2.0, precision,
                                            arcTolerance));
}

double sizeOf(const Area& area) {
    return std::abs(Clipper2Lib::Area(toPaths(area)));
}

Area difference(const Area& area, const Area& removed) {
    return toArea(Clipper2Lib::Difference(toPaths(area), toPaths(removed),
                                          Clipper2Lib::FillRule::NonZero, precision));
}

Area swept(const std::vector<Loop>& paths, bool closed, double radius) {
    return toArea(Clipper2Lib::InflatePaths(toPaths(paths), radius, Clipper2Lib::JoinType::Round,
                                            closed ? Clipper2Lib::EndType::Joined
                                                   : Clipper2Lib::EndType::Round,
                                            2.0, precision, arcTolerance));
}

bool lineInside(const Area& area, const kernel::Point2D& from, const kernel::Point2D& to) {
    // A line that leaves the area leaves something of it behind when the area is taken away.
    Clipper2Lib::PathsD line{
        {Clipper2Lib::PointD(from.x, from.y), Clipper2Lib::PointD(to.x, to.y)}};
    Clipper2Lib::ClipperD clipper(precision);
    clipper.AddOpenSubject(line);
    clipper.AddClip(toPaths(area));
    Clipper2Lib::PathsD closedResult;
    Clipper2Lib::PathsD openResult;
    clipper.Execute(Clipper2Lib::ClipType::Difference, Clipper2Lib::FillRule::NonZero, closedResult,
                    openResult);
    double outside = 0.0;
    for (const Clipper2Lib::PathD& piece : openResult) {
        for (std::size_t i = 1; i < piece.size(); ++i) {
            outside += std::hypot(piece[i].x - piece[i - 1].x, piece[i].y - piece[i - 1].y);
        }
    }
    return outside < 1e-3;
}

bool pointInside(const Area& area, const kernel::Point2D& point) {
    int winding = 0;
    const Clipper2Lib::PointD at(point.x, point.y);
    for (const Clipper2Lib::PathD& path : toPaths(area)) {
        const auto where = Clipper2Lib::PointInPolygon(at, path);
        if (where == Clipper2Lib::PointInPolygonResult::IsOn) {
            return true;
        }
        if (where == Clipper2Lib::PointInPolygonResult::IsInside) {
            winding += Clipper2Lib::IsPositive(path) ? 1 : -1;
        }
    }
    return winding > 0;
}

} // namespace lcad::cam
