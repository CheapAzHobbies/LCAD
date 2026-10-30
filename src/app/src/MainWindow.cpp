// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/MainWindow.h>

#include <lcad/app/Application.h>
#include <lcad/app/AssemblyDialogs.h>
#include <lcad/app/BrowserPanel.h>
#include <lcad/app/CommandSearch.h>
#include <lcad/app/Commands.h>
#include <lcad/app/FeaturePanel.h>
#include <lcad/app/GeometryCheckDialog.h>
#include <lcad/app/Icons.h>
#include <lcad/app/JointPanel.h>
#include <lcad/app/MeasurePanel.h>
#include <lcad/app/MovePanel.h>
#include <lcad/app/ParametersDialog.h>
#include <lcad/app/Paths.h>
#include <lcad/app/PropertiesPanel.h>
#include <lcad/app/Recovery.h>
#include <lcad/app/ShortcutsDialog.h>
#include <lcad/app/SketchPanel.h>
#include <lcad/app/Theme.h>
#include <lcad/app/TimelinePanel.h>
#include <lcad/core/Log.h>
#include <lcad/core/Path.h>
#include <lcad/document/File.h>
#include <lcad/document/Links.h>
#include <lcad/document/SketchShapes.h>
#include <lcad/kernel/Check.h>
#include <lcad/kernel/Error.h>
#include <lcad/kernel/Exchange.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>
#include <lcad/kernel/Version.h>
#include <lcad/viewport/SketchEditor.h>

#include <QApplication>
#include <QBuffer>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QToolBar>
#include <QToolButton>

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace lcad::app {

namespace {

using viewport::DisplayStyle;
using viewport::NavigationPreset;
using viewport::OriginItem;
using viewport::SelectedItem;
using viewport::SelectionFilter;
using viewport::SketchTool;
using viewport::StandardView;

// Settings keys. The layout version goes up when the set of panels changes, so that a layout
// saved by an older LCAD is not applied to a window it no longer fits.
const int layoutVersion = 3;
const char* const geometryKey = "window/geometry";
const char* const layoutKey = "window/layout";
const char* const reverseZoomKey = "view/reverseZoom";
const char* const gridKey = "view/grid";
const char* const axesKey = "view/axes";
const char* const animateKey = "view/animate";
const char* const navigationKey = "view/navigation";
const char* const styleKey = "view/displayStyle";
const char* const recentKey = "files/recent";
const char* const directoryKey = "files/lastDirectory";
const char* const importDirectoryKey = "files/lastImportDirectory";
const char* const exportDirectoryKey = "files/lastExportDirectory";
const char* const exportFormatKey = "export/format";
const char* const exportDeviationKey = "export/deviation";
const char* const exportAngleKey = "export/angle";
const char* const exportTextKey = "export/stlText";
const int recentLimit = 10;

QLabel* groupLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text.toUpper(), parent);
    label->setObjectName(QStringLiteral("groupLabel"));
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() * Theme::instance().tokens().secondaryScale);
    label->setFont(font);
    return label;
}

// A label with a little room to spare. The font's measure of a short word can come out a pixel
// or two narrower than the word is drawn, and its last letter would be cut off.
class RoomyLabel : public QLabel {
public:
    using QLabel::QLabel;
    [[nodiscard]] QSize sizeHint() const override { return roomier(QLabel::sizeHint()); }
    [[nodiscard]] QSize minimumSizeHint() const override {
        return roomier(QLabel::minimumSizeHint());
    }

private:
    [[nodiscard]] QSize roomier(QSize size) const {
        if (!text().isEmpty()) {
            size.rwidth() += fontMetrics().averageCharWidth();
        }
        return size;
    }
};

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), m_viewport(new viewport::ViewportWidget(this)),
      m_commands(new CommandRegistry(this)) {
    // Within the screen, with room for the title bar: Windows lets a window start larger than
    // it, with the bottom of the window behind the taskbar.
    resize(QSize(1280, 800).boundedTo(screen()->availableSize() - QSize(0, 40)));
    setCentralWidget(m_viewport);
    m_viewport->setContextMenuPolicy(Qt::CustomContextMenu);

    createDocks();
    defineCommands();
    defineAssemblyCommands();
    createTopBar();
    createToolbar();
    createSketchToolbar();
    m_search = new CommandSearch(m_commands, this);

    m_selectionLabel = new QLabel(this);
    m_selectionLabel->setObjectName(QStringLiteral("selectionSummary"));
    statusBar()->addWidget(m_selectionLabel, 1);
    m_commandLabel = new RoomyLabel(this);
    m_commandLabel->setObjectName(QStringLiteral("activeCommand"));
    statusBar()->addPermanentWidget(m_commandLabel);
    auto* units = new QLabel(tr("mm"), this);
    units->setToolTip(tr("Lengths are in millimeters."));
    statusBar()->addPermanentWidget(units);

    // The browser and the viewport show one selection of bodies. Whichever of them the user
    // changed it in tells the other, and the other's report of that is not sent back: a hidden
    // body can be selected in the browser only, and the viewport's answer that it has nothing
    // selected must not clear it there.
    connect(m_browser, &BrowserPanel::bodySelectionChanged, this, [this] {
        m_selectionComesFromBrowser = true;
        // Each body where it is listed: under an occurrence, that appearance of it; and every
        // body of an occurrence that is selected.
        std::vector<SelectedItem> items = m_browser->selectedItems();
        for (const auto& path : m_browser->selectedOccurrences()) {
            for (const document::Instance& instance : document()->instances()) {
                if (instance.path.size() < path.size() ||
                    !std::equal(path.begin(), path.end(), instance.path.begin())) {
                    continue;
                }
                for (const document::Body& body : document()->bodies()) {
                    if (body.component == instance.component) {
                        items.push_back({body.id, SelectedItem::Kind::Body, -1, instance.path});
                    }
                }
            }
        }
        m_viewport->setSelection(items);
        m_selectionComesFromBrowser = false;
        updateSelectionState();
    });
    connect(m_viewport, &viewport::ViewportWidget::selectionChanged, this, [this] {
        if (m_featurePanel != nullptr && !m_settingPicks) {
            takeFeaturePicks();
        }
        if (m_jointPanel != nullptr && !m_settingPicks) {
            takeJointPick();
        }
        if (m_measurePanel != nullptr && !m_settingPicks) {
            takeMeasurePick();
        }
        if (m_planePanel != nullptr) {
            // A new sketch is waiting for its plane: a flat face that is clicked is it, and the
            // sketch follows the face.
            const std::vector<SelectedItem> picked = m_viewport->selection();
            if (picked.size() == 1 && picked.front().kind == SelectedItem::Kind::Face) {
                if (const auto plane = selectedFacePlane()) {
                    const kernel::Plane chosen = *plane;
                    const auto face = selectedFaceName();
                    const document::ObjectId body =
                        face.has_value() ? face->first : document::ObjectId{};
                    const std::string name = face.has_value() ? face->second : std::string();
                    const std::vector<document::ObjectId> path = picked.front().path;
                    // Later: this is the viewport telling what was clicked, which is no time to
                    // change what it shows.
                    QMetaObject::invokeMethod(
                        this,
                        [this, chosen, body, name, path] {
                            if (const document::Body* on = document()->findBody(body)) {
                                ensureActive(on->component, path);
                            }
                            createSketch(chosen, body, name);
                        },
                        Qt::QueuedConnection);
                } else {
                    statusBar()->showMessage(
                        tr("That face is not flat. A sketch needs a flat face or a plane."), 6000);
                }
            }
        }
        if (!m_selectionComesFromBrowser) {
            std::vector<document::ObjectId> bodies;
            std::vector<document::ObjectId> sketches;
            for (const SelectedItem& item : m_viewport->selection()) {
                if (item.kind == SelectedItem::Kind::Body) {
                    bodies.push_back(item.body);
                } else if (item.kind == SelectedItem::Kind::Sketch) {
                    sketches.push_back(item.body);
                }
            }
            if (m_documentIsChanging) {
                // The viewport let go of something because the document changed, not because
                // the user picked something else: a body that was hidden, a sketch with
                // nothing in it to show. What is selected in the browser and still exists
                // stays selected there.
                for (const document::ObjectId id : m_browser->selectedBodies()) {
                    const document::Body* body = document()->findBody(id);
                    if (body != nullptr && !body->visible &&
                        std::find(bodies.begin(), bodies.end(), id) == bodies.end()) {
                        bodies.push_back(id);
                    }
                }
                for (const document::ObjectId id : m_browser->selectedSketches()) {
                    if (document()->findSketch(id) != nullptr &&
                        std::find(sketches.begin(), sketches.end(), id) == sketches.end()) {
                        sketches.push_back(id);
                    }
                }
            }
            if (isSketching()) {
                // The viewport lets go of everything while a sketch is open. The browser keeps
                // showing which sketch that is.
                sketches = {m_viewport->editedSketch()};
            }
            // Each body under the occurrence it was picked in.
            std::vector<SelectedItem> listed;
            for (const SelectedItem& item : m_viewport->selection()) {
                if (item.kind == SelectedItem::Kind::Body) {
                    listed.push_back(item);
                }
            }
            for (const document::ObjectId id : bodies) {
                if (std::none_of(listed.begin(), listed.end(),
                                 [id](const SelectedItem& item) { return item.body == id; })) {
                    listed.push_back({id, SelectedItem::Kind::Body, -1});
                }
            }
            for (const document::ObjectId id : sketches) {
                listed.push_back({id, SelectedItem::Kind::Sketch, -1});
            }
            m_browser->setSelection(listed);
        }
        updateSelectionState();
    });
    connect(m_viewport, &viewport::ViewportWidget::sketchEditingChanged, this,
            &MainWindow::onSketchEditingChanged);
    connect(m_viewport, &viewport::ViewportWidget::itemActivated, this,
            [this](const SelectedItem& item) {
                if (item.kind == SelectedItem::Kind::Sketch) {
                    editSketch(item.body);
                }
            });
    // Choosing the plane of a new sketch in the 3D view. Queued: the viewport is in the middle
    // of a click when it says so, and the answer changes what it shows.
    connect(
        m_viewport, &viewport::ViewportWidget::planePicked, this,
        [this](OriginItem plane) {
            if (m_planePanel != nullptr) {
                createSketch(plane == OriginItem::PlaneXZ   ? kernel::planeXZ()
                             : plane == OriginItem::PlaneYZ ? kernel::planeYZ()
                                                            : kernel::planeXY());
            }
        },
        Qt::QueuedConnection);
    connect(m_viewport, &viewport::ViewportWidget::planeHovered, this, [this](int plane) {
        if (m_planePanel == nullptr) {
            return;
        }
        m_planePanel->setHoveredPlane(plane);
        const QString names[3] = {tr("XY Plane"), tr("XZ Plane"), tr("YZ Plane")};
        if (plane >= 0 && plane < 3) {
            statusBar()->showMessage(tr("%1: click to sketch on it").arg(names[plane]));
        } else {
            statusBar()->showMessage(
                tr("Click a plane in the 3D view, or a flat face of a body, to sketch on it."));
        }
    });
    connect(m_browser, &BrowserPanel::sketchActivated, this,
            [this](document::ObjectId sketch) { editSketch(sketch); });
    connect(m_browser, &BrowserPanel::occurrenceActivated, this,
            [this](const std::vector<document::ObjectId>& path) {
                if (!isSketching()) {
                    activateOccurrence(path);
                }
            });
    connect(m_browser, &BrowserPanel::jointActivated, this,
            [this](document::ObjectId joint) { startJoint(joint); });
    // Queued: the browser is in the middle of a drop, and the answer refills it.
    connect(
        m_browser, &BrowserPanel::occurrenceDropped, this,
        [this](document::ObjectId occurrence, document::ObjectId component) {
            try {
                document()->moveOccurrence(occurrence, component);
            } catch (const std::invalid_argument& failure) {
                statusBar()->showMessage(QString::fromUtf8(failure.what()), 9000);
            }
        },
        Qt::QueuedConnection);
    connect(m_browser, &BrowserPanel::planeActivated, this,
            [this](document::ObjectId plane) { editFeature(plane); });
    // A new sketch on a construction plane, clicked in the 3D view. Queued, as for the origin
    // planes.
    connect(
        m_viewport, &viewport::ViewportWidget::constructionPlanePicked, this,
        [this](document::ObjectId plane) {
            if (m_planePanel != nullptr) {
                if (const auto state = document()->planeState(plane)) {
                    createSketch(state->plane, plane);
                }
            }
        },
        Qt::QueuedConnection);
    connect(m_viewport->sketchEditor(), &viewport::SketchEditor::changed, this,
            [this] { updateSelectionState(); });
    connect(m_viewport->sketchEditor(), &viewport::SketchEditor::message, this,
            [this](const QString& text) { statusBar()->showMessage(text, 9000); });
    connect(m_browser, &BrowserPanel::originItemToggled, this,
            [this](OriginItem item, bool visible) {
                current().origin[static_cast<std::size_t>(item)] = visible;
                m_viewport->setOriginItemVisible(item, visible);
            });
    connect(m_browser, &BrowserPanel::contextMenuRequested, this, [this](const QPoint& where) {
        if (!m_browser->selectedOccurrences().empty() || !m_browser->selectedJoints().empty() ||
            (m_browser->selectedBodies().empty() && m_browser->selectedSketches().empty())) {
            showAssemblyMenu(where);
            return;
        }
        m_commands->refresh();
        auto* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        for (const char* id :
             {"sketch.edit", "body.rename", "body.hide", "body.show", "body.isolate"}) {
            if (m_commands->action(QLatin1String(id))->isEnabled()) {
                menu->addAction(m_commands->action(QLatin1String(id)));
            }
        }
        menu->addSeparator();
        menu->addAction(m_commands->action(QStringLiteral("edit.delete")));
        menu->popup(where);
    });
    // A feature takes what is picked in the 3D view.
    connect(m_viewport, &viewport::ViewportWidget::profilesPicked, this,
            &MainWindow::takeFeaturePicks);
    // A pull's distance, dragged or typed at its arrow in the 3D view.
    connect(m_viewport, &viewport::ViewportWidget::distanceDragged, this,
            &MainWindow::setPullDistance);
    connect(m_viewport, &viewport::ViewportWidget::distanceEntered, this,
            [this](const QString& text) {
                QString number = text.trimmed();
                number.remove(QRegularExpression(QStringLiteral("\\s*mm$")));
                bool ok = false;
                double value = QLocale::c().toDouble(number, &ok);
                if (!ok) {
                    value = QLocale().toDouble(number, &ok);
                }
                if (!ok) {
                    // Not a number: an expression of parameters, such as "depth * 2".
                    try {
                        value = document()->evaluate(text.trimmed().toStdString());
                        ok = true;
                    } catch (const std::invalid_argument& failure) {
                        statusBar()->showMessage(
                            tr("\"%1\" is neither a distance nor an expression that can be "
                               "worked out: %2")
                                .arg(text, QString::fromUtf8(failure.what())),
                            9000);
                    }
                }
                if (ok) {
                    setPullDistance(value);
                }
            });
    connect(m_timeline, &TimelinePanel::itemSelected, this, &MainWindow::onTimelineSelection);
    connect(m_timeline, &TimelinePanel::itemActivated, this, [this](document::ObjectId item) {
        if (document()->findFeature(item) != nullptr) {
            editFeature(item);
        } else if (document()->findSketch(item) != nullptr) {
            editSketch(item);
        }
    });
    connect(m_timeline, &TimelinePanel::contextMenuRequested, this, &MainWindow::showTimelineMenu);
    connect(m_timeline, &TimelinePanel::message, this,
            [this](const QString& text) { statusBar()->showMessage(text, 9000); });
    connect(m_viewport, &QWidget::customContextMenuRequested, this, &MainWindow::showViewportMenu);
    // Queued: the viewport reports failure while painting, which is no place to open a dialog.
    connect(m_viewport, &viewport::ViewportWidget::initializationFailed, this,
            &MainWindow::showViewportError, Qt::QueuedConnection);
    connect(&Theme::instance(), &Theme::changed, this, &MainWindow::applyTheme);

    m_defaultLayout = saveState(layoutVersion);
    restoreSettings();
    applyTheme();

    addSession(std::make_unique<document::Document>(), QString());
    activate(0);
}

MainWindow::~MainWindow() {
    // A command's panel would report to a window that is half gone.
    closeFeaturePanel();
    // The viewport and the panels are destroyed after the documents, as child widgets. Detach
    // them first.
    m_viewport->setDocument(nullptr);
    m_browser->setDocument(nullptr, {});
    m_timeline->setDocument(nullptr);
    if (m_parametersDialog != nullptr) {
        m_parametersDialog->setDocument(nullptr);
    }
    m_properties->refresh(nullptr, {}, {});
}

void MainWindow::showParameters() {
    if (m_parametersDialog == nullptr) {
        m_parametersDialog = new ParametersDialog(this);
    }
    m_parametersDialog->setDocument(document());
    m_parametersDialog->show();
    m_parametersDialog->raise();
    m_parametersDialog->activateWindow();
}

// ---------------------------------------------------------------------------------------------
// Commands

