// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/ObjectId.h>

#include <string>
#include <vector>

// What a drawing is: sheets of paper with views of the model on them, and the dimensions and
// notes that go with those. Everything about the geometry on a sheet is worked out from the model
// each time it is shown (see the drawing module), so that a drawing follows the model; what is
// kept here is only what the user chose. The Document holds them; see Document.h.
namespace lcad::document {

enum class SheetSize { A4, A3, A2, A1, A0, Letter, Tabloid, AnsiC, AnsiD };

[[nodiscard]] const char* toString(SheetSize size);
[[nodiscard]] bool sheetSizeFromString(const std::string& name, SheetSize& size);

/// Which rules a drawing follows: where the side views go, and how numbers are written.
enum class DrawingStandard {
    /// First-angle projection: the view from above goes below the front view.
    Iso,
    /// Third-angle projection: the view from above goes above the front view.
    Ansi,
};

enum class ViewKind {
    Front,
    Back,
    Top,
    Bottom,
    Left,
    Right,
    Isometric,
    /// Its parent, cut through along a line and looked at from one side.
    Section,
    /// A round part of its parent, drawn larger.
    Detail,
};

[[nodiscard]] const char* toString(ViewKind kind);
[[nodiscard]] bool viewKindFromString(const std::string& name, ViewKind& kind);

/// A view of the model on a sheet.
struct DrawingView {
    ObjectId id;
    ViewKind kind = ViewKind::Front;
    /// For a section or a detail, the letter it goes by: "A".
    std::string label;
    /// Millimeters on the sheet for one in the model: 0.5 is a scale of 1:2.
    double scale = 1.0;
    /// The middle of the view, in millimeters from the sheet's lower left corner.
    double x = 0.0;
    double y = 0.0;
    /// Whether what lies behind other things is drawn, dashed.
    bool showHidden = true;
    /// Whether circles and arcs of it get center marks.
    bool centerMarks = true;
    /// For a section or a detail: the view it is taken from.
    ObjectId parent;
    /// For a section: the cutting line, in the coordinates of the parent, in millimeters of the
    /// model. Up the parent if `sectionVertical`, and across it otherwise.
    bool sectionVertical = true;
    double sectionPosition = 0.0;
    /// For a section: looks from the other side of the cutting line. Unless flipped, a section
    /// looks left at a line up its parent, and up at a line across it.
    bool sectionFlipped = false;
    /// For a detail: the circle on the parent that it shows, in the parent's coordinates, in
    /// millimeters of the model.
    double detailX = 0.0;
    double detailY = 0.0;
    double detailRadius = 10.0;

    friend bool operator==(const DrawingView&, const DrawingView&) = default;
};

/// A point of the model that a dimension hangs on, by what stays the same when the model changes:
/// which body, seen through which occurrences, which of its edges, and where on it.
struct DrawingPoint {
    ObjectId body;
    std::vector<ObjectId> path;
    std::string edge;
    enum class Where { Start, End, Middle, Center };
    Where where = Where::Start;

    friend bool operator==(const DrawingPoint&, const DrawingPoint&) = default;
};

enum class DimensionKind {
    /// Between two points, along the line between them as the view shows it.
    Aligned,
    /// Between two points, across the sheet.
    Horizontal,
    /// Between two points, up the sheet.
    Vertical,
    /// Of a circle or an arc.
    Radius,
    Diameter,
};

[[nodiscard]] const char* toString(DimensionKind kind);
[[nodiscard]] bool dimensionKindFromString(const std::string& name, DimensionKind& kind);

/// A dimension in a view. Its value is measured from the model each time it is shown.
struct DrawingDimension {
    ObjectId id;
    ObjectId view;
    DimensionKind kind = DimensionKind::Aligned;
    /// What it measures. A radius or a diameter uses `first` alone.
    DrawingPoint first;
    DrawingPoint second;
    /// How far the value stands off from what it measures, in millimeters on the sheet: square
    /// to the dimension for a distance, and along the leader for a radius or a diameter.
    double offset = 10.0;
    /// Shown after the measured value: "H7", "± 0.1", "THRU".
    std::string suffix;

    friend bool operator==(const DrawingDimension&, const DrawingDimension&) = default;
};

/// Text anywhere on a sheet.
struct DrawingNote {
    ObjectId id;
    std::string text;
    /// Its lower left corner, in millimeters from the sheet's lower left corner.
    double x = 0.0;
    double y = 0.0;
    /// The height of its letters, in millimeters.
    double height = 3.5;

    friend bool operator==(const DrawingNote&, const DrawingNote&) = default;
};

/// What the title block says.
struct TitleBlock {
    std::string title;
    std::string partNumber;
    std::string revision;
    std::string drawnBy;
    std::string date;
    std::string material;
    std::string company;

    friend bool operator==(const TitleBlock&, const TitleBlock&) = default;
};

/// One sheet.
struct Drawing {
    ObjectId id;
    std::string name;
    SheetSize size = SheetSize::A3;
    DrawingStandard standard = DrawingStandard::Iso;
    TitleBlock titleBlock;
    std::vector<DrawingView> views;
    std::vector<DrawingDimension> dimensions;
    std::vector<DrawingNote> notes;

    friend bool operator==(const Drawing&, const Drawing&) = default;
};

} // namespace lcad::document
