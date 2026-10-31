// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/SheetView.h>

#include <lcad/app/Theme.h>

#include <QApplication>
#include <QEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QStringList>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace lcad::app {

using drawing::Label;
using drawing::Pen;
using drawing::Point2D;
using drawing::Sheet;

namespace {

constexpr double pi = 3.14159265358979323846;

// How wide each kind of line is on paper, in millimeters, and how it is broken.
struct LineStyle {
    double width;
    QVector<qreal> dashes;
};

LineStyle styleOf(Pen pen) {
    switch (pen) {
    case Pen::Frame:
    case Pen::Visible:
        return {0.5, {}};
    case Pen::Thin:
    case Pen::Dimension:
        return {0.18, {}};
    case Pen::Hidden:
        return {0.25, {3.0, 1.5}};
    case Pen::Center:
        return {0.18, {8.0, 1.5, 1.5, 1.5}};
    case Pen::Hatch:
        return {0.13, {}};
    }
    return {0.25, {}};
}

QPen penFor(Pen kind, double scale) {
    const LineStyle style = styleOf(kind);
    const double width = std::max(style.width * scale, 1.0);
    QPen pen(Qt::black, width, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin);
    if (!style.dashes.isEmpty()) {
        // In widths of the pen.
        QVector<qreal> pattern;
        for (const qreal piece : style.dashes) {
            pattern.append(piece * scale / width);
        }
        pen.setDashPattern(pattern);
    }
    return pen;
}

// Text at the size a sheet asks for, at any scale: a font of a fixed size, scaled by the painter.
constexpr int fontPixels = 100;
// How tall a font's capitals are for its size.
constexpr double capitalHeight = 0.72;

void paintLabel(QPainter& painter, const Label& label, const QPointF& at, double scale) {
    QFont font = QApplication::font();
    font.setPixelSize(fontPixels);
    font.setBold(label.bold);
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    const double factor = label.height * scale / (fontPixels * capitalHeight);
    const QStringList lines = QString::fromStdString(label.text).split(QLatin1Char('\n'));
    painter.save();
    painter.translate(at);
    painter.rotate(-label.angle * 180.0 / pi);
    painter.scale(factor, factor);
    double y = 0.0;
    for (const QString& line : lines) {
        double x = 0.0;
        if (label.align == Label::Align::Center) {
            x = -metrics.horizontalAdvance(line) / 2.0;
        } else if (label.align == Label::Align::Right) {
            x = -metrics.horizontalAdvance(line);
        }
        painter.drawText(QPointF(x, y), line);
        y += metrics.lineSpacing();
    }
    painter.restore();
}

} // namespace

