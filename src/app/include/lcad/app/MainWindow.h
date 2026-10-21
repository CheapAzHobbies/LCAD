// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/app/ExportDialog.h>
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
class QTimer;
class QToolBar;
class QToolButton;

namespace lcad::app {

class BrowserPanel;
class CommandRegistry;
class InterferenceDialog;
class JointPanel;
class LinksDialog;
class MassDialog;
class MaterialsDialog;
class MovePanel;
class RecoveryStore;
class FeaturePanel;
class GeometryCheckDialog;
class MeasurePanel;
class ParametersDialog;
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
    /// Opens a file the system hands LCAD, named on the command line or opened from the desktop,
    /// and says in a dialog why when it cannot, or what went wrong when it opened with problems.
    void openFromSystem(const QString& path);
    /// Opens with openFromSystem() the files the desktop asks the running LCAD to open. On macOS
    /// a document opened in the Finder arrives this way, not on the command line.
    void acceptFileOpenRequests();
    /// Problems the last loadFrom() reported that did not prevent loading.
    [[nodiscard]] QStringList loadWarnings() const { return m_loadWarnings; }
    /// The files opened or saved most recently, newest first.
    [[nodiscard]] QStringList recentFiles() const;

    // Other programs' files

    /// Adds the solids and surfaces of a STEP or IGES file to the current document as bodies,
    /// in one undo step, and shows them. On failure nothing changes and `error` receives the
    /// reason. `notes` receives what was done to the geometry on the way in, or is wrong
    /// with it.
    bool importGeometry(const QString& path, QString* error = nullptr,
                        QStringList* notes = nullptr);
    /// Writes bodies to a file: those shown, or with `options.selectedOnly` those selected.
    bool exportGeometry(const QString& path, const ExportOptions& options,
                        QString* error = nullptr);
    /// The bodies an export takes: those shown that exist, or those selected.
    [[nodiscard]] std::vector<document::ObjectId> exportedBodies(bool selectedOnly) const;
    /// Checks the geometry of the bodies selected, or of all of them, and shows what was found.
    void checkGeometry();
    /// The list Check Geometry shows, once it has been opened, or nullptr.
    [[nodiscard]] GeometryCheckDialog* geometryCheckDialog() const { return m_checkDialog; }

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

    // Measuring

    /// Opens the Measure panel. Each click in the 3D view picks the next thing to measure;
    /// with two, the panel shows the distance between them and, where it makes sense, the
    /// angle, and the 3D view draws the distance. Cancels a command in progress.
    void startMeasure();
    [[nodiscard]] MeasurePanel* measurePanel() const { return m_measurePanel; }
    /// Opens the parameter table of the current document.
    void showParameters();
    /// The parameter table, once it has been opened, or nullptr.
    [[nodiscard]] ParametersDialog* parametersDialog() const { return m_parametersDialog; }

    // Sketching

    /// Starts a new sketch: on the flat face that is selected, or else by asking for a plane in
    /// the right-hand panel. Cancels a command in progress.
    void startSketch();
    /// The panel that asks where a new sketch goes, or nullptr.
    [[nodiscard]] SketchPlanePanel* planePanel() const { return m_planePanel; }
    /// Adds a sketch on a plane, as one undo step, and opens it for editing. A sketch on a
    /// construction plane, or on a flat face of a body, is attached to it and follows it.
    document::ObjectId createSketch(const kernel::Plane& plane, document::ObjectId attachedTo = {},
                                    const std::string& attachedFace = {});
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
    bool fileImport();
    bool fileExport();
    /// Closes a document, asking first if it has unsaved changes. -1 means the current one.
    bool fileClose(int index = -1);

    // Assemblies

