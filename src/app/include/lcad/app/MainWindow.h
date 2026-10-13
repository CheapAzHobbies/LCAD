// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/app/PrimitivePanel.h>
#include <lcad/document/Document.h>

#include <QMainWindow>
#include <QStringList>

#include <memory>
#include <string>
#include <vector>

class QAction;
class QComboBox;
class QDockWidget;
class QLabel;
class QMenuBar;

namespace lcad::viewport {
class ViewportWidget;
}

namespace lcad::app {

class BrowserPanel;

/// LCAD's window: one document, shown in the five regions of the interface.
///
///   top bar       menus; new, open, save, undo, redo; the document's name
///   toolbar       the modeling tools, in groups
///   browser       the document's bodies as a tree (left)
///   viewport      the 3D view (center)
///   timeline      the feature history (bottom; empty until parametric features exist)
///
/// A panel on the right holds the settings of the command in progress.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    [[nodiscard]] viewport::ViewportWidget* viewport() const { return m_viewport; }
    [[nodiscard]] BrowserPanel* browser() const { return m_browser; }
    [[nodiscard]] QMenuBar* mainMenu() const { return m_menuBar; }
    [[nodiscard]] document::Document* document() const { return m_document.get(); }

    /// The file the document was opened from or last saved to. Empty for a new document.
    [[nodiscard]] QString filePath() const { return m_filePath; }
    /// The file's name without its extension, or "Untitled".
    [[nodiscard]] QString documentName() const;
    /// What the status bar says about the selection, for example "Face of Box · area 6000 mm²".
    [[nodiscard]] QString selectionSummary() const;
    /// What Help > About shows: LCAD's version and license, the versions of Qt and Open CASCADE
    /// Technology, the graphics driver in use, and where the log is.
    [[nodiscard]] QString aboutText() const;

    // Document commands without dialogs. The interactive commands in the File menu are built
    // on these; they ask about unsaved changes and for file names, and show errors.

    /// Replaces the document with a new, empty one. Unsaved changes are discarded.
    void newDocument();
    /// Opens the file in place of the current document. On failure the current document stays,
    /// and `error` receives the reason.
    bool loadFrom(const QString& path, QString* error = nullptr);
    /// Saves the document to the file and makes that the document's file.
    bool saveTo(const QString& path, QString* error = nullptr);
    /// Problems the last loadFrom() reported that did not prevent loading.
    [[nodiscard]] QStringList loadWarnings() const { return m_loadWarnings; }

    // Modeling commands

    /// Opens the panel for creating a primitive and previews it. Cancels a command in progress.
    void startPrimitive(PrimitiveKind kind);
    /// The panel of the command in progress, or nullptr.
    [[nodiscard]] PrimitivePanel* activePanel() const { return m_panel; }
    /// Adds the previewed solid to the document as one undo step and closes the panel.
    void acceptCommand();
    void cancelCommand();
    /// Deletes the bodies selected in the browser or the viewport, as one undo step.
    void deleteSelectedBodies();

    // The interactive File commands. Each returns false if the user canceled or it failed.
    bool fileNew();
    bool fileOpen();
    bool fileSave();
    bool fileSaveAs();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void createActions();
    void createTopBar();
    void createToolbar();
    void createDocks();
    void setDocument(std::unique_ptr<document::Document> document, const QString& path);
    void updateDocumentState();
    void updateSelectionState();
    [[nodiscard]] std::vector<document::ObjectId> selectedBodies() const;
    [[nodiscard]] bool confirmDiscard();
    [[nodiscard]] std::string thumbnail();
    void showAbout();
    void showViewportError(const QString& reason);

    viewport::ViewportWidget* m_viewport;
    BrowserPanel* m_browser = nullptr;
    QMenuBar* m_menuBar = nullptr;
    QDockWidget* m_toolDock = nullptr;
    PrimitivePanel* m_panel = nullptr;
    QLabel* m_documentLabel = nullptr;
    QLabel* m_selectionLabel = nullptr;
    QComboBox* m_filterBox = nullptr;

    QAction* m_newAction = nullptr;
    QAction* m_openAction = nullptr;
    QAction* m_saveAction = nullptr;
    QAction* m_saveAsAction = nullptr;
    QAction* m_quitAction = nullptr;
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
    QAction* m_deleteAction = nullptr;
    QAction* m_cancelAction = nullptr;
    QAction* m_perspectiveAction = nullptr;
    QAction* m_reverseZoomAction = nullptr;
    QList<QAction*> m_createActions;
    QList<QAction*> m_viewActions;

    std::unique_ptr<document::Document> m_document;
    document::Document::ListenerId m_listener = 0;
    QString m_filePath;
    QStringList m_loadWarnings;
    bool m_selectionComesFromBrowser = false;
};

} // namespace lcad::app
