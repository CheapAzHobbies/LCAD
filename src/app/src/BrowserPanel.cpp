// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/BrowserPanel.h>

#include <QFont>
#include <QHeaderView>
#include <QLineEdit>
#include <QPalette>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <stdexcept>

namespace lcad::app {

namespace {

constexpr int idRole = Qt::UserRole;

document::ObjectId idOf(const QTreeWidgetItem* item) {
    return document::ObjectId{item->data(0, idRole).toULongLong()};
}

} // namespace

BrowserPanel::BrowserPanel(QWidget* parent)
    : QWidget(parent), m_filter(new QLineEdit(this)), m_tree(new QTreeWidget(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_filter);
    layout->addWidget(m_tree);

    m_filter->setObjectName(QStringLiteral("browserFilter"));
    m_filter->setPlaceholderText(tr("Filter by name"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &BrowserPanel::applyFilter);

    m_tree->setHeaderHidden(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // A double-click renames a body and opens a sketch, so it is handled here, not by the tree.
    m_tree->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (item->parent() == m_bodies && m_bodies != nullptr) {
            m_tree->editItem(item, 0);
        } else if (item->parent() == m_sketches && m_sketches != nullptr) {
            emit sketchActivated(idOf(item));
        }
    });

    connect(m_tree, &QTreeWidget::itemChanged, this, &BrowserPanel::onItemChanged);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &BrowserPanel::bodySelectionChanged);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &BrowserPanel::showContextMenu);
}

BrowserPanel::~BrowserPanel() {
    setDocument(nullptr, {});
}

void BrowserPanel::setDocument(document::Document* document, const QString& name) {
    if (m_document != nullptr) {
        m_document->removeListener(m_listener);
        m_listener = 0;
    }
    m_document = document;
    m_name = name;
    if (m_document != nullptr) {
        m_listener = m_document->addListener(
            [this](const document::Change& change) { onDocumentChanged(change); });
    }
    rebuild();
}

void BrowserPanel::setDocumentName(const QString& name) {
    m_name = name;
    if (m_root != nullptr) {
        const QSignalBlocker blocker(m_tree);
        m_root->setText(0, name);
    }
}

std::vector<document::ObjectId> BrowserPanel::selectedBodies() const {
    std::vector<document::ObjectId> ids;
    if (m_bodies == nullptr) {
        return ids;
    }
    // In document order, whatever order they were clicked in.
    for (int i = 0; i < m_bodies->childCount(); ++i) {
        if (m_bodies->child(i)->isSelected()) {
            ids.push_back(idOf(m_bodies->child(i)));
        }
    }
    return ids;
}

std::vector<document::ObjectId> BrowserPanel::selectedSketches() const {
    std::vector<document::ObjectId> ids;
    if (m_sketches == nullptr) {
        return ids;
    }
    for (int i = 0; i < m_sketches->childCount(); ++i) {
        if (m_sketches->child(i)->isSelected()) {
            ids.push_back(idOf(m_sketches->child(i)));
        }
    }
    return ids;
}

void BrowserPanel::setSelectedBodies(const std::vector<document::ObjectId>& bodies) {
    setSelection(bodies, {});
}

void BrowserPanel::setSelection(const std::vector<document::ObjectId>& bodies,
                                const std::vector<document::ObjectId>& sketches) {
    const QSignalBlocker blocker(m_tree);
    m_tree->clearSelection();
    for (const document::ObjectId id : bodies) {
        if (QTreeWidgetItem* item = bodyItem(id)) {
            item->setSelected(true);
        }
    }
    for (const document::ObjectId id : sketches) {
        if (QTreeWidgetItem* item = sketchItem(id)) {
            item->setSelected(true);
        }
    }
}

QTreeWidgetItem* BrowserPanel::sketchItem(document::ObjectId id) const {
    if (m_sketches == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < m_sketches->childCount(); ++i) {
        if (idOf(m_sketches->child(i)) == id) {
            return m_sketches->child(i);
        }
    }
    return nullptr;
}

