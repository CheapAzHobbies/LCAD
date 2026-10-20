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
/// After them come the occurrences of components, each with its own bodies, sketches,
/// construction geometry, joints, and the occurrences inside it, and last the joints of the
/// document itself. An occurrence is renamed, shown, and hidden like a body; double-clicking it
/// makes it the component being worked on, which is shown in bold, and dragging it onto another
/// occurrence, or onto the document's entry, moves it there.
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

    /// The bodies selected, wherever they are listed, each once, in document order.
    [[nodiscard]] std::vector<document::ObjectId> selectedBodies() const;
    [[nodiscard]] std::vector<document::ObjectId> selectedSketches() const;
    /// The bodies and sketches selected as the 3D view knows them: each with the path to the
    /// occurrence it is listed under.
    [[nodiscard]] std::vector<viewport::SelectedItem> selectedItems() const;
    /// The occurrences selected, each by its path from the root.
    [[nodiscard]] std::vector<std::vector<document::ObjectId>> selectedOccurrences() const;
    [[nodiscard]] std::vector<document::ObjectId> selectedJoints() const;
    /// Selects these bodies' entries, and no others, without emitting bodySelectionChanged().
    void setSelectedBodies(const std::vector<document::ObjectId>& bodies);
    /// Selects exactly these bodies and sketches, without emitting bodySelectionChanged().
    void setSelection(const std::vector<document::ObjectId>& bodies,
                      const std::vector<document::ObjectId>& sketches);
    /// Selects exactly these, each under the occurrence its path leads to, without emitting
    /// bodySelectionChanged().
    void setSelection(const std::vector<viewport::SelectedItem>& items);
    /// Selects exactly these occurrences and joints, without emitting bodySelectionChanged().
    void setAssemblySelection(const std::vector<std::vector<document::ObjectId>>& occurrences,
                              const std::vector<document::ObjectId>& joints);

    [[nodiscard]] QTreeWidget* tree() const { return m_tree; }
    /// The entry for a body, or nullptr: the first, for a body of a component that appears
    /// several times.
    [[nodiscard]] QTreeWidgetItem* bodyItem(document::ObjectId id) const;
    /// The entry for a body listed under the occurrence `path` leads to, or nullptr.
    [[nodiscard]] QTreeWidgetItem* bodyItem(document::ObjectId id,
                                            const std::vector<document::ObjectId>& path) const;
    /// The entry for an origin plane or axis.
    [[nodiscard]] QTreeWidgetItem* originItem(viewport::OriginItem item) const;
    /// The entry for a sketch, or nullptr.
    [[nodiscard]] QTreeWidgetItem* sketchItem(document::ObjectId id) const;
    /// The entry of a construction plane, or nullptr.
    [[nodiscard]] QTreeWidgetItem* planeItem(document::ObjectId id) const;
    /// The entry of the occurrence a path leads to, or nullptr.
    [[nodiscard]] QTreeWidgetItem*
    occurrenceItem(const std::vector<document::ObjectId>& path) const;
    /// The entry of a joint, or nullptr.
    [[nodiscard]] QTreeWidgetItem* jointItem(document::ObjectId id) const;
    /// The construction planes selected, in document order.
    [[nodiscard]] std::vector<document::ObjectId> selectedPlanes() const;
    /// Opens a body's name for editing in place.
    void editBody(document::ObjectId id);
    /// Opens a sketch's name for editing in place.
    void editSketchName(document::ObjectId id);
    /// Opens the name of what is selected (a body, a sketch, an occurrence's component, a
    /// joint) for editing in place. Returns false if nothing that can be renamed is.
    bool editSelectedName();
    /// Marks the sketch that is open for editing, in bold. An invalid id marks none.
    void setEditedSketch(document::ObjectId id);
    /// Marks the occurrence being worked on, in bold: the document itself for an empty path.
    void setActivePath(const std::vector<document::ObjectId>& path);

    /// Lists only the bodies whose name contains `text`, whatever its case. Empty lists all.
    void setFilter(const QString& text);
    [[nodiscard]] QLineEdit* filterField() const { return m_filter; }

    /// Ticks the origin entries to match, without emitting originItemToggled().
    void setOriginVisibility(const std::array<bool, 6>& visible);

    /// What an entry stands for.
    enum class EntryKind { Other, Body, Sketch, Plane, Occurrence, Joint };
    [[nodiscard]] static EntryKind kindOf(const QTreeWidgetItem* item);
    /// The path of an occurrence's entry, or of the occurrence an entry is listed under.
    [[nodiscard]] static std::vector<document::ObjectId> pathOf(const QTreeWidgetItem* item);

