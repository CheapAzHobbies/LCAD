// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/BrowserPanel.h>

#include <lcad/app/Icons.h>

#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFont>
#include <QHeaderView>
#include <QLineEdit>
#include <QPalette>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace lcad::app {

namespace {

constexpr int idRole = Qt::UserRole;
constexpr int kindRole = Qt::UserRole + 1;
constexpr int pathRole = Qt::UserRole + 2;

document::ObjectId idOf(const QTreeWidgetItem* item) {
    return document::ObjectId{item->data(0, idRole).toULongLong()};
}

QVariant toVariant(const std::vector<document::ObjectId>& path) {
    QVariantList list;
    for (const document::ObjectId id : path) {
        list.append(QVariant::fromValue<qulonglong>(id.value));
    }
    return list;
}

void mark(QTreeWidgetItem* item, BrowserPanel::EntryKind kind, document::ObjectId id,
          const std::vector<document::ObjectId>& path) {
    item->setData(0, idRole, QVariant::fromValue<qulonglong>(id.value));
    item->setData(0, kindRole, static_cast<int>(kind));
    item->setData(0, pathRole, toVariant(path));
}

// What identifies an entry across a refill of the tree.
QString keyOf(const QTreeWidgetItem* item) {
    QString key = QString::number(item->data(0, kindRole).toInt()) + QLatin1Char(':') +
                  QString::number(item->data(0, idRole).toULongLong());
    for (const QVariant& id : item->data(0, pathRole).toList()) {
        key += QLatin1Char('/') + QString::number(id.toULongLong());
    }
    if (BrowserPanel::kindOf(item) == BrowserPanel::EntryKind::Other) {
        // A group: by what it is in, and its name.
        key += QLatin1Char('|') + item->text(0);
        if (item->parent() != nullptr) {
            key += QLatin1Char('|') + keyOf(item->parent());
        }
    }
    return key;
}

} // namespace

// The tree, which takes occurrences dragged onto other occurrences and tells the panel, rather
// than moving its entries itself: the document decides, and the tree follows it.
class BrowserPanel::Tree : public QTreeWidget {
public:
    explicit Tree(BrowserPanel* panel) : QTreeWidget(panel), m_panel(panel) {}

protected:
    void dragMoveEvent(QDragMoveEvent* event) override {
        QTreeWidget::dragMoveEvent(event);
        event->setAccepted(target(event->position().toPoint()).has_value());
    }

    void dropEvent(QDropEvent* event) override {
        const auto into = target(event->position().toPoint());
        event->setDropAction(Qt::IgnoreAction);
        event->accept();
        if (!into.has_value()) {
            return;
        }
        std::vector<document::ObjectId> moving;
        for (QTreeWidgetItem* item : selectedItems()) {
            if (kindOf(item) == EntryKind::Occurrence) {
                moving.push_back(idOf(item));
            }
        }
        for (const document::ObjectId occurrence : moving) {
            emit m_panel->occurrenceDropped(occurrence, *into);
        }
    }

private:
    // The component an occurrence dropped at `position` goes into: that of the occurrence
    // there, or the root's (an invalid id) on the document's entry.
    std::optional<document::ObjectId> target(const QPoint& position) const {
        const QTreeWidgetItem* under = itemAt(position);
        if (under == nullptr || m_panel->m_document == nullptr) {
            return std::nullopt;
        }
        if (under == m_panel->m_root) {
            return document::ObjectId{};
        }
        if (kindOf(under) != EntryKind::Occurrence) {
            return std::nullopt;
        }
        const document::Occurrence* occurrence = m_panel->m_document->findOccurrence(idOf(under));
        if (occurrence == nullptr) {
            return std::nullopt;
        }
        const document::Component* component =
            m_panel->m_document->findComponent(occurrence->component);
        if (component == nullptr || !component->link.empty()) {
            return std::nullopt;
        }
        return occurrence->component;
    }

    BrowserPanel* m_panel;
};

BrowserPanel::BrowserPanel(QWidget* parent)
    : QWidget(parent), m_filter(new QLineEdit(this)), m_tree(new Tree(this)) {
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
    // Occurrences are dragged onto the components they are to go into.
    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    m_tree->setDropIndicatorShown(true);
    m_tree->setDragDropMode(QAbstractItemView::DragDrop);
    m_tree->setDefaultDropAction(Qt::MoveAction);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (item == m_root) {
            emit occurrenceActivated({});
            return;
        }
        switch (kindOf(item)) {
        case EntryKind::Body:
            m_tree->editItem(item, 0);
            break;
        case EntryKind::Sketch:
            emit sketchActivated(idOf(item));
            break;
        case EntryKind::Plane:
            emit planeActivated(idOf(item));
            break;
        case EntryKind::Occurrence:
            emit occurrenceActivated(pathOf(item));
            break;
        case EntryKind::Joint:
            emit jointActivated(idOf(item));
            break;
        case EntryKind::Other:
            break;
        }
    });

    connect(m_tree, &QTreeWidget::itemChanged, this, &BrowserPanel::onItemChanged);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &BrowserPanel::bodySelectionChanged);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &BrowserPanel::showContextMenu);
}

