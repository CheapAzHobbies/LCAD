// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/drawing/Sheet.h>

#include <lcad/kernel/Error.h>
#include <lcad/kernel/Transform.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <sstream>
#include <utility>

namespace lcad::drawing {

using document::DimensionKind;
using document::Drawing;
using document::DrawingDimension;
using document::DrawingPoint;
using document::DrawingStandard;
using document::DrawingView;
using document::ObjectId;
using document::SheetSize;
using document::ViewKind;
using kernel::EdgeSummary;
using kernel::FlatRegion;
using kernel::Plane;
using kernel::ProjectedCurve;
using kernel::Projection;
using kernel::Vector;
using kernel::ViewFrame;

namespace {

constexpr double pi = 3.14159265358979323846;

// Millimeters on the sheet.
constexpr double margin = 10.0;
constexpr double titleWidth = 180.0;
constexpr double titleHeight = 32.0;
constexpr double hatchSpacing = 2.5;
constexpr double arrowLength = 3.0;
constexpr double arrowWidth = 1.0;
constexpr double textHeight = 3.5;
constexpr double extensionGap = 1.0;
constexpr double extensionPast = 2.0;
constexpr double centerMarkPast = 2.0;

Vector crossOf(const Vector& a, const Vector& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vector negated(const Vector& v) {
    return {-v.x, -v.y, -v.z};
}

double dotOf(const Vector& a, const Vector& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Point2D operator+(const Point2D& a, const Point2D& b) {
    return {a.x + b.x, a.y + b.y};
}

Point2D operator-(const Point2D& a, const Point2D& b) {
    return {a.x - b.x, a.y - b.y};
}

Point2D operator*(const Point2D& a, double factor) {
    return {a.x * factor, a.y * factor};
}

double dotOf(const Point2D& a, const Point2D& b) {
    return a.x * b.x + a.y * b.y;
}

double lengthOf(const Point2D& a) {
    return std::hypot(a.x, a.y);
}

const DrawingView* findView(const Drawing& drawing, ObjectId id) {
    for (const DrawingView& view : drawing.views) {
        if (view.id == id) {
            return &view;
        }
    }
    return nullptr;
}

ViewFrame frameOfKind(ViewKind kind) {
    const Vector x{1.0, 0.0, 0.0};
    const Vector y{0.0, 1.0, 0.0};
    const Vector z{0.0, 0.0, 1.0};
    switch (kind) {
    case ViewKind::Back:
        return {negated(x), z};
    case ViewKind::Top:
        return {x, y};
    case ViewKind::Bottom:
        return {x, negated(y)};
    case ViewKind::Left:
        return {negated(y), z};
    case ViewKind::Right:
        return {y, z};
    case ViewKind::Isometric: {
        // From the front, the right, and above, as the 3D view's home.
        const double a = std::sqrt(0.5);
        const double b = std::sqrt(1.0 / 6.0);
        return {{a, a, 0.0}, {-b, b, 2.0 * b}};
    }
    default:
        return {x, z};
    }
}

// The side of the cutting line a section's viewer stands on. Unless flipped they look left at
// a line up the parent, and up at a line across it, which shows the section the right way up.
Vector sectionNormal(const ViewFrame& parent, const DrawingView& view) {
    const Vector side = view.sectionVertical ? parent.right : negated(parent.up);
    return view.sectionFlipped ? negated(side) : side;
}

// Where a view looks from. A section is seen square to its cutting line; a detail as its parent.
std::optional<ViewFrame> frameOf(const Drawing& drawing, const DrawingView& view, int depth = 0) {
    if (view.kind != ViewKind::Section && view.kind != ViewKind::Detail) {
        return frameOfKind(view.kind);
    }
    const DrawingView* parent = findView(drawing, view.parent);
    if (parent == nullptr || depth > 8) {
        return std::nullopt;
    }
    const auto from = frameOf(drawing, *parent, depth + 1);
    if (!from.has_value() || view.kind == ViewKind::Detail) {
        return from;
    }
    const Vector normal = sectionNormal(*from, view);
    if (view.sectionVertical) {
        return ViewFrame{crossOf(from->up, normal), from->up};
    }
    // Upright: what faced the parent's viewer is up.
    return ViewFrame{view.sectionFlipped ? negated(from->right) : from->right,
                     crossOf(from->right, from->up)};
}

Plane sectionPlane(const ViewFrame& parent, const ViewFrame& frame, const DrawingView& view) {
    const Vector along = view.sectionVertical ? parent.right : parent.up;
    const kernel::Point origin{along.x * view.sectionPosition, along.y * view.sectionPosition,
                               along.z * view.sectionPosition};
    return kernel::makePlane(origin, sectionNormal(parent, view), frame.right);
}

// A view's lines, in the view's own coordinates, in millimeters of the model.
struct ViewLines {
    Projection projection;
    std::vector<FlatRegion> regions;
    bool hasBounds = false;
    double left = 0.0;
    double bottom = 0.0;
    double right = 0.0;
    double top = 0.0;

    void take(const Point2D& p) {
        if (!hasBounds) {
            left = right = p.x;
            bottom = top = p.y;
            hasBounds = true;
            return;
        }
        left = std::min(left, p.x);
        right = std::max(right, p.x);
        bottom = std::min(bottom, p.y);
        top = std::max(top, p.y);
    }
    void takeAll(const std::vector<ProjectedCurve>& curves) {
        for (const ProjectedCurve& curve : curves) {
            for (const Point2D& p : curve.points) {
                take(p);
            }
        }
    }
    [[nodiscard]] Point2D middle() const { return {(left + right) / 2.0, (bottom + top) / 2.0}; }
};

// From a view's own coordinates to the sheet.
struct Mapping {
    Point2D origin;
    double scale = 1.0;
    Point2D at;

    [[nodiscard]] Point2D operator()(const Point2D& p) const { return at + (p - origin) * scale; }
};

// One body as it appears in the document.
struct Part {
    ObjectId body;
    std::vector<ObjectId> path;
    kernel::Placement placement;
    kernel::Shape shape;
    std::vector<EdgeSummary> edges;
};

// The points of a curve that lie inside a circle, as the pieces they make.
std::vector<std::vector<Point2D>> insideCircle(const std::vector<Point2D>& points,
                                               const Point2D& center, double radius) {
    std::vector<std::vector<Point2D>> pieces;
    std::vector<Point2D> piece;
    const auto inside = [&](const Point2D& p) { return lengthOf(p - center) <= radius; };
    // The parameters along a segment where it crosses the circle.
    const auto crossings = [&](const Point2D& a, const Point2D& b) {
        const Point2D d = b - a;
        const Point2D f = a - center;
        const double qa = dotOf(d, d);
        const double qb = 2.0 * dotOf(f, d);
        const double qc = dotOf(f, f) - radius * radius;
        std::vector<double> found;
        const double discriminant = qb * qb - 4.0 * qa * qc;
        if (qa > 0.0 && discriminant >= 0.0) {
            const double root = std::sqrt(discriminant);
            for (const double t : {(-qb - root) / (2.0 * qa), (-qb + root) / (2.0 * qa)}) {
                if (t > 0.0 && t < 1.0) {
                    found.push_back(t);
                }
            }
        }
        return found;
    };
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const Point2D& a = points[i];
        const Point2D& b = points[i + 1];
        if (piece.empty() && inside(a)) {
            piece.push_back(a);
        }
        bool in = inside(a);
        for (const double t : crossings(a, b)) {
            const Point2D crossing = a + (b - a) * t;
            if (in) {
                piece.push_back(crossing);
                pieces.push_back(std::move(piece));
                piece.clear();
            } else {
                piece.push_back(crossing);
            }
            in = !in;
        }
        if (in) {
            piece.push_back(b);
        }
    }
    if (piece.size() > 1) {
        pieces.push_back(std::move(piece));
    }
    return pieces;
}

// The hatch lines across a region, at 45 degrees: where a line crosses the outlines an odd
// number of times from its start, it is inside.
std::vector<std::pair<Point2D, Point2D>> hatchLines(const std::vector<std::vector<Point2D>>& loops,
                                                    double spacing) {
    const Point2D along{std::sqrt(0.5), std::sqrt(0.5)};
    const Point2D across{-along.y, along.x};
    double lowest = 1e300;
    double highest = -1e300;
    for (const auto& loop : loops) {
        for (const Point2D& p : loop) {
            const double at = dotOf(p, across);
            lowest = std::min(lowest, at);
            highest = std::max(highest, at);
        }
    }
    std::vector<std::pair<Point2D, Point2D>> lines;
    // Lines at whole multiples of the spacing, so that they do not move when the outline does.
    for (double level = std::ceil(lowest / spacing) * spacing; level <= highest; level += spacing) {
        std::vector<double> crossings;
        for (const auto& loop : loops) {
            for (std::size_t i = 0; i < loop.size(); ++i) {
                const Point2D& a = loop[i];
                const Point2D& b = loop[(i + 1) % loop.size()];
                const double from = dotOf(a, across) - level;
                const double to = dotOf(b, across) - level;
                if ((from < 0.0) != (to < 0.0)) {
                    const Point2D crossing = a + (b - a) * (from / (from - to));
                    crossings.push_back(dotOf(crossing, along));
                }
            }
        }
        std::sort(crossings.begin(), crossings.end());
        for (std::size_t i = 0; i + 1 < crossings.size(); i += 2) {
            lines.emplace_back(along * crossings[i] + across * level,
                               along * crossings[i + 1] + across * level);
        }
    }
    return lines;
}

Stroke lineOf(Pen pen, const Point2D& a, const Point2D& b) {
    Stroke stroke;
    stroke.pen = pen;
    stroke.points = {a, b};
    return stroke;
}

Stroke circleOf(Pen pen, const Point2D& center, double radius) {
    Stroke stroke;
    stroke.pen = pen;
    stroke.isArc = true;
    stroke.center = center;
    stroke.radius = radius;
    stroke.startAngle = 0.0;
    stroke.endAngle = 2.0 * pi;
    const int pieces = std::clamp(static_cast<int>(radius * 4.0), 24, 180);
    for (int i = 0; i <= pieces; ++i) {
        const double angle = 2.0 * pi * i / pieces;
        stroke.points.push_back(
            {center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)});
    }
    return stroke;
}

void addArrow(Sheet& sheet, const Point2D& tip, const Point2D& direction) {
    const double length = lengthOf(direction);
    if (length < 1e-9) {
        return;
    }
    const Point2D d = direction * (1.0 / length);
    const Point2D n{-d.y, d.x};
    const Point2D back = tip - d * arrowLength;
    sheet.filled.push_back({{tip, back + n * arrowWidth, back - n * arrowWidth}});
}

void addLabel(Sheet& sheet, std::string text, const Point2D& at, double height,
              Label::Align align = Label::Align::Left, bool bold = false, double angle = 0.0) {
    Label label;
    label.text = std::move(text);
    label.at = at;
    label.height = height;
    label.align = align;
    label.bold = bold;
    label.angle = angle;
    sheet.labels.push_back(std::move(label));
}

// One box of the title block, with its caption above its value.
void addField(Sheet& sheet, double left, double top, double width, double height,
              const std::string& caption, const std::string& value, double valueHeight = 3.5) {
    sheet.strokes.push_back(lineOf(Pen::Frame, {left, top}, {left + width, top}));
    sheet.strokes.push_back(lineOf(Pen::Frame, {left, top - height}, {left + width, top - height}));
    sheet.strokes.push_back(lineOf(Pen::Frame, {left, top}, {left, top - height}));
    sheet.strokes.push_back(lineOf(Pen::Frame, {left + width, top}, {left + width, top - height}));
    addLabel(sheet, caption, {left + 1.0, top - 3.0}, 1.8);
    addLabel(sheet, value, {left + 2.0, top - height + 1.8}, valueHeight);
}

// The sign of the projection a drawing uses: a cone seen from the side and from its end.
void addProjectionSymbol(Sheet& sheet, const Point2D& middle, DrawingStandard standard) {
    const double inner = 1.5;
    const double outer = 2.6;
    const double half = 3.0;
    // First-angle has the circles on the right of the cone, third-angle on the left.
    const double cone = standard == DrawingStandard::Iso ? -5.0 : 5.0;
    const Point2D c{middle.x + cone, middle.y};
    const Point2D rings{middle.x - cone, middle.y};
    sheet.strokes.push_back(
        lineOf(Pen::Frame, {c.x - half, c.y - inner}, {c.x + half, c.y - outer}));
    sheet.strokes.push_back(
        lineOf(Pen::Frame, {c.x - half, c.y + inner}, {c.x + half, c.y + outer}));
    sheet.strokes.push_back(
        lineOf(Pen::Frame, {c.x - half, c.y - inner}, {c.x - half, c.y + inner}));
    sheet.strokes.push_back(
        lineOf(Pen::Frame, {c.x + half, c.y - outer}, {c.x + half, c.y + outer}));
    sheet.strokes.push_back(circleOf(Pen::Frame, rings, inner));
    sheet.strokes.push_back(circleOf(Pen::Frame, rings, outer));
}

void addFrameAndTitle(Sheet& sheet, const Drawing& drawing) {
    const double left = margin;
    const double bottom = margin;
    const double right = sheet.width - margin;
    const double top = sheet.height - margin;
    const std::array<Point2D, 5> corners{
        Point2D{left, bottom}, {right, bottom}, {right, top}, {left, top}, {left, bottom}};
    Stroke frame;
    frame.pen = Pen::Frame;
    frame.points.assign(corners.begin(), corners.end());
    sheet.strokes.push_back(std::move(frame));

    const document::TitleBlock& title = drawing.titleBlock;
    const double x = right - titleWidth;
    const double row = titleHeight / 3.0;
    const double y = bottom + titleHeight;
    addField(sheet, x, y, 110.0, row * 2.0, "Title", title.title, 6.0);
    addField(sheet, x + 110.0, y, 70.0, row, "Company", title.company);
    addField(sheet, x + 110.0, y - row, 50.0, row, "Part number", title.partNumber);
    addField(sheet, x + 160.0, y - row, 20.0, row, "Revision", title.revision);
    addField(sheet, x, y - row * 2.0, 40.0, row, "Drawn by", title.drawnBy);
    addField(sheet, x + 40.0, y - row * 2.0, 30.0, row, "Date", title.date);
    addField(sheet, x + 70.0, y - row * 2.0, 40.0, row, "Material", title.material);
    const std::string scale =
        drawing.views.empty() ? std::string() : scaleText(drawing.views.front().scale);
    addField(sheet, x + 110.0, y - row * 2.0, 20.0, row, "Scale", scale);
    addField(sheet, x + 130.0, y - row * 2.0, 20.0, row, "Sheet", toString(drawing.size));
    addField(sheet, x + 150.0, y - row * 2.0, 30.0, row, "Projection", {});
    // Below its caption.
    addProjectionSymbol(sheet, {x + 165.0, y - row * 2.5 - 1.2}, drawing.standard);
}

// A number with the decimals it needs.
std::string compactNumber(double value, int decimals) {
    // Not printf, whose point is a comma on a German system.
    char text[64];
    const auto end =
        std::to_chars(text, text + sizeof(text), value, std::chars_format::fixed, decimals).ptr;
    std::string out(text, end);
    if (out.find('.') != std::string::npos) {
        out.erase(out.find_last_not_of('0') + 1);
        if (out.back() == '.') {
            out.pop_back();
        }
    }
    return out;
}

} // namespace

SheetDimensions sheetDimensions(SheetSize size) {
    switch (size) {
    case SheetSize::A4:
        return {297.0, 210.0};
    case SheetSize::A3:
        return {420.0, 297.0};
    case SheetSize::A2:
        return {594.0, 420.0};
    case SheetSize::A1:
        return {841.0, 594.0};
    case SheetSize::A0:
        return {1189.0, 841.0};
    case SheetSize::Letter:
        return {279.4, 215.9};
    case SheetSize::Tabloid:
        return {431.8, 279.4};
    case SheetSize::AnsiC:
        return {558.8, 431.8};
    case SheetSize::AnsiD:
        return {863.6, 558.8};
    }
    return {420.0, 297.0};
}

double fittingScale(double modelWidth, double modelHeight, double roomWidth, double roomHeight) {
    for (const double scale :
         {10.0, 5.0, 2.0, 1.0, 0.5, 0.2, 0.1, 0.05, 0.02, 0.01, 0.005, 0.002, 0.001}) {
        if (modelWidth * scale <= roomWidth && modelHeight * scale <= roomHeight) {
            return scale;
        }
    }
    return 0.001;
}

std::string scaleText(double scale) {
    if (scale >= 1.0) {
        return compactNumber(scale, 2) + ":1";
    }
    return "1:" + compactNumber(1.0 / scale, 2);
}

std::string lengthText(double millimeters) {
    return compactNumber(millimeters, 2);
}

struct SheetBuilder::Impl {
    explicit Impl(const document::Document& model) : document(model) {}

