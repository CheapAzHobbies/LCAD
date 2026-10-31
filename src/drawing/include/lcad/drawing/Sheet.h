// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>
#include <lcad/kernel/Projection.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

// What a sheet of a drawing shows, worked out from the model each time: a list of lines, filled
// shapes, and labels, in millimeters from the sheet's lower left corner. The screen, PDF files,
// and DXF files all draw the same list.
namespace lcad::drawing {

using kernel::Point2D;

/// The size of a sheet, lying on its long side, in millimeters.
struct SheetDimensions {
    double width = 0.0;
    double height = 0.0;
};

[[nodiscard]] SheetDimensions sheetDimensions(document::SheetSize size);

/// The kinds of line on a drawing.
enum class Pen {
    /// The sheet's border and the title block.
    Frame,
    /// What can be seen: thick.
    Visible,
    /// Thin lines: where faces run smoothly into each other, the edge of a detail.
    Thin,
    /// What cannot be seen: dashed.
    Hidden,
    /// Center lines and cutting lines: long and short dashes.
    Center,
    /// The lines across a cut face.
    Hatch,
    /// Dimensions and their leaders.
    Dimension,
};

/// A line on the sheet. `points` follows it; a circle or part of one also says so.
struct Stroke {
    Pen pen = Pen::Visible;
    std::vector<Point2D> points;
    bool isArc = false;
    Point2D center;
    double radius = 0.0;
    /// Counter-clockwise, in radians.
    double startAngle = 0.0;
    double endAngle = 0.0;
};

/// A filled shape: an arrowhead.
struct FilledShape {
    std::vector<Point2D> corners;
};

/// Text on the sheet.
struct Label {
    std::string text;
    /// The reference point of the text: on its baseline.
    Point2D at;
    /// How tall its capitals are, in millimeters.
    double height = 3.5;
    /// Counter-clockwise, in radians.
    double angle = 0.0;
    enum class Align { Left, Center, Right };
    Align align = Align::Left;
    bool bold = false;
};

/// The part of a sheet a view takes up.
struct ViewBox {
    document::ObjectId view;
    double left = 0.0;
    double bottom = 0.0;
    double right = 0.0;
    double top = 0.0;
};

/// A point of the model that a dimension can hang on, as it lies on the sheet.
struct SnapPoint {
    document::ObjectId view;
    document::DrawingPoint point;
    Point2D at;
};

/// A circle of the model on the sheet, for a radius or a diameter.
struct SnapCircle {
    document::ObjectId view;
    document::DrawingPoint point;
    Point2D center;
    /// On the sheet.
    double radius = 0.0;
};

/// A dimension as it came out.
struct PlacedDimension {
    document::ObjectId id;
    /// What it measures, in millimeters of the model.
    double value = 0.0;
    std::string text;
    /// Where its text is, for picking it.
    Point2D textAt;
};

/// All there is on a sheet.
struct Sheet {
    double width = 0.0;
    double height = 0.0;
    std::vector<Stroke> strokes;
    std::vector<FilledShape> filled;
    std::vector<Label> labels;
    std::vector<ViewBox> views;
    std::vector<PlacedDimension> dimensions;
    std::vector<SnapPoint> snapPoints;
    std::vector<SnapCircle> snapCircles;
    /// What could not be shown, in sentences: a view of nothing, a dimension whose edge is gone.
    std::vector<std::string> problems;
    /// The dimensions whose edge or point the model no longer has. They stay in the drawing,
    /// in case the model gets them back, and are not shown.
    std::vector<document::ObjectId> lostDimensions;
};

/// Works out sheets of a document's drawings. Projecting a model takes time, so what it worked
/// out for a view is kept for the next time, until the model changes.
class SheetBuilder {
public:
    explicit SheetBuilder(const document::Document& document);
    ~SheetBuilder();

    [[nodiscard]] Sheet build(const document::Drawing& drawing);
    /// The model changed: nothing kept holds any more.
    void invalidate();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Scales that drawings commonly use, largest first; `fit` is the largest that keeps a model
/// of the given size within the given room.
[[nodiscard]] double fittingScale(double modelWidth, double modelHeight, double roomWidth,
                                  double roomHeight);

/// A scale as it is written: "1:2", "5:1".
[[nodiscard]] std::string scaleText(double scale);

/// A number of millimeters as a dimension says it: no more decimals than it needs, at most two.
[[nodiscard]] std::string lengthText(double millimeters);

/// Adds to a drawing a front view, the views from above and from the right where its standard
/// has them, and an isometric view, at a scale that fits the sheet. Does nothing for a document
/// that shows no body.
void addStandardViews(document::Document& document, document::Drawing& drawing);

} // namespace lcad::drawing
