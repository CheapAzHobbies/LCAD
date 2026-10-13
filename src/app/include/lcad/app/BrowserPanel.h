// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>

#include <QWidget>

#include <vector>

class QTreeWidget;
class QTreeWidgetItem;

namespace lcad::app {

/// The model browser: the document's bodies as a tree. Each body can be renamed in place,
/// shown or hidden with its check box, and selected.
///
/// The tree mirrors the document and holds nothing of its own. Edits made here are edits to
/// the document, and so can be undone.
class BrowserPanel : public QWidget {
    Q_OBJECT

public:
    explicit BrowserPanel(QWidget* parent = nullptr);
    ~BrowserPanel() override;

    /// Shows `document` under a top entry called `name`. The document has to stay alive until
    /// another one, or nullptr, is set.
    void setDocument(document::Document* document, const QString& name);
    void setDocumentName(const QString& name);

    [[nodiscard]] std::vector<document::ObjectId> selectedBodies() const;
    /// Selects these bodies' entries without emitting bodySelectionChanged().
    void setSelectedBodies(const std::vector<document::ObjectId>& bodies);

    [[nodiscard]] QTreeWidget* tree() const { return m_tree; }
    /// The entry for a body, or nullptr.
    [[nodiscard]] QTreeWidgetItem* bodyItem(document::ObjectId id) const;

signals:
    /// The user changed which bodies are selected in the tree.
    void bodySelectionChanged();
    /// The user asked to delete the selected bodies from the tree's context menu.
    void deleteRequested();

private:
    void rebuild();
    void onDocumentChanged(const document::Change& change);
    void onItemChanged(QTreeWidgetItem* item);
    void showContextMenu(const QPoint& position);
    QTreeWidgetItem* makeBodyItem(const document::Body& body);

    QTreeWidget* m_tree;
    QTreeWidgetItem* m_root = nullptr;
    QTreeWidgetItem* m_bodies = nullptr;
    document::Document* m_document = nullptr;
    document::Document::ListenerId m_listener = 0;
    QString m_name;
};

} // namespace lcad::app