    const document::Document& document;
    std::vector<Part> parts;
    bool partsKnown = false;
    kernel::Shape whole;
    std::map<std::string, ViewLines> kept;

    void forget() {
        parts.clear();
        partsKnown = false;
        whole = {};
        kept.clear();
    }

    // Each body shown, once for each place it appears.
    const std::vector<Part>& modelParts() {
        if (partsKnown) {
            return parts;
        }
        partsKnown = true;
        std::vector<kernel::Shape> shapes;
        for (const document::Instance& instance : document.instances()) {
            if (!instance.visible) {
                continue;
            }
            for (const document::Body& body : document.bodies()) {
                if (body.component != instance.component || body.shape.isNull() || !body.visible) {
                    continue;
                }
                Part part;
                part.body = body.id;
                part.path = instance.path;
                part.placement = instance.placement;
                part.shape = kernel::placed(body.shape, instance.placement);
                try {
                    part.edges = kernel::edgeSummaries(document.namedBody(body.id));
                } catch (const kernel::Error&) {
                    part.edges.clear();
                }
                shapes.push_back(part.shape);
                parts.push_back(std::move(part));
            }
        }
        whole = kernel::makeCompound(shapes);
        return parts;
    }

    // The lines of a view from `frame`, cut by `section` if there is one.
    const ViewLines& linesOf(const ViewFrame& frame, const std::optional<Plane>& section) {
        modelParts();
        std::ostringstream key;
        key.precision(12);
        key << frame.right.x << ' ' << frame.right.y << ' ' << frame.right.z << ' ' << frame.up.x
            << ' ' << frame.up.y << ' ' << frame.up.z;
        if (section.has_value()) {
            key << " cut " << section->origin.x << ' ' << section->origin.y << ' '
                << section->origin.z << ' ' << section->normal.x << ' ' << section->normal.y << ' '
                << section->normal.z;
        }
        const auto found = kept.find(key.str());
        if (found != kept.end()) {
            return found->second;
        }
        ViewLines lines;
        if (section.has_value()) {
            kernel::SectionCut cut = kernel::sectionBy(whole, *section, frame);
            lines.projection = kernel::project(cut.behind, frame);
            lines.regions = std::move(cut.regions);
        } else {
            lines.projection = kernel::project(whole, frame);
        }
        lines.takeAll(lines.projection.visible);
        lines.takeAll(lines.projection.hidden);
        lines.takeAll(lines.projection.smooth);
        for (const FlatRegion& region : lines.regions) {
            for (const Point2D& p : region.outer) {
                lines.take(p);
            }
        }
        return kept.emplace(key.str(), std::move(lines)).first->second;
    }