void paintSheet(QPainter& painter, const Sheet& sheet, const QPointF& origin, double scale) {
    const auto at = [&](const Point2D& p) {
        return QPointF(origin.x() + p.x * scale, origin.y() - p.y * scale);
    };
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    for (const drawing::Stroke& stroke : sheet.strokes) {
        painter.setPen(penFor(stroke.pen, scale));
        if (stroke.points.size() == 2) {
            painter.drawLine(at(stroke.points[0]), at(stroke.points[1]));
            continue;
        }
        QPainterPath path;
        for (std::size_t i = 0; i < stroke.points.size(); ++i) {
            if (i == 0) {
                path.moveTo(at(stroke.points[i]));
            } else {
                path.lineTo(at(stroke.points[i]));
            }
        }
        painter.drawPath(path);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    for (const drawing::FilledShape& shape : sheet.filled) {
        QPolygonF polygon;
        for (const Point2D& corner : shape.corners) {
            polygon.append(at(corner));
        }
        painter.drawPolygon(polygon);
    }
    painter.setPen(Qt::black);
    painter.setBrush(Qt::NoBrush);
    for (const Label& label : sheet.labels) {
        paintLabel(painter, label, at(label.at), scale);
    }
    painter.restore();
}

SheetView::SheetView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("sheetView"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(200, 200);
}

void SheetView::setSheet(Sheet sheet) {
    const bool sizeChanged = sheet.width != m_sheet.width || sheet.height != m_sheet.height;
    m_sheet = std::move(sheet);
    if (sizeChanged || !m_fitted) {
        fit();
    }
    update();
}

void SheetView::setNotes(std::vector<document::DrawingNote> notes) {
    m_notes = std::move(notes);
}

void SheetView::setMode(Mode mode) {
    m_mode = mode;
    setCursor(mode == Mode::Pick ? Qt::CrossCursor : Qt::ArrowCursor);
}

void SheetView::setSelected(Selected selected) {
    m_selected = selected;
    update();
}

void SheetView::setMark(std::optional<Point2D> at) {
    m_mark = at;
    update();
}

void SheetView::fit() {
    if (m_sheet.width <= 0.0 || m_sheet.height <= 0.0 || width() <= 0 || height() <= 0) {
        return;
    }
    const double room = 24.0;
    m_zoom = std::min((width() - room) / m_sheet.width, (height() - room) / m_sheet.height);
    m_zoom = std::max(m_zoom, 0.05);
    m_origin = QPointF((width() - m_sheet.width * m_zoom) / 2.0,
                       (height() + m_sheet.height * m_zoom) / 2.0);
    m_fitted = true;
    m_following = true;
    update();
}

QPointF SheetView::toWidget(const Point2D& onSheet) const {
    return {m_origin.x() + onSheet.x * m_zoom, m_origin.y() - onSheet.y * m_zoom};
}

Point2D SheetView::toSheet(const QPointF& inWidget) const {
    return {(inWidget.x() - m_origin.x()) / m_zoom, (m_origin.y() - inWidget.y()) / m_zoom};
}

double SheetView::reach() const {
    return 7.0 / m_zoom;
}

void SheetView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const ThemeTokens& tokens = Theme::instance().tokens();
    painter.fillRect(rect(), tokens.window);
    const QRectF paper(toWidget({0.0, m_sheet.height}),
                       QSizeF(m_sheet.width * m_zoom, m_sheet.height * m_zoom));
    painter.fillRect(paper.translated(3.0, 3.0), QColor(0, 0, 0, 50));
    painter.fillRect(paper, Qt::white);
    paintSheet(painter, m_sheet, m_origin, m_zoom);

    const QColor accent = tokens.accent;
    if (m_selected.kind == Selected::Kind::View) {
        for (const drawing::ViewBox& box : m_sheet.views) {
            if (box.view == m_selected.id) {
                QPen pen(accent, 1.5, Qt::DashLine);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                const QPointF low = toWidget({box.left, box.bottom});
                const QPointF high = toWidget({box.right, box.top});
                painter.drawRect(QRectF(low, high).normalized().adjusted(-6.0, -6.0, 6.0, 6.0));
            }
        }
    }
    if (m_selected.kind == Selected::Kind::Dimension) {
        for (const drawing::PlacedDimension& dimension : m_sheet.dimensions) {
            if (dimension.id == m_selected.id) {
                painter.setPen(QPen(accent, 1.5));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(toWidget(dimension.textAt), 7.0 * m_zoom, 4.0 * m_zoom);
            }
        }
    }
    if (m_selected.kind == Selected::Kind::Note) {
        for (const document::DrawingNote& note : m_notes) {
            if (note.id == m_selected.id) {
                painter.setPen(QPen(accent, 1.5, Qt::DashLine));
                painter.setBrush(Qt::NoBrush);
                const QPointF at = toWidget({note.x, note.y});
                painter.drawRect(QRectF(at.x() - 3.0, at.y() - note.height * m_zoom - 3.0,
                                        note.height * m_zoom * 12.0, note.height * m_zoom + 6.0));
            }
        }
    }
    if (m_mark.has_value()) {
        painter.setPen(QPen(accent, 2.0));
        painter.setBrush(accent);
        painter.drawEllipse(toWidget(*m_mark), 4.0, 4.0);
    }
}

void SheetView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_following) {
        fit();
    }
}

const document::DrawingNote* SheetView::noteAt(const Point2D& at) const {
    for (const document::DrawingNote& note : m_notes) {
        const double wide = static_cast<double>(note.text.size()) * note.height * 0.7;
        if (at.x >= note.x - reach() && at.x <= note.x + wide + reach() &&
            at.y >= note.y - reach() && at.y <= note.y + note.height + reach()) {
            return &note;
        }
    }
    return nullptr;
}

SheetView::Selected SheetView::pickAt(const Point2D& at) const {
    for (const drawing::PlacedDimension& dimension : m_sheet.dimensions) {
        if (std::hypot(dimension.textAt.x - at.x, dimension.textAt.y - at.y) < 6.0 + reach()) {
            return {Selected::Kind::Dimension, dimension.id};
        }
    }
    if (const document::DrawingNote* note = noteAt(at)) {
        return {Selected::Kind::Note, note->id};
    }
    for (auto box = m_sheet.views.rbegin(); box != m_sheet.views.rend(); ++box) {
        if (at.x >= box->left - reach() && at.x <= box->right + reach() &&
            at.y >= box->bottom - reach() && at.y <= box->top + reach()) {
            return {Selected::Kind::View, box->view};
        }
    }
    return {};
}

