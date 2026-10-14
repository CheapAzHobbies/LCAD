// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/app/PrimitivePanel.h>
#include <lcad/document/Document.h>
#include <lcad/kernel/Plane.h>
#include <lcad/viewport/ViewportWidget.h>

#include <QByteArray>
#include <QList>
#include <QMainWindow>
#include <QStringList>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QDockWidget;
class QLabel;
class QMenu;
class QMenuBar;
class QStackedWidget;
class QTabBar;
class QToolBar;
class QToolButton;

namespace lcad::app {

class BrowserPanel;
class CommandRegistry;
class FeaturePanel;
class TimelinePanel;
class CommandSearch;
class PropertiesPanel;
class SketchPanel;
class SketchPlanePanel;

/// LCAD's window. It holds the open documents, one per tab, and shows the current one in the
/// regions of the interface:
///
///   top bar       menus; new, open, save, undo, redo; the document tabs; command search
///   toolbar       the modeling tools, in groups; while a sketch is open, the sketch tools
///   browser       the document as a tree (left)
///   viewport      the 3D view (center)
///   properties    the command in progress, or the sketch being edited, or else what is
///                 selected (right)
///   timeline      the sketches and features in the order they are applied (bottom)
///   status bar    what is selected and its measurement, the active command, the units
///
/// Every command is defined once, in a CommandRegistry, and reached from the toolbar, the
/// menus, context menus, shortcuts, and the command search through that one definition.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    [[nodiscard]] viewport::ViewportWidget* viewport() const { return m_viewport; }
    [[nodiscard]] BrowserPanel* browser() const { return m_browser; }
    [[nodiscard]] PropertiesPanel* properties() const { return m_properties; }
    [[nodiscard]] TimelinePanel* timeline() const { return m_timeline; }
    [[nodiscard]] QMenuBar* mainMenu() const { return m_menuBar; }
    [[nodiscard]] CommandRegistry* commands() const { return m_commands; }
    [[nodiscard]] CommandSearch* commandSearch() const { return m_search; }

    // The open documents. There is always at least one.

    [[nodiscard]] int documentCount() const { return static_cast<int>(m_sessions.size()); }
    [[nodiscard]] int currentIndex() const { return m_current; }
    /// Shows another open document. Each keeps its own camera, undo history, and origin guides.
    void setCurrentIndex(int index);
    /// The current document.
    [[nodiscard]] document::Document* document() const;
    /// The file the current document was opened from or last saved to. Empty if it has none.
    [[nodiscard]] QString filePath() const;
    /// The current document's file name without its extension, or "Untitled".
    [[nodiscard]] QString documentName() const;
    [[nodiscard]] QString documentName(int index) const;

    /// What the status bar says about the selection, for example "Face of Box · area 6000 mm²".
    [[nodiscard]] QString selectionSummary() const;
    /// What Help > About shows: LCAD's version and license, the versions of Qt and Open CASCADE
    /// Technology, the graphics driver in use, and where the log is.
    [[nodiscard]] QString aboutText() const;

    // Document commands without dialogs. The interactive commands in the File menu are built
    // on these; they ask about unsaved changes and for file names, and show errors.

    /// Opens a new, empty document in a tab of its own and shows it.
    void newDocument();
    /// Opens the file in a tab and shows it. A file that is already open is shown, not opened
    /// twice. An untouched empty document gives its tab to the file. On failure nothing changes
    /// and `error` receives the reason.
    bool loadFrom(const QString& path, QString* error = nullptr);
    /// Saves the current document to the file and makes that its file.
    bool saveTo(const QString& path, QString* error = nullptr);
    /// Closes a document without asking. Closing the last one leaves a new, empty document.
    void closeDocument(int index);
    /// Problems the last loadFrom() reported that did not prevent loading.
    [[nodiscard]] QStringList loadWarnings() const { return m_loadWarnings; }
    /// The files opened or saved most recently, newest first.
    [[nodiscard]] QStringList recentFiles() const;

    // Modeling commands