void BrowserPanel::editSketchName(document::ObjectId id) {
    if (QTreeWidgetItem* item = sketchItem(id)) {
        m_tree->editItem(item, 0);
    }
}

void BrowserPanel::setEditedSketch(document::ObjectId id) {
    m_editedSketch = id;
    markEditedSketch();
}

void BrowserPanel::markEditedSketch() {
    if (m_sketches == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_tree);
    for (int i = 0; i < m_sketches->childCount(); ++i) {
        QTreeWidgetItem* item = m_sketches->child(i);
        QFont font = item->font(0);
        font.setBold(idOf(item) == m_editedSketch);
        item->setFont(0, font);
    }
}

QTreeWidgetItem* BrowserPanel::bodyItem(document::ObjectId id) const {
    if (m_bodies == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < m_bodies->childCount(); ++i) {
        if (idOf(m_bodies->child(i)) == id) {
            return m_bodies->child(i);
        }
    }
    return nullptr;
}

QTreeWidgetItem* BrowserPanel::originItem(viewport::OriginItem item) const {
    return m_origin != nullptr ? m_origin->child(static_cast<int>(item)) : nullptr;
}

void BrowserPanel::editBody(document::ObjectId id) {
    if (QTreeWidgetItem* item = bodyItem(id)) {
        m_tree->editItem(item, 0);
    }
}

void BrowserPanel::setFilter(const QString& text) {
    m_filter->setText(text);
}

void BrowserPanel::setOriginVisibility(const std::array<bool, 6>& visible) {
    m_originVisible = visible;
    if (m_origin == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_tree);
    for (int i = 0; i < m_origin->childCount(); ++i) {
        m_origin->child(i)->setCheckState(0, visible[static_cast<std::size_t>(i)] ? Qt::Checked
                                                                                  : Qt::Unchecked);
    }
}

void BrowserPanel::applyFilter() {
    if (m_bodies == nullptr || m_sketches == nullptr) {
        return;
    }
    const QString wanted = m_filter->text().trimmed();
    for (QTreeWidgetItem* group : {m_bodies, m_sketches}) {
        for (int i = 0; i < group->childCount(); ++i) {
            QTreeWidgetItem* item = group->child(i);
            item->setHidden(!wanted.isEmpty() &&
                            !item->text(0).contains(wanted, Qt::CaseInsensitive));
        }
    }
    // A document without sketches has no use for an empty "Sketches" entry.
    m_sketches->setHidden(m_sketches->childCount() == 0);
}

void BrowserPanel::rebuild() {
    const QSignalBlocker blocker(m_tree);
    m_tree->clear();
    m_root = nullptr;
    m_origin = nullptr;
    m_bodies = nullptr;
    m_sketches = nullptr;
    if (m_document == nullptr) {
        return;
    }
    m_root = new QTreeWidgetItem(m_tree, {m_name});
    QFont bold = m_root->font(0);
    bold.setBold(true);
    m_root->setFont(0, bold);
    m_root->setFlags(Qt::ItemIsEnabled);

    // In the order of viewport::OriginItem.
    m_origin = new QTreeWidgetItem(m_root, {tr("Origin")});
    m_origin->setFlags(Qt::ItemIsEnabled);
    const QStringList names{tr("XY Plane"), tr("XZ Plane"), tr("YZ Plane"),
                            tr("X Axis"),   tr("Y Axis"),   tr("Z Axis")};
    for (int i = 0; i < names.size(); ++i) {
        auto* item = new QTreeWidgetItem(m_origin, {names.at(i)});
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(0, m_originVisible[static_cast<std::size_t>(i)] ? Qt::Checked
                                                                            : Qt::Unchecked);
        item->setToolTip(0, tr("Check to show this guide in the 3D view."));
    }

    m_bodies = new QTreeWidgetItem(m_root, {tr("Bodies")});
    m_bodies->setFlags(Qt::ItemIsEnabled);
    for (const document::Body& body : m_document->bodies()) {
        m_bodies->addChild(makeBodyItem(body));
    }
    m_sketches = new QTreeWidgetItem(m_root, {tr("Sketches")});
    m_sketches->setFlags(Qt::ItemIsEnabled);
    for (const document::SketchItem& sketch : m_document->sketches()) {
        m_sketches->addChild(makeSketchItem(sketch));
    }
    m_root->setExpanded(true);
    m_bodies->setExpanded(true);
    m_sketches->setExpanded(true);
    markEditedSketch();
    applyFilter();
}