    [[nodiscard]] std::optional<EdgeSummary> edgeOf(const DrawingPoint& point,
                                                    kernel::Placement& placement) {
        for (const Part& part : modelParts()) {
            if (part.body != point.body || part.path != point.path) {
                continue;
            }
            for (const EdgeSummary& edge : part.edges) {
                if (edge.name == point.edge) {
                    placement = part.placement;
                    return edge;
                }
            }
        }
        return std::nullopt;
    }

    // Where a point a dimension hangs on is in the model, if it still has it.
    [[nodiscard]] std::optional<kernel::Point> pointOf(const DrawingPoint& point) {
        kernel::Placement placement;
        const auto edge = edgeOf(point, placement);
        if (!edge.has_value()) {
            return std::nullopt;
        }
        switch (point.where) {
        case DrawingPoint::Where::Start:
            return kernel::apply(placement, edge->start);
        case DrawingPoint::Where::End:
            return kernel::apply(placement, edge->end);
        case DrawingPoint::Where::Middle:
            return kernel::apply(placement, edge->middle);
        case DrawingPoint::Where::Center:
            if (!edge->isCircle) {
                return std::nullopt;
            }
            return kernel::apply(placement, edge->center);
        }
        return std::nullopt;
    }

    struct Placed {
        const DrawingView* view = nullptr;
        ViewFrame frame;
        Mapping mapping;
        std::optional<Plane> section;
    };