    /// Opens the panel for creating a primitive and previews it. Cancels a command in progress.
    void startPrimitive(PrimitiveKind kind);
    /// The panel of the command in progress, or nullptr.
    [[nodiscard]] PrimitivePanel* activePanel() const { return m_panel; }
    /// Adds the previewed solid to the document as one undo step and closes the panel.
    void acceptCommand();
    void cancelCommand();
    /// Deletes the bodies and sketches selected in the browser or the viewport, as one undo
    /// step.
    void deleteSelectedBodies();

    // Features

    /// Starts adding a feature: opens its panel, and lets what it works on be picked in the 3D
    /// view. While a sketch is open, it is finished first, and a pull or a revolve starts from
    /// it. Cancels a command in progress.
    void startFeature(document::FeatureKind kind);
    /// Opens a feature for editing: the model goes back to just before it, its panel opens with
    /// its values, and what it works on is shown picked. Returns false if there is no such
    /// feature.
    bool editFeature(document::ObjectId feature);
    /// The panel of the feature being added or edited, or nullptr.
    [[nodiscard]] FeaturePanel* featurePanel() const { return m_featurePanel; }
    /// The feature being edited, or an invalid id when a new one is being added or none.
    [[nodiscard]] document::ObjectId editedFeature() const { return m_editedFeature; }
    /// Deletes an entry of the timeline: a feature, a sketch, or a body put in as it is.
    void deleteTimelineItem(document::ObjectId item);

    // Sketching

    /// Starts a new sketch: on the flat face that is selected, or else by asking for a plane in
    /// the right-hand panel. Cancels a command in progress.
    void startSketch();
    /// The panel that asks where a new sketch goes, or nullptr.
    [[nodiscard]] SketchPlanePanel* planePanel() const { return m_planePanel; }
    /// Adds a sketch on a plane, as one undo step, and opens it for editing.
    document::ObjectId createSketch(const kernel::Plane& plane);
    /// Opens a sketch for editing: the camera turns to face it and the toolbar shows the sketch
    /// tools. Returns false if there is no such sketch.
    bool editSketch(document::ObjectId sketch);
    /// Closes the sketch being edited and brings the modeling tools back.
    void finishSketch();
    [[nodiscard]] bool isSketching() const;
    /// The panel shown on the right while a sketch is open.
    [[nodiscard]] SketchPanel* sketchPanel() const { return m_sketchPanel; }
    /// What the status bar says about the command or tool in use.
    [[nodiscard]] QString commandText() const;

    // The interactive File commands. Each returns false if the user canceled or it failed.
    bool fileNew();
    bool fileOpen();
    bool fileSave();
    bool fileSaveAs();
    /// Closes a document, asking first if it has unsaved changes. -1 means the current one.
    bool fileClose(int index = -1);

    // The window

    /// Puts the panels back where they are in a new installation.
    void resetLayout();
    /// Gives the whole window to the 3D view, or brings the panels back as they were.
    void setViewportFocus(bool focused);
    [[nodiscard]] bool isViewportFocused() const { return m_focusState.has_value(); }
    /// The menu a right-click at this spot of the 3D view opens. The caller owns it.
    [[nodiscard]] QMenu* createViewportMenu(const QPoint& position);

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    struct Session {
        std::unique_ptr<document::Document> document;
        document::Document::ListenerId listener = 0;
        QString filePath;
        /// The camera as it was when another document was shown instead.
        std::optional<viewport::CameraState> camera;
        std::array<bool, 6> origin{};
    };

    void defineCommands();
    void defineSketchCommands();
    void createTopBar();
    void createToolbar();
    void createSketchToolbar();
    /// Shows the toolbar that goes with what is being done: modeling, or sketching.
    void updateMode();
    /// Drops the labels of the primitive buttons when the window is too narrow for them.
    void fitToolbar();
    void onSketchEditingChanged();
    void createDocks();
    void restoreSettings();
    void applyTheme();

