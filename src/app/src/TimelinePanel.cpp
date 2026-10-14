// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/TimelinePanel.h>

#include <lcad/app/FeaturePanel.h>
#include <lcad/app/Icons.h>
#include <lcad/app/Theme.h>

#include <QApplication>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QToolButton>
#include <QToolTip>

#include <algorithm>
#include <stdexcept>

namespace lcad::app {

namespace {

using document::FeatureKind;
using document::ItemKind;

// The room around and between the entries.
constexpr int margin = 10;
constexpr int gap = 10;

} // namespace

// ---------------------------------------------------------------------------------------------
// TimelinePanel

TimelinePanel::TimelinePanel(QWidget* parent)
    : QWidget(parent), m_scroll(new QScrollArea(this)), m_strip(new TimelineStrip),
      m_compact(new QToolButton(this)) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 4, 0);
    layout->setSpacing(4);

    auto* label = new QLabel(tr("Timeline").toUpper(), this);
    label->setObjectName(QStringLiteral("groupLabel"));
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() * Theme::instance().tokens().secondaryScale);
    label->setFont(font);
    label->setContentsMargins(8, 0, 0, 0);
    layout->addWidget(label);

    m_scroll->setObjectName(QStringLiteral("timelineScroll"));
    m_scroll->setWidget(m_strip);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    layout->addWidget(m_scroll, 1);

    m_compact->setObjectName(QStringLiteral("timelineCompact"));
    m_compact->setCheckable(true);
    m_compact->setAutoRaise(true);
    m_compact->setText(tr("Compact"));
    m_compact->setToolTip(tr("Draw the timeline smaller, to leave more room to the 3D view."));
    connect(m_compact, &QToolButton::toggled, this, &TimelinePanel::setCompact);
    layout->addWidget(m_compact);

    connect(m_strip, &TimelineStrip::itemSelected, this, &TimelinePanel::itemSelected);
    connect(m_strip, &TimelineStrip::itemActivated, this, &TimelinePanel::itemActivated);
    connect(m_strip, &TimelineStrip::contextMenuRequested, this,
            &TimelinePanel::contextMenuRequested);
    connect(m_strip, &TimelineStrip::message, this, &TimelinePanel::message);
}

TimelinePanel::~TimelinePanel() = default;

void TimelinePanel::setDocument(document::Document* document) {
    m_strip->setDocument(document);
}

document::ObjectId TimelinePanel::selectedItem() const {
    return m_strip->selected();
}

void TimelinePanel::setSelectedItem(document::ObjectId item) {
    m_strip->setSelected(item);
    if (item.isValid()) {
        const QRect rect = m_strip->itemRect(item);
        if (!rect.isEmpty()) {
            m_scroll->ensureVisible(rect.center().x(), rect.center().y(), rect.width(), 0);
        }
    }
}

void TimelinePanel::setActiveItem(document::ObjectId item) {
    m_strip->setActive(item);
}

void TimelinePanel::setCompact(bool compact) {
    m_strip->setCompact(compact);
    const QSignalBlocker blocker(m_compact);
    m_compact->setChecked(compact);
}

bool TimelinePanel::isCompact() const {
    return m_strip->isCompact();
}

QRect TimelinePanel::itemRect(document::ObjectId item) const {
    const QRect rect = m_strip->itemRect(item);
    return rect.isEmpty() ? rect : QRect(m_strip->mapTo(this, rect.topLeft()), rect.size());
}

QRect TimelinePanel::markerRect() const {
    const QRect rect = m_strip->markerRect();
    return QRect(m_strip->mapTo(this, rect.topLeft()), rect.size());
}

QString TimelinePanel::describe(document::ObjectId item) const {
    return m_strip->describe(item);
}

// ---------------------------------------------------------------------------------------------
// TimelineStrip

TimelineStrip::TimelineStrip(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("timeline"));
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

TimelineStrip::~TimelineStrip() {
    setDocument(nullptr);
}

void TimelineStrip::setDocument(document::Document* document) {
    if (m_document != nullptr) {
        m_document->removeListener(m_listener);
        m_listener = 0;
    }
    m_document = document;
    m_selected = {};
    m_active = {};
    if (m_document != nullptr) {
        m_listener = m_document->addListener(
            [this](const document::Change& change) { onDocumentChanged(change); });
    }
    updateGeometry();
    setMinimumWidth(sizeHint().width());
    update();
}