    void addCurves(Sheet& sheet, const std::vector<ProjectedCurve>& curves, Pen pen,
                   const Mapping& mapping, const DrawingView& view) {
        const bool detail = view.kind == ViewKind::Detail;
        const Point2D center{view.detailX, view.detailY};
        for (const ProjectedCurve& curve : curves) {
            if (detail) {
                for (const auto& piece : insideCircle(curve.points, center, view.detailRadius)) {
                    Stroke stroke;
                    stroke.pen = pen;
                    for (const Point2D& p : piece) {
                        stroke.points.push_back(mapping(p));
                    }
                    sheet.strokes.push_back(std::move(stroke));
                }
                continue;
            }
            Stroke stroke;
            stroke.pen = pen;
            for (const Point2D& p : curve.points) {
                stroke.points.push_back(mapping(p));
            }
            if (curve.isArc) {
                stroke.isArc = true;
                stroke.center = mapping(curve.center);
                stroke.radius = curve.radius * mapping.scale;
                stroke.startAngle = curve.startAngle;
                stroke.endAngle = curve.endAngle;
            }
            sheet.strokes.push_back(std::move(stroke));
        }
    }

    void addHatch(Sheet& sheet, const std::vector<FlatRegion>& regions, const Mapping& mapping,
                  const DrawingView& view) {
        const bool detail = view.kind == ViewKind::Detail;
        const Point2D center = mapping({view.detailX, view.detailY});
        const double radius = view.detailRadius * mapping.scale;
        for (const FlatRegion& region : regions) {
            std::vector<std::vector<Point2D>> loops;
            loops.emplace_back();
            for (const Point2D& p : region.outer) {
                loops.back().push_back(mapping(p));
            }
            for (const auto& hole : region.holes) {
                loops.emplace_back();
                for (const Point2D& p : hole) {
                    loops.back().push_back(mapping(p));
                }
            }
            for (const auto& [from, to] : hatchLines(loops, hatchSpacing)) {
                if (!detail) {
                    sheet.strokes.push_back(lineOf(Pen::Hatch, from, to));
                    continue;
                }
                for (const auto& piece : insideCircle({from, to}, center, radius)) {
                    sheet.strokes.push_back(lineOf(Pen::Hatch, piece.front(), piece.back()));
                }
            }
        }
    }