    int addSession(std::unique_ptr<document::Document> document, const QString& path);
    void activate(int index);
    void storeViewOf(int index);
    [[nodiscard]] Session& current() const {
        return *m_sessions[static_cast<std::size_t>(m_current)];
    }
    void updateTab(int index);
    void updateDocumentState();
    void updateSelectionState();
    void updateRecentMenu();
    void rememberFile(const QString& path);
    void showInspector();
    /// Opens the panel for a feature and sets up picking for it.
    void openFeaturePanel(document::FeatureKind kind);
    void closeFeaturePanel();
    void acceptFeature();
    /// Tries the panel's feature on the model and shows the outcome.
    void updateFeaturePreview();
    /// Hands what is picked in the 3D view to the feature panel.
    void takeFeaturePicks();
    /// The sketches whose profiles a pull or a revolve can be picked from.
    [[nodiscard]] std::vector<document::ObjectId> profileSketches(document::ObjectId also) const;
    void onTimelineSelection(document::ObjectId item);
    void showTimelineMenu(document::ObjectId item, const QPoint& where);

    [[nodiscard]] std::vector<document::ObjectId> selectedBodies() const;
    [[nodiscard]] std::vector<document::ObjectId> selectedSketches() const;
    /// The plane of the one flat face that is selected, if that is what is selected.
    [[nodiscard]] std::optional<kernel::Plane> selectedFacePlane() const;
    /// The bodies a command about bodies acts on: those selected, and those whose faces,
    /// edges, or vertices are.
    [[nodiscard]] std::vector<document::ObjectId> targetBodies() const;
    void setBodiesVisible(const std::vector<document::ObjectId>& bodies, bool visible,
                          const QString& label);
    [[nodiscard]] bool confirmDiscard();
    [[nodiscard]] std::string thumbnail();
    [[nodiscard]] QString describe(const viewport::SelectedItem& item) const;
    void showAbout();
    void showShortcuts();
    void showViewportError(const QString& reason);
    void showViewportMenu(const QPoint& position);

    viewport::ViewportWidget* m_viewport;
    BrowserPanel* m_browser = nullptr;
    PropertiesPanel* m_properties = nullptr;
    CommandRegistry* m_commands = nullptr;
    CommandSearch* m_search = nullptr;
    QMenuBar* m_menuBar = nullptr;
    QMenu* m_recentMenu = nullptr;
    QTabBar* m_tabs = nullptr;
    QDockWidget* m_browserDock = nullptr;
    QDockWidget* m_timelineDock = nullptr;
    QDockWidget* m_inspectorDock = nullptr;
    QStackedWidget* m_inspectorStack = nullptr;
    PrimitivePanel* m_panel = nullptr;
    SketchPanel* m_sketchPanel = nullptr;
    SketchPlanePanel* m_planePanel = nullptr;
    FeaturePanel* m_featurePanel = nullptr;
    TimelinePanel* m_timeline = nullptr;
    document::ObjectId m_editedFeature;
    /// The selection filter to go back to when a fillet, chamfer, or shell is done.
    std::optional<viewport::SelectionFilter> m_filterBeforeFeature;
    /// The document whose rebuilding is held back while one of its sketches is open.
    document::Document* m_suspendedDocument = nullptr;
    /// Set while the window itself changes what the 3D view has picked.
    bool m_settingPicks = false;
    QToolBar* m_modelToolbar = nullptr;
    QToolBar* m_sketchToolbar = nullptr;
    /// The sketch toolbar's buttons for single constraints, and the one that lists them all.
    QList<QAction*> m_constraintButtons;
    /// The names of the modeling toolbar's groups, which go when the window is narrow.
    QList<QAction*> m_modelGroupLabels;
    QAction* m_constraintsFolded = nullptr;
    QLabel* m_selectionLabel = nullptr;
    QLabel* m_commandLabel = nullptr;
    QComboBox* m_filterBox = nullptr;
    QToolButton* m_styleButton = nullptr;
    QToolButton* m_patternButton = nullptr;

    std::vector<std::unique_ptr<Session>> m_sessions;
    int m_current = -1;
    QStringList m_loadWarnings;
    bool m_selectionComesFromBrowser = false;
    /// Set while a change to the document is being delivered to its listeners.
    bool m_documentIsChanging = false;
    QByteArray m_defaultLayout;
    /// The layout to return to when the 3D view stops having the window to itself.
    std::optional<QByteArray> m_focusState;
};

} // namespace lcad::app