void MainWindow::defineCommands() {
    const auto make = [](const char* id, const QString& text, const QString& group,
                         const QString& description) {
        Command command;
        command.id = QString::fromLatin1(id);
        command.text = text;
        command.group = group;
        command.description = description;
        return command;
    };
    const QString file = tr("File");
    const QString edit = tr("Edit");
    const QString create = tr("Create");
    const QString select = tr("Select");
    const QString view = tr("View");
    const QString window = tr("Window");
    const QString help = tr("Help");

    // File
    {
        Command c = make("file.new", tr("&New"), file, tr("Start a new document in a new tab"));
        c.iconName = QStringLiteral("new");
        c.shortcuts = {QKeySequence::New};
        c.run = [this](bool) { fileNew(); };
        m_commands->add(std::move(c));

        c = make("file.open", tr("&Open…"), file, tr("Open a document"));
        c.iconName = QStringLiteral("open");
        c.shortcuts = {QKeySequence::Open};
        c.aliases = {tr("load")};
        c.run = [this](bool) { fileOpen(); };
        m_commands->add(std::move(c));

        c = make("file.save", tr("&Save"), file, tr("Save the document"));
        c.iconName = QStringLiteral("save");
        c.shortcuts = {QKeySequence::Save};
        c.run = [this](bool) { fileSave(); };
        m_commands->add(std::move(c));

        c = make("file.saveAs", tr("Save &As…"), file, tr("Save the document under another name"));
        c.shortcuts = {QKeySequence::SaveAs};
        c.aliases = {tr("save copy"), tr("rename file")};
        c.run = [this](bool) { fileSaveAs(); };
        m_commands->add(std::move(c));

        c = make("file.import", tr("&Import…"), file,
                 tr("Add the solids of a STEP or IGES file to the document, as bodies"));
        c.iconName = QStringLiteral("import");
        c.shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_I)};
        c.aliases = {tr("step"), tr("stp"), tr("iges"), tr("igs"), tr("open step"), tr("insert")};
        c.run = [this](bool) { fileImport(); };
        c.isEnabled = [this] { return !isSketching(); };
        m_commands->add(std::move(c));

        c = make("file.export", tr("&Export…"), file,
                 tr("Write bodies to a STEP, STL, 3MF, or OBJ file for other programs"));
        c.iconName = QStringLiteral("export");
        c.shortcuts = {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E)};
        c.aliases = {tr("stl"),         tr("3mf"),      tr("obj"), tr("step"),
                     tr("save as stl"), tr("3d print"), tr("mesh")};
        c.run = [this](bool) { fileExport(); };
        c.isEnabled = [this] {
            const auto& bodies = document()->bodies();
            return std::any_of(bodies.begin(), bodies.end(),
                               [](const document::Body& body) { return !body.shape.isNull(); });
        };
        m_commands->add(std::move(c));

        c = make("file.close", tr("&Close Document"), file, tr("Close the current document"));
        c.iconName = QStringLiteral("close");
        c.shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_W)};
        c.aliases = {tr("close tab")};
        c.run = [this](bool) { fileClose(); };
        m_commands->add(std::move(c));

        c = make("file.quit", tr("&Quit"), file, tr("Close LCAD"));
        c.shortcuts = {QKeySequence::Quit};
        c.aliases = {tr("exit")};
        c.run = [this](bool) { close(); };
        m_commands->add(std::move(c));
    }

    // Edit
    {
        Command c = make("edit.undo", tr("&Undo"), edit, tr("Undo the last change"));
        c.iconName = QStringLiteral("undo");
        c.shortcuts = {QKeySequence::Undo};
        c.run = [this](bool) {
            if (document()->canUndo()) {
                document()->undo();
            }
        };
        c.isEnabled = [this] { return document()->canUndo(); };
        c.currentText = [this] {
            const QString label = QString::fromStdString(document()->undoLabel());
            return label.isEmpty() ? tr("&Undo") : tr("&Undo %1").arg(label);
        };
        m_commands->add(std::move(c));

        c = make("edit.redo", tr("&Redo"), edit, tr("Redo the change that was undone"));
        c.iconName = QStringLiteral("redo");
        c.shortcuts = {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z),
                       QKeySequence(Qt::CTRL | Qt::Key_Y)};
        c.run = [this](bool) {
            if (document()->canRedo()) {
                document()->redo();
            }
        };
        c.isEnabled = [this] { return document()->canRedo(); };
        c.currentText = [this] {
            const QString label = QString::fromStdString(document()->redoLabel());
            return label.isEmpty() ? tr("&Redo") : tr("&Redo %1").arg(label);
        };
        m_commands->add(std::move(c));

        c = make("edit.delete", tr("&Delete"), edit, tr("Delete what is selected"));
        c.iconName = QStringLiteral("delete");
        c.shortcuts = {QKeySequence::Delete};
        c.aliases = {tr("remove"), tr("erase")};
        c.run = [this](bool) {
            const auto joints = m_browser->selectedJoints();
            const auto occurrences = m_browser->selectedOccurrences();
            if (!isSketching() && (!joints.empty() || !occurrences.empty())) {
                // Joints and occurrences chosen in the browser, as one step.
                document::Transaction transaction(*document(), tr("Delete").toStdString());
                for (const document::ObjectId joint : joints) {
                    if (document()->findJoint(joint) != nullptr) {
                        document()->removeJoint(joint);
                    }
                }
                for (const auto& path : occurrences) {
                    if (document()->findOccurrence(path.back()) != nullptr) {
                        document()->removeOccurrence(path.back());
                    }
                }
                transaction.commit();
                updateSelectionState();
                return;
            }
            if (isSketching()) {
                m_viewport->sketchEditor()->deleteSelection();
            } else if (m_timeline->selectedItem().isValid() &&
                       (m_timeline->isAncestorOf(QApplication::focusWidget()) ||
                        (selectedBodies().empty() && selectedSketches().empty()))) {
                // What is selected in the timeline, when that is where the user is working or
                // nothing else is selected.
                deleteTimelineItem(m_timeline->selectedItem());
            } else {
                deleteSelectedBodies();
            }
        };
        c.isEnabled = [this] {
            if (isSketching()) {
                const viewport::SketchEditor* editor = m_viewport->sketchEditor();
                return !editor->selectedEntities().empty() ||
                       !editor->selectedConstraints().empty();
            }
            return !selectedBodies().empty() || !selectedSketches().empty() ||
                   m_timeline->selectedItem().isValid() || !m_browser->selectedJoints().empty() ||
                   !m_browser->selectedOccurrences().empty();
        };
        m_commands->add(std::move(c));

        c = make("edit.cancel", tr("&Cancel Command"), edit,
                 tr("Leave the command or the sketch tool in progress"));
        c.shortcuts = {QKeySequence(Qt::Key_Escape)};
        c.aliases = {tr("escape"), tr("abort")};
        c.run = [this](bool) {
            if (m_panel != nullptr || m_planePanel != nullptr || m_featurePanel != nullptr ||
                m_measurePanel != nullptr || m_jointPanel != nullptr || m_movePanel != nullptr) {
                cancelCommand();
            } else if (isAnimating()) {
                stopAnimation();
            } else if (isSketching()) {
                m_viewport->sketchEditor()->cancel();
            }
        };
        c.isEnabled = [this] {
            return m_panel != nullptr || m_planePanel != nullptr || m_featurePanel != nullptr ||
                   m_measurePanel != nullptr || m_jointPanel != nullptr || m_movePanel != nullptr ||
                   isAnimating() || (isSketching() && m_viewport->sketchEditor()->canCancel());
        };
        m_commands->add(std::move(c));

        c = make("body.rename", tr("Re&name"), edit,
                 tr("Rename the selected body, sketch, component, or joint"));
        c.shortcuts = {QKeySequence(Qt::Key_F2)};
        c.run = [this](bool) {
            const auto bodies = selectedBodies();
            const auto sketches = selectedSketches();
            if (bodies.size() == 1 && sketches.empty()) {
                m_browser->editBody(bodies.front());
            } else if (sketches.size() == 1 && bodies.empty()) {
                m_browser->editSketchName(sketches.front());
            } else {
                m_browser->editSelectedName();
            }
        };
        c.isEnabled = [this] {
            return selectedBodies().size() + selectedSketches().size() +
                       m_browser->selectedOccurrences().size() +
                       m_browser->selectedJoints().size() ==
                   1;
        };
        m_commands->add(std::move(c));

        // Hide and Show act on bodies, sketches, and components alike.
        const auto anyShown = [this](bool shown) {
            const auto bodies = targetBodies();
            const auto sketches = selectedSketches();
            const auto occurrences = m_browser->selectedOccurrences();
            return std::any_of(occurrences.begin(), occurrences.end(),
                               [this, shown](const std::vector<document::ObjectId>& path) {
                                   const document::Occurrence* occurrence =
                                       document()->findOccurrence(path.back());
                                   return occurrence != nullptr && occurrence->visible == shown;
                               }) ||
                   std::any_of(bodies.begin(), bodies.end(),
                               [this, shown](document::ObjectId id) {
                                   const document::Body* body = document()->findBody(id);
                                   return body != nullptr && body->visible == shown;
                               }) ||
                   std::any_of(sketches.begin(), sketches.end(),
                               [this, shown](document::ObjectId id) {
                                   const document::SketchItem* sketch = document()->findSketch(id);
                                   return sketch != nullptr && sketch->visible == shown;
                               });
        };
        c = make("body.hide", tr("&Hide"), edit, tr("Hide the selected bodies and sketches"));
        c.iconName = QStringLiteral("eye-off");
        c.aliases = {tr("invisible")};
        c.run = [this](bool) {
            if (!m_browser->selectedOccurrences().empty()) {
                document::Transaction transaction(*document(), tr("Hide").toStdString());
                for (const auto& path : m_browser->selectedOccurrences()) {
                    document()->setOccurrenceVisible(path.back(), false);
                }
                transaction.commit();
                return;
            }
            setBodiesVisible(targetBodies(), false, tr("Hide"));
        };
        c.isEnabled = [this, anyShown] { return !isSketching() && anyShown(true); };
        m_commands->add(std::move(c));

        c = make("body.show", tr("&Show"), edit, tr("Show the selected bodies and sketches"));
        c.iconName = QStringLiteral("eye");
        c.aliases = {tr("visible"), tr("unhide")};
        c.run = [this](bool) {
            if (!m_browser->selectedOccurrences().empty()) {
                document::Transaction transaction(*document(), tr("Show").toStdString());
                for (const auto& path : m_browser->selectedOccurrences()) {
                    document()->setOccurrenceVisible(path.back(), true);
                }
                transaction.commit();
                return;
            }
            setBodiesVisible(targetBodies(), true, tr("Show"));
        };
        c.isEnabled = [this, anyShown] { return !isSketching() && anyShown(false); };
        m_commands->add(std::move(c));

        c = make("body.isolate", tr("&Isolate"), edit,
                 tr("Hide every body except the selected ones"));
        c.iconName = QStringLiteral("isolate");
        c.aliases = {tr("hide others"), tr("solo")};
        c.run = [this](bool) {
            const auto occurrences = m_browser->selectedOccurrences();
            if (!occurrences.empty()) {
                // The chosen components, and nothing else that is a component, shown.
                // What leads to them, and what is inside them, stays shown.
                const auto related = [&occurrences](const std::vector<document::ObjectId>& path) {
                    return std::any_of(
                        occurrences.begin(), occurrences.end(),
                        [&path](const std::vector<document::ObjectId>& chosen) {
                            const std::size_t n = std::min(path.size(), chosen.size());
                            return std::equal(path.begin(),
                                              path.begin() + static_cast<std::ptrdiff_t>(n),
                                              chosen.begin());
                        });
                };
                std::vector<document::ObjectId> kept;
                for (const document::Instance& instance : document()->instances()) {
                    if (!instance.path.empty() && related(instance.path)) {
                        kept.push_back(instance.path.back());
                    }
                }
                document::Transaction transaction(*document(), tr("Isolate").toStdString());
                for (const document::Occurrence& occurrence :
                     std::vector<document::Occurrence>(document()->occurrences())) {
                    document()->setOccurrenceVisible(
                        occurrence.id,
                        std::find(kept.begin(), kept.end(), occurrence.id) != kept.end());
                }
                transaction.commit();
                return;
            }
            const auto keep = targetBodies();
            std::vector<document::ObjectId> others;
            for (const document::Body& body : document()->bodies()) {
                if (std::find(keep.begin(), keep.end(), body.id) == keep.end()) {
                    others.push_back(body.id);
                }
            }
            document::Transaction transaction(*document(), tr("Isolate").toStdString());
            for (const document::ObjectId id : keep) {
                document()->setBodyVisible(id, true);
            }
            for (const document::ObjectId id : others) {
                document()->setBodyVisible(id, false);
            }
            transaction.commit();
        };
        c.isEnabled = [this] {
            return (!targetBodies().empty() && document()->bodies().size() > 1) ||
                   (!m_browser->selectedOccurrences().empty() &&
                    document()->occurrences().size() > 1);
        };
        m_commands->add(std::move(c));

        c = make("body.showAll", tr("Show &All Bodies"), edit, tr("Show every hidden body"));
        c.iconName = QStringLiteral("eye");
        c.aliases = {tr("reveal hidden"), tr("unhide all")};
        c.run = [this](bool) {
            std::vector<document::ObjectId> hidden;
            for (const document::Body& body : document()->bodies()) {
                if (!body.visible) {
                    hidden.push_back(body.id);
                }
            }
            setBodiesVisible(hidden, true, tr("Show All Bodies"));
        };
        c.isEnabled = [this] {
            const auto& bodies = document()->bodies();
            return std::any_of(bodies.begin(), bodies.end(),
                               [](const document::Body& body) { return !body.visible; });
        };
        m_commands->add(std::move(c));
    }

    // Create
    const std::tuple<PrimitiveKind, const char*, const char*, QStringList> primitives[] = {
        {PrimitiveKind::Box, "create.box", "box", {tr("cube"), tr("block"), tr("cuboid")}},
        {PrimitiveKind::Cylinder, "create.cylinder", "cylinder", {tr("rod"), tr("disc")}},
        {PrimitiveKind::Sphere, "create.sphere", "sphere", {tr("ball")}},
        {PrimitiveKind::Cone, "create.cone", "cone", {tr("taper")}},
        {PrimitiveKind::Torus, "create.torus", "torus", {tr("ring"), tr("donut")}},
    };
    for (const auto& [kind, id, iconName, aliases] : primitives) {
        Command c = make(id, primitiveName(kind), create,
                         tr("Create a %1 by its sizes").arg(primitiveName(kind).toLower()));
        c.iconName = QString::fromLatin1(iconName);
        c.aliases = aliases;
        c.run = [this, kind](bool) { startPrimitive(kind); };
        c.isEnabled = [this] { return !isSketching(); };
        m_commands->add(std::move(c));
    }

    // Features
    const auto hasSketch = [this] { return !document()->sketches().empty(); };
    const auto hasBody = [this] {
        const auto& bodies = document()->bodies();
        return std::any_of(bodies.begin(), bodies.end(),
                           [](const document::Body& body) { return !body.shape.isNull(); });
    };
    const QString modify = tr("Modify");
    {
        using document::FeatureKind;
        Command c = make("solid.pull", tr("&Pull"), create,
                         tr("Pull closed areas of a sketch into a solid, or cut them out of one"));
        c.iconName = QStringLiteral("pull");
        c.shortcuts = {QKeySequence(Qt::Key_E)};
        c.aliases = {tr("extrude"), tr("cut"),  tr("push"),       tr("pad"),
                     tr("pocket"),  tr("boss"), tr("extrude cut")};
        c.run = [this](bool) { startFeature(FeatureKind::Pull); };
        c.isEnabled = hasSketch;
        m_commands->add(std::move(c));

        c = make("solid.revolve", tr("Re&volve"), create,
                 tr("Turn closed areas of a sketch about a line of it into a solid"));
        c.iconName = QStringLiteral("revolve");
        c.aliases = {tr("lathe"), tr("turn"), tr("spin"), tr("rotate profile")};
        c.run = [this](bool) { startFeature(FeatureKind::Revolve); };
        c.isEnabled = hasSketch;
        m_commands->add(std::move(c));

        c = make("solid.mirror", tr("&Mirror"), create,
                 tr("Add the mirror image of features, or of a whole body, in a plane"));
        c.iconName = QStringLiteral("mirror");
        c.aliases = {tr("symmetry"), tr("reflect"), tr("flip copy")};
        c.run = [this](bool) { startFeature(FeatureKind::Mirror); };
        c.isEnabled = hasBody;
        m_commands->add(std::move(c));

        c = make("solid.patternRectangular", tr("&Rectangular Pattern"), create,
                 tr("Repeat features, or a whole body, in rows and columns"));
        c.iconName = QStringLiteral("pattern-rectangular");
        c.aliases = {tr("pattern"), tr("array"), tr("linear pattern"), tr("grid"), tr("repeat")};
        c.run = [this](bool) { startFeature(FeatureKind::RectangularPattern); };
        c.isEnabled = hasBody;
        m_commands->add(std::move(c));

        c = make("solid.patternCircular", tr("&Circular Pattern"), create,
                 tr("Repeat features, or a whole body, around an axis"));
        c.iconName = QStringLiteral("pattern-circular");
        c.aliases = {tr("pattern"), tr("polar array"), tr("radial pattern"), tr("repeat")};
        c.run = [this](bool) { startFeature(FeatureKind::CircularPattern); };
        c.isEnabled = hasBody;
        m_commands->add(std::move(c));

        c = make("solid.hole", tr("&Hole"), create,
                 tr("Drill holes into a body at the points of a sketch: simple, counterbored, or "
                    "countersunk"));
        c.iconName = QStringLiteral("hole");
        c.shortcuts = {QKeySequence(Qt::Key_H)};
        c.aliases = {tr("drill"), tr("bore"), tr("counterbore"), tr("countersink"),
                     tr("screw hole")};
        c.run = [this](bool) { startFeature(FeatureKind::Hole); };
        c.isEnabled = hasBody;
        m_commands->add(std::move(c));

        c = make("construct.offsetPlane", tr("&Offset Plane"), create,
                 tr("Add a construction plane to sketch on: an origin plane or a flat face, "
                    "moved along its normal"));
        c.iconName = QStringLiteral("plane");
        c.aliases = {tr("construction plane"), tr("work plane"), tr("datum plane"),
                     tr("reference plane"), tr("plane")};
        c.run = [this](bool) { startFeature(FeatureKind::Plane); };
        c.isEnabled = [this] { return !isSketching(); };
        m_commands->add(std::move(c));

        c = make("construct.axis", tr("&Axis"), create,
                 tr("Add a construction axis to revolve about or pattern around: an origin axis, "
                    "a straight edge, or the axis of a round edge or face"));
        c.iconName = QStringLiteral("axis");
        c.aliases = {tr("construction axis"), tr("work axis"), tr("datum axis"),
                     tr("reference axis"), tr("axis through cylinder")};
        c.run = [this](bool) { startFeature(FeatureKind::Axis); };
        c.isEnabled = [this] { return !isSketching(); };
        m_commands->add(std::move(c));

        c = make("model.parameters", tr("&Parameters…"), modify,
                 tr("Name values, and work other values out from them"));
        c.aliases = {tr("variables"), tr("user parameters"), tr("equations"), tr("expressions"),
                     tr("change parameters")};
        c.run = [this](bool) { showParameters(); };
        m_commands->add(std::move(c));

        c = make("solid.fillet", tr("&Fillet"), modify, tr("Round edges of a body"));
        c.iconName = QStringLiteral("fillet");
        c.shortcuts = {QKeySequence(Qt::Key_F)};
        c.aliases = {tr("round"), tr("radius"), tr("blend")};
        c.run = [this](bool) { startFeature(FeatureKind::Fillet); };
        c.isEnabled = hasBody;
        m_commands->add(std::move(c));

        c = make("solid.chamfer", tr("C&hamfer"), modify, tr("Bevel edges of a body"));
        c.iconName = QStringLiteral("chamfer");
        c.aliases = {tr("bevel")};
        c.run = [this](bool) { startFeature(FeatureKind::Chamfer); };
        c.isEnabled = hasBody;
        m_commands->add(std::move(c));

        c = make("solid.shell", tr("&Shell"), modify,
                 tr("Hollow a body out, leaving walls of a thickness"));
        c.iconName = QStringLiteral("shell");
        c.aliases = {tr("hollow")};
        c.run = [this](bool) { startFeature(FeatureKind::Shell); };
        c.isEnabled = hasBody;
        m_commands->add(std::move(c));

        // Direct edits, for bodies whatever made them, such as one imported from a STEP file.
        c = make("solid.offsetFace", tr("&Offset Face"), modify,
                 tr("Move faces of a body along their normals, the faces around them following: "
                    "make a part thicker, or a hole wider"));
        c.iconName = QStringLiteral("offset-face");
        c.shortcuts = {QKeySequence(Qt::Key_Q)};
        c.aliases = {tr("press pull"), tr("push pull"), tr("offset"), tr("thicken face"),
                     tr("resize hole")};
        c.run = [this](bool) { startFeature(FeatureKind::OffsetFace); };
        c.isEnabled = hasBody;
        m_commands->add(std::move(c));

        c = make("solid.moveFace", tr("Mo&ve Face"), modify,
                 tr("Move faces of a body in a direction, the faces around them following: move "
                    "a hole or a boss across a face"));
        c.iconName = QStringLiteral("move-face");
        c.aliases = {tr("move"), tr("translate face"), tr("move hole"), tr("shift face")};
        c.run = [this](bool) { startFeature(FeatureKind::MoveFace); };
        c.isEnabled = hasBody;
        m_commands->add(std::move(c));

        c = make("solid.deleteFace", tr("&Delete Face"), modify,
                 tr("Take faces out of a body, the faces around them closing the gap: remove a "
                    "fillet, a hole, or a boss"));
        c.iconName = QStringLiteral("delete-face");
        c.aliases = {tr("remove face"), tr("defeature"), tr("remove fillet"), tr("remove hole"),
                     tr("heal")};
        c.run = [this](bool) { startFeature(FeatureKind::DeleteFace); };
        c.isEnabled = hasBody;
        m_commands->add(std::move(c));

        c = make("solid.repair", tr("&Repair Body"), modify,
                 tr("Put right what is wrong with a body's geometry: sew loose faces, turn faces "
                    "outward, fix edges"));
        c.iconName = QStringLiteral("repair");
        c.aliases = {tr("heal"), tr("fix"), tr("sew"), tr("stitch"), tr("make solid")};
        c.run = [this](bool) { startFeature(FeatureKind::Repair); };
        c.isEnabled = hasBody;
        m_commands->add(std::move(c));
    }

    // The timeline
    {
        const QString timelineGroup = tr("Timeline");
        const auto selectedFeature = [this]() -> const document::Feature* {
            return document()->findFeature(m_timeline->selectedItem());
        };
        Command c = make("timeline.edit", tr("&Edit Feature"), timelineGroup,
                         tr("Open the feature selected in the timeline for editing"));
        c.aliases = {tr("edit feature"), tr("change feature")};
        c.run = [this](bool) {
            const document::ObjectId item = m_timeline->selectedItem();
            if (document()->findFeature(item) != nullptr) {
                editFeature(item);
            } else if (document()->findSketch(item) != nullptr) {
                editSketch(item);
            }
        };
        c.isEnabled = [this] {
            const document::ObjectId item = m_timeline->selectedItem();
            return document()->findFeature(item) != nullptr ||
                   document()->findSketch(item) != nullptr;
        };
        m_commands->add(std::move(c));

        c = make("timeline.suppress", tr("&Suppress Feature"), timelineGroup,
                 tr("Leave the selected feature out of the model, or put it back"));
        c.aliases = {tr("disable feature"), tr("unsuppress"), tr("restore feature")};
        c.run = [this, selectedFeature](bool) {
            if (const document::Feature* feature = selectedFeature()) {
                document()->setFeatureSuppressed(feature->id, !feature->suppressed);
            }
        };
        c.isEnabled = [selectedFeature] { return selectedFeature() != nullptr; };
        c.currentText = [selectedFeature] {
            const document::Feature* feature = selectedFeature();
            return feature != nullptr && feature->suppressed ? tr("Re&store Feature")
                                                             : tr("&Suppress Feature");
        };
        m_commands->add(std::move(c));

        c = make("timeline.rename", tr("Re&name Feature…"), timelineGroup,
                 tr("Give the feature selected in the timeline another name"));
        c.run = [this, selectedFeature](bool) {
            const document::Feature* feature = selectedFeature();
            if (feature == nullptr) {
                return;
            }
            const document::ObjectId id = feature->id;
            bool ok = false;
            const QString name =
                QInputDialog::getText(this, tr("Rename Feature"), tr("Name:"), QLineEdit::Normal,
                                      QString::fromStdString(feature->name), &ok);
            if (ok && !name.trimmed().isEmpty()) {
                document()->renameFeature(id, name.trimmed().toStdString());
            }
        };
        c.isEnabled = [selectedFeature] { return selectedFeature() != nullptr; };
        m_commands->add(std::move(c));

        c = make("timeline.rollBack", tr("Roll &Back to Here"), timelineGroup,
                 tr("Move the history marker to just before the selected entry"));
        c.aliases = {tr("history marker"), tr("rollback")};
        c.run = [this](bool) {
            const auto& timeline = document()->timeline();
            const auto found =
                std::find(timeline.begin(), timeline.end(), m_timeline->selectedItem());
            if (found != timeline.end()) {
                document()->setHistoryMarker(static_cast<std::size_t>(found - timeline.begin()));
            }
        };
        c.isEnabled = [this] {
            const auto& timeline = document()->timeline();
            return std::find(timeline.begin(), timeline.end(), m_timeline->selectedItem()) !=
                   timeline.end();
        };
        m_commands->add(std::move(c));

        c = make("timeline.rollToEnd", tr("Roll &Forward to the End"), timelineGroup,
                 tr("Move the history marker after the last entry, so that everything is "
                    "applied"));
        c.aliases = {tr("history marker"), tr("roll forward")};
        c.run = [this](bool) { document()->setHistoryMarker(document()->timeline().size()); };
        c.isEnabled = [this] {
            return document()->historyMarker() < document()->timeline().size();
        };
        m_commands->add(std::move(c));
    }
    defineSketchCommands();

    // Select
    const std::tuple<SelectionFilter, const char*, QString> filters[] = {
        {SelectionFilter::FacesEdgesVertices, "select.everything", tr("Faces, Edges, Vertices")},
        {SelectionFilter::Bodies, "select.bodies", tr("Bodies")},
        {SelectionFilter::Faces, "select.faces", tr("Faces")},
        {SelectionFilter::Edges, "select.edges", tr("Edges")},
        {SelectionFilter::Vertices, "select.vertices", tr("Vertices")},
    };
    for (const auto& [filter, id, text] : filters) {
        Command c =
            make(id, text, select, tr("Let a click in the 3D view pick %1").arg(text.toLower()));
        c.iconName = QStringLiteral("select");
        c.checkable = true;
        c.exclusiveGroup = QStringLiteral("selectionFilter");
        c.aliases = {tr("selection filter"), tr("pick")};
        c.run = [this, filter](bool) { m_viewport->setSelectionFilter(filter); };
        c.isChecked = [this, filter] { return m_viewport->selectionFilter() == filter; };
        // While a sketch is open, a click picks what is in the sketch.
        c.isEnabled = [this] { return !isSketching(); };
        m_commands->add(std::move(c));
    }

    // View: the camera
    {
        Command c = make("view.fit", tr("&Fit"), view, tr("Bring everything into view"));
        c.iconName = QStringLiteral("fit");
        c.shortcuts = {QKeySequence(Qt::Key_F6)};
        c.aliases = {tr("zoom all"), tr("zoom extents"), tr("frame")};
        c.run = [this](bool) { m_viewport->fitAll(); };
        m_commands->add(std::move(c));

        c = make("view.fitSelection", tr("Zoom to &Selection"), view,
                 tr("Bring what is selected into view"));
        c.iconName = QStringLiteral("fit-selection");
        c.shortcuts = {QKeySequence(Qt::SHIFT | Qt::Key_F6)};
        c.aliases = {tr("frame selection"), tr("zoom selected")};
        c.run = [this](bool) { m_viewport->fitSelection(); };
        c.isEnabled = [this] { return !isSketching() && !m_viewport->selection().empty(); };
        m_commands->add(std::move(c));
    }
    {
        Command c = make("inspect.measure", tr("&Measure"), tr("Inspect"),
                         tr("Measure distances and angles between things picked in the 3D view"));
        c.iconName = QStringLiteral("dimension");
        c.shortcuts = {QKeySequence(Qt::Key_M)};
        c.aliases = {tr("distance"), tr("ruler"), tr("angle"), tr("inspect"), tr("radius")};
        c.run = [this](bool) { startMeasure(); };
        c.isEnabled = [this] { return !isSketching(); };
        m_commands->add(std::move(c));

        c = make("inspect.checkGeometry", tr("Check &Geometry"), tr("Inspect"),
                 tr("Find what is wrong with the geometry of bodies: open edges, faces that "
                    "cross, faces turned inward"));
        c.iconName = QStringLiteral("check");
        c.aliases = {tr("validate"), tr("verify"), tr("analyze"), tr("check body"),
                     tr("watertight")};
        c.run = [this](bool) { checkGeometry(); };
        c.isEnabled = [this] {
            const auto& bodies = document()->bodies();
            return !isSketching() &&
                   std::any_of(bodies.begin(), bodies.end(),
                               [](const document::Body& body) { return !body.shape.isNull(); });
        };
        m_commands->add(std::move(c));
    }
    const std::tuple<StandardView, QString, const char*> views[] = {
        {StandardView::Home, tr("&Home"), "view.home"},
        {StandardView::Front, tr("F&ront"), "view.front"},
        {StandardView::Back, tr("&Back"), "view.back"},
        {StandardView::Left, tr("&Left"), "view.left"},
        {StandardView::Right, tr("R&ight"), "view.right"},
        {StandardView::Top, tr("&Top"), "view.top"},
        {StandardView::Bottom, tr("B&ottom"), "view.bottom"},
    };
    for (const auto& [standardView, text, id] : views) {
        Command c = make(id, text, view, tr("Turn the camera to this view"));
        c.iconName =
            standardView == StandardView::Home ? QStringLiteral("home") : QStringLiteral("view");
        if (standardView == StandardView::Home) {
            c.shortcuts = {QKeySequence(Qt::Key_Home)};
            c.aliases = {tr("isometric")};
        } else {
            c.aliases = {tr("%1 view").arg(QString(text).remove(QLatin1Char('&')).toLower())};
        }
        c.run = [this, standardView](bool) { m_viewport->setStandardView(standardView); };
        m_commands->add(std::move(c));
    }
    {
        Command c = make("view.perspective", tr("&Perspective"), view,
                         tr("Switch between perspective and orthographic projection"));
        c.iconName = QStringLiteral("perspective");
        c.checkable = true;
        c.aliases = {tr("orthographic"), tr("projection")};
        c.run = [this](bool on) {
            m_viewport->setProjection(on ? viewport::Projection::Perspective
                                         : viewport::Projection::Orthographic);
        };
        c.isChecked = [this] {
            return m_viewport->projection() == viewport::Projection::Perspective;
        };
        m_commands->add(std::move(c));
    }

    // View: how things are drawn
    const std::tuple<DisplayStyle, const char*, QString, const char*> styles[] = {
        {DisplayStyle::ShadedWithEdges, "view.style.shadedEdges", tr("Shaded with &Edges"),
         "style-shaded-edges"},
        {DisplayStyle::Shaded, "view.style.shaded", tr("&Shaded"), "style-shaded"},
        {DisplayStyle::Wireframe, "view.style.wireframe", tr("&Wireframe"), "style-wireframe"},
        {DisplayStyle::HiddenLine, "view.style.hiddenLine", tr("&Hidden Line"),
         "style-hidden-line"},
        {DisplayStyle::Transparent, "view.style.transparent", tr("&Transparent"),
         "style-transparent"},
    };
    for (const auto& [style, id, text, iconName] : styles) {
        Command c = make(id, text, view, tr("Draw bodies this way"));
        c.iconName = QString::fromLatin1(iconName);
        c.checkable = true;
        c.exclusiveGroup = QStringLiteral("displayStyle");
        c.aliases = {tr("display style"), tr("visual style")};
        c.run = [this, style](bool) {
            m_viewport->setDisplayStyle(style);
            QSettings().setValue(QLatin1String(styleKey), static_cast<int>(style));
            // The toolbar's style button shows the icon of the style in use.
            applyTheme();
        };
        c.isChecked = [this, style] { return m_viewport->displayStyle() == style; };
        m_commands->add(std::move(c));
    }
    {
        Command c = make("view.grid", tr("&Grid"), view,
                         tr("Show a reference grid with lines 10 mm apart"));
        c.iconName = QStringLiteral("grid");
        c.checkable = true;
        c.run = [this](bool on) {
            m_viewport->setGridVisible(on);
            QSettings().setValue(QLatin1String(gridKey), on);
        };
        c.isChecked = [this] { return m_viewport->isGridVisible(); };
        m_commands->add(std::move(c));

        c = make("view.axes", tr("&Axis Marker"), view,
                 tr("Show the X, Y, Z marker in the corner of the 3D view"));
        c.iconName = QStringLiteral("axes");
        c.checkable = true;
        c.aliases = {tr("triad"), tr("axes")};
        c.run = [this](bool on) {
            m_viewport->setAxesVisible(on);
            QSettings().setValue(QLatin1String(axesKey), on);
        };
        c.isChecked = [this] { return m_viewport->areAxesVisible(); };
        m_commands->add(std::move(c));
    }

    // View: the mouse
    const std::tuple<NavigationPreset, const char*, QString, QString> presets[] = {
        {NavigationPreset::MiddlePans, "view.navigation.middlePans", tr("Middle Button &Pans"),
         tr("Middle button pans, Shift + middle button orbits")},
        {NavigationPreset::MiddleOrbits, "view.navigation.middleOrbits",
         tr("Middle Button &Orbits"), tr("Middle button orbits, Shift + middle button pans")},
    };
    for (const auto& [preset, id, text, description] : presets) {
        Command c = make(id, text, view, description);
        c.checkable = true;
        c.exclusiveGroup = QStringLiteral("navigation");
        c.aliases = {tr("mouse"), tr("navigation")};
        c.run = [this, preset](bool) {
            m_viewport->setNavigationPreset(preset);
            QSettings().setValue(QLatin1String(navigationKey), static_cast<int>(preset));
        };
        c.isChecked = [this, preset] { return m_viewport->navigationPreset() == preset; };
        m_commands->add(std::move(c));
    }
    {
        Command c = make("view.reverseZoom", tr("Reverse &Zoom Direction"), view,
                         tr("Make rolling the wheel away from you zoom out instead of in"));
        c.checkable = true;
        c.aliases = {tr("wheel"), tr("scroll")};
        c.run = [this](bool on) {
            m_viewport->setZoomReversed(on);
            QSettings().setValue(QLatin1String(reverseZoomKey), on);
        };
        c.isChecked = [this] { return m_viewport->isZoomReversed(); };
        m_commands->add(std::move(c));

        c = make("view.animate", tr("A&nimate View Changes"), view,
                 tr("Turn the camera to a side of the view cube smoothly, not at once"));
        c.checkable = true;
        c.aliases = {tr("reduced motion"), tr("animation")};
        c.run = [this](bool on) {
            m_viewport->setAnimated(on);
            QSettings().setValue(QLatin1String(animateKey), on);
        };
        c.isChecked = [this] { return m_viewport->isAnimated(); };
        m_commands->add(std::move(c));
    }

    // Window
    const std::tuple<ThemeChoice, const char*, QString, QStringList> themes[] = {
        {ThemeChoice::System,
         "window.theme.system",
         tr("Follow the &System"),
         {tr("automatic theme")}},
        {ThemeChoice::Light, "window.theme.light", tr("&Light Theme"), {tr("light mode")}},
        {ThemeChoice::Dark, "window.theme.dark", tr("&Dark Theme"), {tr("dark mode"), tr("night")}},
    };
    for (const auto& [choice, id, text, aliases] : themes) {
        Command c = make(id, text, window, tr("Choose the colors of the interface"));
        c.iconName = QStringLiteral("theme");
        c.checkable = true;
        c.exclusiveGroup = QStringLiteral("theme");
        c.aliases = aliases + QStringList{tr("theme"), tr("appearance")};
        c.run = [choice](bool) { Theme::instance().setChoice(choice); };
        c.isChecked = [choice] { return Theme::instance().choice() == choice; };
        m_commands->add(std::move(c));
    }
    const std::tuple<QDockWidget*, const char*, QString> panels[] = {
        {m_browserDock, "window.browser", tr("&Browser")},
        {m_inspectorDock, "window.properties", tr("&Properties")},
        {m_timelineDock, "window.timeline", tr("&Timeline")},
    };
    for (const auto& [dock, id, text] : panels) {
        Command c = make(id, text, window, tr("Show or hide this panel"));
        c.checkable = true;
        c.aliases = {tr("panel")};
        c.run = [dock](bool on) { dock->setVisible(on); };
        c.isChecked = [dock] { return !dock->isHidden(); };
        m_commands->add(std::move(c));
    }
    {
        Command c = make("window.focus", tr("&Focus on 3D View"), window,
                         tr("Hide the panels and give the window to the 3D view"));
        c.iconName = QStringLiteral("focus");
        c.checkable = true;
        c.shortcuts = {QKeySequence(Qt::Key_F11)};
        c.aliases = {tr("full screen"), tr("hide panels"), tr("zen")};
        c.run = [this](bool on) { setViewportFocus(on); };
        c.isChecked = [this] { return isViewportFocused(); };
        m_commands->add(std::move(c));

        c = make("window.resetLayout", tr("&Reset Layout"), window,
                 tr("Put the panels back where they started"));
        c.iconName = QStringLiteral("layout");
        c.aliases = {tr("default layout"), tr("restore panels")};
        c.run = [this](bool) { resetLayout(); };
        m_commands->add(std::move(c));

        c = make("window.search", tr("&Search Commands…"), window,
                 tr("Find a command by typing part of its name"));
        c.iconName = QStringLiteral("search");
        c.shortcuts = {QKeySequence(Qt::Key_S), QKeySequence(Qt::CTRL | Qt::Key_K)};
        c.aliases = {tr("find"), tr("command palette")};
        c.remembered = false;
        c.run = [this](bool) { m_search->open(); };
        m_commands->add(std::move(c));

        c = make("window.shortcuts", tr("&Keyboard Shortcuts…"), window,
                 tr("See and change the keyboard shortcuts"));
        c.iconName = QStringLiteral("keyboard");
        c.aliases = {tr("hotkeys"), tr("key bindings")};
        c.run = [this](bool) { showShortcuts(); };
        m_commands->add(std::move(c));

        c = make("help.about", tr("&About LCAD"), help,
                 tr("Versions, license, graphics driver, and where the log is"));
        c.iconName = QStringLiteral("info");
        c.aliases = {tr("version"), tr("license")};
        c.run = [this](bool) { showAbout(); };
        m_commands->add(std::move(c));
    }
}

