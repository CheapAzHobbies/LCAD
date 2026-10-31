// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/drawing/Sheet.h>

#include <QPointF>
#include <QString>
#include <QWidget>

#include <optional>

class QLineEdit;
class QPainter;

namespace lcad::app {

/// Paints a sheet with its lower left corner at `origin` and `scale` device pixels to the
/// millimeter: the screen and PDF files go through this.
void paintSheet(QPainter& painter, const drawing::Sheet& sheet, const QPointF& origin,
                double scale);

/// A sheet of a drawing on screen, like paper on a desk. Scroll to zoom and drag with the middle
/// button to move about. The view holds no drawing of its own: it shows the Sheet it is given,
/// and says what was done to it by signals.
class SheetView : public QWidget {
    Q_OBJECT

public:
    /// What is selected.
    struct Selected {
        enum class Kind { None, View, Dimension, Note };
        Kind kind = Kind::None;
        document::ObjectId id;
    };

    /// What a click does.
    enum class Mode {
        /// Selects, and drags views.
        Select,
        /// A click is for the tool in use: see clicked().
        Pick,
    };

    explicit SheetView(QWidget* parent = nullptr);

    void setSheet(drawing::Sheet sheet);
    [[nodiscard]] const drawing::Sheet& sheet() const { return m_sheet; }
    /// Where notes are, so that they can be selected and opened by a click.
    void setNotes(std::vector<document::DrawingNote> notes);

    void setMode(Mode mode);
    [[nodiscard]] Mode mode() const { return m_mode; }
    [[nodiscard]] Selected selected() const { return m_selected; }
    void setSelected(Selected selected);

    /// Shows the whole sheet.
    void fit();

    /// Where a place on the sheet is in the widget, and the other way.
    [[nodiscard]] QPointF toWidget(const drawing::Point2D& onSheet) const;
    [[nodiscard]] drawing::Point2D toSheet(const QPointF& inWidget) const;
    /// How far on the sheet a click may miss by, in millimeters.
    [[nodiscard]] double reach() const;

    /// Opens a field for typing text at a place on the sheet. textEntered() says what was typed.
    void askForText(const drawing::Point2D& at, const QString& text = {});
    [[nodiscard]] QLineEdit* textField() const { return m_field; }

    /// Marks a place on the sheet: what a tool has picked so far. None marks nothing.
    void setMark(std::optional<drawing::Point2D> at);

signals:
    /// The user changed the selection.
    void selectionChanged();
    /// With a tool in use, a click on the sheet.
    void clicked(const lcad::drawing::Point2D& at, Qt::KeyboardModifiers modifiers);
    /// A view was dragged to a new place.
    void viewMoved(lcad::document::ObjectId view, double x, double y);
    /// A note was dragged to a new place.
    void noteMoved(lcad::document::ObjectId note, double x, double y);
    /// The user double-clicked a note.
    void noteActivated(lcad::document::ObjectId note);
    void textEntered(const lcad::drawing::Point2D& at, const QString& text);
    /// Esc, with no field open.
    void canceled();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    [[nodiscard]] Selected pickAt(const drawing::Point2D& at) const;
    [[nodiscard]] const document::DrawingNote* noteAt(const drawing::Point2D& at) const;
    void closeField();

    drawing::Sheet m_sheet;
    std::vector<document::DrawingNote> m_notes;
    Mode m_mode = Mode::Select;
    Selected m_selected;
    /// Pixels to the millimeter, and where the sheet's lower left corner is.
    double m_zoom = 2.0;
    QPointF m_origin;
    bool m_fitted = false;
    bool m_following = true;
    /// Moving about, or dragging what is selected.
    bool m_panning = false;
    bool m_dragging = false;
    bool m_pressed = false;
    QPointF m_pressPosition;
    QPointF m_lastPosition;
    drawing::Point2D m_grabOffset;
    std::optional<drawing::Point2D> m_mark;
    QLineEdit* m_field = nullptr;
    drawing::Point2D m_fieldAt;
};

} // namespace lcad::app