BrowserPanel::~BrowserPanel() {
    setDocument(nullptr, {});
}

BrowserPanel::EntryKind BrowserPanel::kindOf(const QTreeWidgetItem* item) {
    return item == nullptr ? EntryKind::Other
                           : static_cast<EntryKind>(item->data(0, kindRole).toInt());
}

std::vector<document::ObjectId> BrowserPanel::pathOf(const QTreeWidgetItem* item) {
    std::vector<document::ObjectId> path;
    if (item == nullptr) {
        return path;
    }
    for (const QVariant& id : item->data(0, pathRole).toList()) {
        path.push_back(document::ObjectId{id.toULongLong()});
    }
    return path;
}

void BrowserPanel::setDocument(document::Document* document, const QString& name) {
    if (m_document != nullptr) {
        m_document->removeListener(m_listener);
        m_listener = 0;
    }
    m_document = document;
    m_name = name;
    m_activePath.clear();
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

std::vector<QTreeWidgetItem*> BrowserPanel::entries() const {
    std::vector<QTreeWidgetItem*> found;
    std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) {
        if (kindOf(item) != EntryKind::Other) {
            found.push_back(item);
        }
        for (int i = 0; i < item->childCount(); ++i) {
            visit(item->child(i));
        }
    };
    if (m_root != nullptr) {
        visit(m_root);
    }
    return found;
}

std::vector<document::ObjectId> BrowserPanel::selectedBodies() const {
    std::vector<document::ObjectId> ids;
    if (m_document == nullptr) {
        return ids;
    }
    QSet<qulonglong> selected;
    for (QTreeWidgetItem* item : entries()) {
        if (kindOf(item) == EntryKind::Body && item->isSelected()) {
            selected.insert(idOf(item).value);
        }
    }
    // In document order, whatever order they were clicked in.
    for (const document::Body& body : m_document->bodies()) {
        if (selected.contains(body.id.value)) {
            ids.push_back(body.id);
        }
    }
    return ids;
}

std::vector<document::ObjectId> BrowserPanel::selectedSketches() const {
    std::vector<document::ObjectId> ids;
    if (m_document == nullptr) {
        return ids;
    }
    QSet<qulonglong> selected;
    for (QTreeWidgetItem* item : entries()) {
        if (kindOf(item) == EntryKind::Sketch && item->isSelected()) {
            selected.insert(idOf(item).value);
        }
    }
    for (const document::SketchItem& sketch : m_document->sketches()) {
        if (selected.contains(sketch.id.value)) {
            ids.push_back(sketch.id);
        }
    }
    return ids;
}

std::vector<viewport::SelectedItem> BrowserPanel::selectedItems() const {
    std::vector<viewport::SelectedItem> items;
    for (QTreeWidgetItem* item : entries()) {
        if (!item->isSelected()) {
            continue;
        }
        if (kindOf(item) == EntryKind::Body) {
            items.push_back({idOf(item), viewport::SelectedItem::Kind::Body, -1, pathOf(item)});
        } else if (kindOf(item) == EntryKind::Sketch) {
            const viewport::SelectedItem sketch{idOf(item), viewport::SelectedItem::Kind::Sketch,
                                                -1};
            if (std::find(items.begin(), items.end(), sketch) == items.end()) {
                items.push_back(sketch);
            }
        }
    }
    return items;
}

std::vector<std::vector<document::ObjectId>> BrowserPanel::selectedOccurrences() const {
    std::vector<std::vector<document::ObjectId>> paths;
    for (QTreeWidgetItem* item : entries()) {
        if (kindOf(item) == EntryKind::Occurrence && item->isSelected()) {
            paths.push_back(pathOf(item));
        }
    }
    return paths;
}

std::vector<document::ObjectId> BrowserPanel::selectedJoints() const {
    std::vector<document::ObjectId> joints;
    for (QTreeWidgetItem* item : entries()) {
        if (kindOf(item) == EntryKind::Joint && item->isSelected() &&
            std::find(joints.begin(), joints.end(), idOf(item)) == joints.end()) {
            joints.push_back(idOf(item));
        }
    }
    return joints;
}

void BrowserPanel::setSelectedBodies(const std::vector<document::ObjectId>& bodies) {
    setSelection(bodies, {});
}