void MainWindow::defineSketchCommands() {
    const QString group = tr("Sketch");
    const auto make = [&group](const char* id, const QString& text, const QString& description,
                               const char* iconName) {
        Command command;
        command.id = QString::fromLatin1(id);
        command.text = text;
        command.group = group;
        command.description = description;
        command.iconName = QString::fromLatin1(iconName);
        return command;
    };
    const auto editor = [this] { return m_viewport->sketchEditor(); };
    const auto sketching = [this] { return isSketching(); };

    // Starting and finishing
    {
        Command c = make("sketch.new", tr("New &Sketch"),
                         tr("Start a sketch on a plane or on a flat face"), "sketch");
        c.aliases = {tr("create sketch"), tr("draw"), tr("2d")};
        c.run = [this](bool) { startSketch(); };
        c.isEnabled = [this] { return !isSketching(); };
        m_commands->add(std::move(c));

        const std::tuple<const char*, QString, int> planes[] = {
            {"sketch.new.xy", tr("Sketch on the &XY Plane"), 0},
            {"sketch.new.xz", tr("Sketch on the X&Z Plane"), 1},
            {"sketch.new.yz", tr("Sketch on the &YZ Plane"), 2},
        };
        for (const auto& [id, text, which] : planes) {
            c = make(id, text, tr("Start a sketch on this plane through the origin"), "sketch");
            c.aliases = {tr("create sketch")};
            c.run = [this, which](bool) {
                createSketch(which == 0   ? kernel::planeXY()
                             : which == 1 ? kernel::planeXZ()
                                          : kernel::planeYZ());
            };
            c.isEnabled = [this] { return !isSketching(); };
            m_commands->add(std::move(c));
        }

        c = make("sketch.edit", tr("&Edit Sketch"), tr("Open the selected sketch for editing"),
                 "sketch");
        c.aliases = {tr("open sketch")};
        c.run = [this](bool) {
            const auto sketches = selectedSketches();
            if (sketches.size() == 1) {
                editSketch(sketches.front());
            }
        };
        c.isEnabled = [this] { return !isSketching() && selectedSketches().size() == 1; };
        m_commands->add(std::move(c));

        c = make("sketch.finish", tr("&Finish Sketch"),
                 tr("Close the sketch and go back to the model"), "finish");
        c.shortcuts = {QKeySequence(Qt::CTRL | Qt::Key_Return)};
        c.aliases = {tr("done"), tr("exit sketch"), tr("stop sketch"), tr("close sketch")};
        c.run = [this](bool) { finishSketch(); };
        c.isEnabled = sketching;
        m_commands->add(std::move(c));
    }

    // Tools
    struct ToolEntry {
        SketchTool tool;
        const char* id;
        QString text;
        QString description;
        const char* icon;
        QKeySequence shortcut;
        QStringList aliases;
    };
    const ToolEntry tools[] = {
        {SketchTool::Select,
         "sketch.select",
         tr("&Select"),
         tr("Select sketch geometry, and drag what can still move"),
         "select",
         {},
         {tr("pick")}},
        {SketchTool::Line,
         "sketch.line",
         tr("&Line"),
         tr("Draw lines, one after another; a typed number sets the length"),
         "line",
         QKeySequence(Qt::Key_L),
         {tr("segment"), tr("polyline")}},
        {SketchTool::Rectangle,
         "sketch.rectangle",
         tr("&Rectangle"),
         tr("Draw a rectangle by two opposite corners"),
         "rectangle",
         QKeySequence(Qt::Key_R),
         {tr("box"), tr("square")}},
        {SketchTool::Circle,
         "sketch.circle",
         tr("&Circle"),
         tr("Draw a circle by its center and a point on it"),
         "circle",
         QKeySequence(Qt::Key_C),
         {tr("round"), tr("hole")}},
        {SketchTool::Arc,
         "sketch.arc",
         tr("&Arc"),
         tr("Draw an arc by its center, its start, and its end"),
         "arc",
         QKeySequence(Qt::Key_A),
         {tr("curve")}},
        {SketchTool::Point,
         "sketch.point",
         tr("&Point"),
         tr("Put a point in the sketch"),
         "point",
         {},
         {tr("dot")}},
        {SketchTool::Polygon,
         "sketch.polygon",
         tr("Pol&ygon"),
         tr("Draw a regular polygon by its center and a corner"),
         "polygon",
         {},
         {tr("hexagon"), tr("triangle"), tr("pentagon")}},
        {SketchTool::Slot,
         "sketch.slot",
         tr("Sl&ot"),
         tr("Draw a slot by its two centers and its width"),
         "slot",
         {},
         {tr("oblong")}},
        {SketchTool::Ellipse,
         "sketch.ellipse",
         tr("&Ellipse"),
         tr("Draw an ellipse by its center, the end of its major axis, and its minor radius"),
         "ellipse",
         {},
         {tr("oval")}},
        {SketchTool::Spline,
         "sketch.spline",
         tr("Spli&ne"),
         tr("Draw a smooth curve through its first and last points, toward the others"),
         "spline",
         {},
         {tr("curve"), tr("freeform"), tr("bezier")}},
        {SketchTool::Trim,
         "sketch.trim",
         tr("&Trim"),
         tr("Remove the part of a curve between the curves that cross it"),
         "trim",
         QKeySequence(Qt::Key_T),
         {tr("cut")}},
        {SketchTool::Extend,
         "sketch.extend",
         tr("E&xtend"),
         tr("Lengthen a line or an arc to the next curve it meets"),
         "extend",
         {},
         {tr("lengthen")}},
        {SketchTool::Offset,
         "sketch.offset",
         tr("&Offset"),
         tr("Copy an outline at a distance"),
         "offset",
         QKeySequence(Qt::Key_O),
         {tr("parallel copy")}},
        {SketchTool::Mirror,
         "sketch.mirror",
         tr("&Mirror"),
         tr("Mirror the selected geometry in a line"),
         "mirror",
         {},
         {tr("reflect"), tr("flip")}},
        {SketchTool::Project,
         "sketch.project",
         tr("Pro&ject"),
         tr("Bring an edge or a corner of a body into the sketch as reference geometry"),
         "project",
         QKeySequence(Qt::Key_P),
         {tr("reference"), tr("include"), tr("use edge")}},
    };
    for (const ToolEntry& entry : tools) {
        Command c = make(entry.id, entry.text, entry.description, entry.icon);
        c.checkable = true;
        c.exclusiveGroup = QStringLiteral("sketchTool");
        if (!entry.shortcut.isEmpty()) {
            c.shortcuts = {entry.shortcut};
        }
        c.aliases = entry.aliases;
        c.run = [editor, tool = entry.tool](bool) { editor()->setTool(tool); };
        c.isEnabled = sketching;
        c.isChecked = [this, editor, tool = entry.tool] {
            return isSketching() && editor()->tool() == tool;
        };
        m_commands->add(std::move(c));
    }
    {
        Command c =
            make("sketch.dimension", tr("&Dimension"),
                 tr("Give a length, a distance, a radius, or an angle its value"), "dimension");
        c.checkable = true;
        c.exclusiveGroup = QStringLiteral("sketchTool");
        c.shortcuts = {QKeySequence(Qt::Key_D)};
        c.aliases = {tr("measure"),  tr("size"),   tr("length"),
                     tr("distance"), tr("radius"), tr("angle")};
        // On what is selected, or else on what is clicked next.
        c.run = [editor](bool) { editor()->addDimension(); };
        c.isEnabled = sketching;
        c.isChecked = [this, editor] {
            return isSketching() && editor()->tool() == SketchTool::Dimension;
        };
        m_commands->add(std::move(c));

        c = make("sketch.construction", tr("Co&nstruction"),
                 tr("Turn the selected curves into construction geometry and back, or, with "
                    "nothing selected, draw construction geometry"),
                 "construction");
        c.checkable = true;
        c.shortcuts = {QKeySequence(Qt::Key_X)};
        c.aliases = {tr("reference line"), tr("helper"), tr("guide")};
        c.run = [editor](bool) { editor()->toggleConstruction(); };
        c.isEnabled = sketching;
        c.isChecked = [this, editor] { return isSketching() && editor()->constructionMode(); };
        m_commands->add(std::move(c));

        c = make("sketch.inference", tr("Pick Up Constraints &While Drawing"),
                 tr("Keep ends on the points and curves they land on, and make nearly level or "
                    "upright lines exactly so"),
                 "inference");
        c.checkable = true;
        c.aliases = {tr("snap"), tr("inference"), tr("automatic constraints")};
        c.run = [editor](bool on) { editor()->setInference(on); };
        c.isEnabled = sketching;
        c.isChecked = [editor] { return editor()->hasInference(); };
        m_commands->add(std::move(c));
    }

    // Constraints
    using Kind = sketch::ConstraintKind;
    const std::tuple<Kind, const char*, QString, QString, const char*> constraints[] = {
        {Kind::Coincident, "sketch.coincident", tr("Coi&ncident"),
         tr("Join two points, or put a point on a line or a circle"), "c-coincident"},
        {Kind::Horizontal, "sketch.horizontal", tr("&Horizontal"),
         tr("Make a line level, or put two points at the same height"), "c-horizontal"},
        {Kind::Vertical, "sketch.vertical", tr("&Vertical"),
         tr("Make a line upright, or put one point straight above another"), "c-vertical"},
        {Kind::Parallel, "sketch.parallel", tr("Pa&rallel"), tr("Make two lines parallel"),
         "c-parallel"},
        {Kind::Perpendicular, "sketch.perpendicular", tr("Per&pendicular"),
         tr("Make two lines square to each other"), "c-perpendicular"},
        {Kind::Tangent, "sketch.tangent", tr("Tan&gent"),
         tr("Make a line touch a circle or an arc, or two of those touch each other"), "c-tangent"},
        {Kind::Equal, "sketch.equal", tr("E&qual"),
         tr("Make two lines the same length, or two circles or arcs the same radius"), "c-equal"},
        {Kind::Concentric, "sketch.concentric", tr("Conc&entric"),
         tr("Give two circles or arcs the same center"), "c-concentric"},
        {Kind::Midpoint, "sketch.midpoint", tr("Mi&dpoint"),
         tr("Put a point on the middle of a line"), "c-midpoint"},
        {Kind::Symmetric, "sketch.symmetric", tr("S&ymmetric"),
         tr("Make two points mirror images of each other in a line"), "c-symmetric"},
        {Kind::Collinear, "sketch.collinear", tr("Co&llinear"),
         tr("Put two lines on one straight line"), "c-collinear"},
        {Kind::Fix, "sketch.fix", tr("Fi&x"), tr("Keep the selected points where they are"),
         "c-fix"},
    };
    for (const auto& [kind, id, text, description, iconName] : constraints) {
        Command c = make(id, text, description, iconName);
        c.checkable = true;
        c.aliases = {tr("constraint"), tr("constrain")};
        // On what is selected, if it fits; otherwise it becomes the tool and waits for clicks.
        c.run = [editor, kind](bool) { editor()->constrain(kind); };
        c.isEnabled = sketching;
        c.isChecked = [this, editor, kind] {
            return isSketching() && editor()->tool() == SketchTool::Constraint &&
                   editor()->constraintTool() == kind;
        };
        m_commands->add(std::move(c));
    }
}