QTreeWidgetItem* BrowserPanel::makeSketchItem(const document::SketchItem& sketch) {
    auto* item = new QTreeWidgetItem({QString::fromStdString(sketch.name)});
    item->setData(0, idRole, QVariant::fromValue<qulonglong>(sketch.id.value));
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                   Qt::ItemIsUserCheckable);
    item->setCheckState(0, sketch.visible ? Qt::Checked : Qt::Unchecked);
    item->setToolTip(0, tr("Check to show, clear to hide. Double-click to edit the sketch."));
    return item;
}

QTreeWidgetItem* BrowserPanel::makeBodyItem(const document::Body& body) {
    auto* item = new QTreeWidgetItem({QString::fromStdString(body.name)});
    item->setData(0, idRole, QVariant::fromValue<qulonglong>(body.id.value));
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                   Qt::ItemIsUserCheckable);
    item->setCheckState(0, body.visible ? Qt::Checked : Qt::Unchecked);
    markExistence(item, body);
    return item;
}

void BrowserPanel::markExistence(QTreeWidgetItem* item, const document::Body& body) {
    // A body whose feature fails, is suppressed, or lies beyond the history marker is listed,
    // so that it can still be found and renamed, but in italics and with the reason.
    const bool exists = !body.shape.isNull();
    QFont font = item->font(0);
    font.setItalic(!exists);
    item->setFont(0, font);
    item->setData(0, Qt::ForegroundRole,
                  exists ? QVariant()
                         : QVariant(m_tree->palette().color(QPalette::PlaceholderText)));
    item->setToolTip(0, exists ? tr("Check to show, clear to hide. Double-click to rename.")
                               : tr("This body does not exist at this point in the timeline: "
                                    "the feature that makes it is suppressed, fails, or comes "
                                    "after the history marker."));
}

void BrowserPanel::onDocumentChanged(const document::Change& change) {
    using Kind = document::Change::Kind;
    if (change.kind == Kind::HistoryChanged || m_bodies == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_tree);
    const document::Body* body = m_document->findBody(change.body);
    const document::SketchItem* sketch = m_document->findSketch(change.body);
    QTreeWidgetItem* item = bodyItem(change.body);
    QTreeWidgetItem* sketchEntry = sketchItem(change.body);
    bool selectionAffected = false;
    switch (change.kind) {
    case Kind::BodyAdded: {
        if (body == nullptr) {
            break;
        }
        // At the position the body has in the document, which undo may make the middle.
        const auto& bodies = m_document->bodies();
        const int index = static_cast<int>(body - bodies.data());
        m_bodies->insertChild(index, makeBodyItem(*body));
        m_bodies->setExpanded(true);
        break;
    }
    case Kind::BodyRemoved:
        selectionAffected = item != nullptr && item->isSelected();
        delete item;
        break;
    case Kind::BodyRenamed:
        if (item != nullptr && body != nullptr) {
            item->setText(0, QString::fromStdString(body->name));
        }
        break;
    case Kind::BodyVisibilityChanged:
        if (item != nullptr && body != nullptr) {
            item->setCheckState(0, body->visible ? Qt::Checked : Qt::Unchecked);
        }
        break;
    case Kind::SketchAdded: {
        if (sketch == nullptr || m_sketches == nullptr) {
            break;
        }
        const auto& sketches = m_document->sketches();
        m_sketches->insertChild(static_cast<int>(sketch - sketches.data()),
                                makeSketchItem(*sketch));
        m_sketches->setExpanded(true);
        markEditedSketch();
        break;
    }
    case Kind::SketchRemoved:
        selectionAffected = sketchEntry != nullptr && sketchEntry->isSelected();
        delete sketchEntry;
        break;
    case Kind::SketchRenamed:
        if (sketchEntry != nullptr && sketch != nullptr) {
            sketchEntry->setText(0, QString::fromStdString(sketch->name));
        }
        break;
    case Kind::SketchVisibilityChanged:
        if (sketchEntry != nullptr && sketch != nullptr) {
            sketchEntry->setCheckState(0, sketch->visible ? Qt::Checked : Qt::Unchecked);
        }
        break;
    case Kind::BodyGeometryChanged:
        if (item != nullptr && body != nullptr) {
            markExistence(item, *body);
        }
        break;
    case Kind::SketchGeometryChanged:
    case Kind::TimelineChanged:
    case Kind::HistoryChanged:
        break;
    }
    applyFilter();
    if (selectionAffected) {
        // Signals are blocked here, and a selected entry has just gone.
        QMetaObject::invokeMethod(this, &BrowserPanel::bodySelectionChanged, Qt::QueuedConnection);
    }
}