void BrowserPanel::setSelection(const std::vector<document::ObjectId>& bodies,
                                const std::vector<document::ObjectId>& sketches) {
    const QSignalBlocker blocker(m_tree);
    m_tree->clearSelection();
    for (QTreeWidgetItem* item : entries()) {
        const document::ObjectId id = idOf(item);
        if ((kindOf(item) == EntryKind::Body &&
             std::find(bodies.begin(), bodies.end(), id) != bodies.end()) ||
            (kindOf(item) == EntryKind::Sketch &&
             std::find(sketches.begin(), sketches.end(), id) != sketches.end())) {
            item->setSelected(true);
        }
    }
}

void BrowserPanel::setSelection(const std::vector<viewport::SelectedItem>& items) {
    const QSignalBlocker blocker(m_tree);
    m_tree->clearSelection();
    for (QTreeWidgetItem* item : entries()) {
        const document::ObjectId id = idOf(item);
        for (const viewport::SelectedItem& wanted : items) {
            const bool body = kindOf(item) == EntryKind::Body &&
                              wanted.kind == viewport::SelectedItem::Kind::Body &&
                              wanted.body == id && wanted.path == pathOf(item);
            const bool sketch = kindOf(item) == EntryKind::Sketch &&
                                wanted.kind == viewport::SelectedItem::Kind::Sketch &&
                                wanted.body == id;
            if (body || sketch) {
                item->setSelected(true);
            }
        }
    }
}

void BrowserPanel::setAssemblySelection(
    const std::vector<std::vector<document::ObjectId>>& occurrences,
    const std::vector<document::ObjectId>& joints) {
    const QSignalBlocker blocker(m_tree);
    m_tree->clearSelection();
    for (QTreeWidgetItem* item : entries()) {
        if ((kindOf(item) == EntryKind::Occurrence &&
             std::find(occurrences.begin(), occurrences.end(), pathOf(item)) !=
                 occurrences.end()) ||
            (kindOf(item) == EntryKind::Joint &&
             std::find(joints.begin(), joints.end(), idOf(item)) != joints.end())) {
            item->setSelected(true);
        }
    }
}

QTreeWidgetItem* BrowserPanel::sketchItem(document::ObjectId id) const {
    for (QTreeWidgetItem* item : entries()) {
        if (kindOf(item) == EntryKind::Sketch && idOf(item) == id) {
            return item;
        }
    }
    return nullptr;
}

QTreeWidgetItem* BrowserPanel::planeItem(document::ObjectId id) const {
    for (QTreeWidgetItem* item : entries()) {
        if (kindOf(item) == EntryKind::Plane && idOf(item) == id) {
            return item;
        }
    }
    return nullptr;
}

QTreeWidgetItem* BrowserPanel::occurrenceItem(const std::vector<document::ObjectId>& path) const {
    for (QTreeWidgetItem* item : entries()) {
        if (kindOf(item) == EntryKind::Occurrence && pathOf(item) == path) {
            return item;
        }
    }
    return nullptr;
}

QTreeWidgetItem* BrowserPanel::jointItem(document::ObjectId id) const {
    for (QTreeWidgetItem* item : entries()) {
        if (kindOf(item) == EntryKind::Joint && idOf(item) == id) {
            return item;
        }
    }
    return nullptr;
}

std::vector<document::ObjectId> BrowserPanel::selectedPlanes() const {
    std::vector<document::ObjectId> ids;
    for (QTreeWidgetItem* item : entries()) {
        if (kindOf(item) == EntryKind::Plane && item->isSelected()) {
            ids.push_back(idOf(item));
        }
    }
    return ids;
}

QTreeWidgetItem* BrowserPanel::makePlaneItem(const document::Feature& feature,
                                             const std::vector<document::ObjectId>& path) {
    auto* item = new QTreeWidgetItem({QString::fromStdString(feature.name)});
    mark(item, EntryKind::Plane, feature.id, path);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                   Qt::ItemIsUserCheckable);
    item->setCheckState(0, feature.visible ? Qt::Checked : Qt::Unchecked);
    item->setToolTip(0, feature.kind == document::FeatureKind::Plane
                            ? tr("A construction plane. Check to show, clear to hide. "
                                 "Double-click to change where it is.")
                            : tr("A construction axis. Check to show, clear to hide. "
                                 "Double-click to change where it is."));
    return item;
}

void BrowserPanel::fillPlanes() {
    if (m_planes == nullptr || m_document == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_tree);
    const std::vector<document::ObjectId> selected = selectedPlanes();
    while (m_planes->childCount() > 0) {
        delete m_planes->takeChild(0);
    }
    for (const document::Feature& feature : m_document->features()) {
        if ((feature.kind != document::FeatureKind::Plane &&
             feature.kind != document::FeatureKind::Axis) ||
            feature.component.isValid()) {
            continue;
        }
        QTreeWidgetItem* item = makePlaneItem(feature);
        m_planes->addChild(item);
        item->setSelected(std::find(selected.begin(), selected.end(), feature.id) !=
                          selected.end());
    }
    m_planes->setHidden(m_planes->childCount() == 0);
}

