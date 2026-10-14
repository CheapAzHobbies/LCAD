// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/Icons.h>

#include <lcad/app/Theme.h>

#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <functional>

namespace lcad::app {

namespace {

// What an icon's drawing function gets: a painter set up for the 24 unit grid with the stroke
// pen selected, and the two colors.
struct Ink {
    QPainter& p;
    QColor stroke;
    QColor accent;

    void line(qreal x1, qreal y1, qreal x2, qreal y2) const { p.drawLine(QLineF(x1, y1, x2, y2)); }
    void poly(std::initializer_list<QPointF> points, bool closed = true) const {
        QPainterPath path;
        bool first = true;
        for (const QPointF& point : points) {
            if (first) {
                path.moveTo(point);
                first = false;
            } else {
                path.lineTo(point);
            }
        }
        if (closed) {
            path.closeSubpath();
        }
        p.drawPath(path);
    }
    void fill(std::initializer_list<QPointF> points) const {
        QPainterPath path;
        bool first = true;
        for (const QPointF& point : points) {
            if (first) {
                path.moveTo(point);
                first = false;
            } else {
                path.lineTo(point);
            }
        }
        path.closeSubpath();
        p.fillPath(path, accent);
    }
    void ellipse(qreal cx, qreal cy, qreal rx, qreal ry) const {
        p.drawEllipse(QPointF(cx, cy), rx, ry);
    }
    void arc(qreal cx, qreal cy, qreal rx, qreal ry, qreal startDegrees, qreal spanDegrees) const {
        p.drawArc(QRectF(cx - rx, cy - ry, 2 * rx, 2 * ry), qRound(startDegrees * 16),
                  qRound(spanDegrees * 16));
    }
};

using Drawing = std::function<void(const Ink&)>;

// The outline of a box seen from above its front right corner. Several icons build on it.
void isoBox(const Ink& ink, bool tinted) {
    if (tinted) {
        ink.fill({{12, 4}, {20, 8.5}, {12, 13}, {4, 8.5}});
    }
    ink.poly({{12, 4}, {20, 8.5}, {20, 16}, {12, 20.5}, {4, 16}, {4, 8.5}});
    ink.poly({{4, 8.5}, {12, 13}, {20, 8.5}}, false);
    ink.line(12, 13, 12, 20.5);
}

// A filled dot: an end, a center, a point of a sketch.
void node(const Ink& ink, qreal x, qreal y) {
    QPainterPath dot;
    dot.addEllipse(QPointF(x, y), 1.9, 1.9);
    ink.p.fillPath(dot, ink.accent);
}

const QHash<QString, Drawing>& drawings() {
    static const QHash<QString, Drawing> table{
        // Documents
        {QStringLiteral("new"),
         [](const Ink& i) {
             i.poly({{6, 3.5}, {14, 3.5}, {18, 7.5}, {18, 20.5}, {6, 20.5}});
             i.poly({{14, 3.5}, {14, 7.5}, {18, 7.5}}, false);
             i.line(12, 11, 12, 17);
             i.line(9, 14, 15, 14);
         }},
        {QStringLiteral("open"),
         [](const Ink& i) {
             i.poly({{3.5, 19}, {3.5, 5.5}, {9, 5.5}, {11, 8}, {19, 8}, {19, 11}}, false);
             i.poly({{3.5, 19}, {6.5, 11}, {21, 11}, {18, 19}});
         }},
        {QStringLiteral("save"),
         [](const Ink& i) {
             i.poly({{4, 4}, {16.5, 4}, {20, 7.5}, {20, 20}, {4, 20}});
             i.poly({{8, 4}, {8, 9}, {15, 9}, {15, 4}}, false);
             i.poly({{7.5, 20}, {7.5, 14}, {16.5, 14}, {16.5, 20}}, false);
         }},
        {QStringLiteral("undo"),
         [](const Ink& i) {
             i.poly({{9, 5.5}, {4.5, 10}, {9, 14.5}}, false);
             QPainterPath path(QPointF(4.5, 10));
             path.lineTo(14, 10);
             path.cubicTo(18, 10, 20, 12.5, 20, 15);
             path.cubicTo(20, 17.5, 18, 19.5, 14, 19.5);
             path.lineTo(11, 19.5);
             i.p.drawPath(path);
         }},
        {QStringLiteral("redo"),
         [](const Ink& i) {
             i.poly({{15, 5.5}, {19.5, 10}, {15, 14.5}}, false);
             QPainterPath path(QPointF(19.5, 10));
             path.lineTo(10, 10);
             path.cubicTo(6, 10, 4, 12.5, 4, 15);
             path.cubicTo(4, 17.5, 6, 19.5, 10, 19.5);
             path.lineTo(13, 19.5);
             i.p.drawPath(path);
         }},
        {QStringLiteral("close"),
         [](const Ink& i) {
             i.line(7, 7, 17, 17);
             i.line(17, 7, 7, 17);
         }},
        {QStringLiteral("plus"),
         [](const Ink& i) {
             i.line(12, 5.5, 12, 18.5);
             i.line(5.5, 12, 18.5, 12);
         }},
        {QStringLiteral("delete"),
         [](const Ink& i) {
             i.line(4.5, 7, 19.5, 7);
             i.poly({{9, 7}, {9.5, 4}, {14.5, 4}, {15, 7}}, false);
             i.poly({{6.5, 7}, {7.5, 20}, {16.5, 20}, {17.5, 7}}, false);
             i.line(10.5, 10.5, 10.5, 16.5);
             i.line(13.5, 10.5, 13.5, 16.5);
         }},
        {QStringLiteral("search"),
         [](const Ink& i) {
             i.ellipse(10.5, 10.5, 6, 6);
             i.line(15, 15, 20, 20);
         }},
        {QStringLiteral("keyboard"),
         [](const Ink& i) {
             i.poly({{3, 7}, {21, 7}, {21, 17.5}, {3, 17.5}});
             i.line(6.5, 10.5, 7.5, 10.5);
             i.line(11.5, 10.5, 12.5, 10.5);
             i.line(16.5, 10.5, 17.5, 10.5);
             i.line(8, 14, 16, 14);
         }},
        // Solids
        {QStringLiteral("box"), [](const Ink& i) { isoBox(i, true); }},
        {QStringLiteral("cylinder"),
         [](const Ink& i) {
             QPainterPath top;
             top.addEllipse(QPointF(12, 6.5), 6.5, 3);
             i.p.fillPath(top, i.accent);
             i.ellipse(12, 6.5, 6.5, 3);
             i.line(5.5, 6.5, 5.5, 17.5);
             i.line(18.5, 6.5, 18.5, 17.5);
             i.arc(12, 17.5, 6.5, 3, 180, 180);
         }},
        {QStringLiteral("sphere"),
         [](const Ink& i) {
             i.ellipse(12, 12, 8, 8);
             i.arc(12, 12, 8, 3, 180, 180);
             i.arc(12, 12, 3, 8, 90, 180);
         }},
        {QStringLiteral("cone"),
         [](const Ink& i) {
             i.poly({{5.5, 17.5}, {12, 4}, {18.5, 17.5}}, false);
             i.arc(12, 17.5, 6.5, 3, 180, 180);
             i.arc(12, 17.5, 6.5, 3, 0, 180);
         }},
        {QStringLiteral("torus"),
         [](const Ink& i) {
             i.ellipse(12, 12, 9, 5.5);
             i.ellipse(12, 11.5, 3.5, 1.6);
         }},
        // Features
        {QStringLiteral("pull"),
         [](const Ink& i) {
             // A flat outline, and the arrow it is pulled along.
             i.fill({{3.5, 18.5}, {13.5, 18.5}, {20.5, 13.5}, {10.5, 13.5}});
             i.poly({{3.5, 18.5}, {13.5, 18.5}, {20.5, 13.5}, {10.5, 13.5}});
             i.line(12, 16, 12, 3.5);
             i.poly({{8.5, 7}, {12, 3.5}, {15.5, 7}}, false);
         }},
        {QStringLiteral("revolve"),
         [](const Ink& i) {
             // An outline beside an axis, and the turn about it.
             i.p.save();
             QPen dashed = i.p.pen();
             dashed.setStyle(Qt::DashLine);
             i.p.setPen(dashed);
             i.line(6, 3, 6, 21);
             i.p.restore();
             i.fill({{9, 6}, {15, 6}, {15, 13}, {9, 13}});
             i.poly({{9, 6}, {15, 6}, {15, 13}, {9, 13}});
             i.arc(6, 17.5, 13, 3, 200, 290);
             i.poly({{16.5, 14}, {19.2, 16.3}, {16, 18}}, false);
         }},
        {QStringLiteral("fillet"),
         [](const Ink& i) {
             QPainterPath body(QPointF(4, 20));
             body.lineTo(4, 12);
             body.arcTo(QRectF(4, 4, 16, 16), 180, -90);
             body.lineTo(20, 4);
             body.lineTo(20, 20);
             body.closeSubpath();
             i.p.fillPath(body, i.accent);
             i.p.drawPath(body);
         }},
        {QStringLiteral("chamfer"),
         [](const Ink& i) {
             i.fill({{4, 20}, {4, 11}, {11, 4}, {20, 4}, {20, 20}});
             i.poly({{4, 20}, {4, 11}, {11, 4}, {20, 4}, {20, 20}});
         }},
        {QStringLiteral("shell"),
         [](const Ink& i) {
             // A hollowed solid cut through: walls, and a floor.
             i.fill({{4, 4},
                     {4, 20},
                     {20, 20},
                     {20, 4},
                     {16.5, 4},
                     {16.5, 16.5},
                     {7.5, 16.5},
                     {7.5, 4}});
             i.poly({{4, 4},
                     {4, 20},
                     {20, 20},
                     {20, 4},
                     {16.5, 4},
                     {16.5, 16.5},
                     {7.5, 16.5},
                     {7.5, 4}});
         }},
        // View
        {QStringLiteral("fit"),
         [](const Ink& i) {
             i.poly({{4, 9}, {4, 4}, {9, 4}}, false);
             i.poly({{15, 4}, {20, 4}, {20, 9}}, false);
             i.poly({{20, 15}, {20, 20}, {15, 20}}, false);
             i.poly({{9, 20}, {4, 20}, {4, 15}}, false);
             i.poly({{9, 9}, {15, 9}, {15, 15}, {9, 15}});
         }},
        {QStringLiteral("fit-selection"),
         [](const Ink& i) {
             i.poly({{4, 9}, {4, 4}, {9, 4}}, false);
             i.poly({{15, 4}, {20, 4}, {20, 9}}, false);
             i.poly({{20, 15}, {20, 20}, {15, 20}}, false);
             i.poly({{9, 20}, {4, 20}, {4, 15}}, false);
             i.fill({{9, 9}, {15, 9}, {15, 15}, {9, 15}});
             i.poly({{9, 9}, {15, 9}, {15, 15}, {9, 15}});
         }},
        {QStringLiteral("home"),
         [](const Ink& i) {
             i.poly({{3.5, 12}, {12, 4.5}, {20.5, 12}}, false);
             i.poly({{6, 10.5}, {6, 19.5}, {18, 19.5}, {18, 10.5}}, false);
             i.poly({{10, 19.5}, {10, 14}, {14, 14}, {14, 19.5}}, false);
         }},
        {QStringLiteral("view"), [](const Ink& i) { isoBox(i, false); }},
        {QStringLiteral("perspective"),
         [](const Ink& i) {
             i.poly({{4, 6}, {14, 8}, {14, 16}, {4, 18}});
             i.poly({{14, 8}, {20, 9.5}, {20, 14.5}, {14, 16}}, false);
         }},
        {QStringLiteral("grid"),
         [](const Ink& i) {
             i.poly({{4, 4}, {20, 4}, {20, 20}, {4, 20}});
             i.line(9.3, 4, 9.3, 20);
             i.line(14.7, 4, 14.7, 20);
             i.line(4, 9.3, 20, 9.3);
             i.line(4, 14.7, 20, 14.7);
         }},
        {QStringLiteral("axes"),
         [](const Ink& i) {
             i.line(12, 13, 12, 4);
             i.line(12, 13, 4.5, 18);
             i.line(12, 13, 19.5, 18);
             i.poly({{10, 6.5}, {12, 4}, {14, 6.5}}, false);
         }},
        {QStringLiteral("eye"),
         [](const Ink& i) {
             QPainterPath path(QPointF(2.5, 12));
             path.quadTo(12, 2.5, 21.5, 12);
             path.quadTo(12, 21.5, 2.5, 12);
             i.p.drawPath(path);
             i.ellipse(12, 12, 3, 3);
         }},
        {QStringLiteral("eye-off"),
         [](const Ink& i) {
             QPainterPath path(QPointF(2.5, 12));
             path.quadTo(12, 2.5, 21.5, 12);
             path.quadTo(12, 21.5, 2.5, 12);
             i.p.drawPath(path);
             i.line(5, 19.5, 19, 4.5);
         }},
        {QStringLiteral("isolate"),
         [](const Ink& i) {
             i.fill({{8, 8}, {16, 8}, {16, 16}, {8, 16}});
             i.poly({{8, 8}, {16, 8}, {16, 16}, {8, 16}});
             i.line(4, 4, 6.5, 4);
             i.line(17.5, 4, 20, 4);
             i.line(4, 20, 6.5, 20);
             i.line(17.5, 20, 20, 20);
             i.line(4, 4, 4, 6.5);
             i.line(20, 4, 20, 6.5);
             i.line(4, 17.5, 4, 20);
             i.line(20, 17.5, 20, 20);
         }},
        // Display styles
        {QStringLiteral("style-shaded-edges"), [](const Ink& i) { isoBox(i, true); }},
        {QStringLiteral("style-shaded"),
         [](const Ink& i) {
             i.fill({{12, 4}, {20, 8.5}, {20, 16}, {12, 20.5}, {4, 16}, {4, 8.5}});
             i.poly({{12, 4}, {20, 8.5}, {20, 16}, {12, 20.5}, {4, 16}, {4, 8.5}});
         }},
        {QStringLiteral("style-wireframe"),
         [](const Ink& i) {
             isoBox(i, false);
             i.line(12, 4, 12, 11.5);
             i.poly({{4, 16}, {12, 11.5}, {20, 16}}, false);
         }},
        {QStringLiteral("style-hidden-line"), [](const Ink& i) { isoBox(i, false); }},
        {QStringLiteral("style-transparent"),
         [](const Ink& i) {
             isoBox(i, false);
             i.p.setOpacity(0.35);
             i.fill({{12, 4}, {20, 8.5}, {20, 16}, {12, 20.5}, {4, 16}, {4, 8.5}});
             i.p.setOpacity(1.0);
         }},
        // Sketching
        {QStringLiteral("sketch"),
         [](const Ink& i) {
             i.poly({{3.5, 19.5}, {8, 11.5}, {20.5, 11.5}, {16, 19.5}});
             i.line(10, 15.5, 18, 4.5);
             i.fill({{10, 15.5}, {10.6, 12.6}, {12.6, 14.1}});
         }},
        {QStringLiteral("finish"),
         [](const Ink& i) { i.poly({{5, 12.5}, {10, 17.5}, {19.5, 6.5}}, false); }},
        {QStringLiteral("line"),
         [](const Ink& i) {
             i.line(6, 18, 18, 6);
             node(i, 6, 18);
             node(i, 18, 6);
         }},
        {QStringLiteral("rectangle"),
         [](const Ink& i) {
             i.poly({{4.5, 7}, {19.5, 7}, {19.5, 17}, {4.5, 17}});
             node(i, 4.5, 17);
             node(i, 19.5, 7);
         }},
        {QStringLiteral("circle"),
         [](const Ink& i) {
             i.ellipse(12, 12, 7.5, 7.5);
             node(i, 12, 12);
         }},
        {QStringLiteral("arc"),
         [](const Ink& i) {
             i.arc(6, 18.5, 12.5, 12.5, 0, 90);
             node(i, 18.5, 18.5);
             node(i, 6, 6);
             node(i, 6, 18.5);
         }},
        {QStringLiteral("point"),
         [](const Ink& i) {
             i.line(12, 4.5, 12, 8.5);
             i.line(12, 15.5, 12, 19.5);
             i.line(4.5, 12, 8.5, 12);
             i.line(15.5, 12, 19.5, 12);
             node(i, 12, 12);
         }},
        {QStringLiteral("polygon"),
         [](const Ink& i) {
             i.poly(
                 {{12, 3.5}, {19.4, 7.75}, {19.4, 16.25}, {12, 20.5}, {4.6, 16.25}, {4.6, 7.75}});
         }},
        {QStringLiteral("slot"),
         [](const Ink& i) {
             QPainterPath path;
             path.addRoundedRect(QRectF(3.5, 8, 17, 8), 4, 4);
             i.p.drawPath(path);
         }},
        {QStringLiteral("dimension"),
         [](const Ink& i) {
             i.line(5, 5, 5, 13);
             i.line(19, 5, 19, 13);
             i.line(5, 8, 19, 8);
             i.poly({{8, 6}, {5.4, 8}, {8, 10}}, false);
             i.poly({{16, 6}, {18.6, 8}, {16, 10}}, false);
             i.line(5, 18.5, 19, 18.5);
         }},
        {QStringLiteral("trim"),
         [](const Ink& i) {
             i.line(8.5, 4.5, 8.5, 19.5);
             i.line(15.5, 4.5, 15.5, 19.5);
             i.line(3.5, 12, 8.5, 12);
             i.line(15.5, 12, 20.5, 12);
             i.p.setPen(QPen(i.accent, 1.6, Qt::SolidLine, Qt::RoundCap));
             i.line(10.6, 10.6, 13.4, 13.4);
             i.line(13.4, 10.6, 10.6, 13.4);
         }},
        {QStringLiteral("extend"),
         [](const Ink& i) {
             i.line(19.5, 4.5, 19.5, 19.5);
             i.line(3.5, 12, 10, 12);
             i.p.setPen(QPen(i.accent, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
             i.line(12, 12, 17.5, 12);
             i.poly({{14.8, 9.3}, {17.5, 12}, {14.8, 14.7}}, false);
         }},
        {QStringLiteral("offset"),
         [](const Ink& i) {
             i.poly({{4.5, 19.5}, {4.5, 5}, {19.5, 5}}, false);
             i.p.setPen(QPen(i.accent, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
             i.poly({{9.5, 19.5}, {9.5, 10}, {19.5, 10}}, false);
         }},
        {QStringLiteral("mirror"),
         [](const Ink& i) {
             i.poly({{4, 17.5}, {9.5, 17.5}, {9.5, 7}});
             i.poly({{20, 17.5}, {14.5, 17.5}, {14.5, 7}});
             i.p.setPen(QPen(i.accent, 1.4, Qt::DashLine, Qt::RoundCap));
             i.line(12, 3.5, 12, 20.5);
         }},
        {QStringLiteral("project"),
         [](const Ink& i) {
             i.poly({{3.5, 20.5}, {8, 13.5}, {20.5, 13.5}, {16, 20.5}});
             i.line(7, 5, 17, 5);
             i.p.setPen(QPen(i.accent, 1.5, Qt::DotLine, Qt::RoundCap));
             i.line(7, 5, 7, 17);
             i.line(17, 5, 17, 17);
             i.p.setPen(QPen(i.accent, 1.8, Qt::SolidLine, Qt::RoundCap));
             i.line(7, 17, 17, 17);
         }},
        {QStringLiteral("construction"),
         [](const Ink& i) {
             i.line(4.5, 19.5, 8, 16);
             i.line(10.5, 13.5, 13.5, 10.5);
             i.line(16, 8, 19.5, 4.5);
         }},
        {QStringLiteral("inference"),
         [](const Ink& i) {
             i.line(4.5, 17.5, 12, 10);
             i.poly({{12, 6}, {16, 10}, {12, 14}, {8, 10}});
             node(i, 18.5, 17.5);
         }},
        {QStringLiteral("c-coincident"),
         [](const Ink& i) {
             i.ellipse(12, 12, 5.5, 5.5);
             node(i, 12, 12);
         }},
        {QStringLiteral("c-horizontal"),
         [](const Ink& i) {
             i.line(4.5, 12, 19.5, 12);
             node(i, 4.5, 12);
             node(i, 19.5, 12);
         }},
        {QStringLiteral("c-vertical"),
         [](const Ink& i) {
             i.line(12, 4.5, 12, 19.5);
             node(i, 12, 4.5);
             node(i, 12, 19.5);
         }},
        {QStringLiteral("c-parallel"),
         [](const Ink& i) {
             i.line(5, 19, 11, 5);
             i.line(13, 19, 19, 5);
         }},
        {QStringLiteral("c-perpendicular"),
         [](const Ink& i) {
             i.line(4.5, 18.5, 19.5, 18.5);
             i.line(12, 18.5, 12, 5);
         }},
        {QStringLiteral("c-tangent"),
         [](const Ink& i) {
             i.ellipse(11, 14, 6, 6);
             i.line(4, 8, 20.5, 8);
         }},
        {QStringLiteral("c-equal"),
         [](const Ink& i) {
             i.line(5.5, 9.5, 18.5, 9.5);
             i.line(5.5, 14.5, 18.5, 14.5);
         }},
        {QStringLiteral("c-concentric"),
         [](const Ink& i) {
             i.ellipse(12, 12, 8, 8);
             i.ellipse(12, 12, 3.5, 3.5);
         }},
        {QStringLiteral("c-midpoint"),
         [](const Ink& i) {
             i.line(4, 17.5, 20, 17.5);
             i.poly({{12, 17}, {8.5, 9}, {15.5, 9}});
         }},
        {QStringLiteral("c-symmetric"),
         [](const Ink& i) {
             i.line(12, 4, 12, 20);
             i.poly({{3.5, 12}, {8.5, 8.5}, {8.5, 15.5}});
             i.poly({{20.5, 12}, {15.5, 8.5}, {15.5, 15.5}});
         }},
        {QStringLiteral("c-collinear"),
         [](const Ink& i) {
             i.line(4, 20, 9.5, 14.5);
             i.line(14.5, 9.5, 20, 4);
             node(i, 12, 12);
         }},
        {QStringLiteral("c-fix"),
         [](const Ink& i) {
             i.poly({{7, 11}, {17, 11}, {17, 19.5}, {7, 19.5}});
             i.arc(12, 11, 3.5, 5, 0, 180);
         }},
        // Window
        {QStringLiteral("layout"),
         [](const Ink& i) {
             i.poly({{3.5, 4.5}, {20.5, 4.5}, {20.5, 19.5}, {3.5, 19.5}});
             i.line(9, 4.5, 9, 19.5);
             i.line(9, 15, 20.5, 15);
         }},
        {QStringLiteral("focus"),
         [](const Ink& i) {
             i.poly({{3.5, 4.5}, {20.5, 4.5}, {20.5, 19.5}, {3.5, 19.5}});
             i.fill({{6.5, 7.5}, {17.5, 7.5}, {17.5, 16.5}, {6.5, 16.5}});
         }},
        {QStringLiteral("theme"),
         [](const Ink& i) {
             i.ellipse(12, 12, 8, 8);
             QPainterPath half;
             half.moveTo(12, 4);
             half.arcTo(QRectF(4, 4, 16, 16), 90, 180);
             half.closeSubpath();
             i.p.fillPath(half, i.stroke);
         }},
        {QStringLiteral("select"),
         [](const Ink& i) {
             i.poly({{6, 3.5},
                     {6, 18},
                     {10, 14.5},
                     {13, 20.5},
                     {15.5, 19.3},
                     {12.6, 13.5},
                     {18, 13.5}});
         }},
        {QStringLiteral("info"),
         [](const Ink& i) {
             i.ellipse(12, 12, 8.5, 8.5);
             i.line(12, 11, 12, 16.5);
             i.line(12, 7.5, 12, 7.6);
         }},
    };
    return table;
}

class LineIconEngine : public QIconEngine {
public:
    explicit LineIconEngine(QString name) : m_name(std::move(name)) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override {
        const auto drawing = drawings().constFind(m_name);
        if (drawing == drawings().constEnd()) {
            return;
        }
        const ThemeTokens& tokens = Theme::instance().tokens();
        const QColor stroke = mode == QIcon::Disabled   ? tokens.textDisabled
                              : mode == QIcon::Selected ? tokens.accentText
                                                        : tokens.text;
        QColor accent = tokens.accent;
        if (mode == QIcon::Disabled) {
            accent = tokens.textDisabled;
            accent.setAlphaF(0.4F);
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->translate(rect.topLeft());
        painter->scale(rect.width() / 24.0, rect.height() / 24.0);
        // One stroke width for every icon, with round ends and corners.
        painter->setPen(QPen(stroke, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        (*drawing)(Ink{*painter, stroke, accent});
        painter->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap pixmap(size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        return pixmap;
    }

    QIconEngine* clone() const override { return new LineIconEngine(m_name); }

private:
    QString m_name;
};

} // namespace

QIcon icon(const QString& name) {
    if (!drawings().contains(name)) {
        return {};
    }
    return QIcon(new LineIconEngine(name));
}

QStringList iconNames() {
    QStringList names = drawings().keys();
    names.sort();
    return names;
}

} // namespace lcad::app