// ---------------------------------------------------------------------------------------------
// The window's parts

void MainWindow::createTopBar() {
    const auto action = [this](const char* id) { return m_commands->action(QLatin1String(id)); };

    auto* topBar = new QWidget(this);
    topBar->setObjectName(QStringLiteral("topBar"));
    auto* layout = new QHBoxLayout(topBar);
    layout->setContentsMargins(2, 0, 6, 0);
    layout->setSpacing(2);

    m_menuBar = new QMenuBar(topBar);
    m_menuBar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    layout->addWidget(m_menuBar);

    QMenu* file = m_menuBar->addMenu(tr("&File"));
    file->addActions({action("file.new"), action("file.open")});
    m_recentMenu = file->addMenu(tr("Open &Recent"));
    file->addSeparator();
    file->addActions({action("file.save"), action("file.saveAs")});
    file->addSeparator();
    file->addActions({action("file.import"), action("file.export")});
    file->addSeparator();
    file->addActions({action("file.links"), action("file.updateLinks"), action("file.package")});
    file->addSeparator();
    file->addActions({action("file.close"), action("file.quit")});

    QMenu* edit = m_menuBar->addMenu(tr("&Edit"));
    edit->addActions({action("edit.undo"), action("edit.redo")});
    edit->addSeparator();
    edit->addActions({action("body.rename"), action("body.hide"), action("body.show"),
                      action("body.isolate"), action("body.showAll")});
    edit->addSeparator();
    edit->addActions({action("edit.delete"), action("edit.cancel")});
    // What a click picks: also the box in the toolbar. A menu of its own until Phase 5, when
    // the menu bar needed its room for Assemble.
    edit->addSeparator();
    QMenu* select = edit->addMenu(tr("&Select"));
    select->setObjectName(QStringLiteral("menu.select"));
    for (const char* id : {"select.everything", "select.bodies", "select.faces", "select.edges",
                           "select.vertices"}) {
        select->addAction(action(id));
    }

    QMenu* create = m_menuBar->addMenu(tr("&Create"));
    create->addActions({action("solid.pull"), action("solid.revolve"), action("solid.hole")});
    QMenu* pattern = create->addMenu(tr("&Pattern"));
    pattern->setObjectName(QStringLiteral("menu.pattern"));
    pattern->addActions({action("solid.patternRectangular"), action("solid.patternCircular")});
    create->addAction(action("solid.mirror"));
    create->addSeparator();
    create->addActions({action("construct.offsetPlane"), action("construct.axis")});
    create->addSeparator();
    for (const char* id :
         {"create.box", "create.cylinder", "create.sphere", "create.cone", "create.torus"}) {
        create->addAction(action(id));
    }

    QMenu* modifyMenu = m_menuBar->addMenu(tr("&Modify"));
    modifyMenu->addActions(
        {action("solid.fillet"), action("solid.chamfer"), action("solid.shell")});
    modifyMenu->addSeparator();
    modifyMenu->addActions({action("solid.offsetFace"), action("solid.moveFace"),
                            action("solid.deleteFace"), action("solid.repair")});
    modifyMenu->addSeparator();
    modifyMenu->addAction(action("model.parameters"));
    QMenu* timelineMenu = modifyMenu->addMenu(tr("&Timeline"));
    timelineMenu->addActions(
        {action("timeline.edit"), action("timeline.rename"), action("timeline.suppress")});
    timelineMenu->addSeparator();
    timelineMenu->addActions({action("timeline.rollBack"), action("timeline.rollToEnd")});

    QMenu* assembleMenu = m_menuBar->addMenu(tr("&Assemble"));
    assembleMenu->setObjectName(QStringLiteral("menu.assemble"));
    assembleMenu->addActions({action("assembly.newComponent"), action("assembly.makeComponent"),
                              action("assembly.insert")});
    assembleMenu->addSeparator();
    assembleMenu->addActions(
        {action("assembly.joint"), action("assembly.editJoint"), action("assembly.animate")});
    assembleMenu->addSeparator();
    assembleMenu->addActions({action("assembly.move"), action("assembly.ground"),
                              action("assembly.duplicate"), action("assembly.copy"),
                              action("assembly.suppress")});
    assembleMenu->addSeparator();
    assembleMenu->addActions({action("assembly.activate"), action("assembly.activateRoot")});
    assembleMenu->addSeparator();
    assembleMenu->addActions({action("assembly.openLinked"), action("assembly.relink")});

    QMenu* sketchMenu = m_menuBar->addMenu(tr("S&ketch"));
    sketchMenu->addAction(action("sketch.new"));
    QMenu* onPlane = sketchMenu->addMenu(tr("New Sketch &On"));
    onPlane->addActions(
        {action("sketch.new.xy"), action("sketch.new.xz"), action("sketch.new.yz")});
    sketchMenu->addActions({action("sketch.edit"), action("sketch.finish")});
    sketchMenu->addSeparator();
    for (const char* id :
         {"sketch.select", "sketch.line", "sketch.rectangle", "sketch.circle", "sketch.arc",
          "sketch.point", "sketch.polygon", "sketch.slot", "sketch.ellipse", "sketch.spline"}) {
        sketchMenu->addAction(action(id));
    }
    sketchMenu->addSeparator();
    for (const char* id :
         {"sketch.trim", "sketch.extend", "sketch.offset", "sketch.mirror", "sketch.project"}) {
        sketchMenu->addAction(action(id));
    }
    sketchMenu->addSeparator();
    sketchMenu->addAction(action("sketch.dimension"));
    QMenu* constrain = sketchMenu->addMenu(tr("&Constrain"));
    constrain->setObjectName(QStringLiteral("menu.constrain"));
    for (const char* id :
         {"sketch.coincident", "sketch.horizontal", "sketch.vertical", "sketch.parallel",
          "sketch.perpendicular", "sketch.tangent", "sketch.equal", "sketch.concentric",
          "sketch.midpoint", "sketch.symmetric", "sketch.collinear", "sketch.fix"}) {
        constrain->addAction(action(id));
    }
    sketchMenu->addSeparator();
    sketchMenu->addActions({action("sketch.construction"), action("sketch.inference")});

    QMenu* view = m_menuBar->addMenu(tr("&View"));
    view->addActions({action("view.fit"), action("view.fitSelection")});
    view->addActions({action("inspect.measure"), action("inspect.checkGeometry"),
                      action("inspect.interference"), action("inspect.physical"),
                      action("inspect.materials")});
    view->addSeparator();
    for (const char* id : {"view.home", "view.front", "view.back", "view.left", "view.right",
                           "view.top", "view.bottom"}) {
        view->addAction(action(id));
    }
    view->addSeparator();
    view->addAction(action("view.perspective"));
    QMenu* style = view->addMenu(tr("&Display Style"));
    style->setObjectName(QStringLiteral("menu.displayStyle"));
    for (const char* id : {"view.style.shadedEdges", "view.style.shaded", "view.style.wireframe",
                           "view.style.hiddenLine", "view.style.transparent"}) {
        style->addAction(action(id));
    }
    view->addActions({action("view.grid"), action("view.axes")});
    view->addSeparator();
    QMenu* mouse = view->addMenu(tr("&Mouse"));
    mouse->addActions(
        {action("view.navigation.middlePans"), action("view.navigation.middleOrbits")});
    mouse->addSeparator();
    mouse->addAction(action("view.reverseZoom"));
    view->addAction(action("view.animate"));

    QMenu* window = m_menuBar->addMenu(tr("&Window"));
    window->addActions({action("window.search"), action("window.shortcuts")});
    window->addSeparator();
    QMenu* theme = window->addMenu(tr("&Theme"));
    theme->addActions(
        {action("window.theme.system"), action("window.theme.light"), action("window.theme.dark")});
    window->addSeparator();
    window->addActions(
        {action("window.browser"), action("window.properties"), action("window.timeline")});
    window->addSeparator();
    window->addActions({action("window.focus"), action("window.resetLayout")});

    QMenu* helpMenu = m_menuBar->addMenu(tr("&Help"));
    helpMenu->addAction(action("help.about"));

    layout->addSpacing(8);
    for (const char* id : {"file.new", "file.open", "file.save", "edit.undo", "edit.redo"}) {
        auto* button = new QToolButton(topBar);
        button->setDefaultAction(action(id));
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        layout->addWidget(button);
        if (QLatin1String(id) == QLatin1String("file.save")) {
            layout->addSpacing(6);
        }
    }
    layout->addSpacing(10);

    // One tab per open document.
    m_tabs = new QTabBar(topBar);
    m_tabs->setObjectName(QStringLiteral("documentTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);
    m_tabs->setElideMode(Qt::ElideMiddle);
    m_tabs->setUsesScrollButtons(true);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index != m_current) {
            setCurrentIndex(index);
        }
    });
    layout->addWidget(m_tabs, 1);

    auto* search = new QToolButton(topBar);
    search->setObjectName(QStringLiteral("searchButton"));
    search->setDefaultAction(action("window.search"));
    search->setAutoRaise(true);
    search->setToolButtonStyle(Qt::ToolButtonIconOnly);
    layout->addWidget(search);

    setMenuWidget(topBar);
}