    // A center mark for each circle or half circle seen, once.
    void addCenterMarks(Sheet& sheet, const std::vector<ProjectedCurve>& curves,
                        const Mapping& mapping, const DrawingView& view) {
        std::vector<Point2D> marked;
        for (const ProjectedCurve& curve : curves) {
            if (!curve.isArc || curve.endAngle - curve.startAngle < pi - 1e-6) {
                continue;
            }
            if (view.kind == ViewKind::Detail &&
                lengthOf(curve.center - Point2D{view.detailX, view.detailY}) > view.detailRadius) {
                continue;
            }
            const Point2D center = mapping(curve.center);
            const bool done =
                std::any_of(marked.begin(), marked.end(), [&center](const Point2D& other) {
                    return lengthOf(other - center) < 1e-3;
                });
            if (done) {
                continue;
            }
            marked.push_back(center);
            const double reach = curve.radius * mapping.scale + centerMarkPast;
            sheet.strokes.push_back(
                lineOf(Pen::Center, {center.x - reach, center.y}, {center.x + reach, center.y}));
            sheet.strokes.push_back(
                lineOf(Pen::Center, {center.x, center.y - reach}, {center.x, center.y + reach}));
        }
    }

    void addSnaps(Sheet& sheet, const Placed& placed) {
        const DrawingView& view = *placed.view;
        if (view.kind == ViewKind::Isometric) {
            return;
        }
        const auto keep = [&](const kernel::Point& p) {
            if (placed.section.has_value()) {
                const Plane& plane = *placed.section;
                const Vector from{p.x - plane.origin.x, p.y - plane.origin.y, p.z - plane.origin.z};
                if (dotOf(from, plane.normal) > 1e-6) {
                    return false;
                }
            }
            if (view.kind == ViewKind::Detail) {
                const Point2D flat = kernel::projected(placed.frame, p);
                return lengthOf(flat - Point2D{view.detailX, view.detailY}) <= view.detailRadius;
            }
            return true;
        };
        for (const Part& part : modelParts()) {
            for (const EdgeSummary& edge : part.edges) {
                const auto add = [&](const kernel::Point& p, DrawingPoint::Where where) {
                    const kernel::Point inModel = kernel::apply(part.placement, p);
                    if (keep(inModel)) {
                        sheet.snapPoints.push_back(
                            {view.id,
                             {part.body, part.path, edge.name, where},
                             placed.mapping(kernel::projected(placed.frame, inModel))});
                    }
                };
                if (edge.isCircle) {
                    add(edge.center, DrawingPoint::Where::Center);
                    const kernel::Point center = kernel::apply(part.placement, edge.center);
                    // Round only in a view along its axis.
                    const Vector axis = kernel::apply(part.placement, edge.axis);
                    const double facing =
                        std::abs(kernel::depthOf(placed.frame, {axis.x, axis.y, axis.z}));
                    if (keep(center) && facing > 1.0 - 1e-6) {
                        const double closed =
                            std::hypot(edge.start.x - edge.end.x, edge.start.y - edge.end.y) +
                            std::abs(edge.start.z - edge.end.z);
                        sheet.snapCircles.push_back(
                            {view.id,
                             {part.body, part.path, edge.name, DrawingPoint::Where::Center},
                             placed.mapping(kernel::projected(placed.frame, center)),
                             edge.radius * placed.mapping.scale,
                             closed < 1e-6});
                    }
                }
                // Where a whole circle starts is only where its seam happens to be.
                const bool closedCircle =
                    edge.isCircle &&
                    std::hypot(edge.start.x - edge.end.x, edge.start.y - edge.end.y) +
                            std::abs(edge.start.z - edge.end.z) <
                        1e-6;
                if (edge.isCircle && !closedCircle) {
                    add(edge.start, DrawingPoint::Where::Start);
                    add(edge.end, DrawingPoint::Where::End);
                }
                if (edge.isLine) {
                    const kernel::Point from = kernel::apply(part.placement, edge.start);
                    const kernel::Point to = kernel::apply(part.placement, edge.end);
                    const Point2D a = placed.mapping(kernel::projected(placed.frame, from));
                    const Point2D b = placed.mapping(kernel::projected(placed.frame, to));
                    // An edge seen end on is a point, and has no length to give; what shows
                    // there belongs to what it runs away from.
                    if (lengthOf(b - a) > 1e-6) {
                        add(edge.start, DrawingPoint::Where::Start);
                        add(edge.end, DrawingPoint::Where::End);
                        add(edge.middle, DrawingPoint::Where::Middle);
                        if (keep(from) && keep(to)) {
                            sheet.snapEdges.push_back(
                                {view.id,
                                 {part.body, part.path, edge.name, DrawingPoint::Where::Start},
                                 {part.body, part.path, edge.name, DrawingPoint::Where::End},
                                 a,
                                 b});
                        }
                    }
                }
            }
        }
    }