void TimelineStrip::onDocumentChanged(const document::Change& change) {
    using Kind = document::Change::Kind;
    if (change.kind == Kind::BodyGeometryChanged || change.kind == Kind::BodyVisibilityChanged ||
        change.kind == Kind::SketchVisibilityChanged ||
        change.kind == Kind::SketchGeometryChanged) {
        return;
    }
    const auto& timeline = m_document->timeline();
    if (m_selected.isValid() &&
        std::find(timeline.begin(), timeline.end(), m_selected) == timeline.end()) {
        m_selected = {};
    }
    setMinimumWidth(sizeHint().width());
    updateGeometry();
    update();
}

void TimelineStrip::setSelected(document::ObjectId item) {
    if (item != m_selected) {
        m_selected = item;
        update();
    }
}

void TimelineStrip::select(document::ObjectId item) {
    // Told also when the entry was selected already: a click on it is meant to show what it
    // stands for, which may have been selected elsewhere since.
    m_selected = item;
    update();
    emit itemSelected(item);
}

void TimelineStrip::setActive(document::ObjectId item) {
    if (item != m_active) {
        m_active = item;
        update();
    }
}

void TimelineStrip::setCompact(bool compact) {
    if (compact != m_compact) {
        m_compact = compact;
        setMinimumWidth(sizeHint().width());
        updateGeometry();
        update();
    }
}

int TimelineStrip::itemSize() const {
    return m_compact ? 20 : 28;
}

int TimelineStrip::pitch() const {
    return itemSize() + gap;
}

int TimelineStrip::left() const {
    return margin;
}

QSize TimelineStrip::sizeHint() const {
    const std::size_t count = m_document != nullptr ? m_document->timeline().size() : 0;
    return {2 * margin + static_cast<int>(count) * pitch(), itemSize() + 12};
}

QRect TimelineStrip::itemRect(document::ObjectId item) const {
    if (m_document == nullptr) {
        return {};
    }
    const auto& timeline = m_document->timeline();
    const auto found = std::find(timeline.begin(), timeline.end(), item);
    if (found == timeline.end()) {
        return {};
    }
    const int index = static_cast<int>(found - timeline.begin());
    return {left() + index * pitch(), (height() - itemSize()) / 2, itemSize(), itemSize()};
}

int TimelineStrip::gapX(std::size_t index) const {
    return left() + static_cast<int>(index) * pitch() - gap / 2;
}

std::size_t TimelineStrip::gapAt(int x) const {
    const std::size_t count = m_document != nullptr ? m_document->timeline().size() : 0;
    const int nearest = (x - left() + gap / 2 + pitch() / 2) / pitch();
    return static_cast<std::size_t>(std::clamp(nearest, 0, static_cast<int>(count)));
}

QRect TimelineStrip::markerRect() const {
    const std::size_t marker = m_document != nullptr ? m_document->historyMarker() : 0;
    return {gapX(marker) - 5, 0, 10, height()};
}

std::optional<std::size_t> TimelineStrip::indexAt(const QPoint& position) const {
    if (m_document == nullptr) {
        return std::nullopt;
    }
    const auto& timeline = m_document->timeline();
    for (std::size_t i = 0; i < timeline.size(); ++i) {
        if (itemRect(timeline[i]).adjusted(-2, -2, 2, 2).contains(position)) {
            return i;
        }
    }
    return std::nullopt;
}

QString TimelineStrip::iconName(document::ObjectId item) const {
    switch (m_document->kindOf(item)) {
    case ItemKind::Body:
        return QStringLiteral("box");
    case ItemKind::Sketch:
        return QStringLiteral("sketch");
    case ItemKind::Feature:
        switch (m_document->findFeature(item)->kind) {
        case FeatureKind::Pull:
            return QStringLiteral("pull");
        case FeatureKind::Revolve:
            return QStringLiteral("revolve");
        case FeatureKind::Fillet:
            return QStringLiteral("fillet");
        case FeatureKind::Chamfer:
            return QStringLiteral("chamfer");
        case FeatureKind::Shell:
            return QStringLiteral("shell");
        case FeatureKind::Mirror:
            return QStringLiteral("mirror");
        case FeatureKind::RectangularPattern:
            return QStringLiteral("pattern-rectangular");
        case FeatureKind::CircularPattern:
            return QStringLiteral("pattern-circular");
        case FeatureKind::Plane:
            return QStringLiteral("plane");
        case FeatureKind::Hole:
            return QStringLiteral("hole");
        case FeatureKind::Axis:
            return QStringLiteral("axis");
        }
    }
    return QStringLiteral("box");
}