    /// The occurrence being worked on in the current document, by its path from the root:
    /// empty for the document itself. New bodies and sketches go into its component.
    [[nodiscard]] std::vector<document::ObjectId> activePath() const;
    /// The component being worked on: not valid for the document itself.
    [[nodiscard]] document::ObjectId activeComponent() const;
    /// Works on an occurrence's component, or on the document itself (an empty path). A path
    /// that leads nowhere is taken as the document's.
    void activateOccurrence(const std::vector<document::ObjectId>& path);
    /// Makes an empty component inside the one being worked on, as one undo step, and works on
    /// it. Returns the component.
    document::ObjectId newComponent();
    /// Makes a component of the bodies selected. On failure nothing changes and `error`
    /// receives the reason.
    bool makeComponentFromSelection(QString* error = nullptr);
    /// Inserts another LCAD document as a linked part into the component being worked on. On
    /// failure nothing changes and `error` receives the reason.
    bool insertLinkedPart(const QString& path, QString* error = nullptr);
    /// The occurrences selected in the browser, or whose bodies are selected in the 3D view,
    /// each by its path, as seen from the component being worked on.
    [[nodiscard]] std::vector<std::vector<document::ObjectId>> selectedOccurrences() const;
    /// Opens the Joint panel: to add a joint, or with `joint` valid to edit that one.
    void startJoint(document::ObjectId joint = {});
    [[nodiscard]] JointPanel* jointPanel() const { return m_jointPanel; }
    /// Opens the panel that moves the selected occurrence by hand.
    void startMoveComponent();
    [[nodiscard]] MovePanel* movePanel() const { return m_movePanel; }
    /// Shows a joint (one being set up in the Joint panel, for an invalid id) going through its
    /// range in the 3D view, for a few seconds. The document is not changed.
    void animateJoint(document::ObjectId joint = {});
    void stopAnimation();
    [[nodiscard]] bool isAnimating() const;
    /// Finds where the parts shown overlap, and lists it.
    void checkInterference();
    [[nodiscard]] InterferenceDialog* interferenceDialog() const { return m_interferenceDialog; }
    /// Shows the mass properties of the bodies selected, or of all that are shown.
    void showPhysicalProperties();
    [[nodiscard]] MassDialog* massDialog() const { return m_massDialog; }
    /// Opens the list of materials, where the document's own are added and changed.
    void showMaterials();
    [[nodiscard]] MaterialsDialog* materialsDialog() const { return m_materialsDialog; }
    /// Lists the files the document links to, and the state of each.
    void showLinks();
    [[nodiscard]] LinksDialog* linksDialog() const { return m_linksDialog; }
    /// Reads the linked files that changed, as one undo step. Returns what could not be read.
    QStringList updateLinks();
    /// Gathers the document and the files it links to into a folder. On failure `error`
    /// receives the reason; `problems` what could not be gathered.
    bool packageTo(const QString& folder, QString* error = nullptr,
                   QStringList* problems = nullptr);
    bool filePackage();

    // Autosave and recovery

    /// Keeps copies of unsaved work in `folder`, every `seconds`, and offers what an LCAD that
    /// did not close properly left there. Off until called; tests give it a folder of their
    /// own.
    void enableRecovery(const QString& folder, int seconds = 60);
    /// Writes the copies of unsaved work now, as the timer would.
    void autosave();
    /// Offers to restore what was left behind; returns how many documents were restored.
    /// `ask` false restores without asking.
    int offerRecovery(bool ask = true);
    [[nodiscard]] RecoveryStore* recovery() const { return m_recovery.get(); }

    // The window

    /// Puts the panels back where they are in a new installation.
    void resetLayout();
    /// Gives the whole window to the 3D view, or brings the panels back as they were.
    void setViewportFocus(bool focused);
    [[nodiscard]] bool isViewportFocused() const { return m_focusState.has_value(); }
    /// The menu a right-click at this spot of the 3D view opens. The caller owns it.
    [[nodiscard]] QMenu* createViewportMenu(const QPoint& position);

    bool eventFilter(QObject* watched, QEvent* event) override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    struct Session {
        std::unique_ptr<document::Document> document;
        document::Document::ListenerId listener = 0;
        QString filePath;
        /// The occurrence being worked on.
        std::vector<document::ObjectId> activePath;
        /// What tells this document's copy of unsaved work apart, and whether it changed since
        /// that was last written.
        QString recoveryKey;
        bool autosaveDue = false;
        /// The camera as it was when another document was shown instead.
        std::optional<viewport::CameraState> camera;
        std::array<bool, 6> origin{};
    };