void MainWindow::createToolbar() {
    const auto action = [this](const char* id) { return m_commands->action(QLatin1String(id)); };

    QToolBar* toolbar = addToolBar(tr("Modeling"));
    m_modelToolbar = toolbar;
    toolbar->setObjectName(QStringLiteral("modelingToolbar"));
    toolbar->setProperty("tools", true);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->setContextMenuPolicy(Qt::PreventContextMenu);
    const int iconSize = Theme::instance().tokens().iconSize;
    toolbar->setIconSize(QSize(iconSize, iconSize));

    // A sketch is where most solids start, so it leads the group.
    m_modelGroupLabels.append(toolbar->addWidget(groupLabel(tr("Create"), toolbar)));
    for (const char* id : {"sketch.new", "solid.pull", "solid.revolve"}) {
        toolbar->addAction(action(id));
    }
    auto* patterns = new QToolButton(toolbar);
    patterns->setObjectName(QStringLiteral("patternButton"));
    patterns->setToolTip(tr("Pattern and mirror"));
    patterns->setPopupMode(QToolButton::InstantPopup);
    patterns->setToolButtonStyle(Qt::ToolButtonIconOnly);
    auto* patternMenu = new QMenu(patterns);
    patternMenu->addActions({action("solid.patternRectangular"), action("solid.patternCircular"),
                             action("solid.mirror")});
    patterns->setMenu(patternMenu);
    patterns->setIcon(icon(QStringLiteral("pattern-rectangular")));
    m_patternButton = patterns;
    toolbar->addWidget(patterns);
    for (const char* id :
         {"create.box", "create.cylinder", "create.sphere", "create.cone", "create.torus"}) {
        toolbar->addAction(action(id));
    }
    toolbar->addSeparator();

    m_modelGroupLabels.append(toolbar->addWidget(groupLabel(tr("Modify"), toolbar)));
    for (const char* id : {"solid.hole", "solid.fillet", "solid.chamfer", "solid.shell"}) {
        toolbar->addAction(action(id));
    }
    toolbar->addSeparator();

    // Components, and what joins them; the rest of the assembly tools in a menu beside them.
    m_modelGroupLabels.append(toolbar->addWidget(groupLabel(tr("Assemble"), toolbar)));
    // Buttons of their own, so that a narrow window can put them away without hiding the
    // commands from the menus: see fitToolbar().
    for (const char* id : {"assembly.newComponent", "assembly.joint"}) {
        auto* button = new QToolButton(toolbar);
        button->setObjectName(QLatin1String(id));
        button->setDefaultAction(action(id));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_assemblyButtons.append(toolbar->addWidget(button));
    }
    auto* assembly = new QToolButton(toolbar);
    assembly->setObjectName(QStringLiteral("assemblyButton"));
    assembly->setToolTip(tr("More assembly tools"));
    assembly->setPopupMode(QToolButton::InstantPopup);
    assembly->setToolButtonStyle(Qt::ToolButtonIconOnly);
    assembly->setIcon(icon(QStringLiteral("insert-part")));
    auto* assemblyMenu = new QMenu(assembly);
    // New Component and Joint lead the list, for when the window is too narrow for their own
    // buttons.
    assemblyMenu->addActions({action("assembly.newComponent"), action("assembly.joint")});
    assemblyMenu->addSeparator();
    assemblyMenu->addActions({action("assembly.insert"), action("assembly.makeComponent"),
                              action("assembly.move"), action("assembly.ground")});
    assemblyMenu->addSeparator();
    assemblyMenu->addActions({action("inspect.interference"), action("inspect.physical")});
    assembly->setMenu(assemblyMenu);
    toolbar->addWidget(assembly);
    toolbar->addSeparator();

    m_modelGroupLabels.append(toolbar->addWidget(groupLabel(tr("Select"), toolbar)));
    m_filterBox = new QComboBox(toolbar);
    m_filterBox->setObjectName(QStringLiteral("selectionFilter"));
    m_filterBox->setToolTip(tr("What a click in the 3D view picks"));
    for (const char* id : {"select.everything", "select.bodies", "select.faces", "select.edges",
                           "select.vertices"}) {
        m_filterBox->addItem(m_commands->displayName(QLatin1String(id)), QLatin1String(id));
    }
    connect(m_filterBox, &QComboBox::activated, this,
            [this](int index) { m_commands->trigger(m_filterBox->itemData(index).toString()); });
    toolbar->addWidget(m_filterBox);
    toolbar->addSeparator();

    m_modelGroupLabels.append(toolbar->addWidget(groupLabel(tr("View"), toolbar)));
    for (const char* id : {"inspect.measure", "view.fit", "view.fitSelection", "view.grid"}) {
        toolbar->addAction(action(id));
        auto* button = qobject_cast<QToolButton*>(toolbar->widgetForAction(action(id)));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
    m_styleButton = new QToolButton(toolbar);
    m_styleButton->setObjectName(QStringLiteral("displayStyleButton"));
    m_styleButton->setToolTip(tr("How bodies are drawn"));
    m_styleButton->setPopupMode(QToolButton::InstantPopup);
    m_styleButton->setMenu(m_menuBar->findChild<QMenu*>(QStringLiteral("menu.displayStyle")));
    toolbar->addWidget(m_styleButton);
}

// The toolbar that takes the modeling toolbar's place while a sketch is open: the drawing,
// modifying, and constraining tools, each in its group, and the way out on the right.
void MainWindow::createSketchToolbar() {
    const auto action = [this](const char* id) { return m_commands->action(QLatin1String(id)); };

    QToolBar* toolbar = addToolBar(tr("Sketching"));
    m_sketchToolbar = toolbar;
    toolbar->setObjectName(QStringLiteral("sketchToolbar"));
    toolbar->setProperty("tools", true);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setContextMenuPolicy(Qt::PreventContextMenu);
    const int iconSize = Theme::instance().tokens().iconSize;
    toolbar->setIconSize(QSize(iconSize, iconSize));

    m_sketchGroupLabels.append(toolbar->addWidget(groupLabel(tr("Draw"), toolbar)));
    for (const char* id :
         {"sketch.select", "sketch.line", "sketch.rectangle", "sketch.circle", "sketch.arc",
          "sketch.point", "sketch.polygon", "sketch.slot", "sketch.ellipse", "sketch.spline"}) {
        toolbar->addAction(action(id));
    }
    toolbar->addAction(action("sketch.construction"));
    toolbar->addSeparator();
    m_sketchGroupLabels.append(toolbar->addWidget(groupLabel(tr("Modify"), toolbar)));
    for (const char* id :
         {"sketch.trim", "sketch.extend", "sketch.offset", "sketch.mirror", "sketch.project"}) {
        toolbar->addAction(action(id));
    }
    toolbar->addSeparator();
    m_sketchGroupLabels.append(toolbar->addWidget(groupLabel(tr("Constrain"), toolbar)));
    toolbar->addAction(action("sketch.dimension"));
    // Each constraint has a button of its own while there is room. In a narrow window they
    // fold into one button that opens the list: see fitToolbar().
    for (const char* id :
         {"sketch.coincident", "sketch.horizontal", "sketch.vertical", "sketch.parallel",
          "sketch.perpendicular", "sketch.tangent", "sketch.equal", "sketch.concentric",
          "sketch.midpoint", "sketch.symmetric", "sketch.collinear", "sketch.fix"}) {
        auto* button = new QToolButton(toolbar);
        button->setDefaultAction(action(id));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        m_constraintButtons.append(toolbar->addWidget(button));
    }
    auto* constraints = new QToolButton(toolbar);
    constraints->setObjectName(QStringLiteral("constraintsButton"));
    constraints->setText(tr("Constraints"));
    constraints->setToolTip(tr("Geometric constraints"));
    constraints->setPopupMode(QToolButton::InstantPopup);
    constraints->setToolButtonStyle(Qt::ToolButtonTextOnly);
    constraints->setMenu(m_menuBar->findChild<QMenu*>(QStringLiteral("menu.constrain")));
    m_constraintsFolded = toolbar->addWidget(constraints);
    m_constraintsFolded->setVisible(false);

    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    auto* finish = new QToolButton(toolbar);
    finish->setObjectName(QStringLiteral("finishSketch"));
    finish->setDefaultAction(action("sketch.finish"));
    finish->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_finishButton = finish;
    toolbar->addWidget(finish);
    toolbar->hide();
}

void MainWindow::updateMode() {
    const bool sketching = isSketching();
    const bool focused = isViewportFocused();
    m_modelToolbar->setVisible(!sketching && !focused);
    m_sketchToolbar->setVisible(sketching && !focused);
}

// A toolbar that does not fit hides what is left over behind an arrow. Before it comes to
// that, the primitives give up their labels: their icons and tooltips still say what they are.
void MainWindow::fitToolbar() {
    if (m_modelToolbar == nullptr) {
        return;
    }
    // How wide a toolbar wants to be now. Its layout keeps the answer it last gave until told
    // that what is in it has changed.
    const auto wanted = [](QToolBar* toolbar) {
        toolbar->layout()->invalidate();
        return toolbar->sizeHint().width();
    };
    const auto setStyle = [this](Qt::ToolButtonStyle style) {
        for (const char* id :
             {"solid.revolve", "create.box", "create.cylinder", "create.sphere", "create.cone",
              "create.torus", "solid.hole", "solid.chamfer", "solid.shell"}) {
            if (auto* button = qobject_cast<QToolButton*>(
                    m_modelToolbar->widgetForAction(m_commands->action(QLatin1String(id))))) {
                button->setToolButtonStyle(style);
            }
        }
    };
    setStyle(Qt::ToolButtonTextBesideIcon);
    const auto setAssembly = [this](Qt::ToolButtonStyle style) {
        for (QAction* entry : std::as_const(m_assemblyButtons)) {
            if (auto* button = qobject_cast<QToolButton*>(m_modelToolbar->widgetForAction(entry))) {
                button->setToolButtonStyle(style);
            }
        }
    };
    setAssembly(Qt::ToolButtonTextBesideIcon);
    const auto setLeading = [this](Qt::ToolButtonStyle style) {
        for (const char* id : {"sketch.new", "solid.pull", "solid.fillet"}) {
            if (auto* button = qobject_cast<QToolButton*>(
                    m_modelToolbar->widgetForAction(m_commands->action(QLatin1String(id))))) {
                button->setToolButtonStyle(style);
            }
        }
    };
    setLeading(Qt::ToolButtonTextBesideIcon);
    if (wanted(m_modelToolbar) > width()) {
        setStyle(Qt::ToolButtonIconOnly);
        setAssembly(Qt::ToolButtonIconOnly);
    }
    // Then New Component and Joint give their buttons up for the list beside them, which
    // lists them first.
    const auto foldAssembly = [this](bool folded) {
        for (QAction* button : std::as_const(m_assemblyButtons)) {
            button->setVisible(!folded);
        }
    };
    foldAssembly(false);
    if (wanted(m_modelToolbar) > width()) {
        foldAssembly(true);
    }
    // The commands most used keep their labels longest.
    if (wanted(m_modelToolbar) > width()) {
        setLeading(Qt::ToolButtonIconOnly);
    }
    // Last, the names of the groups go; the separators still divide them.
    for (QAction* label : std::as_const(m_modelGroupLabels)) {
        label->setVisible(true);
    }
    if (wanted(m_modelToolbar) > width()) {
        for (QAction* label : std::as_const(m_modelGroupLabels)) {
            label->setVisible(false);
        }
    }

    // The sketch toolbar gives up the constraints' own buttons for one that lists them; then
    // the names of its groups; and last, Finish Sketch its label, which the panel's button
    // still has.
    if (m_sketchToolbar == nullptr || m_constraintsFolded == nullptr) {
        return;
    }
    const auto fold = [this](bool folded) {
        for (QAction* button : std::as_const(m_constraintButtons)) {
            button->setVisible(!folded);
        }
        m_constraintsFolded->setVisible(folded);
    };
    fold(false);
    for (QAction* label : std::as_const(m_sketchGroupLabels)) {
        label->setVisible(true);
    }
    m_finishButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    if (wanted(m_sketchToolbar) > width()) {
        fold(true);
    }
    if (wanted(m_sketchToolbar) > width()) {
        for (QAction* label : std::as_const(m_sketchGroupLabels)) {
            label->setVisible(false);
        }
    }
    if (wanted(m_sketchToolbar) > width()) {
        m_finishButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    fitToolbar();
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    // The toolbar's buttons know their sizes only once the style sheet has been applied to
    // them, which happens as they are shown.
    QMetaObject::invokeMethod(this, &MainWindow::fitToolbar, Qt::QueuedConnection);
}

void MainWindow::createDocks() {
    m_browser = new BrowserPanel(this);
    m_browserDock = new QDockWidget(tr("Browser"), this);
    m_browserDock->setObjectName(QStringLiteral("browserDock"));
    m_browserDock->setWidget(m_browser);
    addDockWidget(Qt::LeftDockWidgetArea, m_browserDock);

    // A strip, not a panel with a title bar of its own: it has to leave the height to the 3D
    // view.
    m_timeline = new TimelinePanel(this);
    m_timeline->setMinimumHeight(34);
    m_timelineDock = new QDockWidget(tr("Timeline"), this);
    m_timelineDock->setObjectName(QStringLiteral("timelineDock"));
    m_timelineDock->setTitleBarWidget(new QWidget(m_timelineDock));
    m_timelineDock->setWidget(m_timeline);
    addDockWidget(Qt::BottomDockWidgetArea, m_timelineDock);

    // The right-hand panel holds the command in progress, or else what is selected.
    m_properties = new PropertiesPanel(this);
    auto* stack = new QStackedWidget(this);
    stack->setObjectName(QStringLiteral("inspectorStack"));
    stack->addWidget(m_properties);
    m_sketchPanel = new SketchPanel(m_viewport->sketchEditor(), this);
    stack->addWidget(m_sketchPanel);
    connect(m_sketchPanel, &SketchPanel::finishRequested, this, &MainWindow::finishSketch);
    // In a scroll area, so that what the panel shows never changes how wide the panel is: the
    // 3D view must not jump sideways each time something else is selected.
    auto* scroll = new QScrollArea(this);
    scroll->setWidget(stack);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_inspectorStack = stack;
    m_inspectorDock = new QDockWidget(tr("Properties"), this);
    m_inspectorDock->setObjectName(QStringLiteral("propertiesDock"));
    m_inspectorDock->setWidget(scroll);
    m_inspectorDock->setMinimumWidth(240);
    addDockWidget(Qt::RightDockWidgetArea, m_inspectorDock);
    // Narrow side panels and a thin timeline, so that the 3D view has most of the window.
    resizeDocks({m_browserDock, m_inspectorDock}, {210, 240}, Qt::Horizontal);
    resizeDocks({m_timelineDock}, {40}, Qt::Vertical);

    // The browser and the properties run the full height of the window; the timeline sits
    // under the viewport.
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
}

void MainWindow::restoreSettings() {
    const QSettings settings;
    m_viewport->setZoomReversed(settings.value(QLatin1String(reverseZoomKey), false).toBool());
    m_viewport->setGridVisible(settings.value(QLatin1String(gridKey), true).toBool());
    m_viewport->setAxesVisible(settings.value(QLatin1String(axesKey), true).toBool());
    m_viewport->setAnimated(settings.value(QLatin1String(animateKey), true).toBool());
    m_viewport->setNavigationPreset(settings.value(QLatin1String(navigationKey)).toInt() ==
                                            static_cast<int>(NavigationPreset::MiddleOrbits)
                                        ? NavigationPreset::MiddleOrbits
                                        : NavigationPreset::MiddlePans);
    const int style = settings.value(QLatin1String(styleKey), 0).toInt();
    m_viewport->setDisplayStyle(style >= 0 && style <= static_cast<int>(DisplayStyle::Transparent)
                                    ? static_cast<DisplayStyle>(style)
                                    : DisplayStyle::ShadedWithEdges);

    const QByteArray geometry = settings.value(QLatin1String(geometryKey)).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    const QByteArray layout = settings.value(QLatin1String(layoutKey)).toByteArray();
    if (!layout.isEmpty()) {
        restoreState(layout, layoutVersion);
    }
    updateMode();
}

void MainWindow::applyTheme() {
    m_viewport->setColors(Theme::instance().tokens().viewport);
    fitToolbar();
    // Icons are drawn in the theme's colors, so fetch them again.
    for (const QString& id : m_commands->ids()) {
        const Command* command = m_commands->command(id);
        if (!command->iconName.isEmpty()) {
            m_commands->action(id)->setIcon(icon(command->iconName));
        }
    }
    if (m_patternButton != nullptr) {
        m_patternButton->setIcon(icon(QStringLiteral("pattern-rectangular")));
    }
    if (m_styleButton != nullptr) {
        for (const QString& id : m_commands->ids()) {
            const Command* command = m_commands->command(id);
            if (command->exclusiveGroup == QLatin1String("displayStyle") && command->isChecked()) {
                m_styleButton->setIcon(icon(command->iconName));
            }
        }
    }
    if (m_tabs != nullptr) {
        for (int i = 0; i < m_tabs->count(); ++i) {
            updateTab(i);
            if (auto* close =
                    qobject_cast<QToolButton*>(m_tabs->tabButton(i, QTabBar::RightSide))) {
                close->setIcon(icon(QStringLiteral("close")));
            }
        }
    }
}

void MainWindow::resetLayout() {
    if (m_focusState.has_value()) {
        m_focusState.reset();
    }
    restoreState(m_defaultLayout, layoutVersion);
    updateMode();
    m_commands->refresh();
}

void MainWindow::setViewportFocus(bool focused) {
    if (focused == m_focusState.has_value()) {
        return;
    }
    if (focused) {
        m_focusState = saveState(layoutVersion);
        for (QDockWidget* dock : {m_browserDock, m_inspectorDock, m_timelineDock}) {
            dock->hide();
        }
    } else {
        const QByteArray state = *m_focusState;
        m_focusState.reset();
        restoreState(state, layoutVersion);
    }
    updateMode();
    m_commands->refresh();
}

// ---------------------------------------------------------------------------------------------
// Documents

int MainWindow::addSession(std::unique_ptr<document::Document> document, const QString& path) {
    auto session = std::make_unique<Session>();
    session->document = std::move(document);
    session->filePath = path;
    Session* raw = session.get();
    session->listener = raw->document->addListener([this, raw](const document::Change& change) {
        const auto found =
            std::find_if(m_sessions.begin(), m_sessions.end(),
                         [raw](const std::unique_ptr<Session>& s) { return s.get() == raw; });
        if (found == m_sessions.end()) {
            return;
        }
        const int index = static_cast<int>(found - m_sessions.begin());
        updateTab(index);
        if (change.kind == document::Change::Kind::HistoryChanged) {
            raw->autosaveDue = true;
        }
        if (index != m_current) {
            return;
        }
        // This listener was added before the viewport's and the browser's, so it hears of a
        // change first. Every edit, undo, and redo ends with HistoryChanged.
        if (change.kind == document::Change::Kind::HistoryChanged) {
            m_documentIsChanging = false;
            updateDocumentState();
            updateSelectionState();
        } else {
            m_documentIsChanging = true;
        }
    });
    m_sessions.push_back(std::move(session));
    const int index = static_cast<int>(m_sessions.size()) - 1;
    {
        const QSignalBlocker blocker(m_tabs);
        m_tabs->addTab(QString());
    }
    // A close button drawn like LCAD's other icons, in place of the style's own.
    auto* close = new QToolButton(m_tabs);
    close->setObjectName(QStringLiteral("closeTab"));
    close->setAutoRaise(true);
    close->setIcon(icon(QStringLiteral("close")));
    close->setIconSize(QSize(12, 12));
    close->setToolTip(tr("Close this document"));
    connect(close, &QToolButton::clicked, this, [this, close] {
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (m_tabs->tabButton(i, QTabBar::RightSide) == close) {
                fileClose(i);
                return;
            }
        }
    });
    m_tabs->setTabButton(index, QTabBar::RightSide, close);
    updateTab(index);
    return index;
}

void MainWindow::storeViewOf(int index) {
    if (index < 0 || index >= documentCount()) {
        return;
    }
    m_sessions[static_cast<std::size_t>(index)]->camera = m_viewport->cameraState();
}

void MainWindow::setCurrentIndex(int index) {
    if (index < 0 || index >= documentCount() || index == m_current) {
        return;
    }
    storeViewOf(m_current);
    activate(index);
}

void MainWindow::activate(int index) {
    cancelCommand();
    m_current = index;
    Session& session = current();
    m_viewport->setDocument(session.document.get());
    if (session.camera.has_value()) {
        m_viewport->setCameraState(*session.camera);
    } else {
        m_viewport->setStandardView(StandardView::Home);
    }
    for (std::size_t i = 0; i < session.origin.size(); ++i) {
        m_viewport->setOriginItemVisible(static_cast<OriginItem>(i), session.origin[i]);
    }
    m_browser->setOriginVisibility(session.origin);
    m_browser->setDocument(session.document.get(), documentName());
    m_viewport->setEditPath(session.activePath);
    m_browser->setActivePath(session.activePath);
    if (m_materialsDialog != nullptr) {
        m_materialsDialog->setDocument(session.document.get());
    }
    m_timeline->setDocument(session.document.get());
    if (m_parametersDialog != nullptr) {
        m_parametersDialog->setDocument(session.document.get());
    }
    {
        const QSignalBlocker blocker(m_tabs);
        m_tabs->setCurrentIndex(index);
    }
    updateDocumentState();
    updateSelectionState();
}

document::Document* MainWindow::document() const {
    return current().document.get();
}

QString MainWindow::filePath() const {
    return current().filePath;
}

QString MainWindow::documentName() const {
    return documentName(m_current);
}

QString MainWindow::documentName(int index) const {
    const QString& path = m_sessions[static_cast<std::size_t>(index)]->filePath;
    return path.isEmpty() ? tr("Untitled") : QFileInfo(path).completeBaseName();
}

void MainWindow::updateTab(int index) {
    const Session& session = *m_sessions[static_cast<std::size_t>(index)];
    const QString name = documentName(index);
    // Unsaved changes are marked in the text, not by color alone.
    m_tabs->setTabText(index, session.document->isModified() ? tr("%1 *").arg(name) : name);
    m_tabs->setTabToolTip(index, session.filePath.isEmpty()
                                     ? tr("Not saved to a file yet")
                                     : QDir::toNativeSeparators(session.filePath));
}

void MainWindow::updateDocumentState() {
    // The occurrence being worked on may have gone with an undo.
    if (!current().activePath.empty()) {
        const auto& instances = document()->instances();
        const std::vector<document::ObjectId>& path = current().activePath;
        if (std::none_of(instances.begin(), instances.end(),
                         [&path](const document::Instance& i) { return i.path == path; })) {
            activateOccurrence({});
        }
    }
    m_commands->refresh();
    const QString name = documentName();
    setWindowTitle(QStringLiteral("%1[*] - LCAD").arg(name));
    setWindowModified(document()->isModified());
    updateTab(m_current);
    m_browser->setDocumentName(name);
}

void MainWindow::newDocument() {
    m_loadWarnings.clear();
    storeViewOf(m_current);
    activate(addSession(std::make_unique<document::Document>(), QString()));
}

void MainWindow::openFromSystem(const QString& path) {
    QString error;
    if (!loadFrom(path, &error)) {
        QMessageBox::critical(this, tr("Could not open the document"), error);
    } else if (!m_loadWarnings.isEmpty()) {
        QMessageBox::warning(this, tr("The document was opened with problems"),
                             m_loadWarnings.join(QLatin1Char('\n')));
    }
}

void MainWindow::acceptFileOpenRequests() {
    qApp->installEventFilter(this);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == qApp && event->type() == QEvent::FileOpen) {
        const QString file = static_cast<QFileOpenEvent*>(event)->file();
        if (!file.isEmpty()) {
            openFromSystem(file);
            raise();
            activateWindow();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::loadFrom(const QString& path, QString* error) {
    m_loadWarnings.clear();
    const QString absolute = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < documentCount(); ++i) {
        if (m_sessions[static_cast<std::size_t>(i)]->filePath == absolute) {
            setCurrentIndex(i);
            return true;
        }
    }
    std::unique_ptr<document::Document> loaded;
    try {
        document::LoadedDocument result = document::loadDocument(toPath(path));
        for (const std::string& warning : result.warnings) {
            m_loadWarnings.append(QString::fromStdString(warning));
        }
        loaded = std::move(result.document);
    } catch (const document::FileError& failure) {
        if (error != nullptr) {
            *error = QString::fromStdString(failure.what());
        }
        return false;
    }

    // An empty document nobody has touched gives its tab to the file.
    const document::Document* shown = document();
    const bool untouched = filePath().isEmpty() && shown->bodies().empty() &&
                           !shown->isModified() && !shown->canUndo() && !shown->canRedo();
    const int replaced = untouched ? m_current : -1;
    storeViewOf(m_current);
    int index = addSession(std::move(loaded), absolute);
    if (replaced >= 0) {
        m_viewport->setDocument(nullptr);
        m_browser->setDocument(nullptr, {});
        m_timeline->setDocument(nullptr);
        if (m_parametersDialog != nullptr) {
            m_parametersDialog->setDocument(nullptr);
        }
        m_sessions.erase(m_sessions.begin() + replaced);
        {
            const QSignalBlocker blocker(m_tabs);
            m_tabs->removeTab(replaced);
        }
        --index;
    }
    activate(index);
    rememberFile(absolute);
    core::log(core::LogLevel::Info, "app", "Opened " + core::toUtf8(toPath(absolute)));
    statusBar()->showMessage(tr("Opened %1").arg(documentName()), 4000);
    // Linked parts that changed, or cannot be found, are said; nothing is changed until the
    // user updates them.
    int changed = 0;
    for (const document::LinkStatus& status : document::checkLinks(*document(), toPath(absolute))) {
        if (status.state == document::LinkStatus::State::Changed) {
            ++changed;
        } else if (status.state != document::LinkStatus::State::UpToDate) {
            m_loadWarnings.append(tr("The linked part %1 cannot be found or read; the copy kept in "
                                     "the document is shown. %2")
                                      .arg(QString::fromStdString(status.name),
                                           QString::fromStdString(status.problem)));
        }
    }
    if (changed > 0) {
        statusBar()->showMessage(
            (changed == 1
                 ? tr("A linked part changed since this document was saved.")
                 : tr("%1 linked parts changed since this document was saved.").arg(changed)) +
                QLatin1Char(' ') + tr("File > Update Linked Parts takes the changes."),
            12000);
    }
    return true;
}

bool MainWindow::saveTo(const QString& path, QString* error) {
    // Links are kept relative to the document's folder: saved in another folder, they are
    // written to lead to the same files from there.
    const std::filesystem::path from = filePath().isEmpty()
                                           ? std::filesystem::path()
                                           : toPath(QFileInfo(filePath()).absolutePath());
    const std::filesystem::path to = toPath(QFileInfo(path).absolutePath());
    if (from != to) {
        std::vector<std::pair<document::ObjectId, std::string>> moved;
        for (const document::Component& component : document()->components()) {
            if (component.link.empty()) {
                continue;
            }
            const std::string link =
                document::makeLink(document::resolveLink(component.link, from), to);
            if (link != component.link) {
                moved.emplace_back(component.id, link);
            }
        }
        if (!moved.empty()) {
            document::Transaction transaction(*document(), tr("Save As").toStdString());
            for (const auto& [component, link] : moved) {
                document()->setComponentLink(component, link);
            }
            transaction.commit();
        }
    }
    try {
        document::SaveOptions options;
        options.thumbnailPng = thumbnail();
        document::saveDocument(*document(), toPath(path), options);
    } catch (const document::FileError& failure) {
        if (error != nullptr) {
            *error = QString::fromStdString(failure.what());
        }
        return false;
    }
    current().filePath = QFileInfo(path).absoluteFilePath();
    document()->markSaved();
    if (m_recovery != nullptr && !current().recoveryKey.isEmpty()) {
        m_recovery->discard(current().recoveryKey);
    }
    current().autosaveDue = false;
    updateDocumentState();
    rememberFile(current().filePath);
    statusBar()->showMessage(tr("Saved %1").arg(documentName()), 4000);
    return true;
}

void MainWindow::closeDocument(int index) {
    if (index < 0 || index >= documentCount()) {
        return;
    }
    const bool wasCurrent = index == m_current;
    if (m_recovery != nullptr) {
        const QString& key = m_sessions[static_cast<std::size_t>(index)]->recoveryKey;
        if (!key.isEmpty()) {
            m_recovery->discard(key);
        }
    }
    if (wasCurrent) {
        cancelCommand();
        m_viewport->setDocument(nullptr);
        m_browser->setDocument(nullptr, {});
        m_timeline->setDocument(nullptr);
        if (m_parametersDialog != nullptr) {
            m_parametersDialog->setDocument(nullptr);
        }
        m_properties->refresh(nullptr, {}, {});
    } else {
        storeViewOf(m_current);
    }
    m_sessions.erase(m_sessions.begin() + index);
    {
        const QSignalBlocker blocker(m_tabs);
        m_tabs->removeTab(index);
    }
    if (m_sessions.empty()) {
        // LCAD always has a document to work in.
        m_current = -1;
        activate(addSession(std::make_unique<document::Document>(), QString()));
        return;
    }
    int next = m_current;
    if (wasCurrent) {
        next = std::min(index, documentCount() - 1);
    } else if (index < m_current) {
        next = m_current - 1;
    }
    activate(next);
}

QStringList MainWindow::recentFiles() const {
    return QSettings().value(QLatin1String(recentKey)).toStringList();
}

void MainWindow::rememberFile(const QString& path) {
    QSettings settings;
    QStringList files = settings.value(QLatin1String(recentKey)).toStringList();
    files.removeAll(path);
    files.prepend(path);
    while (files.size() > recentLimit) {
        files.removeLast();
    }
    settings.setValue(QLatin1String(recentKey), files);
    settings.setValue(QLatin1String(directoryKey), QFileInfo(path).absolutePath());
    updateRecentMenu();
}

void MainWindow::updateRecentMenu() {
    m_recentMenu->clear();
    const QStringList files = recentFiles();
    for (const QString& path : files) {
        const QFileInfo info(path);
        QAction* open = m_recentMenu->addAction(
            tr("%1  —  %2").arg(info.fileName(), QDir::toNativeSeparators(info.absolutePath())));
        open->setData(path);
        connect(open, &QAction::triggered, this, [this, path] {
            QString error;
            if (!loadFrom(path, &error)) {
                QMessageBox::critical(this, tr("Could not open the document"), error);
            } else if (!m_loadWarnings.isEmpty()) {
                QMessageBox::warning(this, tr("The document was opened with problems"),
                                     m_loadWarnings.join(QLatin1Char('\n')));
            }
        });
    }
    if (files.isEmpty()) {
        m_recentMenu->addAction(tr("No recent documents"))->setEnabled(false);
    }
}

// ---------------------------------------------------------------------------------------------
// Selection

std::vector<document::ObjectId> MainWindow::selectedBodies() const {
    std::vector<document::ObjectId> bodies = m_browser->selectedBodies();
    for (const SelectedItem& item : m_viewport->selection()) {
        if (item.kind == SelectedItem::Kind::Body &&
            std::find(bodies.begin(), bodies.end(), item.body) == bodies.end()) {
            bodies.push_back(item.body);
        }
    }
    return bodies;
}

std::vector<document::ObjectId> MainWindow::selectedSketches() const {
    std::vector<document::ObjectId> sketches = m_browser->selectedSketches();
    for (const SelectedItem& item : m_viewport->selection()) {
        if (item.kind == SelectedItem::Kind::Sketch &&
            std::find(sketches.begin(), sketches.end(), item.body) == sketches.end()) {
            sketches.push_back(item.body);
        }
    }
    return sketches;
}

std::vector<document::ObjectId> MainWindow::targetBodies() const {
    std::vector<document::ObjectId> bodies = selectedBodies();
    for (const SelectedItem& item : m_viewport->selection()) {
        if (item.kind != SelectedItem::Kind::Sketch &&
            std::find(bodies.begin(), bodies.end(), item.body) == bodies.end()) {
            bodies.push_back(item.body);
        }
    }
    return bodies;
}

std::optional<kernel::Plane> MainWindow::selectedFacePlane() const {
    const std::vector<SelectedItem> picked = m_viewport->selection();
    if (picked.size() != 1 || picked.front().kind != SelectedItem::Kind::Face) {
        return std::nullopt;
    }
    const document::Body* body = document()->findBody(picked.front().body);
    if (body == nullptr) {
        return std::nullopt;
    }
    try {
        return kernel::planeOfFace(
            kernel::subShape(body->shape, kernel::SubShapeKind::Face, picked.front().index));
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

std::optional<std::pair<document::ObjectId, std::string>> MainWindow::selectedFaceName() const {
    const std::vector<SelectedItem> picked = m_viewport->selection();
    if (picked.size() != 1 || picked.front().kind != SelectedItem::Kind::Face ||
        !selectedFacePlane().has_value()) {
        return std::nullopt;
    }
    const kernel::NamedShape named = document()->namedBody(picked.front().body);
    const auto index = static_cast<std::size_t>(picked.front().index);
    if (index >= named.faceNames.size()) {
        return std::nullopt;
    }
    return std::pair{picked.front().body, named.faceNames[index]};
}

document::ObjectId MainWindow::selectedConstructionPlane() const {
    // The Construction group lists axes too.
    for (const document::ObjectId id : m_browser->selectedPlanes()) {
        const document::Feature* plane = document()->findFeature(id);
        if (plane != nullptr && plane->kind == document::FeatureKind::Plane) {
            return id;
        }
    }
    const document::Feature* feature = document()->findFeature(m_timeline->selectedItem());
    if (feature != nullptr && feature->kind == document::FeatureKind::Plane &&
        m_timeline->isAncestorOf(QApplication::focusWidget())) {
        return feature->id;
    }
    return {};
}

// Shows or hides bodies, and the sketches that are selected with them, as one undo step.
void MainWindow::setBodiesVisible(const std::vector<document::ObjectId>& bodies, bool visible,
                                  const QString& label) {
    const std::vector<document::ObjectId> sketches = label == tr("Hide") || label == tr("Show")
                                                         ? selectedSketches()
                                                         : std::vector<document::ObjectId>();
    if (bodies.empty() && sketches.empty()) {
        return;
    }
    document::Transaction transaction(*document(), label.toStdString());
    for (const document::ObjectId id : bodies) {
        document()->setBodyVisible(id, visible);
    }
    for (const document::ObjectId id : sketches) {
        document()->setSketchVisible(id, visible);
    }
    transaction.commit();
}

void MainWindow::updateSelectionState() {
    m_commands->refresh();
    if (m_filterBox != nullptr) {
        const QSignalBlocker blocker(m_filterBox);
        for (int i = 0; i < m_filterBox->count(); ++i) {
            if (m_commands->action(m_filterBox->itemData(i).toString())->isChecked()) {
                m_filterBox->setCurrentIndex(i);
            }
        }
    }
    m_selectionLabel->setText(selectionSummary());
    if (isSketching()) {
        const document::SketchItem* sketch = document()->findSketch(m_viewport->editedSketch());
        m_sketchPanel->refresh(sketch != nullptr ? QString::fromStdString(sketch->name)
                                                 : QString());
        if (m_panel == nullptr && m_planePanel == nullptr) {
            m_commandLabel->setText(m_viewport->sketchEditor()->hint());
        }
    } else if (m_panel == nullptr && m_planePanel == nullptr && m_featurePanel == nullptr &&
               m_measurePanel == nullptr && m_jointPanel == nullptr && m_movePanel == nullptr) {
        // A sketch selected in the browser while it is hidden is not in the viewport's
        // selection, and is what is selected all the same.
        std::vector<SelectedItem> parts = m_viewport->selection();
        for (const document::ObjectId id : m_browser->selectedSketches()) {
            const SelectedItem item{id, SelectedItem::Kind::Sketch, -1};
            if (std::find(parts.begin(), parts.end(), item) == parts.end()) {
                parts.push_back(item);
            }
        }
        const auto occurrences = m_browser->selectedOccurrences();
        const auto joints = m_browser->selectedJoints();
        if (occurrences.size() == 1 && joints.empty() && m_browser->selectedBodies().empty()) {
            m_properties->showOccurrence(document(), occurrences.front().back());
        } else if (joints.size() == 1 && occurrences.empty()) {
            m_properties->showJoint(document(), joints.front());
        } else {
            m_properties->refresh(document(), selectedBodies(), parts);
        }
    }
}

std::string MainWindow::nameOf(const SelectedItem& item) const {
    const kernel::NamedShape named = document()->namedBody(item.body);
    const auto index = static_cast<std::size_t>(item.index);
    if (item.kind == SelectedItem::Kind::Face && index < named.faceNames.size()) {
        return named.faceNames[index];
    }
    if (item.kind == SelectedItem::Kind::Edge && !named.shape.isNull()) {
        const std::vector<std::string> edges = kernel::edgeNames(named);
        if (index < edges.size()) {
            return edges[index];
        }
    }
    return {};
}

QString MainWindow::describe(const SelectedItem& item) const {
    if (item.kind == SelectedItem::Kind::Sketch) {
        const document::SketchItem* sketch = document()->findSketch(item.body);
        if (sketch == nullptr) {
            return {};
        }
        return tr("%1 · %2").arg(QString::fromStdString(sketch->name),
                                 describeSketchState(sketch->geometry, sketch->geometry.state()));
    }
    const document::Body* body = document()->findBody(item.body);
    if (body == nullptr) {
        return {};
    }
    const QString owner = QString::fromStdString(body->name);
    try {
        switch (item.kind) {
        case SelectedItem::Kind::Body:
            return tr("%1 · volume %2 mm³ · area %3 mm²")
                .arg(owner, formatNumber(kernel::volume(body->shape)),
                     formatNumber(kernel::surfaceArea(body->shape)));
        case SelectedItem::Kind::Face:
            return tr("Face of %1 · area %2 mm²")
                .arg(owner, formatNumber(kernel::surfaceArea(kernel::subShape(
                                body->shape, kernel::SubShapeKind::Face, item.index))));
        case SelectedItem::Kind::Edge:
            return tr("Edge of %1 · length %2 mm")
                .arg(owner, formatNumber(kernel::length(kernel::subShape(
                                body->shape, kernel::SubShapeKind::Edge, item.index))));
        case SelectedItem::Kind::Vertex: {
            const auto point = kernel::boundingBox(
                kernel::subShape(body->shape, kernel::SubShapeKind::Vertex, item.index));
            if (!point.has_value()) {
                return {};
            }
            return tr("Vertex of %1 · X %2 · Y %3 · Z %4 mm")
                .arg(owner, formatNumber(point->min.x), formatNumber(point->min.y),
                     formatNumber(point->min.z));
        }
        case SelectedItem::Kind::Sketch:
            break;
        }
    } catch (const std::out_of_range&) {
        // The body changed under the selection; there is nothing to say about it.
    }
    return {};
}

QString MainWindow::selectionSummary() const {
    if (isSketching()) {
        // The sketch being edited, how much of it can still move, and what is selected in it.
        const viewport::SketchEditor* editor = m_viewport->sketchEditor();
        const document::SketchItem* sketch = document()->findSketch(m_viewport->editedSketch());
        if (sketch == nullptr) {
            return {};
        }
        QString text = tr("%1 · %2").arg(QString::fromStdString(sketch->name),
                                         describeSketchState(editor->geometry(), editor->state()));
        const std::vector<sketch::EntityId> entities = editor->selectedEntities();
        const std::vector<sketch::ConstraintId> constraints = editor->selectedConstraints();
        const std::size_t total = entities.size() + constraints.size();
        if (total > 1) {
            text += tr(" · %1 items selected").arg(total);
        } else if (entities.size() == 1) {
            text += QStringLiteral(" · ") + describeSketchEntity(editor->geometry(), entities[0]);
        } else if (constraints.size() == 1) {
            if (const sketch::Constraint* constraint =
                    editor->geometry().findConstraint(constraints[0])) {
                text += QStringLiteral(" · ") + describeConstraint(*constraint);
            }
        }
        return text;
    }
    const std::vector<SelectedItem> picked = m_viewport->selection();
    const std::vector<document::ObjectId> bodies = selectedBodies();
    std::vector<SelectedItem> parts;
    for (const SelectedItem& item : picked) {
        if (item.kind != SelectedItem::Kind::Body) {
            parts.push_back(item);
        }
    }
    // Sketches selected in the browser only, because they are hidden.
    for (const document::ObjectId id : m_browser->selectedSketches()) {
        const SelectedItem item{id, SelectedItem::Kind::Sketch, -1};
        if (std::find(parts.begin(), parts.end(), item) == parts.end()) {
            parts.push_back(item);
        }
    }
    const std::size_t total = parts.size() + bodies.size();
    if (total == 0) {
        return {};
    }
    if (total > 1) {
        return tr("%1 items selected").arg(total);
    }
    return describe(parts.empty() ? SelectedItem{bodies.front(), SelectedItem::Kind::Body, -1}
                                  : parts.front());
}

QMenu* MainWindow::createViewportMenu(const QPoint& position) {
    const auto action = [this](const char* id) { return m_commands->action(QLatin1String(id)); };
    if (isSketching()) {
        // A right-click on something of the sketch that is not selected selects it.
        viewport::SketchEditor* editor = m_viewport->sketchEditor();
        const sketch::ConstraintId constraint = editor->constraintAt(position);
        const sketch::EntityId entity = constraint == 0 ? editor->entityAt(position) : 0;
        const auto entities = editor->selectedEntities();
        const auto constraints = editor->selectedConstraints();
        if (constraint != 0 &&
            std::find(constraints.begin(), constraints.end(), constraint) == constraints.end()) {
            editor->setSelection({}, {constraint});
        } else if (entity != 0 &&
                   std::find(entities.begin(), entities.end(), entity) == entities.end()) {
            editor->setSelection({entity});
        }
        m_commands->refresh();
        auto* menu = new QMenu(this);
        if (!editor->selectedEntities().empty()) {
            menu->addAction(action("sketch.dimension"));
            menu->addAction(action("sketch.construction"));
            QMenu* constrain = menu->addMenu(tr("&Constrain"));
            constrain->addActions(
                m_menuBar->findChild<QMenu*>(QStringLiteral("menu.constrain"))->actions());
        }
        if (action("edit.delete")->isEnabled()) {
            menu->addAction(action("edit.delete"));
        }
        menu->addSeparator();
        menu->addAction(action("sketch.select"));
        menu->addAction(action("view.fit"));
        menu->addSeparator();
        menu->addAction(action("sketch.finish"));
        return menu;
    }
    const std::vector<SelectedItem> under = m_viewport->itemsAt(position);
    // A right-click on something that is not selected selects it, as a click would.
    if (!under.empty()) {
        const std::vector<SelectedItem> selected = m_viewport->selection();
        if (std::find(selected.begin(), selected.end(), under.front()) == selected.end()) {
            m_viewport->setSelection({under.front()});
        }
    }
    m_commands->refresh();

    auto* menu = new QMenu(this);
    if (under.size() > 1) {
        // What a click cannot reach because something lies in front of it.
        QMenu* other = menu->addMenu(tr("Select &Other"));
        other->setObjectName(QStringLiteral("menu.selectOther"));
        for (const SelectedItem& item : under) {
            other->addAction(describe(item), this,
                             [this, item] { m_viewport->setSelection({item}); });
        }
        menu->addSeparator();
    }
    if (!under.empty()) {
        if (action("sketch.edit")->isEnabled()) {
            menu->addAction(action("sketch.edit"));
        }
        menu->addAction(action("view.fitSelection"));
        for (const char* id : {"body.hide", "body.isolate"}) {
            if (action(id)->isEnabled()) {
                menu->addAction(action(id));
            }
        }
        // What was clicked is part of a component: what can be done with that component.
        if (!selectedOccurrences().empty()) {
            menu->addSeparator();
            for (const char* id : {"assembly.activate", "assembly.move", "assembly.ground",
                                   "assembly.openLinked", "inspect.physical"}) {
                if (action(id)->isEnabled()) {
                    menu->addAction(action(id));
                }
            }
        }
        if (action("edit.delete")->isEnabled()) {
            menu->addAction(action("edit.delete"));
        }
        menu->addSeparator();
    }
    if (selectedFacePlane().has_value()) {
        // On the flat face that was clicked.
        menu->addAction(action("sketch.new"));
    }
    if (action("body.showAll")->isEnabled()) {
        menu->addAction(action("body.showAll"));
    }
    menu->addAction(action("view.fit"));
    menu->addAction(action("view.home"));
    return menu;
}

void MainWindow::showViewportMenu(const QPoint& position) {
    QMenu* menu = createViewportMenu(position);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(m_viewport->mapToGlobal(position));
}

// ---------------------------------------------------------------------------------------------
// Modeling commands

void MainWindow::showInspector() {
    QStackedWidget* stack = m_inspectorStack;
    if (m_panel != nullptr) {
        stack->setCurrentWidget(m_panel);
        const QString title = tr("Create %1").arg(primitiveName(m_panel->kind()));
        m_inspectorDock->setWindowTitle(title);
        m_commandLabel->setText(title);
    } else if (m_featurePanel != nullptr) {
        stack->setCurrentWidget(m_featurePanel);
        const QString name = featureKindName(m_featurePanel->kind());
        const document::Feature* edited = document()->findFeature(m_editedFeature);
        const QString title =
            edited != nullptr ? tr("Edit %1").arg(QString::fromStdString(edited->name)) : name;
        m_inspectorDock->setWindowTitle(title);
        m_commandLabel->setText(title);
    } else if (m_jointPanel != nullptr) {
        stack->setCurrentWidget(m_jointPanel);
        const document::Joint* edited = document()->findJoint(m_editedJoint);
        const QString title = edited != nullptr
                                  ? tr("Edit %1").arg(QString::fromStdString(edited->name))
                                  : tr("Joint");
        m_inspectorDock->setWindowTitle(title);
        m_commandLabel->setText(title);
    } else if (m_movePanel != nullptr) {
        stack->setCurrentWidget(m_movePanel);
        m_inspectorDock->setWindowTitle(tr("Move Component"));
        m_commandLabel->setText(tr("Move Component"));
    } else if (m_measurePanel != nullptr) {
        stack->setCurrentWidget(m_measurePanel);
        m_inspectorDock->setWindowTitle(tr("Measure"));
        m_commandLabel->setText(tr("Measure"));
    } else if (m_planePanel != nullptr) {
        stack->setCurrentWidget(m_planePanel);
        m_inspectorDock->setWindowTitle(tr("New Sketch"));
        m_commandLabel->setText(tr("New Sketch: choose a plane"));
    } else if (isSketching()) {
        stack->setCurrentWidget(m_sketchPanel);
        m_inspectorDock->setWindowTitle(tr("Sketch"));
        m_commandLabel->setText(m_viewport->sketchEditor()->hint());
    } else {
        stack->setCurrentWidget(m_properties);
        m_inspectorDock->setWindowTitle(tr("Properties"));
        m_commandLabel->clear();
    }
}

void MainWindow::startPrimitive(PrimitiveKind kind) {
    cancelCommand();
    QStackedWidget* stack = m_inspectorStack;
    m_panel = new PrimitivePanel(kind, stack);
    stack->addWidget(m_panel);
    connect(m_panel, &PrimitivePanel::changed, this,
            [this] { m_viewport->setPreview(m_panel->shape()); });
    connect(m_panel, &PrimitivePanel::accepted, this, &MainWindow::acceptCommand);
    connect(m_panel, &PrimitivePanel::canceled, this, &MainWindow::cancelCommand);
    showInspector();
    // A command needs its panel, even if the user had closed it.
    m_inspectorDock->show();
    m_viewport->setPreview(m_panel->shape());
    m_panel->setFocus();
    m_commands->refresh();
}

void MainWindow::acceptCommand() {
    if (m_panel == nullptr || m_panel->shape().isNull()) {
        return;
    }
    const QString name = primitiveName(m_panel->kind());
    try {
        document::Transaction transaction(*document(), tr("Create %1").arg(name).toStdString());
        document()->addBody(document()->uniqueBodyName(name.toStdString()), m_panel->shape(),
                            activeComponent());
        transaction.commit();
    } catch (const std::invalid_argument& failure) {
        // The transaction has put the document back. Leave the panel open to try other values.
        QMessageBox::warning(this, tr("Could not create the %1").arg(name.toLower()),
                             QString::fromUtf8(failure.what()));
        return;
    }
    cancelCommand();
}

void MainWindow::cancelCommand() {
    if (m_featurePanel != nullptr) {
        closeFeaturePanel();
    }
    if (m_jointPanel != nullptr) {
        closeJointPanel();
    }
    if (m_movePanel != nullptr) {
        closeMovePanel();
    }
    if (isAnimating()) {
        stopAnimation();
    }
    if (m_measurePanel != nullptr) {
        closeMeasure();
    }
    if (m_panel == nullptr && m_planePanel == nullptr) {
        return;
    }
    if (m_panel != nullptr) {
        m_viewport->setPreview(kernel::Shape());
        m_inspectorStack->removeWidget(m_panel);
        // Later: this may be running inside one of the panel's own signals.
        m_panel->deleteLater();
        m_panel = nullptr;
    }
    if (m_planePanel != nullptr) {
        m_inspectorStack->removeWidget(m_planePanel);
        m_planePanel->deleteLater();
        m_planePanel = nullptr;
        statusBar()->clearMessage();
        m_viewport->setPlanePicking(false);
    }
    showInspector();
    m_viewport->setFocus();
    updateSelectionState();
}

void MainWindow::deleteSelectedBodies() {
    const std::vector<document::ObjectId> bodies = selectedBodies();
    const std::vector<document::ObjectId> sketches = selectedSketches();
    const std::size_t total = bodies.size() + sketches.size();
    if (total == 0) {
        return;
    }
    QString label = tr("Delete %1 Items").arg(total);
    if (sketches.empty()) {
        label = bodies.size() == 1 ? tr("Delete Body") : tr("Delete %1 Bodies").arg(bodies.size());
    } else if (bodies.empty()) {
        label = sketches.size() == 1 ? tr("Delete Sketch")
                                     : tr("Delete %1 Sketches").arg(sketches.size());
    }
    document::Transaction transaction(*document(), label.toStdString());
    for (const document::ObjectId id : bodies) {
        document()->removeBody(id);
    }
    for (const document::ObjectId id : sketches) {
        document()->removeSketch(id);
    }
    transaction.commit();
    updateSelectionState();
}

// ---------------------------------------------------------------------------------------------
// Features

namespace {

bool usesProfiles(document::FeatureKind kind) {
    return kind == document::FeatureKind::Pull || kind == document::FeatureKind::Revolve;
}

bool copiesFeatures(document::FeatureKind kind) {
    return kind == document::FeatureKind::Mirror ||
           kind == document::FeatureKind::RectangularPattern ||
           kind == document::FeatureKind::CircularPattern;
}

// Features whose input is faces of one body, clicked in the 3D view.
bool picksFaces(document::FeatureKind kind) {
    return kind == document::FeatureKind::Shell || kind == document::FeatureKind::OffsetFace ||
           kind == document::FeatureKind::MoveFace || kind == document::FeatureKind::DeleteFace;
}

} // namespace

std::vector<document::ObjectId> MainWindow::profileSketches(document::ObjectId also) const {
    std::vector<document::ObjectId> sketches;
    for (const document::SketchItem& sketch : document()->sketches()) {
        if (sketch.visible || sketch.id == also) {
            sketches.push_back(sketch.id);
        }
    }
    // Hidden ones that are selected count as well: picking them in the browser is how to say
    // which sketch is meant.
    for (const document::ObjectId id : selectedSketches()) {
        if (std::find(sketches.begin(), sketches.end(), id) == sketches.end()) {
            sketches.push_back(id);
        }
    }
    return sketches;
}

void MainWindow::startFeature(document::FeatureKind kind) {
    // The sketch that was open, or else the one selected, is where a pull starts from.
    document::ObjectId from;
    if (isSketching()) {
        from = m_viewport->editedSketch();
        finishSketch();
    } else if (const auto sketches = selectedSketches(); sketches.size() == 1) {
        from = sketches.front();
    }
    // Edges or faces already selected are what a fillet, chamfer, or shell works on.
    const std::vector<SelectedItem> picked = m_viewport->selection();
    // A pull or a revolve selected in the timeline is what a pattern or a mirror copies; or a
    // body selected is what is copied whole.
    document::Feature seed;
    seed.kind = kind;
    if (copiesFeatures(kind)) {
        const document::Feature* source = document()->findFeature(m_timeline->selectedItem());
        const std::vector<document::ObjectId> bodies = targetBodies();
        if (source != nullptr && usesProfiles(source->kind) &&
            document()->findBody(source->body) != nullptr) {
            seed.body = source->body;
            seed.sources = {source->id};
        } else if (!bodies.empty()) {
            seed.body = bodies.front();
        } else {
            for (const document::Body& body : document()->bodies()) {
                if (!body.shape.isNull()) {
                    seed.body = body.id;
                    break;
                }
            }
        }
        seed.reference = kind == document::FeatureKind::Mirror            ? "YZ"
                         : kind == document::FeatureKind::CircularPattern ? "Z"
                                                                          : "X";
        seed.count = kind == document::FeatureKind::CircularPattern ? 4 : 3;
        seed.count2 = 2;
        seed.spacing = 20.0;
        seed.spacing2 = 20.0;
        seed.angle = 360.0;
    }
    if (kind == document::FeatureKind::Hole) {
        // The points of the sketch that was open or is selected, or else of the last sketch
        // that has any; into the face it is on, or the first body.
        seed.size = 5.0;
        seed.distance = 10.0;
        seed.sketch = from;
        if (!seed.sketch.isValid()) {
            for (const document::SketchItem& sketch : document()->sketches()) {
                for (const sketch::Entity& entity : sketch.geometry.entities()) {
                    if (entity.kind == sketch::EntityKind::Point && entity.owner == 0 &&
                        !entity.construction && entity.id != sketch.geometry.origin()) {
                        seed.sketch = sketch.id;
                    }
                }
            }
        }
        const document::SketchItem* points = document()->findSketch(seed.sketch);
        if (points != nullptr && document()->findBody(points->attachedTo) != nullptr) {
            seed.body = points->attachedTo;
        } else {
            for (const document::Body& body : document()->bodies()) {
                if (!body.shape.isNull()) {
                    seed.body = body.id;
                    break;
                }
            }
        }
    }
    if (kind == document::FeatureKind::Plane) {
        // Against the flat face that is selected, or else the XY plane.
        seed.reference = "XY";
        seed.distance = 20.0;
        if (const auto face = selectedFaceName()) {
            seed.body = face->first;
            seed.reference = face->second;
        }
    }
    if (kind == document::FeatureKind::Axis) {
        // Along the edge or face that is selected, or else the Z axis.
        seed.reference = "Z";
        if (picked.size() == 1 && (picked.front().kind == SelectedItem::Kind::Edge ||
                                   picked.front().kind == SelectedItem::Kind::Face)) {
            if (const std::string name = nameOf(picked.front()); !name.empty()) {
                seed.body = picked.front().body;
                seed.reference = name;
            }
        }
    }
    cancelCommand();
    m_editedFeature = {};
    // The feature goes into the component of what it starts from.
    if (const document::SketchItem* sketch = document()->findSketch(from)) {
        ensureActive(sketch->component);
    } else if (!picked.empty() && picked.front().kind != SelectedItem::Kind::Sketch) {
        if (const document::Body* body = document()->findBody(picked.front().body)) {
            ensureActive(body->component, picked.front().path);
        }
    }
    openFeaturePanel(kind);
    if (kind == document::FeatureKind::Axis) {
        m_featurePanel->setFeature(seed);
        if (seed.body.isValid()) {
            // What it is placed along stays picked, and is named in the panel.
            m_settingPicks = true;
            m_viewport->setSelection(picked);
            m_settingPicks = false;
        }
    } else if (kind == document::FeatureKind::Plane || kind == document::FeatureKind::Hole) {
        m_featurePanel->setFeature(seed);
        m_settingPicks = true;
        m_viewport->clearSelection();
        m_settingPicks = false;
    } else if (copiesFeatures(kind)) {
        m_featurePanel->setFeature(seed);
        m_settingPicks = true;
        m_viewport->clearSelection();
        m_settingPicks = false;
    } else if (usesProfiles(kind)) {
        m_viewport->setProfileSketches(profileSketches(from), kind == document::FeatureKind::Pull);
        // A sketch with one closed area leaves no doubt about which is meant.
        std::vector<viewport::Profile> profiles;
        for (const viewport::Profile& profile : m_viewport->pickableProfiles()) {
            if (profile.sketch == from) {
                profiles.push_back(profile);
            }
        }
        if (profiles.size() == 1) {
            m_viewport->setPickedProfiles(profiles);
        }
    } else {
        m_settingPicks = true;
        m_viewport->setSelection(picked);
        m_settingPicks = false;
    }
    takeFeaturePicks();
    statusBar()->showMessage(
        m_featurePanel->findChild<QLabel*>(QStringLiteral("featureInput"))->text(), 9000);
}

bool MainWindow::editFeature(document::ObjectId id) {
    const document::Feature* found = document()->findFeature(id);
    if (found == nullptr) {
        return false;
    }
    if (isSketching()) {
        finishSketch();
    }
    cancelCommand();
    const document::Feature feature = *found;
    ensureActive(feature.component);
    // The model as it was just before the feature: what it works on can be seen and picked.
    document()->setEditedFeature(id);
    m_editedFeature = id;
    m_timeline->setActiveItem(id);
    openFeaturePanel(feature.kind);
    m_featurePanel->setFeature(feature);
    m_settingPicks = true;
    if (usesProfiles(feature.kind)) {
        m_viewport->setProfileSketches(profileSketches(feature.sketch),
                                       feature.kind == document::FeatureKind::Pull);
        std::vector<viewport::Profile> profiles;
        for (const viewport::Profile& profile : m_viewport->pickableProfiles()) {
            const bool used =
                feature.regions.empty() || std::find(feature.regions.begin(), feature.regions.end(),
                                                     profile.outline) != feature.regions.end();
            if (profile.sketch == feature.sketch && used) {
                profiles.push_back(profile);
            }
        }
        m_viewport->setPickedProfiles(profiles);
    } else if (copiesFeatures(feature.kind) || feature.kind == document::FeatureKind::Plane ||
               feature.kind == document::FeatureKind::Axis ||
               feature.kind == document::FeatureKind::Hole) {
        m_viewport->clearSelection();
    } else {
        // The edges or faces it works on, as far as the body still has them.
        const kernel::NamedShape body = document()->namedBody(feature.body);
        std::vector<SelectedItem> items;
        if (feature.kind == document::FeatureKind::Repair) {
            items.push_back({feature.body, SelectedItem::Kind::Body, -1});
        } else if (picksFaces(feature.kind)) {
            for (const std::string& name : feature.faces) {
                if (const int index = kernel::findFace(body, name); index >= 0) {
                    items.push_back({feature.body, SelectedItem::Kind::Face, index});
                }
            }
            if (feature.faces.empty()) {
                items.push_back({feature.body, SelectedItem::Kind::Body, -1});
            }
        } else {
            for (const std::string& name : feature.edges) {
                if (const int index = kernel::findEdge(body, name); index >= 0) {
                    items.push_back({feature.body, SelectedItem::Kind::Edge, index});
                }
            }
        }
        m_viewport->setSelection(items);
    }
    m_settingPicks = false;
    updateFeaturePreview();
    return true;
}

void MainWindow::openFeaturePanel(document::FeatureKind kind) {
    m_featurePanel = new FeaturePanel(kind, document(), m_inspectorStack);
    m_featurePanel->setObjectName(QStringLiteral("featurePanel"));
    m_inspectorStack->addWidget(m_featurePanel);
    connect(m_featurePanel, &FeaturePanel::changed, this, &MainWindow::updateFeaturePreview);
    connect(m_featurePanel, &FeaturePanel::accepted, this, &MainWindow::acceptFeature);
    connect(m_featurePanel, &FeaturePanel::canceled, this, &MainWindow::cancelCommand);
    if (!usesProfiles(kind)) {
        // Edges are what a fillet or a chamfer is clicked on, faces what a shell opens or a
        // mirror is in; a pattern takes its direction or axis from either.
        m_filterBeforeFeature = m_viewport->selectionFilter();
        SelectionFilter filter = SelectionFilter::Edges;
        if (kind == document::FeatureKind::Repair) {
            filter = SelectionFilter::Bodies;
        } else if (picksFaces(kind) || kind == document::FeatureKind::Mirror ||
                   kind == document::FeatureKind::Plane) {
            filter = SelectionFilter::Faces;
        } else if (copiesFeatures(kind) || kind == document::FeatureKind::Hole ||
                   kind == document::FeatureKind::Axis) {
            // A hole's sketch is picked by clicking it, which needs edges to be pickable; an
            // axis runs along an edge or a round face.
            filter = SelectionFilter::FacesEdgesVertices;
        }
        m_viewport->setSelectionFilter(filter);
    }
    showInspector();
    // A command needs its panel, even if the user had closed it.
    m_inspectorDock->show();
    m_commands->refresh();
}

void MainWindow::closeFeaturePanel() {
    if (m_featurePanel == nullptr) {
        return;
    }
    m_viewport->setProfileSketches({});
    m_viewport->setFeaturePreview({}, kernel::Shape());
    m_viewport->setPreview(kernel::Shape());
    m_viewport->clearDistanceHandle();
    if (m_filterBeforeFeature.has_value()) {
        m_viewport->setSelectionFilter(*m_filterBeforeFeature);
        m_filterBeforeFeature.reset();
    }
    m_inspectorStack->removeWidget(m_featurePanel);
    // Nothing it says from now on is news: a box that loses the focus as the panel goes still
    // reports its value.
    m_featurePanel->disconnect(this);
    // Later: this may be running inside one of the panel's own signals.
    m_featurePanel->deleteLater();
    m_featurePanel = nullptr;
    if (m_editedFeature.isValid()) {
        m_editedFeature = {};
        m_timeline->setActiveItem({});
        // The whole model again.
        document()->setEditedFeature({});
    }
    statusBar()->clearMessage();
    showInspector();
    m_viewport->setFocus();
    updateSelectionState();
}

void MainWindow::takeFeaturePicks() {
    if (m_featurePanel == nullptr) {
        return;
    }
    const document::FeatureKind kind = m_featurePanel->kind();
    if (usesProfiles(kind)) {
        // A face clicked is where a pull up to a face ends.
        for (const SelectedItem& item : m_viewport->selection()) {
            if (item.kind != SelectedItem::Kind::Face) {
                continue;
            }
            const kernel::NamedShape named = document()->namedBody(item.body);
            const auto index = static_cast<std::size_t>(item.index);
            if (m_featurePanel->goesToFace() && index < named.faceNames.size()) {
                m_featurePanel->setExtentFace(item.body, named.faceNames[index], describe(item));
            } else if (!m_featurePanel->goesToFace()) {
                statusBar()->showMessage(
                    tr("To pull up to that face, choose To Face under Direction first."), 6000);
                m_settingPicks = true;
                m_viewport->clearSelection();
                m_settingPicks = false;
            }
            break;
        }
        // All from one sketch: the one clicked last.
        std::vector<viewport::Profile> picked = m_viewport->pickedProfiles();
        if (picked.empty()) {
            m_featurePanel->setProfiles({}, {});
            return;
        }
        const document::ObjectId sketch = picked.back().sketch;
        const auto others = std::erase_if(
            picked, [sketch](const viewport::Profile& p) { return p.sketch != sketch; });
        if (others > 0) {
            m_viewport->setPickedProfiles(picked);
        }
        std::vector<std::vector<sketch::EntityId>> outlines;
        for (const viewport::Profile& profile : picked) {
            outlines.emplace_back(profile.outline.begin(), profile.outline.end());
        }
        m_featurePanel->setProfiles(sketch, outlines);
        return;
    }
    if (kind == document::FeatureKind::Hole) {
        // A sketch clicked is where the holes go.
        for (const SelectedItem& item : m_viewport->selection()) {
            if (item.kind == SelectedItem::Kind::Sketch) {
                m_featurePanel->setSketch(item.body);
                break;
            }
        }
        return;
    }
    if (copiesFeatures(kind) || kind == document::FeatureKind::Plane ||
        kind == document::FeatureKind::Axis) {
        // The edge or face that gives the plane, the direction, or the axis.
        for (const SelectedItem& item : m_viewport->selection()) {
            if (item.kind != SelectedItem::Kind::Face &&
                (item.kind != SelectedItem::Kind::Edge || kind == document::FeatureKind::Plane)) {
                continue;
            }
            if (const std::string name = nameOf(item); !name.empty()) {
                m_featurePanel->setPickedReference(item.body, name, describe(item));
            }
            break;
        }
        return;
    }
    if (kind == document::FeatureKind::Repair) {
        // The body clicked is the one to repair.
        for (const SelectedItem& item : m_viewport->selection()) {
            m_featurePanel->setBody(item.body);
            break;
        }
        return;
    }
    // Edges or faces, of one body: the one picked first.
    const std::vector<SelectedItem> selection = m_viewport->selection();
    document::ObjectId body;
    std::vector<int> indices;
    for (const SelectedItem& item : selection) {
        if (!body.isValid()) {
            body = item.body;
        }
        if (item.body == body && item.index >= 0) {
            indices.push_back(item.index);
        }
    }
    const kernel::NamedShape named = document()->namedBody(body);
    std::vector<std::string> names;
    if (picksFaces(kind)) {
        for (const int index : indices) {
            if (index < static_cast<int>(named.faceNames.size())) {
                names.push_back(named.faceNames[static_cast<std::size_t>(index)]);
            }
        }
        m_featurePanel->setFaces(body, names);
    } else {
        const std::vector<std::string> edges =
            named.shape.isNull() ? std::vector<std::string>() : kernel::edgeNames(named);
        for (const int index : indices) {
            if (index < static_cast<int>(edges.size())) {
                names.push_back(edges[static_cast<std::size_t>(index)]);
            }
        }
        m_featurePanel->setEdges(names.empty() ? document::ObjectId{} : body, names);
    }
}

void MainWindow::updateFeaturePreview() {
    if (m_featurePanel == nullptr) {
        return;
    }
    const auto clear = [this] {
        m_viewport->setFeaturePreview({}, kernel::Shape());
        m_viewport->setPreview(kernel::Shape());
        m_viewport->clearDistanceHandle();
    };
    if (!m_featurePanel->hasInput()) {
        m_featurePanel->setError({});
        clear();
        return;
    }
    document::Feature feature = m_featurePanel->feature();
    showPullHandle(feature);
    if (feature.kind == document::FeatureKind::Axis) {
        const document::FeatureTrial trial = document()->tryFeature(feature);
        m_featurePanel->setError(QString::fromStdString(trial.error));
        m_viewport->setFeaturePreview({}, kernel::Shape());
        kernel::Shape line;
        if (trial.axis.has_value()) {
            try {
                line = document::axisShape(*trial.axis);
            } catch (const std::exception&) {
                // Nothing to show; the panel says what is wrong, if anything.
            }
        }
        m_viewport->setPreview(line);
        return;
    }
    if (feature.kind == document::FeatureKind::Plane) {
        const document::FeatureTrial trial = document()->tryFeature(feature);
        m_featurePanel->setError(QString::fromStdString(trial.error));
        m_viewport->setFeaturePreview({}, kernel::Shape());
        kernel::Shape square;
        if (trial.plane.has_value()) {
            try {
                square = document::planeShape(*trial.plane);
            } catch (const std::exception&) {
                // Nothing to show; the panel says what is wrong, if anything.
            }
        }
        m_viewport->setPreview(square);
        return;
    }
    if (usesProfiles(feature.kind) && !m_featurePanel->operationChosen() &&
        feature.body.isValid()) {
        // Until the user chooses, LCAD guesses: a solid that lies mostly inside the body is
        // meant to cut it, one that meets it to join it, and one apart from it to be a body of
        // its own.
        document::Feature join = feature;
        join.operation = document::Operation::Join;
        const document::FeatureTrial trial = document()->tryFeature(join);
        if (trial.error.empty()) {
            const kernel::NamedShape target = document()->namedBody(feature.body);
            const double tool = kernel::volume(trial.tool.shape);
            const double shared =
                kernel::volume(target.shape) + tool - kernel::volume(trial.result.shape);
            const bool apart = kernel::countTopology(trial.result.shape).solids >
                               kernel::countTopology(target.shape).solids;
            const document::Operation guess = shared > 0.5 * tool ? document::Operation::Cut
                                              : apart             ? document::Operation::NewBody
                                                                  : document::Operation::Join;
            if (guess != feature.operation) {
                // The panel reports the change, which brings this back with the guess in place.
                m_featurePanel->suggestOperation(guess);
                return;
            }
        }
    }
    const document::FeatureTrial trial = document()->tryFeature(feature);
    m_featurePanel->setError(QString::fromStdString(trial.error));
    if (usesProfiles(feature.kind)) {
        // The body as the feature leaves it, in place of the body; and the solid itself, seen
        // through, where it takes something away or does not work.
        const bool showTool = !trial.error.empty() ||
                              feature.operation == document::Operation::Cut ||
                              feature.operation == document::Operation::Intersect;
        m_viewport->setFeaturePreview(feature.body, trial.result.shape);
        m_viewport->setPreview(showTool ? trial.tool.shape : kernel::Shape());
    } else {
        // In place of the body, which stays there unseen, so that its edges and faces can still
        // be picked. Where the feature fails, the body as it is.
        m_viewport->setPreview(kernel::Shape());
        m_viewport->setFeaturePreview(feature.body, trial.result.shape, true);
    }
}

void MainWindow::showPullHandle(const document::Feature& feature) {
    const document::SketchItem* sketch = document()->findSketch(feature.sketch);
    if (feature.kind != document::FeatureKind::Pull || sketch == nullptr ||
        m_featurePanel->goesToFace() || !feature.extentFace.empty()) {
        m_viewport->clearDistanceHandle();
        return;
    }
    // From the middle of the outlines it pulls, along the sketch's normal.
    const std::vector<sketch::Region> regions = sketch->geometry.regions();
    const std::vector<kernel::Shape> faces = document::sketchRegionFaces(*sketch);
    std::optional<kernel::BoundingBox> box;
    for (std::size_t i = 0; i < regions.size() && i < faces.size(); ++i) {
        std::vector<sketch::EntityId> outline = regions[i].outer;
        std::sort(outline.begin(), outline.end());
        const bool used = feature.regions.empty() ||
                          std::find(feature.regions.begin(), feature.regions.end(), outline) !=
                              feature.regions.end();
        if (!used || faces[i].isNull()) {
            continue;
        }
        if (const auto bounds = kernel::boundingBox(faces[i])) {
            if (!box.has_value()) {
                box = bounds;
            } else {
                box->min = {std::min(box->min.x, bounds->min.x),
                            std::min(box->min.y, bounds->min.y),
                            std::min(box->min.z, bounds->min.z)};
                box->max = {std::max(box->max.x, bounds->max.x),
                            std::max(box->max.y, bounds->max.y),
                            std::max(box->max.z, bounds->max.z)};
            }
        }
    }
    if (!box.has_value()) {
        m_viewport->clearDistanceHandle();
        return;
    }
    const kernel::Point middle{(box->min.x + box->max.x) / 2.0, (box->min.y + box->max.y) / 2.0,
                               (box->min.z + box->max.z) / 2.0};
    // A pull to both sides evenly has half its distance each way; the arrow is at the front.
    const double distance = feature.symmetric ? feature.distance / 2.0 : feature.distance;
    m_viewport->setDistanceHandle(middle, sketch->plane.normal, distance);
}

void MainWindow::setPullDistance(double distance) {
    if (m_featurePanel == nullptr || m_featurePanel->kind() != document::FeatureKind::Pull) {
        return;
    }
    const bool symmetric = m_featurePanel->feature().symmetric;
    // The panel reports the change, which brings the preview and the arrow along.
    m_featurePanel->setValue(QStringLiteral("distance"), symmetric ? 2.0 * distance : distance);
}

void MainWindow::acceptFeature() {
    if (m_featurePanel == nullptr || !m_featurePanel->hasInput() ||
        !m_featurePanel->errorText().isEmpty()) {
        return;
    }
    document::Feature feature = m_featurePanel->feature();
    // Construction geometry on an origin plane or axis goes into the component being worked on.
    feature.component = activeComponent();
    const document::ObjectId editing = m_editedFeature;
    try {
        if (editing.isValid()) {
            m_editedFeature = {};
            m_timeline->setActiveItem({});
            document()->setEditedFeature({});
            document()->changeFeature(editing, feature);
        } else {
            document::Transaction transaction(*document(),
                                              featureKindName(feature.kind).toStdString());
            const document::ObjectId id = document()->addFeature(feature);
            // The sketch has done its work, and its lines would only clutter the solid.
            if (usesProfiles(feature.kind) || feature.kind == document::FeatureKind::Hole) {
                document()->setSketchVisible(feature.sketch, false);
            }
            transaction.commit();
            m_timeline->setSelectedItem(id);
        }
    } catch (const std::invalid_argument& failure) {
        QMessageBox::warning(this, tr("Could not apply the %1").arg(featureKindName(feature.kind)),
                             QString::fromUtf8(failure.what()));
        return;
    }
    closeFeaturePanel();
    // What was picked for the feature has done its work.
    m_viewport->clearSelection();
    m_browser->setSelection({}, {});
    updateSelectionState();
}

void MainWindow::deleteTimelineItem(document::ObjectId item) {
    if (document()->findFeature(item) != nullptr) {
        document()->removeFeature(item);
    } else if (document()->findSketch(item) != nullptr) {
        document::Transaction transaction(*document(), tr("Delete Sketch").toStdString());
        document()->removeSketch(item);
        transaction.commit();
    } else if (document()->findBody(item) != nullptr) {
        document::Transaction transaction(*document(), tr("Delete Body").toStdString());
        document()->removeBody(item);
        transaction.commit();
    }
    updateSelectionState();
}

void MainWindow::onTimelineSelection(document::ObjectId item) {
    // What an entry stands for is selected in the 3D view and the browser too: a sketch, a
    // body, or the body a feature makes or changes.
    if (m_featurePanel != nullptr || isSketching() || m_planePanel != nullptr ||
        m_panel != nullptr) {
        updateSelectionState();
        return;
    }
    std::vector<document::ObjectId> bodies;
    std::vector<document::ObjectId> sketches;
    if (const document::Feature* feature = document()->findFeature(item)) {
        const document::Body* body = document()->findBody(feature->body);
        if (body != nullptr && !body->shape.isNull()) {
            bodies.push_back(body->id);
        }
    } else if (document()->findSketch(item) != nullptr) {
        sketches.push_back(item);
    } else if (document()->findBody(item) != nullptr) {
        bodies.push_back(item);
    }
    m_browser->setSelection(bodies, sketches);
    std::vector<SelectedItem> items;
    for (const document::ObjectId id : bodies) {
        items.push_back({id, SelectedItem::Kind::Body, -1});
    }
    for (const document::ObjectId id : sketches) {
        items.push_back({id, SelectedItem::Kind::Sketch, -1});
    }
    m_selectionComesFromBrowser = true;
    m_viewport->setSelection(items);
    m_selectionComesFromBrowser = false;
    updateSelectionState();
    if (const document::Feature* feature = document()->findFeature(item)) {
        if (const std::string error = document()->featureError(item); !error.empty()) {
            statusBar()->showMessage(
                tr("%1 fails: %2")
                    .arg(QString::fromStdString(feature->name), QString::fromStdString(error)),
                12000);
        }
    }
}

void MainWindow::showTimelineMenu(document::ObjectId item, const QPoint& where) {
    m_commands->refresh();
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const auto action = [this](const char* id) { return m_commands->action(QLatin1String(id)); };
    if (document()->findFeature(item) != nullptr) {
        menu->addActions(
            {action("timeline.edit"), action("timeline.rename"), action("timeline.suppress")});
    } else if (document()->findSketch(item) != nullptr) {
        menu->addAction(action("timeline.edit"));
    }
    menu->addSeparator();
    menu->addActions({action("timeline.rollBack"), action("timeline.rollToEnd")});
    menu->addSeparator();
    menu->addAction(tr("&Delete"), this, [this, item] { deleteTimelineItem(item); });
    menu->popup(where);
}

// ---------------------------------------------------------------------------------------------
// Measuring

void MainWindow::startMeasure() {
    if (isSketching()) {
        return;
    }
    // What is selected already is the first thing measured, and the second if there are two.
    std::vector<SelectedItem> picked = m_viewport->selection();
    cancelCommand();
    m_measurePanel = new MeasurePanel(m_inspectorStack);
    m_measurePanel->setObjectName(QStringLiteral("measurePanel"));
    m_inspectorStack->addWidget(m_measurePanel);
    connect(m_measurePanel, &MeasurePanel::closed, this, &MainWindow::cancelCommand);
    m_filterBeforeMeasure = m_viewport->selectionFilter();
    m_viewport->setSelectionFilter(SelectionFilter::FacesEdgesVertices);
    if (picked.size() > 2) {
        picked.erase(picked.begin(), picked.end() - 2);
    }
    m_measured = picked;
    showInspector();
    m_inspectorDock->show();
    m_settingPicks = true;
    m_viewport->setSelection(m_measured);
    m_settingPicks = false;
    showMeasurement();
    m_commands->refresh();
}

void MainWindow::closeMeasure() {
    if (m_measurePanel == nullptr) {
        return;
    }
    m_viewport->setPreview(kernel::Shape());
    if (m_filterBeforeMeasure.has_value()) {
        m_viewport->setSelectionFilter(*m_filterBeforeMeasure);
        m_filterBeforeMeasure.reset();
    }
    m_measured.clear();
    m_inspectorStack->removeWidget(m_measurePanel);
    m_measurePanel->disconnect(this);
    m_measurePanel->deleteLater();
    m_measurePanel = nullptr;
    showInspector();
    updateSelectionState();
}

void MainWindow::takeMeasurePick() {
    const std::vector<SelectedItem> now = m_viewport->selection();
    if (now.size() >= 2) {
        // Picked together, with Shift: the last two.
        m_measured.assign(now.end() - 2, now.end());
    } else if (now.size() == 1) {
        // A click picks the next thing: the second, or after two, the first again.
        if (m_measured.size() == 1 && !(m_measured.front() == now.front())) {
            m_measured.push_back(now.front());
        } else {
            m_measured = now;
        }
    } else {
        m_measured.clear();
    }
    m_settingPicks = true;
    m_viewport->setSelection(m_measured);
    m_settingPicks = false;
    showMeasurement();
}

kernel::Shape MainWindow::shapeOf(const SelectedItem& item) const {
    if (item.kind == SelectedItem::Kind::Sketch) {
        const document::SketchItem* sketch = document()->findSketch(item.body);
        if (sketch == nullptr) {
            return {};
        }
        // Where the sketch is shown: at the first occurrence of its component.
        kernel::Placement at;
        for (const document::Instance& instance : document()->instances()) {
            if (instance.component == sketch->component) {
                at = instance.placement;
                break;
            }
        }
        return kernel::placed(document::sketchShape(*sketch, document::SketchPart::Ordinary), at);
    }
    const document::Body* body = document()->findBody(item.body);
    if (body == nullptr || body->shape.isNull()) {
        return {};
    }
    // Where it is in the assembly, so that things of different components measure against
    // each other.
    const kernel::Placement at = document()->placementOf(item.path);
    try {
        switch (item.kind) {
        case SelectedItem::Kind::Face:
            return kernel::placed(
                kernel::subShape(body->shape, kernel::SubShapeKind::Face, item.index), at);
        case SelectedItem::Kind::Edge:
            return kernel::placed(
                kernel::subShape(body->shape, kernel::SubShapeKind::Edge, item.index), at);
        case SelectedItem::Kind::Vertex:
            return kernel::placed(
                kernel::subShape(body->shape, kernel::SubShapeKind::Vertex, item.index), at);
        case SelectedItem::Kind::Body:
        case SelectedItem::Kind::Sketch:
            break;
        }
    } catch (const std::out_of_range&) {
        return {};
    }
    return kernel::placed(body->shape, at);
}

void MainWindow::showMeasurement() {
    if (m_measurePanel == nullptr) {
        return;
    }
    m_viewport->setPreview(kernel::Shape());
    const auto millimeters = [](double value) { return tr("%1 mm").arg(formatNumber(value)); };
    QList<QPair<QString, QString>> rows;
    if (m_measured.empty()) {
        m_measurePanel->show(tr("Nothing picked yet"), rows);
        return;
    }
    const kernel::Shape first = shapeOf(m_measured.front());
    if (m_measured.size() == 1 || first.isNull()) {
        // One thing: what it is, and its own measures.
        if (const auto radius = kernel::radiusOf(first)) {
            rows.append({tr("Radius"), millimeters(*radius)});
            rows.append({tr("Diameter"), millimeters(2.0 * *radius)});
        }
        switch (m_measured.front().kind) {
        case SelectedItem::Kind::Edge:
            rows.append({tr("Length"), millimeters(kernel::length(first))});
            break;
        case SelectedItem::Kind::Face:
            rows.append({tr("Area"), tr("%1 mm²").arg(formatNumber(kernel::surfaceArea(first)))});
            break;
        case SelectedItem::Kind::Vertex:
            if (const auto box = kernel::boundingBox(first)) {
                rows.append(
                    {tr("Position"), tr("%1, %2, %3 mm")
                                         .arg(formatNumber(box->min.x), formatNumber(box->min.y),
                                              formatNumber(box->min.z))});
            }
            break;
        case SelectedItem::Kind::Body:
            rows.append({tr("Volume"), tr("%1 mm³").arg(formatNumber(kernel::volume(first)))});
            break;
        case SelectedItem::Kind::Sketch:
            rows.append({tr("Length"), millimeters(kernel::length(first))});
            break;
        }
        m_measurePanel->show(describe(m_measured.front()).section(QStringLiteral(" · "), 0, 0),
                             rows);
        return;
    }
    const kernel::Shape second = shapeOf(m_measured.back());
    if (second.isNull()) {
        m_measurePanel->show(tr("That cannot be measured"), rows);
        return;
    }
    const kernel::Measurement between = kernel::measureBetween(first, second);
    rows.append({tr("Distance"), millimeters(between.distance)});
    rows.append({tr("Along X"), millimeters(between.to.x - between.from.x)});
    rows.append({tr("Along Y"), millimeters(between.to.y - between.from.y)});
    rows.append({tr("Along Z"), millimeters(between.to.z - between.from.z)});
    if (between.angle.has_value()) {
        rows.append({tr("Angle"), tr("%1°").arg(formatNumber(*between.angle))});
    }
    m_measurePanel->show(tr("%1 to %2")
                             .arg(describe(m_measured.front()).section(QStringLiteral(" · "), 0, 0),
                                  describe(m_measured.back()).section(QStringLiteral(" · "), 0, 0)),
                         rows);
    // The shortest distance, drawn between where it is.
    if (between.distance > 1e-9) {
        m_viewport->setPreview(kernel::makeSegment(between.from, between.to), true);
    }
}

// ---------------------------------------------------------------------------------------------
// Sketching

bool MainWindow::isSketching() const {
    return m_viewport->editedSketch().isValid() && m_viewport->sketchEditor()->isActive();
}

QString MainWindow::commandText() const {
    return m_commandLabel->text();
}

void MainWindow::startSketch() {
    if (isSketching()) {
        return;
    }
    // A construction plane that is selected, or else a flat face that is, is where the sketch
    // goes, and what it follows.
    const document::ObjectId construction = selectedConstructionPlane();
    const std::optional<kernel::Plane> face = selectedFacePlane();
    const auto faceName = selectedFaceName();
    cancelCommand();
    if (const auto state = document()->planeState(construction)) {
        createSketch(state->plane, construction);
        return;
    }
    if (face.has_value()) {
        if (faceName.has_value()) {
            createSketch(*face, faceName->first, faceName->second);
        } else {
            createSketch(*face);
        }
        return;
    }
    m_planePanel = new SketchPlanePanel(m_inspectorStack);
    m_inspectorStack->addWidget(m_planePanel);
    connect(m_planePanel, &SketchPlanePanel::planeChosen, this, [this](int which) {
        createSketch(which == 0   ? kernel::planeXY()
                     : which == 1 ? kernel::planeXZ()
                                  : kernel::planeYZ());
    });
    connect(m_planePanel, &SketchPlanePanel::canceled, this, &MainWindow::cancelCommand);
    // The three planes, large enough to click, in the 3D view.
    m_viewport->setPlanePicking(true);
    showInspector();
    m_inspectorDock->show();
    m_planePanel->setFocus();
    m_commands->refresh();
    statusBar()->showMessage(
        tr("Click a plane in the 3D view, or a flat face of a body, to sketch on it."), 9000);
}

document::ObjectId MainWindow::createSketch(const kernel::Plane& plane,
                                            document::ObjectId attachedTo,
                                            const std::string& attachedFace) {
    if (isSketching()) {
        finishSketch();
    }
    cancelCommand();
    // On a plane of the component being worked on; on a face, in the face's component.
    const document::ObjectId id = document()->addSketch(
        document()->uniqueSketchName("Sketch"), plane, attachedTo, attachedFace, activeComponent());
    editSketch(id);
    return id;
}

bool MainWindow::editSketch(document::ObjectId sketch) {
    cancelCommand();
    // A sketch of a component is edited where that component is being worked on.
    if (const document::SketchItem* found = document()->findSketch(sketch)) {
        ensureActive(found->component);
    }
    if (!m_viewport->editSketch(sketch)) {
        return false;
    }
    m_viewport->setFocus();
    return true;
}

void MainWindow::finishSketch() {
    m_viewport->finishSketch();
}

void MainWindow::onSketchEditingChanged() {
    const bool sketching = isSketching();
    // While a sketch is drawn, the features that depend on it wait: its outlines may be open
    // halfway through. They are built again once, when the sketch is finished.
    if (sketching && m_suspendedDocument == nullptr) {
        m_suspendedDocument = document();
        m_suspendedDocument->suspendRebuild();
    } else if (!sketching && m_suspendedDocument != nullptr) {
        document::Document* held = m_suspendedDocument;
        m_suspendedDocument = nullptr;
        held->resumeRebuild();
    }
    m_browser->setEditedSketch(m_viewport->editedSketch());
    if (sketching) {
        m_browser->setSelection({}, {m_viewport->editedSketch()});
        // The sketch needs its panel, even if the user had closed it.
        m_inspectorDock->show();
        statusBar()->showMessage(
            tr("Editing a sketch. Draw with the tools above; Finish Sketch returns to the model."),
            9000);
    } else {
        statusBar()->clearMessage();
    }
    updateMode();
    showInspector();
    updateSelectionState();
}

std::string MainWindow::thumbnail() {
    if (!m_viewport->isVisible() || !m_viewport->errorString().isEmpty()) {
        return {};
    }
    const QImage frame = m_viewport->grabFramebuffer();
    if (frame.isNull()) {
        return {};
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    frame.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buffer, "PNG");
    return bytes.toStdString();
}

// ---------------------------------------------------------------------------------------------
// The interactive File commands

bool MainWindow::confirmDiscard() {
    if (!document()->isModified()) {
        return true;
    }
    QMessageBox box(QMessageBox::Question, tr("Unsaved changes"),
                    tr("Save the changes to \"%1\" before closing it?").arg(documentName()),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setInformativeText(tr("If you do not save, the changes are lost."));
    box.setDefaultButton(QMessageBox::Save);
    switch (box.exec()) {
    case QMessageBox::Save:
        return fileSave();
    case QMessageBox::Discard:
        return true;
    default:
        return false;
    }
}

bool MainWindow::fileNew() {
    newDocument();
    return true;
}

bool MainWindow::fileOpen() {
    QString directory = QFileInfo(filePath()).absolutePath();
    if (filePath().isEmpty()) {
        directory = QSettings().value(QLatin1String(directoryKey)).toString();
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Document"), directory,
                                                      tr("LCAD documents (*.lcad);;All files (*)"));
    if (path.isEmpty()) {
        return false;
    }
    QString error;
    if (!loadFrom(path, &error)) {
        QMessageBox::critical(this, tr("Could not open the document"), error);
        return false;
    }
    if (!m_loadWarnings.isEmpty()) {
        QMessageBox::warning(this, tr("The document was opened with problems"),
                             m_loadWarnings.join(QLatin1Char('\n')));
    }
    return true;
}

bool MainWindow::fileSave() {
    if (filePath().isEmpty()) {
        return fileSaveAs();
    }
    QString error;
    if (!saveTo(filePath(), &error)) {
        QMessageBox::critical(this, tr("Could not save the document"), error);
        return false;
    }
    return true;
}

bool MainWindow::fileSaveAs() {
    QString suggestion = filePath();
    if (suggestion.isEmpty()) {
        const QString directory = QSettings().value(QLatin1String(directoryKey)).toString();
        suggestion = QDir(directory).filePath(documentName() + QStringLiteral(".lcad"));
    }
    QString path = QFileDialog::getSaveFileName(this, tr("Save Document"), suggestion,
                                                tr("LCAD documents (*.lcad)"));
    if (path.isEmpty()) {
        return false;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".lcad");
    }
    QString error;
    if (!saveTo(path, &error)) {
        QMessageBox::critical(this, tr("Could not save the document"), error);
        return false;
    }
    return true;
}

bool MainWindow::importGeometry(const QString& path, QString* error, QStringList* notes) {
    const auto fail = [error](const QString& why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool iges = suffix == QStringLiteral("igs") || suffix == QStringLiteral("iges");
    const bool step = suffix == QStringLiteral("step") || suffix == QStringLiteral("stp");
    kernel::ImportResult read;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        if (iges) {
            read = kernel::importIges(toPath(path));
        } else {
            try {
                read = kernel::importStep(toPath(path));
            } catch (const kernel::Error&) {
                // A file whose name does not say what it is may still be IGES.
                if (step) {
                    throw;
                }
                read = kernel::importIges(toPath(path));
            }
        }
    } catch (const kernel::Error& failure) {
        QApplication::restoreOverrideCursor();
        return fail(tr("Could not %1.").arg(QString::fromStdString(failure.what())));
    }
    QApplication::restoreOverrideCursor();
    if (isSketching()) {
        finishSketch();
    }
    cancelCommand();
    std::vector<document::ObjectId> added;
    try {
        document::Transaction transaction(
            *document(), tr("Import %1").arg(QFileInfo(path).fileName()).toStdString());
        for (const kernel::NamedBody& body : read.bodies) {
            added.push_back(document()->addBody(body.name, body.shape, activeComponent()));
        }
        transaction.commit();
    } catch (const std::invalid_argument& failure) {
        return fail(tr("Could not import %1: %2")
                        .arg(QFileInfo(path).fileName(), QString::fromUtf8(failure.what())));
    }
    if (notes != nullptr) {
        notes->clear();
        for (const std::string& note : read.notes) {
            notes->append(QString::fromStdString(note));
        }
    }
    m_browser->setSelection(added, {});
    m_viewport->fitAll();
    statusBar()->showMessage(
        (added.size() == 1 ? tr("Imported 1 body from %1.")
                           : tr("Imported %1 bodies from %2.").arg(added.size()))
                .arg(QFileInfo(path).fileName()) +
            QLatin1Char(' ') +
            tr("Change imported bodies with Offset Face, Move Face, and Delete Face."),
        12000);
    return true;
}

std::vector<document::ObjectId> MainWindow::exportedBodies(bool selectedOnly) const {
    // One for each appearance of a body: a component that appears twice is exported twice.
    std::vector<document::ObjectId> ids;
    for (const PlacedBody& placed : placedBodies(selectedOnly)) {
        ids.push_back(placed.body);
    }
    return ids;
}

bool MainWindow::exportGeometry(const QString& path, const ExportOptions& options, QString* error) {
    const auto fail = [error](const QString& why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    const std::vector<PlacedBody> placed = placedBodies(options.selectedOnly);
    if (placed.empty()) {
        return fail(tr("There is no body to export."));
    }
    // Each where it is in the assembly.
    std::vector<kernel::NamedBody> bodies;
    for (const PlacedBody& body : placed) {
        bodies.push_back({body.name.toStdString(), body.shape});
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        switch (options.format) {
        case ExportFormat::Step:
            kernel::exportStep(bodies, toPath(path));
            break;
        case ExportFormat::Stl:
            kernel::exportStl(bodies, toPath(path), options.mesh, options.stlEncoding);
            break;
        case ExportFormat::ThreeMf:
            kernel::export3mf(bodies, toPath(path), options.mesh);
            break;
        case ExportFormat::Obj:
            kernel::exportObj(bodies, toPath(path), options.mesh);
            break;
        }
    } catch (const kernel::Error& failure) {
        QApplication::restoreOverrideCursor();
        return fail(tr("Could not %1.").arg(QString::fromStdString(failure.what())));
    } catch (const std::invalid_argument& failure) {
        QApplication::restoreOverrideCursor();
        return fail(tr("Could not export: %1").arg(QString::fromUtf8(failure.what())));
    }
    QApplication::restoreOverrideCursor();
    statusBar()->showMessage((bodies.size() == 1
                                  ? tr("Exported 1 body to %1.")
                                  : tr("Exported %1 bodies to %2.").arg(bodies.size()))
                                 .arg(QFileInfo(path).fileName()),
                             9000);
    return true;
}

bool MainWindow::fileImport() {
    QSettings settings;
    QString directory = settings.value(QLatin1String(importDirectoryKey)).toString();
    if (directory.isEmpty()) {
        directory = settings.value(QLatin1String(directoryKey)).toString();
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import"), directory,
        tr("STEP and IGES files (*.step *.stp *.igs *.iges);;STEP files (*.step *.stp);;"
           "IGES files (*.igs *.iges);;All files (*)"));
    if (path.isEmpty()) {
        return false;
    }
    settings.setValue(QLatin1String(importDirectoryKey), QFileInfo(path).absolutePath());
    QString error;
    QStringList notes;
    if (!importGeometry(path, &error, &notes)) {
        QMessageBox::critical(this, tr("Could not import the file"), error);
        return false;
    }
    if (!notes.isEmpty()) {
        QMessageBox::information(this, tr("Imported %1").arg(QFileInfo(path).fileName()),
                                 notes.join(QStringLiteral("\n\n")));
    }
    return true;
}

bool MainWindow::fileExport() {
    const std::vector<document::ObjectId> shown = exportedBodies(false);
    const std::vector<document::ObjectId> selected = exportedBodies(true);
    if (shown.empty() && selected.empty()) {
        QMessageBox::information(this, tr("Export"), tr("There is no body to export."));
        return false;
    }
    QSettings settings;
    ExportOptions options;
    options.format = static_cast<ExportFormat>(
        settings.value(QLatin1String(exportFormatKey), static_cast<int>(ExportFormat::Step))
            .toInt());
    options.mesh.deviation =
        settings.value(QLatin1String(exportDeviationKey), options.mesh.deviation).toDouble();
    options.mesh.angle =
        settings.value(QLatin1String(exportAngleKey), options.mesh.angle).toDouble();
    options.stlEncoding = settings.value(QLatin1String(exportTextKey), false).toBool()
                              ? kernel::StlEncoding::Text
                              : kernel::StlEncoding::Binary;
    // What is selected is what is meant, when something is.
    options.selectedOnly = !selected.empty();
    ExportDialog dialog(static_cast<int>(shown.size()), static_cast<int>(selected.size()), this);
    dialog.setOptions(options);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    options = dialog.options();
    settings.setValue(QLatin1String(exportFormatKey), static_cast<int>(options.format));
    settings.setValue(QLatin1String(exportDeviationKey), options.mesh.deviation);
    settings.setValue(QLatin1String(exportAngleKey), options.mesh.angle);
    settings.setValue(QLatin1String(exportTextKey),
                      options.stlEncoding == kernel::StlEncoding::Text);

    QString directory = settings.value(QLatin1String(exportDirectoryKey)).toString();
    if (directory.isEmpty()) {
        directory = filePath().isEmpty() ? settings.value(QLatin1String(directoryKey)).toString()
                                         : QFileInfo(filePath()).absolutePath();
    }
    const QString extension = extensionOf(options.format);
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export"), QDir(directory).filePath(documentName() + QLatin1Char('.') + extension),
        filterOf(options.format));
    if (path.isEmpty()) {
        return false;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QLatin1Char('.') + extension;
    }
    settings.setValue(QLatin1String(exportDirectoryKey), QFileInfo(path).absolutePath());
    QString error;
    if (!exportGeometry(path, options, &error)) {
        QMessageBox::critical(this, tr("Could not export"), error);
        return false;
    }
    return true;
}

void MainWindow::checkGeometry() {
    std::vector<document::ObjectId> ids = exportedBodies(true);
    if (ids.empty()) {
        for (const document::Body& body : document()->bodies()) {
            if (!body.shape.isNull()) {
                ids.push_back(body.id);
            }
        }
    }
    std::vector<BodyCheck> results;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (const document::ObjectId id : ids) {
        const document::Body* body = document()->findBody(id);
        BodyCheck check{id, QString::fromStdString(body->name), {}};
        try {
            check.problems = kernel::checkGeometry(body->shape);
        } catch (const kernel::Error& failure) {
            check.problems.push_back(
                {"It could not be checked: " + failure.reason() + ".", {}, {}});
        }
        results.push_back(std::move(check));
    }
    QApplication::restoreOverrideCursor();
    if (m_checkDialog == nullptr) {
        m_checkDialog = new GeometryCheckDialog(this);
        connect(m_checkDialog, &GeometryCheckDialog::problemChosen, this,
                [this](document::ObjectId body, const std::vector<int>& faces,
                       const std::vector<int>& edges) {
                    if (document()->findBody(body) == nullptr) {
                        return;
                    }
                    // What the problem is about, picked in the 3D view, or the whole body.
                    std::vector<SelectedItem> items;
                    for (const int face : faces) {
                        items.push_back({body, SelectedItem::Kind::Face, face});
                    }
                    for (const int edge : edges) {
                        items.push_back({body, SelectedItem::Kind::Edge, edge});
                    }
                    if (items.empty()) {
                        items.push_back({body, SelectedItem::Kind::Body, -1});
                    }
                    m_viewport->setSelection(items);
                });
        connect(m_checkDialog, &GeometryCheckDialog::repairRequested, this,
                [this](document::ObjectId body) {
                    if (document()->findBody(body) == nullptr) {
                        return;
                    }
                    m_checkDialog->close();
                    m_viewport->setSelection({{body, SelectedItem::Kind::Body, -1}});
                    startFeature(document::FeatureKind::Repair);
                });
    }
    m_checkDialog->setResults(results);
    m_checkDialog->show();
    m_checkDialog->raise();
    const auto faulty = std::count_if(results.begin(), results.end(), [](const BodyCheck& check) {
        return !check.problems.empty();
    });
    statusBar()->showMessage(
        faulty == 0
            ? (results.size() == 1
                   ? tr("Checked 1 body: no problems found.")
                   : tr("Checked %1 bodies: no problems found.").arg(results.size()))
            : tr("Problems found in %1 of %2 bodies checked.").arg(faulty).arg(results.size()),
        9000);
}

bool MainWindow::fileClose(int index) {
    if (index < 0) {
        index = m_current;
    }
    if (index >= documentCount()) {
        return false;
    }
    // The question is about a document the user can see.
    setCurrentIndex(index);
    if (!confirmDiscard()) {
        return false;
    }
    closeDocument(index);
    return true;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    for (int i = 0; i < documentCount(); ++i) {
        if (!m_sessions[static_cast<std::size_t>(i)]->document->isModified()) {
            continue;
        }
        setCurrentIndex(i);
        if (!confirmDiscard()) {
            event->ignore();
            return;
        }
    }
    cancelCommand();
    // Remember the layout as it is for modeling, with its panels, not the sketch tools or the
    // 3D view on its own.
    finishSketch();
    setViewportFocus(false);
    QSettings settings;
    settings.setValue(QLatin1String(geometryKey), saveGeometry());
    settings.setValue(QLatin1String(layoutKey), saveState(layoutVersion));
    // A proper close: the copies of unsaved work are not needed again.
    if (m_recovery != nullptr) {
        m_recovery->close();
    }
    event->accept();
}

// ---------------------------------------------------------------------------------------------

QString MainWindow::aboutText() const {
    QString text = tr("LCAD %1\n\n"
                      "Free software under the GNU General Public License, version 3 or later. "
                      "It comes with no warranty.\n\n"
                      "Built with Qt %2 and Open CASCADE Technology %3.")
                       .arg(versionString(), QString::fromLatin1(qVersion()),
                            QString::fromStdString(kernel::occtVersion()));
    const QString graphics = m_viewport->graphicsInfo();
    if (!graphics.isEmpty()) {
        text += QStringLiteral("\n\n") + graphics;
    }
    const QString log = logFilePath();
    if (!log.isEmpty()) {
        text += QStringLiteral("\n\n") + tr("Log file: %1").arg(log);
    }
    return text;
}

void MainWindow::showAbout() {
    QMessageBox::about(this, tr("About LCAD"), aboutText());
}

void MainWindow::showShortcuts() {
    ShortcutsDialog dialog(m_commands, this);
    dialog.exec();
}

void MainWindow::showViewportError(const QString& reason) {
    QMessageBox::critical(this, tr("3D viewport unavailable"), reason);
}

} // namespace lcad::app