QString TimelineStrip::describe(document::ObjectId item) const {
    if (m_document == nullptr) {
        return {};
    }
    QStringList lines;
    ItemKind kind = ItemKind::Body;
    try {
        kind = m_document->kindOf(item);
    } catch (const std::invalid_argument&) {
        return {};
    }
    const auto name = [](const std::string& text) { return QString::fromStdString(text); };
    // What uses an item: the features that take a sketch's outlines or work on a body.
    const auto usedBy = [this, &name](document::ObjectId id) {
        QStringList users;
        for (const document::Feature& feature : m_document->features()) {
            const bool makes =
                feature.operation == document::Operation::NewBody &&
                (feature.kind == FeatureKind::Pull || feature.kind == FeatureKind::Revolve);
            const bool copiesIt = std::find(feature.sources.begin(), feature.sources.end(), id) !=
                                  feature.sources.end();
            if (feature.id != id &&
                (feature.sketch == id || (feature.body == id && !makes) || copiesIt)) {
                users.append(name(feature.name));
            }
        }
        return users;
    };
    switch (kind) {
    case ItemKind::Body: {
        const document::Body* body = m_document->findBody(item);
        lines.append(tr("%1 · Body").arg(name(body->name)));
        lines.append(tr("Put into the document as it is."));
        if (const QStringList users = usedBy(item); !users.isEmpty()) {
            lines.append(tr("Changed by %1").arg(users.join(QStringLiteral(", "))));
        }
        break;
    }
    case ItemKind::Sketch: {
        const document::SketchItem* sketch = m_document->findSketch(item);
        lines.append(tr("%1 · Sketch").arg(name(sketch->name)));
        if (const document::Feature* plane = m_document->findFeature(sketch->attachedTo)) {
            lines.append(tr("Drawn on %1, and follows it").arg(name(plane->name)));
        } else if (const document::Body* body = m_document->findBody(sketch->attachedTo)) {
            lines.append(tr("Drawn on a face of %1, and follows it").arg(name(body->name)));
        }
        if (const std::string error = m_document->featureError(item); !error.empty()) {
            lines.append(tr("Cannot follow: %1").arg(name(error)));
        }
        if (const QStringList users = usedBy(item); !users.isEmpty()) {
            lines.append(tr("Used by %1").arg(users.join(QStringLiteral(", "))));
        }
        break;
    }
    case ItemKind::Feature: {
        const document::Feature* feature = m_document->findFeature(item);
        lines.append(tr("%1 · %2").arg(name(feature->name), featureKindName(feature->kind)));
        if (const document::SketchItem* sketch = m_document->findSketch(feature->sketch)) {
            lines.append(tr("Made from %1").arg(name(sketch->name)));
        }
        QStringList copied;
        for (const document::ObjectId source : feature->sources) {
            if (const document::Feature* original = m_document->findFeature(source)) {
                copied.append(name(original->name));
            }
        }
        if (!copied.isEmpty()) {
            lines.append(tr("Copies %1").arg(copied.join(QStringLiteral(", "))));
        }
        if (const document::Body* body = m_document->findBody(feature->body)) {
            const bool makes =
                feature->operation == document::Operation::NewBody &&
                (feature->kind == FeatureKind::Pull || feature->kind == FeatureKind::Revolve);
            lines.append(makes ? tr("Makes %1").arg(name(body->name))
                               : tr("Works on %1").arg(name(body->name)));
            if (makes) {
                if (const QStringList users = usedBy(body->id); !users.isEmpty()) {
                    lines.append(tr("Then changed by %1").arg(users.join(QStringLiteral(", "))));
                }
            }
        }
        if (feature->suppressed) {
            lines.append(tr("Suppressed: it is not applied."));
        }
        if (const std::string error = m_document->featureError(item); !error.empty()) {
            lines.append(tr("Fails: %1").arg(name(error)));
        }
        break;
    }
    }
    const auto& timeline = m_document->timeline();
    const auto place = static_cast<std::size_t>(std::find(timeline.begin(), timeline.end(), item) -
                                                timeline.begin());
    if (place >= m_document->historyMarker()) {
        lines.append(tr("After the history marker: not applied now."));
    }
    lines.append(kind == ItemKind::Body ? tr("Drag to move it.")
                                        : tr("Double-click to edit. Drag to move it."));
    return lines.join(QLatin1Char('\n'));
}