    void addSection(Sheet& sheet, const Placed& parent, const DrawingView& view,
                    const ViewBox& box) {
        // The cutting line across the parent, with arrows the way it is looked at.
        const double position = view.sectionPosition;
        const Mapping& map = parent.mapping;
        Point2D from;
        Point2D to;
        if (view.sectionVertical) {
            const double x = map({position, 0.0}).x;
            from = {x, box.bottom - 6.0};
            to = {x, box.top + 6.0};
        } else {
            const double y = map({0.0, position}).y;
            from = {box.left - 6.0, y};
            to = {box.right + 6.0, y};
        }
        sheet.strokes.push_back(lineOf(Pen::Center, from, to));
        Point2D looking = view.sectionVertical ? Point2D{-1.0, 0.0} : Point2D{0.0, 1.0};
        if (view.sectionFlipped) {
            looking = looking * -1.0;
        }
        for (const Point2D& end : {from, to}) {
            const Point2D tail = end - looking * 8.0;
            sheet.strokes.push_back(lineOf(Pen::Dimension, tail, end));
            addArrow(sheet, end, looking);
            addLabel(sheet, view.label, tail - looking * 1.0 + Point2D{0.0, -1.5}, 5.0,
                     Label::Align::Center, true);
        }
    }

    Sheet build(const Drawing& drawing) {
        Sheet sheet;
        const SheetDimensions size = sheetDimensions(drawing.size);
        sheet.width = size.width;
        sheet.height = size.height;
        addFrameAndTitle(sheet, drawing);
        modelParts();

        std::map<ObjectId, Placed> placedViews;
        std::map<ObjectId, ViewBox> boxes;
        // Parents first: a section or a detail draws on its parent.
        std::vector<const DrawingView*> ordered;
        for (const DrawingView& view : drawing.views) {
            if (view.kind != ViewKind::Section && view.kind != ViewKind::Detail) {
                ordered.push_back(&view);
            }
        }
        for (const DrawingView& view : drawing.views) {
            if (view.kind == ViewKind::Section || view.kind == ViewKind::Detail) {
                ordered.push_back(&view);
            }
        }

        for (const DrawingView* viewPointer : ordered) {
            const DrawingView& view = *viewPointer;
            const auto frame = frameOf(drawing, view);
            if (!frame.has_value()) {
                sheet.problems.push_back("A view has lost the view it was taken from.");
                continue;
            }
            if (whole.isNull()) {
                sheet.problems.push_back("There is no body to show.");
                continue;
            }
            std::optional<Plane> section;
            const DrawingView* parent = findView(drawing, view.parent);
            if (view.kind == ViewKind::Section) {
                section = sectionPlane(*frameOf(drawing, *parent), *frame, view);
            } else if (view.kind == ViewKind::Detail) {
                // What the parent shows, cut as it is.
                if (parent->kind == ViewKind::Section) {
                    const DrawingView* grandparent = findView(drawing, parent->parent);
                    section = sectionPlane(*frameOf(drawing, *grandparent), *frame, *parent);
                }
            }
            const ViewLines* lines = nullptr;
            try {
                lines = &linesOf(*frame, section);
            } catch (const kernel::Error& error) {
                sheet.problems.push_back(view.kind == ViewKind::Section
                                             ? "Section " + view.label +
                                                   " does not pass through the model."
                                             : "A view could not be worked out: " + error.reason());
                continue;
            }
            Placed placed;
            placed.view = &view;
            placed.frame = *frame;
            placed.section = section;
            placed.mapping.scale = view.scale;
            placed.mapping.at = {view.x, view.y};
            placed.mapping.origin = view.kind == ViewKind::Detail
                                        ? Point2D{view.detailX, view.detailY}
                                        : lines->middle();
            const Mapping& map = placed.mapping;

            addCurves(sheet, lines->projection.visible, Pen::Visible, map, view);
            if (view.kind == ViewKind::Isometric) {
                addCurves(sheet, lines->projection.smooth, Pen::Thin, map, view);
            } else {
                addCurves(sheet, lines->projection.smooth, Pen::Visible, map, view);
            }
            if (view.showHidden) {
                addCurves(sheet, lines->projection.hidden, Pen::Hidden, map, view);
            }
            addHatch(sheet, lines->regions, map, view);
            if (view.centerMarks && view.kind != ViewKind::Isometric) {
                addCenterMarks(sheet, lines->projection.visible, map, view);
            }

            ViewBox box;
            box.view = view.id;
            box.origin = map.origin;
            box.x = view.x;
            box.y = view.y;
            box.scale = view.scale;
            if (view.kind == ViewKind::Detail) {
                const double radius = view.detailRadius * view.scale;
                box.left = view.x - radius;
                box.right = view.x + radius;
                box.bottom = view.y - radius;
                box.top = view.y + radius;
                sheet.strokes.push_back(circleOf(Pen::Thin, {view.x, view.y}, radius));
            } else {
                const Point2D low = map({lines->left, lines->bottom});
                const Point2D high = map({lines->right, lines->top});
                box.left = low.x;
                box.bottom = low.y;
                box.right = high.x;
                box.top = high.y;
            }

            if (view.kind == ViewKind::Section) {
                addLabel(sheet, "SECTION " + view.label + "-" + view.label,
                         {view.x, box.bottom - 8.0}, 4.0, Label::Align::Center, true);
                const auto parentPlaced = placedViews.find(view.parent);
                if (parentPlaced != placedViews.end()) {
                    addSection(sheet, parentPlaced->second, view, boxes[view.parent]);
                }
            } else if (view.kind == ViewKind::Detail) {
                addLabel(sheet, "DETAIL " + view.label + " (" + scaleText(view.scale) + ")",
                         {view.x, box.bottom - 8.0}, 4.0, Label::Align::Center, true);
                const auto parentPlaced = placedViews.find(view.parent);
                if (parentPlaced != placedViews.end()) {
                    const Mapping& onParent = parentPlaced->second.mapping;
                    const Point2D center = onParent({view.detailX, view.detailY});
                    const double radius = view.detailRadius * onParent.scale;
                    sheet.strokes.push_back(circleOf(Pen::Thin, center, radius));
                    addLabel(sheet, view.label,
                             center + Point2D{radius * 0.75 + 1.0, radius * 0.75 + 1.0}, 5.0,
                             Label::Align::Left, true);
                }
            }
            addSnaps(sheet, placed);
            boxes[view.id] = box;
            sheet.views.push_back(box);
            placedViews[view.id] = placed;
        }

        for (const DrawingDimension& dimension : drawing.dimensions) {
            const auto placed = placedViews.find(dimension.view);
            if (placed == placedViews.end() || !addDimension(sheet, dimension, placed->second)) {
                sheet.lostDimensions.push_back(dimension.id);
                continue;
            }
        }
        if (!sheet.lostDimensions.empty()) {
            const std::size_t lost = sheet.lostDimensions.size();
            sheet.problems.push_back(
                lost == 1 ? "1 dimension has lost what it measures, and is not shown."
                          : std::to_string(lost) +
                                " dimensions have lost what they measure, and are not shown.");
        }
        for (const document::DrawingNote& note : drawing.notes) {
            addLabel(sheet, note.text, {note.x, note.y}, note.height);
        }
        return sheet;
    }

