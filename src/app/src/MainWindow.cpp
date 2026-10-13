// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/MainWindow.h>

#include <lcad/app/Application.h>
#include <lcad/app/BrowserPanel.h>
#include <lcad/app/CommandSearch.h>
#include <lcad/app/Commands.h>
#include <lcad/app/Icons.h>
#include <lcad/app/PropertiesPanel.h>
#include <lcad/app/ShortcutsDialog.h>
#include <lcad/app/SketchPanel.h>
#include <lcad/app/Theme.h>
#include <lcad/document/File.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Version.h>
#include <lcad/viewport/SketchEditor.h>

#include <QBuffer>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
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
const int recentLimit = 10;

std::filesystem::path toPath(const QString& path) {
    return std::filesystem::path(QFile::encodeName(path).toStdString());
}

QLabel* groupLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text.toUpper(), parent);
    label->setObjectName(QStringLiteral("groupLabel"));
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() * Theme::instance().tokens().secondaryScale);
    label->setFont(font);
    return label;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), m_viewport(new viewport::ViewportWidget(this)),
      m_commands(new CommandRegistry(this)) {
    resize(1280, 800);
    setCentralWidget(m_viewport);
    m_viewport->setContextMenuPolicy(Qt::CustomContextMenu);

    createDocks();
    defineCommands();
    createTopBar();
    createToolbar();
    createSketchToolbar();
    m_search = new CommandSearch(m_commands, this);

    m_selectionLabel = new QLabel(this);
    m_selectionLabel->setObjectName(QStringLiteral("selectionSummary"));
    statusBar()->addWidget(m_selectionLabel, 1);
    m_commandLabel = new QLabel(this);
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
        std::vector<SelectedItem> items;
        for (const document::ObjectId id : m_browser->selectedBodies()) {
            items.push_back({id, SelectedItem::Kind::Body, -1});
        }
        for (const document::ObjectId id : m_browser->selectedSketches()) {
            items.push_back({id, SelectedItem::Kind::Sketch, -1});
        }
        m_viewport->setSelection(items);
        m_selectionComesFromBrowser = false;
        updateSelectionState();
    });
    connect(m_viewport, &viewport::ViewportWidget::selectionChanged, this, [this] {
        if (m_planePanel != nullptr) {
            // A new sketch is waiting for its plane: a flat face that is clicked is it.
            const std::vector<SelectedItem> picked = m_viewport->selection();
            if (picked.size() == 1 && picked.front().kind == SelectedItem::Kind::Face) {
                if (const auto plane = selectedFacePlane()) {
                    const kernel::Plane chosen = *plane;
                    // Later: this is the viewport telling what was clicked, which is no time to
                    // change what it shows.
                    QMetaObject::invokeMethod(
                        this, [this, chosen] { createSketch(chosen); }, Qt::QueuedConnection);
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
            m_browser->setSelection(bodies, sketches);
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
    // The viewport and the panels are destroyed after the documents, as child widgets. Detach
    // them first.
    m_viewport->setDocument(nullptr);
    m_browser->setDocument(nullptr, {});
    m_properties->refresh(nullptr, {}, {});
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
            if (isSketching()) {
                m_viewport->sketchEditor()->deleteSelection();
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
            return !selectedBodies().empty() || !selectedSketches().empty();
        };
        m_commands->add(std::move(c));

        c = make("edit.cancel", tr("&Cancel Command"), edit,
                 tr("Leave the command or the sketch tool in progress"));
        c.shortcuts = {QKeySequence(Qt::Key_Escape)};
        c.aliases = {tr("escape"), tr("abort")};
        c.run = [this](bool) {
            if (m_panel != nullptr || m_planePanel != nullptr) {
                cancelCommand();
            } else if (isSketching()) {
                m_viewport->sketchEditor()->cancel();
            }
        };
        c.isEnabled = [this] {
            return m_panel != nullptr || m_planePanel != nullptr ||
                   (isSketching() && m_viewport->sketchEditor()->canCancel());
        };
        m_commands->add(std::move(c));

        c = make("body.rename", tr("Re&name"), edit, tr("Rename the selected body or sketch"));
        c.shortcuts = {QKeySequence(Qt::Key_F2)};
        c.run = [this](bool) {
            const auto bodies = selectedBodies();
            const auto sketches = selectedSketches();
            if (bodies.size() == 1 && sketches.empty()) {
                m_browser->editBody(bodies.front());
            } else if (sketches.size() == 1 && bodies.empty()) {
                m_browser->editSketchName(sketches.front());
            }
        };
        c.isEnabled = [this] { return selectedBodies().size() + selectedSketches().size() == 1; };
        m_commands->add(std::move(c));

        // Hide and Show act on bodies and sketches alike.
        const auto anyShown = [this](bool shown) {
            const auto bodies = targetBodies();
            const auto sketches = selectedSketches();
            return std::any_of(bodies.begin(), bodies.end(),
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
        c.run = [this](bool) { setBodiesVisible(targetBodies(), false, tr("Hide")); };
        c.isEnabled = [this, anyShown] { return !isSketching() && anyShown(true); };
        m_commands->add(std::move(c));

        c = make("body.show", tr("&Show"), edit, tr("Show the selected bodies and sketches"));
        c.iconName = QStringLiteral("eye");
        c.aliases = {tr("visible"), tr("unhide")};
        c.run = [this](bool) { setBodiesVisible(targetBodies(), true, tr("Show")); };
        c.isEnabled = [this, anyShown] { return !isSketching() && anyShown(false); };
        m_commands->add(std::move(c));

        c = make("body.isolate", tr("&Isolate"), edit,
                 tr("Hide every body except the selected ones"));
        c.iconName = QStringLiteral("isolate");
        c.aliases = {tr("hide others"), tr("solo")};
        c.run = [this](bool) {
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
        c.isEnabled = [this] { return !targetBodies().empty() && document()->bodies().size() > 1; };
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
        c.run = [this, kind = kind](bool) { startPrimitive(kind); };
        c.isEnabled = [this] { return !isSketching(); };
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
        c.run = [this, filter = filter](bool) { m_viewport->setSelectionFilter(filter); };
        c.isChecked = [this, filter = filter] { return m_viewport->selectionFilter() == filter; };
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
        c.run = [this, standardView = standardView](bool) {
            m_viewport->setStandardView(standardView);
        };
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
        c.run = [this, style = style](bool) {
            m_viewport->setDisplayStyle(style);
            QSettings().setValue(QLatin1String(styleKey), static_cast<int>(style));
            // The toolbar's style button shows the icon of the style in use.
            applyTheme();
        };
        c.isChecked = [this, style = style] { return m_viewport->displayStyle() == style; };
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
        c.run = [this, preset = preset](bool) {
            m_viewport->setNavigationPreset(preset);
            QSettings().setValue(QLatin1String(navigationKey), static_cast<int>(preset));
        };
        c.isChecked = [this, preset = preset] { return m_viewport->navigationPreset() == preset; };
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
        c.run = [choice = choice](bool) { Theme::instance().setChoice(choice); };
        c.isChecked = [choice = choice] { return Theme::instance().choice() == choice; };
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
        c.run = [dock = dock](bool on) { dock->setVisible(on); };
        c.isChecked = [dock = dock] { return !dock->isHidden(); };
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
            c.run = [this, which = which](bool) {
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
        c.run = [editor, kind = kind](bool) { editor()->constrain(kind); };
        c.isEnabled = sketching;
        c.isChecked = [this, editor, kind = kind] {
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
    file->addActions({action("file.close"), action("file.quit")});

    QMenu* edit = m_menuBar->addMenu(tr("&Edit"));
    edit->addActions({action("edit.undo"), action("edit.redo")});
    edit->addSeparator();
    edit->addActions({action("body.rename"), action("body.hide"), action("body.show"),
                      action("body.isolate"), action("body.showAll")});
    edit->addSeparator();
    edit->addActions({action("edit.delete"), action("edit.cancel")});

    QMenu* create = m_menuBar->addMenu(tr("&Create"));
    for (const char* id :
         {"create.box", "create.cylinder", "create.sphere", "create.cone", "create.torus"}) {
        create->addAction(action(id));
    }

    QMenu* sketchMenu = m_menuBar->addMenu(tr("S&ketch"));
    sketchMenu->addAction(action("sketch.new"));
    QMenu* onPlane = sketchMenu->addMenu(tr("New Sketch &On"));
    onPlane->addActions(
        {action("sketch.new.xy"), action("sketch.new.xz"), action("sketch.new.yz")});
    sketchMenu->addActions({action("sketch.edit"), action("sketch.finish")});
    sketchMenu->addSeparator();
    for (const char* id : {"sketch.select", "sketch.line", "sketch.rectangle", "sketch.circle",
                           "sketch.arc", "sketch.point", "sketch.polygon", "sketch.slot"}) {
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

    QMenu* select = m_menuBar->addMenu(tr("&Select"));
    for (const char* id : {"select.everything", "select.bodies", "select.faces", "select.edges",
                           "select.vertices"}) {
        select->addAction(action(id));
    }

    QMenu* view = m_menuBar->addMenu(tr("&View"));
    view->addActions({action("view.fit"), action("view.fitSelection")});
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
    toolbar->addWidget(groupLabel(tr("Create"), toolbar));
    toolbar->addAction(action("sketch.new"));
    for (const char* id :
         {"create.box", "create.cylinder", "create.sphere", "create.cone", "create.torus"}) {
        toolbar->addAction(action(id));
    }
    toolbar->addSeparator();

    toolbar->addWidget(groupLabel(tr("Select"), toolbar));
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

    toolbar->addWidget(groupLabel(tr("View"), toolbar));
    for (const char* id : {"view.fit", "view.fitSelection", "view.grid"}) {
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

    toolbar->addWidget(groupLabel(tr("Draw"), toolbar));
    for (const char* id : {"sketch.select", "sketch.line", "sketch.rectangle", "sketch.circle",
                           "sketch.arc", "sketch.point", "sketch.polygon", "sketch.slot"}) {
        toolbar->addAction(action(id));
    }
    toolbar->addAction(action("sketch.construction"));
    toolbar->addSeparator();
    toolbar->addWidget(groupLabel(tr("Modify"), toolbar));
    for (const char* id :
         {"sketch.trim", "sketch.extend", "sketch.offset", "sketch.mirror", "sketch.project"}) {
        toolbar->addAction(action(id));
    }
    toolbar->addSeparator();
    toolbar->addWidget(groupLabel(tr("Constrain"), toolbar));
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
    const auto setStyle = [this](Qt::ToolButtonStyle style) {
        for (const char* id :
             {"create.box", "create.cylinder", "create.sphere", "create.cone", "create.torus"}) {
            if (auto* button = qobject_cast<QToolButton*>(
                    m_modelToolbar->widgetForAction(m_commands->action(QLatin1String(id))))) {
                button->setToolButtonStyle(style);
            }
        }
    };
    setStyle(Qt::ToolButtonTextBesideIcon);
    if (m_modelToolbar->sizeHint().width() > width()) {
        setStyle(Qt::ToolButtonIconOnly);
    }

    // The sketch toolbar gives up the constraints' own buttons for one that lists them.
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
    if (m_sketchToolbar->sizeHint().width() > width()) {
        fold(true);
    }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    fitToolbar();
}

void MainWindow::createDocks() {
    m_browser = new BrowserPanel(this);
    m_browserDock = new QDockWidget(tr("Browser"), this);
    m_browserDock->setObjectName(QStringLiteral("browserDock"));
    m_browserDock->setWidget(m_browser);
    addDockWidget(Qt::LeftDockWidgetArea, m_browserDock);

    // A strip, not a panel with a title bar of its own: it has to leave the height to the 3D
    // view.
    auto* timeline = new QWidget(this);
    auto* strip = new QHBoxLayout(timeline);
    strip->setContentsMargins(0, 4, 8, 4);
    strip->addWidget(groupLabel(tr("Timeline"), timeline));
    auto* empty = new QLabel(tr("No features yet"), timeline);
    empty->setObjectName(QStringLiteral("timeline"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setToolTip(tr("Sketches and features will be listed here in the order they were made."));
    strip->addWidget(empty, 1);
    timeline->setMinimumHeight(34);
    m_timelineDock = new QDockWidget(tr("Timeline"), this);
    m_timelineDock->setObjectName(QStringLiteral("timelineDock"));
    m_timelineDock->setTitleBarWidget(new QWidget(m_timelineDock));
    m_timelineDock->setWidget(timeline);
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
        m_sessions.erase(m_sessions.begin() + replaced);
        {
            const QSignalBlocker blocker(m_tabs);
            m_tabs->removeTab(replaced);
        }
        --index;
    }
    activate(index);
    rememberFile(absolute);
    statusBar()->showMessage(tr("Opened %1").arg(documentName()), 4000);
    return true;
}

bool MainWindow::saveTo(const QString& path, QString* error) {
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
    if (wasCurrent) {
        cancelCommand();
        m_viewport->setDocument(nullptr);
        m_browser->setDocument(nullptr, {});
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
    } else if (m_panel == nullptr && m_planePanel == nullptr) {
        // A sketch selected in the browser while it is hidden is not in the viewport's
        // selection, and is what is selected all the same.
        std::vector<SelectedItem> parts = m_viewport->selection();
        for (const document::ObjectId id : m_browser->selectedSketches()) {
            const SelectedItem item{id, SelectedItem::Kind::Sketch, -1};
            if (std::find(parts.begin(), parts.end(), item) == parts.end()) {
                parts.push_back(item);
            }
        }
        m_properties->refresh(document(), selectedBodies(), parts);
    }
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
        document()->addBody(document()->uniqueBodyName(name.toStdString()), m_panel->shape());
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
    // A flat face that is selected is where the sketch goes.
    const std::optional<kernel::Plane> face = selectedFacePlane();
    cancelCommand();
    if (face.has_value()) {
        createSketch(*face);
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

document::ObjectId MainWindow::createSketch(const kernel::Plane& plane) {
    if (isSketching()) {
        finishSketch();
    }
    cancelCommand();
    const document::ObjectId id =
        document()->addSketch(document()->uniqueSketchName("Sketch"), plane);
    editSketch(id);
    return id;
}

bool MainWindow::editSketch(document::ObjectId sketch) {
    cancelCommand();
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