signals:
    /// The user changed which bodies, sketches, occurrences, or joints are selected in the
    /// tree.
    void bodySelectionChanged();
    /// The user right-clicked an entry, which is now part of the selection. The window puts
    /// up the menu.
    void contextMenuRequested(const QPoint& globalPosition);
    /// The user double-clicked a sketch: open it.
    void sketchActivated(lcad::document::ObjectId sketch);
    /// The user double-clicked a construction plane.
    void planeActivated(lcad::document::ObjectId plane);
    /// The user double-clicked an occurrence, or the document's entry (an empty path): work on
    /// that component.
    void occurrenceActivated(const std::vector<lcad::document::ObjectId>& path);
    /// The user double-clicked a joint.
    void jointActivated(lcad::document::ObjectId joint);
    /// The user dropped an occurrence onto another, or onto the document's entry (an invalid
    /// id): move it into that component.
    void occurrenceDropped(lcad::document::ObjectId occurrence, lcad::document::ObjectId component);
    /// The user ticked or cleared an origin plane or axis.
    void originItemToggled(lcad::viewport::OriginItem item, bool visible);

private:
    class Tree;

    void rebuild();
    /// Lists the occurrences and the joints again, as they now are, keeping what was expanded
    /// and selected.
    void fillAssembly();
    void addComponentContents(QTreeWidgetItem* parent, document::ObjectId component,
                              const std::vector<document::ObjectId>& path);
    void applyFilter();
    void onDocumentChanged(const document::Change& change);
    void onItemChanged(QTreeWidgetItem* item);
    void showContextMenu(const QPoint& position);
    QTreeWidgetItem* makeBodyItem(const document::Body& body,
                                  const std::vector<document::ObjectId>& path = {});
    void markExistence(QTreeWidgetItem* item, const document::Body& body);
    QTreeWidgetItem* makeSketchItem(const document::SketchItem& sketch,
                                    const std::vector<document::ObjectId>& path = {});
    QTreeWidgetItem* makePlaneItem(const document::Feature& feature,
                                   const std::vector<document::ObjectId>& path = {});
    QTreeWidgetItem* makeJointItem(const document::Joint& joint);
    void markEditedSketch();
    void markActive();
    /// Lists the construction planes again.
    void fillPlanes();
    /// Every entry of the tree that stands for something, in order.
    [[nodiscard]] std::vector<QTreeWidgetItem*> entries() const;

    QLineEdit* m_filter;
    QTreeWidget* m_tree;
    QTreeWidgetItem* m_root = nullptr;
    QTreeWidgetItem* m_origin = nullptr;
    QTreeWidgetItem* m_bodies = nullptr;
    QTreeWidgetItem* m_sketches = nullptr;
    QTreeWidgetItem* m_planes = nullptr;
    QTreeWidgetItem* m_joints = nullptr;
    document::ObjectId m_editedSketch;
    std::vector<document::ObjectId> m_activePath;
    std::array<bool, 6> m_originVisible{};
    document::Document* m_document = nullptr;
    document::Document::ListenerId m_listener = 0;
    QString m_name;
};

} // namespace lcad::app