    bool addDimension(Sheet& sheet, const DrawingDimension& dimension, const Placed& placed) {
        if (dimension.kind == DimensionKind::Radius || dimension.kind == DimensionKind::Diameter) {
            return addRoundDimension(sheet, dimension, placed);
        }
        const auto first = pointOf(dimension.first);
        const auto second = pointOf(dimension.second);
        if (!first.has_value() || !second.has_value()) {
            return false;
        }
        const Point2D a3 = kernel::projected(placed.frame, *first);
        const Point2D b3 = kernel::projected(placed.frame, *second);
        const Point2D a = placed.mapping(a3);
        const Point2D b = placed.mapping(b3);

        Point2D along{1.0, 0.0};
        if (dimension.kind == DimensionKind::Vertical) {
            along = {0.0, 1.0};
        } else if (dimension.kind == DimensionKind::Aligned) {
            const double length = lengthOf(b - a);
            if (length < 1e-9) {
                return false;
            }
            along = (b - a) * (1.0 / length);
        }
        const Point2D across{-along.y, along.x};
        const double side = dimension.offset >= 0.0 ? 1.0 : -1.0;
        const double start = side > 0.0 ? std::max(dotOf(a, across), dotOf(b, across))
                                        : std::min(dotOf(a, across), dotOf(b, across));
        const double level = start + dimension.offset;
        const Point2D lineA = a + across * (level - dotOf(a, across));
        const Point2D lineB = b + across * (level - dotOf(b, across));
        const double value = std::abs(dotOf(b3 - a3, along));

        sheet.strokes.push_back(lineOf(Pen::Dimension, a + across * (side * extensionGap),
                                       lineA + across * (side * extensionPast)));
        sheet.strokes.push_back(lineOf(Pen::Dimension, b + across * (side * extensionGap),
                                       lineB + across * (side * extensionPast)));
        sheet.strokes.push_back(lineOf(Pen::Dimension, lineA, lineB));
        const Point2D between = lineB - lineA;
        if (lengthOf(between) > arrowLength * 2.5) {
            addArrow(sheet, lineA, between * -1.0);
            addArrow(sheet, lineB, between);
        } else {
            // No room inside: the arrows point in from outside.
            addArrow(sheet, lineA, between);
            addArrow(sheet, lineB, between * -1.0);
        }

        double angle = std::atan2(along.y, along.x);
        Point2D lift = across;
        if (angle > pi / 2.0 + 1e-9 || angle <= -pi / 2.0 + 1e-9) {
            angle += angle > 0.0 ? -pi : pi;
            lift = lift * -1.0;
        }
        const Point2D textAt = (lineA + lineB) * 0.5 + lift * 1.0;
        std::string text = lengthText(value) + dimension.suffix;
        addLabel(sheet, text, textAt, textHeight, Label::Align::Center, false, angle);
        sheet.dimensions.push_back({dimension.id, value, std::move(text), textAt});
        return true;
    }

