// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/drawing/Dxf.h>

#include <lcad/core/Path.h>

#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace lcad::drawing {

namespace {

constexpr double pi = 3.14159265358979323846;

const char* layerOf(Pen pen) {
    switch (pen) {
    case Pen::Frame:
        return "FRAME";
    case Pen::Visible:
        return "VISIBLE";
    case Pen::Thin:
        return "THIN";
    case Pen::Hidden:
        return "HIDDEN";
    case Pen::Center:
        return "CENTER";
    case Pen::Hatch:
        return "HATCH";
    case Pen::Dimension:
        return "DIMENSION";
    }
    return "0";
}

// Writes group codes and values, one to a line, as DXF has them.
class Writer {
public:
    Writer() { m_out << std::fixed << std::setprecision(6); }

    void text(int code, const std::string& value) { m_out << code << '\n' << value << '\n'; }
    void integer(int code, int value) { m_out << code << '\n' << value << '\n'; }
    void number(int code, double value) { m_out << code << '\n' << value << '\n'; }
    void point(const Point2D& p, int base = 10) {
        number(base, p.x);
        number(base + 10, p.y);
    }
    [[nodiscard]] std::string result() const { return m_out.str(); }

private:
    std::ostringstream m_out;
};

void writeLayer(Writer& out, const char* name, const char* lineType, int color) {
    out.text(0, "LAYER");
    out.text(2, name);
    out.integer(70, 0);
    out.integer(62, color);
    out.text(6, lineType);
}

void writeLineType(Writer& out, const char* name, const char* description,
                   const std::vector<double>& pattern) {
    double total = 0.0;
    for (const double piece : pattern) {
        total += std::abs(piece);
    }
    out.text(0, "LTYPE");
    out.text(2, name);
    out.integer(70, 0);
    out.text(3, description);
    out.integer(72, 65);
    out.integer(73, static_cast<int>(pattern.size()));
    out.number(40, total);
    for (const double piece : pattern) {
        out.number(49, piece);
    }
}

void writeTables(Writer& out) {
    out.text(0, "SECTION");
    out.text(2, "TABLES");

    out.text(0, "TABLE");
    out.text(2, "LTYPE");
    out.integer(70, 3);
    writeLineType(out, "CONTINUOUS", "Solid line", {});
    writeLineType(out, "DASHED", "Dashed", {3.0, -1.5});
    writeLineType(out, "CENTER", "Long and short dashes", {8.0, -1.5, 1.5, -1.5});
    out.text(0, "ENDTAB");

    out.text(0, "TABLE");
    out.text(2, "LAYER");
    out.integer(70, 7);
    writeLayer(out, "FRAME", "CONTINUOUS", 7);
    writeLayer(out, "VISIBLE", "CONTINUOUS", 7);
    writeLayer(out, "THIN", "CONTINUOUS", 8);
    writeLayer(out, "HIDDEN", "DASHED", 8);
    writeLayer(out, "CENTER", "CENTER", 1);
    writeLayer(out, "HATCH", "CONTINUOUS", 8);
    writeLayer(out, "DIMENSION", "CONTINUOUS", 3);
    out.text(0, "ENDTAB");

    out.text(0, "ENDSEC");
}

void writeStroke(Writer& out, const Stroke& stroke) {
    const char* layer = layerOf(stroke.pen);
    if (stroke.isArc) {
        const bool whole = stroke.endAngle - stroke.startAngle >= 2.0 * pi - 1e-9;
        out.text(0, whole ? "CIRCLE" : "ARC");
        out.text(8, layer);
        out.point(stroke.center);
        out.number(30, 0.0);
        out.number(40, stroke.radius);
        if (!whole) {
            out.number(50, stroke.startAngle * 180.0 / pi);
            out.number(51, stroke.endAngle * 180.0 / pi);
        }
        return;
    }
    if (stroke.points.size() == 2) {
        out.text(0, "LINE");
        out.text(8, layer);
        out.point(stroke.points[0], 10);
        out.number(30, 0.0);
        out.point(stroke.points[1], 11);
        out.number(31, 0.0);
        return;
    }
    out.text(0, "POLYLINE");
    out.text(8, layer);
    out.integer(66, 1);
    out.number(10, 0.0);
    out.number(20, 0.0);
    out.number(30, 0.0);
    for (const Point2D& p : stroke.points) {
        out.text(0, "VERTEX");
        out.text(8, layer);
        out.point(p);
        out.number(30, 0.0);
    }
    out.text(0, "SEQEND");
    out.text(8, layer);
}

void writeLabel(Writer& out, const Label& label) {
    // DXF has one line of text to an entity.
    std::istringstream lines(label.text);
    std::string line;
    Point2D at = label.at;
    while (std::getline(lines, line)) {
        out.text(0, "TEXT");
        out.text(8, label.bold ? "VISIBLE" : "DIMENSION");
        out.point(at, 10);
        out.number(30, 0.0);
        out.number(40, label.height);
        out.text(1, line);
        out.number(50, label.angle * 180.0 / pi);
        if (label.align != Label::Align::Left) {
            out.integer(72, label.align == Label::Align::Center ? 1 : 2);
            out.point(at, 11);
            out.number(31, 0.0);
        }
        at.x += std::sin(label.angle) * label.height * 1.5;
        at.y -= std::cos(label.angle) * label.height * 1.5;
    }
}

} // namespace

std::string dxfText(const Sheet& sheet) {
    Writer out;
    out.text(0, "SECTION");
    out.text(2, "HEADER");
    out.text(9, "$ACADVER");
    out.text(1, "AC1009");
    // Millimeters, as far as R12 can say so.
    out.text(9, "$MEASUREMENT");
    out.integer(70, 1);
    out.text(9, "$EXTMIN");
    out.point({0.0, 0.0});
    out.text(9, "$EXTMAX");
    out.point({sheet.width, sheet.height});
    out.text(0, "ENDSEC");

    writeTables(out);

    out.text(0, "SECTION");
    out.text(2, "ENTITIES");
    for (const Stroke& stroke : sheet.strokes) {
        writeStroke(out, stroke);
    }
    for (const FilledShape& shape : sheet.filled) {
        // A triangle: SOLID takes four corners, the last two the same.
        out.text(0, "SOLID");
        out.text(8, "DIMENSION");
        for (std::size_t i = 0; i < 4; ++i) {
            const Point2D& corner = shape.corners[std::min(i, shape.corners.size() - 1)];
            out.point(corner, 10 + static_cast<int>(i));
            out.number(30 + static_cast<int>(i), 0.0);
        }
    }
    for (const Label& label : sheet.labels) {
        writeLabel(out, label);
    }
    out.text(0, "ENDSEC");
    out.text(0, "EOF");
    return out.result();
}

void writeDxf(const Sheet& sheet, const std::filesystem::path& path) {
    std::ofstream file(path, std::ios::binary);
    if (file) {
        file << dxfText(sheet);
        file.close();
    }
    if (!file) {
        throw std::runtime_error("Could not write " + core::toUtf8(path) + ".");
    }
}

} // namespace lcad::drawing