QTreeWidgetItem* BrowserPanel::makeJointItem(const document::Joint& joint) {
    auto* item = new QTreeWidgetItem({QString::fromStdString(joint.name)});
    mark(item, EntryKind::Joint, joint.id, {});
    item->setIcon(0, icon(QStringLiteral("joint")));
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                   Qt::ItemIsUserCheckable);
    item->setCheckState(0, joint.suppressed ? Qt::Unchecked : Qt::Checked);
    const std::string error = m_document->jointError(joint.id);
    const auto sideName = [this](const document::JointSide& side) {
        return side.path.empty()
                   ? tr("the assembly")
                   : QString::fromStdString(m_document->occurrenceName(side.path.front()));
    };
    QString tip = tr("A %1 joint holding %2 to %3. Clear to suppress it; double-click to edit it.")
                      .arg(QString::fromLatin1(document::toString(joint.kind)).toLower(),
                           sideName(joint.moving), sideName(joint.fixed));
    QFont font = item->font(0);
    font.setItalic(joint.suppressed || !error.empty());
    item->setFont(0, font);
    if (!error.empty()) {
        tip += QLatin1Char('\n') + tr("It does not hold: %1").arg(QString::fromStdString(error));
        item->setData(0, Qt::ForegroundRole, m_tree->palette().color(QPalette::PlaceholderText));
    }
    item->setToolTip(0, tip);
    return item;
}

void BrowserPanel::addComponentContents(QTreeWidgetItem* parent, document::ObjectId component,
                                        const std::vector<document::ObjectId>& path) {
    const auto group = [parent](const QString& name) {
        auto* item = new QTreeWidgetItem(parent, {name});
        item->setFlags(Qt::ItemIsEnabled);
        return item;
    };
    QTreeWidgetItem* bodies = nullptr;
    for (const document::Body& body : m_document->bodies()) {
        if (body.component != component) {
            continue;
        }
        if (bodies == nullptr) {
            bodies = group(tr("Bodies"));
        }
        bodies->addChild(makeBodyItem(body, path));
    }
    QTreeWidgetItem* sketches = nullptr;
    for (const document::SketchItem& sketch : m_document->sketches()) {
        if (sketch.component != component) {
            continue;
        }
        if (sketches == nullptr) {
            sketches = group(tr("Sketches"));
        }
        sketches->addChild(makeSketchItem(sketch, path));
    }
    QTreeWidgetItem* construction = nullptr;
    for (const document::Feature& feature : m_document->features()) {
        if ((feature.kind != document::FeatureKind::Plane &&
             feature.kind != document::FeatureKind::Axis) ||
            feature.component != component) {
            continue;
        }
        if (construction == nullptr) {
            construction = group(tr("Construction"));
        }
        construction->addChild(makePlaneItem(feature, path));
    }
    QTreeWidgetItem* joints = nullptr;
    for (const document::Joint& joint : m_document->joints()) {
        if (joint.component != component) {
            continue;
        }
        if (joints == nullptr) {
            joints = group(tr("Joints"));
        }
        joints->addChild(makeJointItem(joint));
    }
    for (const document::ObjectId id : m_document->occurrencesIn(component)) {
        const document::Occurrence* occurrence = m_document->findOccurrence(id);
        const document::Component* inner = m_document->findComponent(occurrence->component);
        std::vector<document::ObjectId> innerPath = path;
        innerPath.push_back(id);
        QString text = QString::fromStdString(m_document->occurrenceName(id));
        if (occurrence->grounded) {
            text += tr(" (grounded)");
        }
        auto* item = new QTreeWidgetItem(parent, {text});
        mark(item, EntryKind::Occurrence, id, innerPath);
        const bool linked = inner != nullptr && !inner->link.empty();
        item->setIcon(
            0, icon(linked ? QStringLiteral("component-linked") : QStringLiteral("component")));
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                              Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled;
        if (!linked) {
            flags |= Qt::ItemIsDropEnabled;
        }
        item->setFlags(flags);
        item->setCheckState(0, occurrence->visible ? Qt::Checked : Qt::Unchecked);
        QString tip = linked ? tr("A linked part: %1. It is changed in that file.")
                                   .arg(QString::fromStdString(inner->link))
                             : tr("A component. Double-click to work on it; check to show, "
                                  "clear to hide; drag it onto another to move it there.");
        if (occurrence->grounded) {
            tip += QLatin1Char('\n') + tr("Grounded: joints move other parts, not this one.");
        }
        if (occurrence->suppressed) {
            tip += QLatin1Char('\n') + tr("Suppressed: left out of the assembly.");
            QFont font = item->font(0);
            font.setItalic(true);
            item->setFont(0, font);
            item->setData(0, Qt::ForegroundRole,
                          m_tree->palette().color(QPalette::PlaceholderText));
        }
        item->setToolTip(0, tip);
        addComponentContents(item, occurrence->component, innerPath);
    }
}