void SheetView::mousePressEvent(QMouseEvent* event) {
    setFocus();
    m_lastPosition = event->position();
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const Point2D at = toSheet(event->position());
    if (m_mode == Mode::Pick) {
        emit clicked(at, event->modifiers());
        return;
    }
    m_selected = pickAt(at);
    m_pressed = true;
    m_pressPosition = event->position();
    update();
    emit selectionChanged();
    if (m_selected.kind == Selected::Kind::View) {
        for (const drawing::ViewBox& box : m_sheet.views) {
            if (box.view == m_selected.id) {
                m_grabOffset = {box.x - at.x, box.y - at.y};
            }
        }
    } else if (m_selected.kind == Selected::Kind::Note) {
        for (const document::DrawingNote& note : m_notes) {
            if (note.id == m_selected.id) {
                m_grabOffset = {note.x - at.x, note.y - at.y};
            }
        }
    }
}

void SheetView::mouseMoveEvent(QMouseEvent* event) {
    if (m_panning) {
        m_origin += event->position() - m_lastPosition;
        m_lastPosition = event->position();
        m_following = false;
        update();
        return;
    }
    if (m_pressed && !m_dragging &&
        (m_selected.kind == Selected::Kind::View || m_selected.kind == Selected::Kind::Note)) {
        const QPointF moved = event->position() - m_pressPosition;
        if (moved.manhattanLength() >= QApplication::startDragDistance()) {
            m_dragging = true;
            setCursor(Qt::SizeAllCursor);
        }
    }
    m_lastPosition = event->position();
}

void SheetView::mouseReleaseEvent(QMouseEvent* event) {
    if (m_panning && event->button() == Qt::MiddleButton) {
        m_panning = false;
        setCursor(m_mode == Mode::Pick ? Qt::CrossCursor : Qt::ArrowCursor);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (m_dragging) {
        const Point2D at = toSheet(event->position());
        const Point2D place{at.x + m_grabOffset.x, at.y + m_grabOffset.y};
        if (m_selected.kind == Selected::Kind::View) {
            emit viewMoved(m_selected.id, place.x, place.y);
        } else if (m_selected.kind == Selected::Kind::Note) {
            emit noteMoved(m_selected.id, place.x, place.y);
        }
        setCursor(Qt::ArrowCursor);
    }
    m_dragging = false;
    m_pressed = false;
}

void SheetView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || m_mode != Mode::Select) {
        return;
    }
    if (const document::DrawingNote* note = noteAt(toSheet(event->position()))) {
        emit noteActivated(note->id);
    }
}

void SheetView::wheelEvent(QWheelEvent* event) {
    const Point2D under = toSheet(event->position());
    const double factor = std::pow(1.0015, event->angleDelta().y());
    m_zoom = std::clamp(m_zoom * factor, 0.1, 40.0);
    const QPointF now = toWidget(under);
    m_origin += event->position() - now;
    m_following = false;
    update();
}

void SheetView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        emit canceled();
        return;
    }
    QWidget::keyPressEvent(event);
}

void SheetView::askForText(const Point2D& at, const QString& text) {
    closeField();
    m_fieldAt = at;
    m_field = new QLineEdit(this);
    m_field->setObjectName(QStringLiteral("sheetText"));
    m_field->setText(text);
    m_field->selectAll();
    const QPointF place = toWidget(at);
    m_field->setGeometry(static_cast<int>(place.x()),
                         static_cast<int>(place.y()) - m_field->sizeHint().height(), 220,
                         m_field->sizeHint().height());
    m_field->installEventFilter(this);
    m_field->show();
    m_field->setFocus();
    connect(m_field, &QLineEdit::returnPressed, this, [this] {
        const QString typed = m_field->text();
        const Point2D entered = m_fieldAt;
        closeField();
        emit textEntered(entered, typed);
    });
}

void SheetView::closeField() {
    if (m_field == nullptr) {
        return;
    }
    QLineEdit* closing = m_field;
    m_field = nullptr;
    closing->hide();
    closing->deleteLater();
    setFocus();
}

bool SheetView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_field && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        closeField();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace lcad::app
