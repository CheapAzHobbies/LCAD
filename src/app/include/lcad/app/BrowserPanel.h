// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>
#include <lcad/viewport/ViewportWidget.h>

#include <QWidget>

#include <array>
#include <vector>

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace lcad::app {

/// The model browser: the document as a tree. Under "Origin" are the reference planes and axes,
/// each of which can be shown in the 3D view. Under "Bodies" and "Sketches" each entry can be
/// renamed in place, shown or hidden with its check box, and selected; double-clicking a sketch
/// asks for it to be opened. A box above the tree filters bodies and sketches by name.
///
/// The tree mirrors the document and holds nothing of its own. Edits made here are edits to
/// the document, and so can be undone. Which origin items are shown is not part of the
/// document: the window keeps it, per open document.
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
    [[nodiscard]] std::vector<document::ObjectId> selectedSketches() const;
    /// Selects these bodies' entries, and no others, without emitting bodySelectionChanged().
    void setSelectedBodies(const std::vector<document::ObjectId>& bodies);
    /// Selects exactly these bodies and sketches, without emitting bodySelectionChanged().
    void setSelection(const std::vector<document::ObjectId>& bodies,
                      const std::vector<document::ObjectId>& sketches);

    [[nodiscard]] QTreeWidget* tree() const { return m_tree; }
    /// The entry for a body, or nullptr.
    [[nodiscard]] QTreeWidgetItem* bodyItem(document::ObjectId id) const;
    /// The entry for an origin plane or axis.
    [[nodiscard]] QTreeWidgetItem* originItem(viewport::OriginItem item) const;
    /// The entry for a sketch, or nullptr.
    [[nodiscard]] QTreeWidgetItem* sketchItem(document::ObjectId id) const;
    /// Opens a body's name for editing in place.
    void editBody(document::ObjectId id);
    /// Opens a sketch's name for editing in place.
    void editSketchName(document::ObjectId id);
    /// Marks the sketch that is open for editing, in bold. An invalid id marks none.
    void setEditedSketch(document::ObjectId id);

    /// Lists only the bodies whose name contains `text`, whatever its case. Empty lists all.
    void setFilter(const QString& text);
    [[nodiscard]] QLineEdit* filterField() const { return m_filter; }

    /// Ticks the origin entries to match, without emitting originItemToggled().
    void setOriginVisibility(const std::array<bool, 6>& visible);

signals:
    /// The user changed which bodies and sketches are selected in the tree.
    void bodySelectionChanged();
    /// The user right-clicked a body or a sketch, which is now part of the selection. The
    /// window puts up the menu.
    void contextMenuRequested(const QPoint& globalPosition);
    /// The user double-clicked a sketch: open it.
    void sketchActivated(lcad::document::ObjectId sketch);
    /// The user ticked or cleared an origin plane or axis.
    void originItemToggled(lcad::viewport::OriginItem item, bool visible);

private:
    void rebuild();
    void applyFilter();
    void onDocumentChanged(const document::Change& change);
    void onItemChanged(QTreeWidgetItem* item);
    void showContextMenu(const QPoint& position);
    QTreeWidgetItem* makeBodyItem(const document::Body& body);
    void markExistence(QTreeWidgetItem* item, const document::Body& body);
    QTreeWidgetItem* makeSketchItem(const document::SketchItem& sketch);
    void markEditedSketch();

    QLineEdit* m_filter;
    QTreeWidget* m_tree;
    QTreeWidgetItem* m_root = nullptr;
    QTreeWidgetItem* m_origin = nullptr;
    QTreeWidgetItem* m_bodies = nullptr;
    QTreeWidgetItem* m_sketches = nullptr;
    document::ObjectId m_editedSketch;
    std::array<bool, 6> m_originVisible{};
    document::Document* m_document = nullptr;
    document::Document::ListenerId m_listener = 0;
    QString m_name;
};

} // namespace lcad::app