void BrowserPanel::fillAssembly() {
    if (m_root == nullptr || m_document == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_tree);
    // What was expanded and selected, to be so again.
    QSet<QString> expanded;
    QSet<QString> selected;
    std::function<void(QTreeWidgetItem*)> remember = [&](QTreeWidgetItem* item) {
        if (item->isExpanded()) {
            expanded.insert(keyOf(item));
        }
        if (item->isSelected()) {
            selected.insert(keyOf(item));
        }
        for (int i = 0; i < item->childCount(); ++i) {
            remember(item->child(i));
        }
    };
    for (int i = 0; i < m_root->childCount(); ++i) {
        remember(m_root->child(i));
    }
    // Everything after the construction group but the joints is an occurrence of the root.
    for (int i = m_root->childCount() - 1; i >= 0; --i) {
        QTreeWidgetItem* child = m_root->child(i);
        if (kindOf(child) == EntryKind::Occurrence) {
            delete m_root->takeChild(i);
        }
    }
    while (m_joints->childCount() > 0) {
        delete m_joints->takeChild(0);
    }
    // The root's own occurrences go before its joints; its bodies and sketches are listed
    // apart, in the groups above.
    m_root->removeChild(m_joints);
    {
        // Built in a holder, then moved under the root.
        QTreeWidgetItem holder;
        std::vector<QTreeWidgetItem*> made;
        addComponentContents(&holder, {}, {});
        while (holder.childCount() > 0) {
            QTreeWidgetItem* child = holder.takeChild(0);
            if (kindOf(child) == EntryKind::Occurrence) {
                made.push_back(child);
            } else {
                // The root's bodies, sketches, and construction are in their own groups.
                delete child;
            }
        }
        for (QTreeWidgetItem* item : made) {
            m_root->addChild(item);
        }
    }
    m_root->addChild(m_joints);
    for (const document::Joint& joint : m_document->joints()) {
        if (!joint.component.isValid()) {
            m_joints->addChild(makeJointItem(joint));
        }
    }
    m_joints->setHidden(m_joints->childCount() == 0);
    std::function<void(QTreeWidgetItem*)> restore = [&](QTreeWidgetItem* item) {
        const QString key = keyOf(item);
        if (expanded.contains(key)) {
            item->setExpanded(true);
        }
        if (selected.contains(key)) {
            item->setSelected(true);
        }
        for (int i = 0; i < item->childCount(); ++i) {
            restore(item->child(i));
        }
    };
    for (int i = 0; i < m_root->childCount(); ++i) {
        QTreeWidgetItem* child = m_root->child(i);
        if (kindOf(child) == EntryKind::Occurrence || child == m_joints) {
            restore(child);
        }
    }
    if (!expanded.contains(keyOf(m_joints))) {
        m_joints->setExpanded(true);
    }
    markEditedSketch();
    markActive();
}

void BrowserPanel::editSketchName(document::ObjectId id) {
    if (QTreeWidgetItem* item = sketchItem(id)) {
        m_tree->editItem(item, 0);
    }
}

bool BrowserPanel::editSelectedName() {
    for (QTreeWidgetItem* item : entries()) {
        if (item->isSelected() && (item->flags() & Qt::ItemIsEditable)) {
            m_tree->editItem(item, 0);
            return true;
        }
    }
    return false;
}

void BrowserPanel::setEditedSketch(document::ObjectId id) {
    m_editedSketch = id;
    markEditedSketch();
}

void BrowserPanel::markEditedSketch() {
    const QSignalBlocker blocker(m_tree);
    for (QTreeWidgetItem* item : entries()) {
        if (kindOf(item) != EntryKind::Sketch) {
            continue;
        }
        QFont font = item->font(0);
        font.setBold(idOf(item) == m_editedSketch);
        item->setFont(0, font);
    }
}

void BrowserPanel::setActivePath(const std::vector<document::ObjectId>& path) {
    m_activePath = path;
    markActive();
}

void BrowserPanel::markActive() {
    const QSignalBlocker blocker(m_tree);
    for (QTreeWidgetItem* item : entries()) {
        if (kindOf(item) != EntryKind::Occurrence) {
            continue;
        }
        QFont font = item->font(0);
        font.setBold(!m_activePath.empty() && pathOf(item) == m_activePath);
        item->setFont(0, font);
    }
    if (m_root != nullptr) {
        m_root->setToolTip(0, m_activePath.empty()
                                  ? tr("The document. New bodies and sketches go here.")
                                  : tr("The document. Double-click to work on it again, rather "
                                       "than on a component of it."));
    }
}

