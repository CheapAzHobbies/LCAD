// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>

#include <QPoint>
#include <QRect>
#include <QString>
#include <QWidget>

#include <optional>
#include <vector>

class QScrollArea;
class QToolButton;

namespace lcad::app {

class TimelineStrip;

/// The timeline: the sketches and features of a document, and the bodies put in as they are,
/// left to right in the order they are applied, with the history marker after the last one
/// that is applied.
///
/// An entry is selected with a click and opened for editing with a double-click. Dragging an
/// entry moves it in the timeline, unless that would put it before something it depends on;
/// then nothing moves and message() says why. Dragging the marker rolls the model back to an
/// earlier state, or forward again. Suppressed entries are drawn faded and struck through,
/// failing ones in the error color with the reason in the tooltip, and the entries past the
/// marker faded.
class TimelinePanel : public QWidget {
    Q_OBJECT

public:
    explicit TimelinePanel(QWidget* parent = nullptr);
    ~TimelinePanel() override;

    /// Shows a document's timeline and follows its changes. The document has to stay alive
    /// until another one, or nullptr, is set.
    void setDocument(document::Document* document);

    /// The entry that is selected, if any.
    [[nodiscard]] document::ObjectId selectedItem() const;
    void setSelectedItem(document::ObjectId item);
    /// The feature being edited, which is drawn as such. An invalid id for none.
    void setActiveItem(document::ObjectId item);

    /// Smaller entries, for a window short of room.
    void setCompact(bool compact);
    [[nodiscard]] bool isCompact() const;

    /// Where an entry, and the marker, are drawn, in the panel's coordinates. An empty
    /// rectangle for an entry that is not in the timeline.
    [[nodiscard]] QRect itemRect(document::ObjectId item) const;
    [[nodiscard]] QRect markerRect() const;
    /// The text of an entry's tooltip: its name and kind, what it depends on, and why it fails.
    [[nodiscard]] QString describe(document::ObjectId item) const;

signals:
    void itemSelected(document::ObjectId item);
    /// The user double-clicked an entry.
    void itemActivated(document::ObjectId item);
    void contextMenuRequested(document::ObjectId item, const QPoint& globalPosition);
    /// Something the user tried was refused, and why.
    void message(const QString& text);

private:
    QScrollArea* m_scroll;
    TimelineStrip* m_strip;
    QToolButton* m_compact;
};

/// The entries and the marker, drawn and handled. Part of TimelinePanel.
class TimelineStrip : public QWidget {
    Q_OBJECT

public:
    explicit TimelineStrip(QWidget* parent = nullptr);
    ~TimelineStrip() override;

    void setDocument(document::Document* document);
    [[nodiscard]] document::ObjectId selected() const { return m_selected; }
    void setSelected(document::ObjectId item);
    void setActive(document::ObjectId item);
    void setCompact(bool compact);
    [[nodiscard]] bool isCompact() const { return m_compact; }
    [[nodiscard]] QRect itemRect(document::ObjectId item) const;
    [[nodiscard]] QRect markerRect() const;
    [[nodiscard]] QString describe(document::ObjectId item) const;
    [[nodiscard]] QSize sizeHint() const override;

signals:
    void itemSelected(document::ObjectId item);
    void itemActivated(document::ObjectId item);
    void contextMenuRequested(document::ObjectId item, const QPoint& globalPosition);
    void message(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;

private:
    [[nodiscard]] int itemSize() const;
    [[nodiscard]] int pitch() const;
    [[nodiscard]] int left() const;
    /// The x of the gap before entry `index`: where the marker or a dragged entry goes.
    [[nodiscard]] int gapX(std::size_t index) const;
    /// The gap nearest to an x, as the index of the entry after it.
    [[nodiscard]] std::size_t gapAt(int x) const;
    [[nodiscard]] std::optional<std::size_t> indexAt(const QPoint& position) const;
    [[nodiscard]] QString iconName(document::ObjectId item) const;
    void onDocumentChanged(const document::Change& change);
    void select(document::ObjectId item);

    document::Document* m_document = nullptr;
    document::Document::ListenerId m_listener = 0;
    document::ObjectId m_selected;
    document::ObjectId m_active;
    bool m_compact = false;
    // A press, and what it may turn into: dragging an entry, or the marker.
    std::optional<QPoint> m_pressAt;
    std::optional<std::size_t> m_pressedIndex;
    bool m_pressedMarker = false;
    bool m_dragging = false;
    int m_dragX = 0;
};

} // namespace lcad::app