    void defineCommands();
    void defineSketchCommands();
    void defineAssemblyCommands();
    /// Works on `component` if it is not the one being worked on: through `path` if that leads
    /// to it, or else its first occurrence.
    void ensureActive(document::ObjectId component,
                      const std::vector<document::ObjectId>& path = {});
    void closeJointPanel();
    void acceptJoint();
    void updateJointPreview();
    void takeJointPick();
    void closeMovePanel();
    void updateMovePreview();
    void stepAnimation();
    /// The bodies a mesh, a weighing, or an overlap check takes: each shown appearance of each
    /// body, placed in the root's coordinates, and what it is called.
    struct PlacedBody {
        document::ObjectId body;
        std::vector<document::ObjectId> path;
        kernel::Shape shape;
        QString name;
    };
    [[nodiscard]] std::vector<PlacedBody> placedBodies(bool selectedOnly) const;
    void showAssemblyMenu(const QPoint& where);
    void relink(document::ObjectId component);
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
    /// For a pull by a distance: the arrow in the 3D view that is dragged to set it, at the
    /// middle of the profiles picked. Nothing for anything else.
    void showPullHandle(const document::Feature& feature);
    /// Takes a distance dragged or typed at the pull's arrow.
    void setPullDistance(double distance);
    /// Hands what is picked in the 3D view to the feature panel.
    void takeFeaturePicks();
    /// The sketches whose profiles a pull or a revolve can be picked from.
    [[nodiscard]] std::vector<document::ObjectId> profileSketches(document::ObjectId also) const;
    void onTimelineSelection(document::ObjectId item);
    void closeMeasure();
    void takeMeasurePick();
    void showMeasurement();
    /// The shape a selection entry stands for, or a null shape.
    [[nodiscard]] kernel::Shape shapeOf(const viewport::SelectedItem& item) const;
    void showTimelineMenu(document::ObjectId item, const QPoint& where);

    [[nodiscard]] std::vector<document::ObjectId> selectedBodies() const;
    [[nodiscard]] std::vector<document::ObjectId> selectedSketches() const;
    /// The plane of the one flat face that is selected, if that is what is selected.
    [[nodiscard]] std::optional<kernel::Plane> selectedFacePlane() const;
    /// The body and the name of the one flat face that is selected, if that is what is selected.
    [[nodiscard]] std::optional<std::pair<document::ObjectId, std::string>>
    selectedFaceName() const;
    /// The stable name of a face or an edge in the 3D view, or an empty string.
    [[nodiscard]] std::string nameOf(const viewport::SelectedItem& item) const;
    /// The construction plane selected in the browser or the timeline, if there is one.
    [[nodiscard]] document::ObjectId selectedConstructionPlane() const;
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
    MeasurePanel* m_measurePanel = nullptr;
    ParametersDialog* m_parametersDialog = nullptr;
    GeometryCheckDialog* m_checkDialog = nullptr;
    JointPanel* m_jointPanel = nullptr;
    /// The joint being edited in the Joint panel, or an invalid id for a new one.
    document::ObjectId m_editedJoint;
    MovePanel* m_movePanel = nullptr;
    /// The occurrence the Move panel moves.
    document::ObjectId m_movedOccurrence;
    InterferenceDialog* m_interferenceDialog = nullptr;
    std::vector<kernel::Shape> m_overlaps;
    MassDialog* m_massDialog = nullptr;
    MaterialsDialog* m_materialsDialog = nullptr;
    LinksDialog* m_linksDialog = nullptr;
    /// Showing a joint in motion: the timer, the joint, and how far along.
    QTimer* m_animationTimer = nullptr;
    document::Joint m_animatedJoint;
    int m_animationStep = 0;
    std::unique_ptr<RecoveryStore> m_recovery;
    QTimer* m_autosaveTimer = nullptr;
    int m_nextRecoveryKey = 1;
    /// The settings the last export was made with.
    ExportOptions m_exportOptions;
    /// What is being measured: the first and the second thing picked.
    std::vector<viewport::SelectedItem> m_measured;
    std::optional<viewport::SelectionFilter> m_filterBeforeMeasure;
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
    QAction* m_constraintsFolded = nullptr;
    /// The toolbar's buttons for New Component and Joint, which a narrow window puts away: the
    /// assembly list beside them has both.
    QList<QAction*> m_assemblyButtons;
    /// The names of the modeling toolbar's groups, which go when the window is narrow.
    QList<QAction*> m_modelGroupLabels;
    /// The names of the sketch toolbar's groups, which go when the window is too narrow.
    QList<QAction*> m_sketchGroupLabels;
    QToolButton* m_finishButton = nullptr;
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