void TimelineStrip::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const ThemeTokens& tokens = Theme::instance().tokens();
    if (m_document == nullptr) {
        return;
    }
    const auto& timeline = m_document->timeline();
    const std::size_t marker = m_document->historyMarker();
    if (timeline.empty()) {
        painter.setPen(tokens.textSecondary);
        painter.drawText(rect().adjusted(margin, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                         tr("Nothing yet. Sketches and features appear here in the order they "
                            "are made."));
        return;
    }
    for (std::size_t i = 0; i < timeline.size(); ++i) {
        const document::ObjectId item = timeline[i];
        const QRect box = itemRect(item);
        const document::Feature* feature = m_document->findFeature(item);
        const bool suppressed = feature != nullptr && feature->suppressed;
        // A feature that fails, or a sketch that cannot follow what it was drawn on.
        const bool failed = !m_document->featureError(item).empty();
        const bool beyond = i >= marker;
        painter.setOpacity(suppressed || beyond ? 0.4 : 1.0);
        if (item == m_selected) {
            QColor fill = tokens.accent;
            fill.setAlpha(90);
            painter.setPen(QPen(tokens.accent, 1.5));
            painter.setBrush(fill);
        } else {
            painter.setPen(QPen(tokens.border, 1.0));
            painter.setBrush(tokens.field);
        }
        if (failed) {
            painter.setPen(QPen(tokens.error, 2.0));
        }
        if (item == m_active) {
            painter.setPen(QPen(tokens.accent, 2.5));
        }
        painter.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), tokens.radius,
                                tokens.radius);
        const int inset = m_compact ? 2 : 4;
        icon(iconName(item)).paint(&painter, box.adjusted(inset, inset, -inset, -inset));
        if (suppressed) {
            painter.setPen(QPen(tokens.text, 1.5));
            painter.drawLine(box.bottomLeft() + QPoint(3, -3), box.topRight() + QPoint(-3, 3));
        }
        painter.setOpacity(1.0);
        if (failed) {
            // A dot in the corner, which still shows when the entry is faded.
            painter.setPen(Qt::NoPen);
            painter.setBrush(tokens.error);
            painter.drawEllipse(QPointF(box.right() - 1, box.top() + 1), 4.0, 4.0);
        }
    }

    // Where a dragged entry or the marker would go.
    if (m_dragging && m_pressedIndex.has_value()) {
        const int x = gapX(gapAt(m_dragX));
        painter.setPen(QPen(tokens.accent, 2.0));
        painter.drawLine(x, 2, x, height() - 2);
    }
    const int markerX = m_dragging && m_pressedMarker ? gapX(gapAt(m_dragX)) : gapX(marker);
    painter.setPen(QPen(tokens.text, 2.0));
    painter.drawLine(markerX, 6, markerX, height() - 2);
    QPainterPath handle;
    handle.moveTo(markerX - 5, 1);
    handle.lineTo(markerX + 5, 1);
    handle.lineTo(markerX, 7);
    handle.closeSubpath();
    painter.fillPath(handle, tokens.text);
}