QTreeWidgetItem* BrowserPanel::bodyItem(document::ObjectId id) const {
    for (QTreeWidgetItem* item : entries()) {
        if (kindOf(item) == EntryKind::Body && idOf(item) == id) {
            return item;
        }
    }
    return nullptr;
}

QTreeWidgetItem* BrowserPanel::bodyItem(document::ObjectId id,
                                        const std::vector<document::ObjectId>& path) const {
    for (QTreeWidgetItem* item : entries()) {
        if (kindOf(item) == EntryKind::Body && idOf(item) == id && pathOf(item) == path) {
            return item;
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
    for (QTreeWidgetItem* item : entries()) {
        if (kindOf(item) == EntryKind::Body || kindOf(item) == EntryKind::Sketch) {
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
    m_planes = nullptr;
    m_joints = nullptr;
    if (m_document == nullptr) {
        return;
    }
    m_root = new QTreeWidgetItem(m_tree, {m_name});
    QFont bold = m_root->font(0);
    bold.setBold(true);
    m_root->setFont(0, bold);
    // Occurrences can be dropped onto it, to move them into the document itself.
    m_root->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);

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
        if (!body.component.isValid()) {
            m_bodies->addChild(makeBodyItem(body));
        }
    }
    m_sketches = new QTreeWidgetItem(m_root, {tr("Sketches")});
    m_sketches->setFlags(Qt::ItemIsEnabled);
    for (const document::SketchItem& sketch : m_document->sketches()) {
        if (!sketch.component.isValid()) {
            m_sketches->addChild(makeSketchItem(sketch));
        }
    }
    m_planes = new QTreeWidgetItem(m_root, {tr("Construction")});
    m_planes->setFlags(Qt::ItemIsEnabled);
    fillPlanes();
    m_joints = new QTreeWidgetItem(m_root, {tr("Joints")});
    m_joints->setFlags(Qt::ItemIsEnabled);
    m_root->setExpanded(true);
    m_bodies->setExpanded(true);
    m_sketches->setExpanded(true);
    m_planes->setExpanded(true);
    fillAssembly();
    markEditedSketch();
    markActive();
    applyFilter();
}

QTreeWidgetItem* BrowserPanel::makeSketchItem(const document::SketchItem& sketch,
                                              const std::vector<document::ObjectId>& path) {
    auto* item = new QTreeWidgetItem({QString::fromStdString(sketch.name)});
    mark(item, EntryKind::Sketch, sketch.id, path);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                   Qt::ItemIsUserCheckable);
    item->setCheckState(0, sketch.visible ? Qt::Checked : Qt::Unchecked);
    item->setToolTip(0, tr("Check to show, clear to hide. Double-click to edit the sketch."));
    return item;
}

QTreeWidgetItem* BrowserPanel::makeBodyItem(const document::Body& body,
                                            const std::vector<document::ObjectId>& path) {
    auto* item = new QTreeWidgetItem({QString::fromStdString(body.name)});
    mark(item, EntryKind::Body, body.id, path);
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
    // A linked body is named in its own document.
    if (body.linkKey.empty()) {
        flags |= Qt::ItemIsEditable;
    }
    item->setFlags(flags);
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
    item->setToolTip(0, !exists ? tr("This body does not exist at this point in the timeline: "
                                     "the feature that makes it is suppressed, fails, or comes "
                                     "after the history marker.")
                        : body.linkKey.empty()
                            ? tr("Check to show, clear to hide. Double-click to rename.")
                            : tr("A body of a linked part. Check to show, clear to hide."));
}

void BrowserPanel::onDocumentChanged(const document::Change& change) {
    using Kind = document::Change::Kind;
    if (change.kind == Kind::HistoryChanged || m_bodies == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_tree);
    const document::Body* body = m_document->findBody(change.body);
    const document::SketchItem* sketch = m_document->findSketch(change.body);
    QTreeWidgetItem* sketchEntry = sketchItem(change.body);
    bool selectionAffected = false;
    // Every entry of the body or sketch concerned: one for each appearance of its component.
    const auto entriesOf = [this, &change](EntryKind kind) {
        std::vector<QTreeWidgetItem*> found;
        for (QTreeWidgetItem* item : entries()) {
            if (kindOf(item) == kind && idOf(item) == change.body) {
                found.push_back(item);
            }
        }
        return found;
    };
    switch (change.kind) {
    case Kind::BodyAdded: {
        if (body == nullptr) {
            break;
        }
        if (body->component.isValid()) {
            fillAssembly();
            break;
        }
        // At the position the body has among the root's, which undo may make the middle.
        int index = 0;
        for (const document::Body& other : m_document->bodies()) {
            if (other.id == body->id) {
                break;
            }
            index += other.component.isValid() ? 0 : 1;
        }
        m_bodies->insertChild(index, makeBodyItem(*body));
        m_bodies->setExpanded(true);
        break;
    }
    case Kind::BodyRemoved:
        for (QTreeWidgetItem* item : entriesOf(EntryKind::Body)) {
            selectionAffected = selectionAffected || item->isSelected();
            delete item;
        }
        break;
    case Kind::BodyRenamed:
        if (body != nullptr) {
            for (QTreeWidgetItem* item : entriesOf(EntryKind::Body)) {
                item->setText(0, QString::fromStdString(body->name));
            }
        }
        break;
    case Kind::BodyVisibilityChanged:
        if (body != nullptr) {
            for (QTreeWidgetItem* item : entriesOf(EntryKind::Body)) {
                item->setCheckState(0, body->visible ? Qt::Checked : Qt::Unchecked);
            }
        }
        break;
    case Kind::SketchAdded: {
        if (sketch == nullptr || m_sketches == nullptr) {
            break;
        }
        if (sketch->component.isValid()) {
            fillAssembly();
            break;
        }
        int index = 0;
        for (const document::SketchItem& other : m_document->sketches()) {
            if (other.id == sketch->id) {
                break;
            }
            index += other.component.isValid() ? 0 : 1;
        }
        m_sketches->insertChild(index, makeSketchItem(*sketch));
        m_sketches->setExpanded(true);
        markEditedSketch();
        break;
    }
    case Kind::SketchRemoved:
        for (QTreeWidgetItem* item : entriesOf(EntryKind::Sketch)) {
            selectionAffected = selectionAffected || item->isSelected();
            delete item;
        }
        break;
    case Kind::SketchRenamed:
        if (sketch != nullptr) {
            for (QTreeWidgetItem* item : entriesOf(EntryKind::Sketch)) {
                item->setText(0, QString::fromStdString(sketch->name));
            }
        }
        break;
    case Kind::SketchVisibilityChanged:
        if (sketch != nullptr) {
            for (QTreeWidgetItem* item : entriesOf(EntryKind::Sketch)) {
                item->setCheckState(0, sketch->visible ? Qt::Checked : Qt::Unchecked);
            }
        }
        break;
    case Kind::BodyGeometryChanged:
        if (body != nullptr) {
            for (QTreeWidgetItem* item : entriesOf(EntryKind::Body)) {
                markExistence(item, *body);
            }
        }
        break;
    case Kind::TimelineChanged:
        fillPlanes();
        if (!m_document->components().empty()) {
            // Construction geometry of components is listed with them.
            fillAssembly();
        }
        break;
    case Kind::AssemblyChanged:
        fillAssembly();
        break;
    case Kind::SketchGeometryChanged:
        // A sketch moved into a component is listed there now.
        if (sketch != nullptr && sketchEntry != nullptr &&
            (sketch->component.isValid() != (sketchEntry->parent() != m_sketches))) {
            if (!sketch->component.isValid()) {
                delete sketchEntry;
                int index = 0;
                for (const document::SketchItem& other : m_document->sketches()) {
                    if (other.id == sketch->id) {
                        break;
                    }
                    index += other.component.isValid() ? 0 : 1;
                }
                m_sketches->insertChild(index, makeSketchItem(*sketch));
            } else {
                delete sketchEntry;
            }
            fillAssembly();
        }
        break;
    case Kind::PlacementsChanged:
    case Kind::MaterialsChanged:
    case Kind::ParametersChanged:
    case Kind::HistoryChanged:
        break;
    }
    // A body moved into or out of a component: its entry moves with it.
    if (change.kind == Kind::AssemblyChanged) {
        for (const document::Body& each : m_document->bodies()) {
            QTreeWidgetItem* rootEntry = nullptr;
            for (int i = 0; i < m_bodies->childCount(); ++i) {
                if (idOf(m_bodies->child(i)) == each.id) {
                    rootEntry = m_bodies->child(i);
                }
            }
            if (each.component.isValid() && rootEntry != nullptr) {
                selectionAffected = selectionAffected || rootEntry->isSelected();
                delete rootEntry;
            } else if (!each.component.isValid() && rootEntry == nullptr) {
                int index = 0;
                for (const document::Body& other : m_document->bodies()) {
                    if (other.id == each.id) {
                        break;
                    }
                    index += other.component.isValid() ? 0 : 1;
                }
                m_bodies->insertChild(index, makeBodyItem(each));
            }
        }
        for (const document::SketchItem& each : m_document->sketches()) {
            QTreeWidgetItem* rootEntry = nullptr;
            for (int i = 0; i < m_sketches->childCount(); ++i) {
                if (idOf(m_sketches->child(i)) == each.id) {
                    rootEntry = m_sketches->child(i);
                }
            }
            if (each.component.isValid() && rootEntry != nullptr) {
                delete rootEntry;
            } else if (!each.component.isValid() && rootEntry == nullptr) {
                int index = 0;
                for (const document::SketchItem& other : m_document->sketches()) {
                    if (other.id == each.id) {
                        break;
                    }
                    index += other.component.isValid() ? 0 : 1;
                }
                m_sketches->insertChild(index, makeSketchItem(each));
            }
        }
        fillPlanes();
    }
    applyFilter();
    if (selectionAffected) {
        // Signals are blocked here, and a selected entry has just gone.
        QMetaObject::invokeMethod(this, &BrowserPanel::bodySelectionChanged, Qt::QueuedConnection);
    }
}

void BrowserPanel::onItemChanged(QTreeWidgetItem* item) {
    if (m_document == nullptr) {
        return;
    }
    const document::ObjectId id = idOf(item);
    const QString text = item->text(0).trimmed();
    const bool checked = item->checkState(0) == Qt::Checked;
    // Later, for what the tree is rebuilt for: the document's answer refills it, which is no
    // thing to do while the tree is still delivering this change.
    const auto later = [this](std::function<void()> edit) {
        QMetaObject::invokeMethod(
            this,
            [this, edit = std::move(edit)] {
                try {
                    edit();
                } catch (const std::invalid_argument&) {
                }
                fillPlanes();
                fillAssembly();
            },
            Qt::QueuedConnection);
    };
    if (item->parent() == m_origin && m_origin != nullptr) {
        const int index = m_origin->indexOfChild(item);
        m_originVisible[static_cast<std::size_t>(index)] = checked;
        emit originItemToggled(static_cast<viewport::OriginItem>(index), checked);
        return;
    }
    switch (kindOf(item)) {
    case EntryKind::Plane: {
        const std::string name = text.toStdString();
        later([this, id, name, checked] {
            const document::Feature* current = m_document->findFeature(id);
            if (current == nullptr) {
                return;
            }
            if (!name.empty() && name != current->name) {
                m_document->renameFeature(id, name);
            } else if (checked != current->visible) {
                document::Feature shown = *current;
                shown.visible = checked;
                m_document->changeFeature(id, shown);
            }
        });
        return;
    }
    case EntryKind::Occurrence: {
        // The entry shows "Name:2 (grounded)"; what is typed is the component's name.
        QString name = text;
        name.remove(QRegularExpression(QStringLiteral("\\s*\\(grounded\\)$")));
        name.remove(QRegularExpression(QStringLiteral(":\\d+$")));
        const std::string wanted = name.trimmed().toStdString();
        later([this, id, wanted, checked] {
            const document::Occurrence* occurrence = m_document->findOccurrence(id);
            if (occurrence == nullptr) {
                return;
            }
            const document::Component* component = m_document->findComponent(occurrence->component);
            if (component != nullptr && !wanted.empty() && wanted != component->name) {
                m_document->renameComponent(component->id, wanted);
            } else if (checked != occurrence->visible) {
                m_document->setOccurrenceVisible(id, checked);
            }
        });
        return;
    }
    case EntryKind::Joint: {
        const std::string name = text.toStdString();
        later([this, id, name, checked] {
            const document::Joint* joint = m_document->findJoint(id);
            if (joint == nullptr) {
                return;
            }
            document::Joint changed = *joint;
            if (!name.empty()) {
                changed.name = name;
            }
            changed.suppressed = !checked;
            m_document->changeJoint(id, changed);
        });
        return;
    }
    case EntryKind::Sketch: {
        const document::SketchItem* sketch = m_document->findSketch(id);
        if (sketch == nullptr) {
            return;
        }
        const std::string name = text.toStdString();
        try {
            if (name != sketch->name) {
                m_document->renameSketch(id, name);
            } else if (checked != sketch->visible) {
                m_document->setSketchVisible(id, checked);
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
    case EntryKind::Body: {
        const document::Body* body = m_document->findBody(id);
        if (body == nullptr) {
            return;
        }
        const std::string name = text.toStdString();
        try {
            if (name != body->name) {
                m_document->renameBody(id, name);
            } else if (checked != body->visible) {
                m_document->setBodyVisible(id, checked);
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
        return;
    }
    case EntryKind::Other:
        return;
    }
}

void BrowserPanel::showContextMenu(const QPoint& position) {
    QTreeWidgetItem* item = m_tree->itemAt(position);
    if (m_document == nullptr || item == nullptr ||
        (kindOf(item) == EntryKind::Other && item != m_root)) {
        return;
    }
    // The menu acts on the selection, so the entry that was clicked has to be in it.
    if (!item->isSelected() && item != m_root) {
        m_tree->clearSelection();
        item->setSelected(true);
    }
    emit contextMenuRequested(m_tree->viewport()->mapToGlobal(position));
}

} // namespace lcad::app