void BrowserPanel::onItemChanged(QTreeWidgetItem* item) {
    if (m_document != nullptr && item->parent() == m_origin && m_origin != nullptr) {
        const int index = m_origin->indexOfChild(item);
        const bool visible = item->checkState(0) == Qt::Checked;
        m_originVisible[static_cast<std::size_t>(index)] = visible;
        emit originItemToggled(static_cast<viewport::OriginItem>(index), visible);
        return;
    }
    if (m_document != nullptr && item->parent() == m_sketches && m_sketches != nullptr) {
        const document::ObjectId id = idOf(item);
        const document::SketchItem* sketch = m_document->findSketch(id);
        if (sketch == nullptr) {
            return;
        }
        const std::string name = item->text(0).trimmed().toStdString();
        const bool visible = item->checkState(0) == Qt::Checked;
        try {
            if (name != sketch->name) {
                m_document->renameSketch(id, name);
            } else if (visible != sketch->visible) {
                m_document->setSketchVisible(id, visible);
            }
        } catch (const std::invalid_argument&) {
            // An empty name. Fall through and show what the document has.
        }
        sketch = m_document->findSketch(id);
        if (sketch != nullptr) {
            const QSignalBlocker blocker(m_tree);
            item->setText(0, QString::fromStdString(sketch->name));
            item->setCheckState(0, sketch->visible ? Qt::Checked : Qt::Unchecked);
        }
        return;
    }
    if (m_document == nullptr || item->parent() != m_bodies) {
        return;
    }
    const document::ObjectId id = idOf(item);
    const document::Body* body = m_document->findBody(id);
    if (body == nullptr) {
        return;
    }
    const std::string name = item->text(0).trimmed().toStdString();
    const bool visible = item->checkState(0) == Qt::Checked;
    try {
        if (name != body->name) {
            m_document->renameBody(id, name);
        } else if (visible != body->visible) {
            m_document->setBodyVisible(id, visible);
        }
    } catch (const std::invalid_argument&) {
        // An empty name. Fall through and put the entry back the way the document has it.
    }
    // Show what the document now says, which is the old name if the new one was refused.
    body = m_document->findBody(id);
    if (body != nullptr) {
        const QSignalBlocker blocker(m_tree);
        item->setText(0, QString::fromStdString(body->name));
        item->setCheckState(0, body->visible ? Qt::Checked : Qt::Unchecked);
    }
}

void BrowserPanel::showContextMenu(const QPoint& position) {
    QTreeWidgetItem* item = m_tree->itemAt(position);
    if (m_document == nullptr || item == nullptr ||
        (item->parent() != m_bodies && item->parent() != m_sketches)) {
        return;
    }
    // The menu acts on the selection, so the entry that was clicked has to be in it.
    if (!item->isSelected()) {
        m_tree->clearSelection();
        item->setSelected(true);
    }
    emit contextMenuRequested(m_tree->viewport()->mapToGlobal(position));
}

} // namespace lcad::app