    bool addRoundDimension(Sheet& sheet, const DrawingDimension& dimension, const Placed& placed) {
        kernel::Placement placement;
        const auto edge = edgeOf(dimension.first, placement);
        if (!edge.has_value() || !edge->isCircle) {
            return false;
        }
        const Point2D center =
            placed.mapping(kernel::projected(placed.frame, kernel::apply(placement, edge->center)));
        const double radius = edge->radius * placed.mapping.scale;
        const Point2D out{std::sqrt(0.5), std::sqrt(0.5)};
        const Point2D rim = center + out * radius;
        const Point2D end = rim + out * std::max(dimension.offset, 4.0);
        const Point2D shelf = end + Point2D{8.0, 0.0};
        sheet.strokes.push_back(lineOf(Pen::Dimension, rim, end));
        sheet.strokes.push_back(lineOf(Pen::Dimension, end, shelf));
        addArrow(sheet, rim, out * -1.0);

        const bool diameter = dimension.kind == DimensionKind::Diameter;
        const double value = diameter ? edge->radius * 2.0 : edge->radius;
        std::string text = std::string(diameter ? "Ø" : "R") + lengthText(value) + dimension.suffix;
        const Point2D textAt = end + Point2D{0.5, 1.0};
        addLabel(sheet, text, textAt, textHeight);
        sheet.dimensions.push_back({dimension.id, value, std::move(text), textAt});
        return true;
    }
};

SheetBuilder::SheetBuilder(const document::Document& document)
    : m_impl(std::make_unique<Impl>(document)) {}

SheetBuilder::~SheetBuilder() = default;

Sheet SheetBuilder::build(const Drawing& drawing) {
    return m_impl->build(drawing);
}

void SheetBuilder::invalidate() {
    m_impl->forget();
}

void addStandardViews(document::Document& document, Drawing& drawing) {
    std::vector<kernel::Shape> shapes;
    for (const document::Instance& instance : document.instances()) {
        if (!instance.visible) {
            continue;
        }
        for (const document::Body& body : document.bodies()) {
            if (body.component == instance.component && !body.shape.isNull() && body.visible) {
                shapes.push_back(kernel::placed(body.shape, instance.placement));
            }
        }
    }
    const auto bounds = kernel::boundingBox(kernel::makeCompound(shapes));
    if (!bounds.has_value()) {
        return;
    }
    const double dx = bounds->max.x - bounds->min.x;
    const double dy = bounds->max.y - bounds->min.y;
    const double dz = bounds->max.z - bounds->min.z;

    const SheetDimensions size = sheetDimensions(drawing.size);
    const double gap = 25.0;
    const double left = margin + 10.0;
    const double bottom = margin + titleHeight + 12.0;
    const double right = size.width - margin - 10.0;
    const double top = size.height - margin - 10.0;
    // Two views across and two up, and to their side a third of the room for the isometric one.
    const double roomWidth = (right - left - gap) * 0.68;
    const double roomHeight = top - bottom - gap;
    const double scale = fittingScale(dx + dy, dz + dy, roomWidth, roomHeight);

    const double frontWidth = dx * scale;
    const double frontHeight = dz * scale;
    const double depth = dy * scale;
    const bool firstAngle = drawing.standard == DrawingStandard::Iso;

    const auto add = [&](ViewKind kind, double x, double y) {
        DrawingView view;
        view.id = document.newId();
        view.kind = kind;
        view.scale = scale;
        view.x = x;
        view.y = y;
        drawing.views.push_back(view);
    };
    double frontX = 0.0;
    double frontY = 0.0;
    if (firstAngle) {
        // The view from above goes below, and the view from the right to the left.
        frontX = left + depth + gap + frontWidth / 2.0;
        frontY = bottom + depth + gap + frontHeight / 2.0;
        add(ViewKind::Front, frontX, frontY);
        add(ViewKind::Top, frontX, bottom + depth / 2.0);
        add(ViewKind::Right, left + depth / 2.0, frontY);
    } else {
        // The view from above goes above, and the view from the right to the right.
        frontX = left + frontWidth / 2.0;
        frontY = bottom + frontHeight / 2.0;
        add(ViewKind::Front, frontX, frontY);
        add(ViewKind::Top, frontX, bottom + frontHeight + gap + depth / 2.0);
        add(ViewKind::Right, left + frontWidth + gap + depth / 2.0, frontY);
    }

    // The isometric view in what room is left, at the upper right.
    const double used =
        firstAngle ? left + depth + gap + frontWidth : left + frontWidth + gap + depth;
    const double isoRoomWidth = right - used - gap / 2.0;
    const double isoWidth = (dx + dy) * std::sqrt(0.5);
    const double isoHeight = (dx + dy) * std::sqrt(1.0 / 6.0) + dz * std::sqrt(2.0 / 3.0);
    if (isoRoomWidth > 20.0) {
        const double isoScale =
            std::min(scale, fittingScale(isoWidth, isoHeight, isoRoomWidth, (top - bottom) * 0.75));
        DrawingView view;
        view.id = document.newId();
        view.kind = ViewKind::Isometric;
        view.scale = isoScale;
        view.x = right - isoRoomWidth / 2.0;
        view.y = top - isoHeight * isoScale / 2.0 - 4.0;
        view.showHidden = false;
        view.centerMarks = false;
        drawing.views.push_back(view);
    }
}

} // namespace lcad::drawing