void TimelineStrip::mousePressEvent(QMouseEvent* event) {
    if (m_document == nullptr || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_pressAt = event->position().toPoint();
    m_dragging = false;
    m_pressedMarker = markerRect().contains(event->position().toPoint());
    m_pressedIndex.reset();
    if (!m_pressedMarker) {
        m_pressedIndex = indexAt(event->position().toPoint());
        select(m_pressedIndex.has_value() ? m_document->timeline()[*m_pressedIndex]
                                          : document::ObjectId{});
    }
}

void TimelineStrip::mouseMoveEvent(QMouseEvent* event) {
    if (!m_pressAt.has_value() || !(event->buttons() & Qt::LeftButton)) {
        // The pointer over the marker says it can be dragged.
        setCursor(markerRect().contains(event->position().toPoint()) ? Qt::SizeHorCursor
                                                                     : Qt::ArrowCursor);
        return;
    }
    if (!m_dragging && (event->position().toPoint() - *m_pressAt).manhattanLength() <
                           QApplication::startDragDistance()) {
        return;
    }
    if (m_pressedMarker || m_pressedIndex.has_value()) {
        m_dragging = true;
        m_dragX = event->position().toPoint().x();
        update();
    }
}

void TimelineStrip::mouseReleaseEvent(QMouseEvent* event) {
    if (!m_pressAt.has_value() || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool dragging = m_dragging;
    const bool marker = m_pressedMarker;
    const std::optional<std::size_t> pressed = m_pressedIndex;
    m_pressAt.reset();
    m_pressedIndex.reset();
    m_pressedMarker = false;
    m_dragging = false;
    update();
    if (!dragging || m_document == nullptr) {
        return;
    }
    const std::size_t target = gapAt(event->position().toPoint().x());
    if (marker) {
        m_document->setHistoryMarker(target);
        return;
    }
    if (!pressed.has_value()) {
        return;
    }
    // A gap after the entry's own place is one place less once it is taken out.
    const std::size_t index = target > *pressed ? target - 1 : target;
    if (index == *pressed) {
        return;
    }
    try {
        m_document->moveInTimeline(m_document->timeline()[*pressed], index);
    } catch (const std::invalid_argument& refused) {
        emit message(QString::fromUtf8(refused.what()));
    }
}

void TimelineStrip::mouseDoubleClickEvent(QMouseEvent* event) {
    if (m_document == nullptr || event->button() != Qt::LeftButton) {
        return;
    }
    if (const auto index = indexAt(event->position().toPoint())) {
        const document::ObjectId item = m_document->timeline()[*index];
        select(item);
        emit itemActivated(item);
    }
}

void TimelineStrip::contextMenuEvent(QContextMenuEvent* event) {
    if (m_document == nullptr) {
        return;
    }
    const auto index = indexAt(event->pos());
    if (!index.has_value()) {
        return;
    }
    const document::ObjectId item = m_document->timeline()[*index];
    select(item);
    emit contextMenuRequested(item, event->globalPos());
}

void TimelineStrip::keyPressEvent(QKeyEvent* event) {
    if (m_document == nullptr || m_document->timeline().empty()) {
        QWidget::keyPressEvent(event);
        return;
    }
    const auto& timeline = m_document->timeline();
    const auto found = std::find(timeline.begin(), timeline.end(), m_selected);
    const auto index = static_cast<std::ptrdiff_t>(found - timeline.begin());
    const auto last = static_cast<std::ptrdiff_t>(timeline.size()) - 1;
    switch (event->key()) {
    case Qt::Key_Left:
        select(timeline[static_cast<std::size_t>(
            found == timeline.end() ? last : std::max<std::ptrdiff_t>(index - 1, 0))]);
        return;
    case Qt::Key_Right:
        select(timeline[static_cast<std::size_t>(
            found == timeline.end() ? 0 : std::min(index + 1, last))]);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_selected.isValid()) {
            emit itemActivated(m_selected);
        }
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

bool TimelineStrip::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip && m_document != nullptr) {
        auto* help = static_cast<QHelpEvent*>(event);
        QString text;
        if (const auto index = indexAt(help->pos())) {
            text = describe(m_document->timeline()[*index]);
        } else if (markerRect().contains(help->pos())) {
            text = tr("The history marker. Everything to its left is applied. Drag it to look "
                      "at, or work on, an earlier state of the model.");
        }
        if (text.isEmpty()) {
            QToolTip::hideText();
            event->ignore();
        } else {
            QToolTip::showText(help->globalPos(), text, this);
        }
        return true;
    }
    return QWidget::event(event);
}

} // namespace lcad::app
