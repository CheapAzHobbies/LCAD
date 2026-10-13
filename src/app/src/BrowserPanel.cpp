// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/BrowserPanel.h>

#include <QFont>
#include <QHeaderView>
#include <QLineEdit>
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
    m_filter->setPlaceholderText(tr("Filter bodies by name"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &BrowserPanel::applyFilter);

    m_tree->setHeaderHidden(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::DoubleClicked);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);

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

void BrowserPanel::setSelectedBodies(const std::vector<document::ObjectId>& bodies) {
    const QSignalBlocker blocker(m_tree);
    m_tree->clearSelection();
    for (const document::ObjectId id : bodies) {
        if (QTreeWidgetItem* item = bodyItem(id)) {
            item->setSelected(true);
        }
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
    if (m_bodies == nullptr) {
        return;
    }
    const QString wanted = m_filter->text().trimmed();
    for (int i = 0; i < m_bodies->childCount(); ++i) {
        QTreeWidgetItem* item = m_bodies->child(i);
        item->setHidden(!wanted.isEmpty() && !item->text(0).contains(wanted, Qt::CaseInsensitive));
    }
}

void BrowserPanel::rebuild() {
    const QSignalBlocker blocker(m_tree);
    m_tree->clear();
    m_root = nullptr;
    m_origin = nullptr;
    m_bodies = nullptr;
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
    m_root->setExpanded(true);
    m_bodies->setExpanded(true);
    applyFilter();
}

QTreeWidgetItem* BrowserPanel::makeBodyItem(const document::Body& body) {
    auto* item = new QTreeWidgetItem({QString::fromStdString(body.name)});
    item->setData(0, idRole, QVariant::fromValue<qulonglong>(body.id.value));
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                   Qt::ItemIsUserCheckable);
    item->setCheckState(0, body.visible ? Qt::Checked : Qt::Unchecked);
    item->setToolTip(0, tr("Check to show, clear to hide. Double-click to rename."));
    return item;
}

void BrowserPanel::onDocumentChanged(const document::Change& change) {
    using Kind = document::Change::Kind;
    if (change.kind == Kind::HistoryChanged || m_bodies == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_tree);
    const document::Body* body = m_document->findBody(change.body);
    QTreeWidgetItem* item = bodyItem(change.body);
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
    case Kind::SketchAdded:
    case Kind::SketchRemoved:
    case Kind::SketchRenamed:
    case Kind::SketchVisibilityChanged:
    case Kind::SketchGeometryChanged:
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
    if (m_document == nullptr || item == nullptr || item->parent() != m_bodies) {
        return;
    }
    // The menu acts on the selection, so the body that was clicked has to be in it.
    if (!item->isSelected()) {
        m_tree->clearSelection();
        item->setSelected(true);
    }
    emit contextMenuRequested(m_tree->viewport()->mapToGlobal(position));
}

} // namespace lcad::app
