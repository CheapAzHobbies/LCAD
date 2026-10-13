// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/MainWindow.h>

#include <lcad/app/Application.h>
#include <lcad/app/BrowserPanel.h>
#include <lcad/document/File.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Version.h>
#include <lcad/viewport/ViewportWidget.h>

#include <QAction>
#include <QActionGroup>
#include <QBuffer>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace lcad::app {

namespace {

using viewport::SelectedItem;
using viewport::SelectionFilter;
using viewport::StandardView;

const char* const reverseZoomKey = "view/reverseZoom";

std::filesystem::path toPath(const QString& path) {
    return std::filesystem::path(QFile::encodeName(path).toStdString());
}

// Three decimals at most, without trailing zeros: 6000, 12.5, 0.125.
QString formatNumber(double value) {
    QString text = QString::number(value, 'f', 3);
    while (text.endsWith(QLatin1Char('0'))) {
        text.chop(1);
    }
    if (text.endsWith(QLatin1Char('.'))) {
        text.chop(1);
    }
    return text == QStringLiteral("-0") ? QStringLiteral("0") : text;
}

QLabel* groupLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text.toUpper(), parent);
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() * 0.8);
    font.setBold(true);
    label->setFont(font);
    label->setEnabled(false);
    label->setContentsMargins(8, 0, 4, 0);
    return label;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), m_viewport(new viewport::ViewportWidget(this)) {
    resize(1280, 800);
    setCentralWidget(m_viewport);

    createActions();
    createTopBar();
    createToolbar();
    createDocks();

    m_selectionLabel = new QLabel(this);
    m_selectionLabel->setObjectName(QStringLiteral("selectionSummary"));
    statusBar()->addWidget(m_selectionLabel, 1);
    auto* units = new QLabel(tr("mm"), this);
    units->setToolTip(tr("Lengths are in millimeters."));
    statusBar()->addPermanentWidget(units);

    // The browser and the viewport show one selection of bodies. Whichever of them the user
    // changed it in tells the other, and the other's report of that is not sent back: a hidden
    // body can be selected in the browser only, and the viewport's answer that it has nothing
    // selected must not clear it there.
    connect(m_browser, &BrowserPanel::bodySelectionChanged, this, [this] {
        m_selectionComesFromBrowser = true;
        m_viewport->setSelectedBodies(m_browser->selectedBodies());
        m_selectionComesFromBrowser = false;
        updateSelectionState();
    });
    connect(m_browser, &BrowserPanel::deleteRequested, this, &MainWindow::deleteSelectedBodies);
    connect(m_viewport, &viewport::ViewportWidget::selectionChanged, this, [this] {
        if (!m_selectionComesFromBrowser) {
            std::vector<document::ObjectId> bodies;
            for (const SelectedItem& item : m_viewport->selection()) {
                if (item.kind == SelectedItem::Kind::Body) {
                    bodies.push_back(item.body);
                }
            }
            m_browser->setSelectedBodies(bodies);
        }
        updateSelectionState();
    });
    // Queued: the viewport reports failure while painting, which is no place to open a dialog.
    connect(m_viewport, &viewport::ViewportWidget::initializationFailed, this,
            &MainWindow::showViewportError, Qt::QueuedConnection);

    setDocument(std::make_unique<document::Document>(), QString());
}

MainWindow::~MainWindow() {
    // The viewport and the browser are destroyed after the document, as child widgets. Detach
    // them from it first.
    m_viewport->setDocument(nullptr);
    m_browser->setDocument(nullptr, {});
}

void MainWindow::createActions() {
    const auto icon = [this](const char* themeName, QStyle::StandardPixmap fallback) {
        return QIcon::fromTheme(QString::fromLatin1(themeName), style()->standardIcon(fallback));
    };
    const auto action = [this](const QString& text, const char* name, const QString& tip) {
        auto* created = new QAction(text, this);
        created->setObjectName(QString::fromLatin1(name));
        created->setToolTip(tip);
        created->setStatusTip(tip);
        return created;
    };

    m_newAction = action(tr("&New"), "file.new", tr("Start a new document"));
    m_newAction->setIcon(icon("document-new", QStyle::SP_FileIcon));
    m_newAction->setShortcut(QKeySequence::New);
    connect(m_newAction, &QAction::triggered, this, &MainWindow::fileNew);

    m_openAction = action(tr("&Open…"), "file.open", tr("Open a document"));
    m_openAction->setIcon(icon("document-open", QStyle::SP_DialogOpenButton));
    m_openAction->setShortcut(QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::fileOpen);

    m_saveAction = action(tr("&Save"), "file.save", tr("Save the document"));
    m_saveAction->setIcon(icon("document-save", QStyle::SP_DialogSaveButton));
    m_saveAction->setShortcut(QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::fileSave);

    m_saveAsAction =
        action(tr("Save &As…"), "file.saveAs", tr("Save the document under another name"));
    m_saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(m_saveAsAction, &QAction::triggered, this, &MainWindow::fileSaveAs);

    m_quitAction = action(tr("&Quit"), "file.quit", tr("Close LCAD"));
    m_quitAction->setShortcut(QKeySequence::Quit);
    connect(m_quitAction, &QAction::triggered, this, &QWidget::close);

    m_undoAction = action(tr("&Undo"), "edit.undo", tr("Undo the last change"));
    m_undoAction->setIcon(icon("edit-undo", QStyle::SP_ArrowBack));
    m_undoAction->setShortcut(QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this, [this] {
        if (m_document->canUndo()) {
            m_document->undo();
        }
    });

    m_redoAction = action(tr("&Redo"), "edit.redo", tr("Redo the change that was undone"));
    m_redoAction->setIcon(icon("edit-redo", QStyle::SP_ArrowForward));
    m_redoAction->setShortcuts(
        {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), QKeySequence(Qt::CTRL | Qt::Key_Y)});
    connect(m_redoAction, &QAction::triggered, this, [this] {
        if (m_document->canRedo()) {
            m_document->redo();
        }
    });

    m_deleteAction = action(tr("&Delete"), "edit.delete", tr("Delete the selected bodies"));
    m_deleteAction->setShortcut(QKeySequence::Delete);
    connect(m_deleteAction, &QAction::triggered, this, &MainWindow::deleteSelectedBodies);

    m_cancelAction =
        action(tr("&Cancel Command"), "edit.cancel", tr("Leave the command in progress"));
    m_cancelAction->setShortcut(Qt::Key_Escape);
    m_cancelAction->setEnabled(false);
    connect(m_cancelAction, &QAction::triggered, this, &MainWindow::cancelCommand);

    const std::pair<PrimitiveKind, const char*> primitives[] = {
        {PrimitiveKind::Box, "create.box"},       {PrimitiveKind::Cylinder, "create.cylinder"},
        {PrimitiveKind::Sphere, "create.sphere"}, {PrimitiveKind::Cone, "create.cone"},
        {PrimitiveKind::Torus, "create.torus"},
    };
    for (const auto& [kind, name] : primitives) {
        QAction* create = action(primitiveName(kind), name,
                                 tr("Create a %1 by its sizes").arg(primitiveName(kind).toLower()));
        connect(create, &QAction::triggered, this, [this, kind = kind] { startPrimitive(kind); });
        m_createActions.append(create);
    }

    QAction* fit = action(tr("&Fit"), "view.fit", tr("Bring everything into view"));
    fit->setShortcut(Qt::Key_F6);
    connect(fit, &QAction::triggered, m_viewport, &viewport::ViewportWidget::fitAll);
    m_viewActions.append(fit);

    const std::tuple<StandardView, QString, const char*> views[] = {
        {StandardView::Home, tr("&Home"), "view.home"},
        {StandardView::Front, tr("F&ront"), "view.front"},
        {StandardView::Back, tr("&Back"), "view.back"},
        {StandardView::Left, tr("&Left"), "view.left"},
        {StandardView::Right, tr("R&ight"), "view.right"},
        {StandardView::Top, tr("&Top"), "view.top"},
        {StandardView::Bottom, tr("B&ottom"), "view.bottom"},
    };
    for (const auto& [view, text, name] : views) {
        QAction* look = action(text, name, tr("Turn the camera to this view"));
        if (view == StandardView::Home) {
            look->setShortcut(Qt::Key_Home);
        }
        connect(look, &QAction::triggered, this,
                [this, view = view] { m_viewport->setStandardView(view); });
        m_viewActions.append(look);
    }

    m_perspectiveAction = action(tr("&Perspective"), "view.perspective",
                                 tr("Switch between perspective and orthographic projection"));
    m_perspectiveAction->setCheckable(true);
    connect(m_perspectiveAction, &QAction::toggled, this, [this](bool on) {
        m_viewport->setProjection(on ? viewport::Projection::Perspective
                                     : viewport::Projection::Orthographic);
    });

    m_reverseZoomAction = action(tr("Reverse &Zoom Direction"), "view.reverseZoom",
                                 tr("Make rolling the wheel away from you zoom out instead of in"));
    m_reverseZoomAction->setCheckable(true);
    m_reverseZoomAction->setChecked(QSettings().value(QLatin1String(reverseZoomKey)).toBool());
    m_viewport->setZoomReversed(m_reverseZoomAction->isChecked());
    connect(m_reverseZoomAction, &QAction::toggled, this, [this](bool on) {
        m_viewport->setZoomReversed(on);
        QSettings().setValue(QLatin1String(reverseZoomKey), on);
    });
}

void MainWindow::createTopBar() {
    auto* topBar = new QWidget(this);
    topBar->setObjectName(QStringLiteral("topBar"));
    auto* layout = new QHBoxLayout(topBar);
    layout->setContentsMargins(2, 0, 10, 0);
    layout->setSpacing(2);

    m_menuBar = new QMenuBar(topBar);
    m_menuBar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    layout->addWidget(m_menuBar);

    QMenu* file = m_menuBar->addMenu(tr("&File"));
    file->addActions({m_newAction, m_openAction});
    file->addSeparator();
    file->addActions({m_saveAction, m_saveAsAction});
    file->addSeparator();
    file->addAction(m_quitAction);

    QMenu* edit = m_menuBar->addMenu(tr("&Edit"));
    edit->addActions({m_undoAction, m_redoAction});
    edit->addSeparator();
    edit->addActions({m_deleteAction, m_cancelAction});

    QMenu* create = m_menuBar->addMenu(tr("&Create"));
    create->addActions(m_createActions);

    QMenu* view = m_menuBar->addMenu(tr("&View"));
    view->setObjectName(QStringLiteral("menu.view"));
    view->addActions(m_viewActions);
    view->addSeparator();
    view->addActions({m_perspectiveAction, m_reverseZoomAction});

    QMenu* help = m_menuBar->addMenu(tr("&Help"));
    QAction* about = help->addAction(tr("&About LCAD"));
    about->setObjectName(QStringLiteral("help.about"));
    connect(about, &QAction::triggered, this, &MainWindow::showAbout);

    layout->addSpacing(12);
    for (QAction* quick : {m_newAction, m_openAction, m_saveAction, m_undoAction, m_redoAction}) {
        auto* button = new QToolButton(topBar);
        button->setDefaultAction(quick);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        layout->addWidget(button);
        if (quick == m_saveAction) {
            layout->addSpacing(8);
        }
    }
    layout->addStretch(1);

    m_documentLabel = new QLabel(topBar);
    m_documentLabel->setObjectName(QStringLiteral("documentName"));
    layout->addWidget(m_documentLabel);

    setMenuWidget(topBar);
}

void MainWindow::createToolbar() {
    QToolBar* toolbar = addToolBar(tr("Modeling"));
    toolbar->setObjectName(QStringLiteral("modelingToolbar"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toolbar->setContextMenuPolicy(Qt::PreventContextMenu);

    toolbar->addWidget(groupLabel(tr("Create"), toolbar));
    toolbar->addActions(m_createActions);
    toolbar->addSeparator();

    toolbar->addWidget(groupLabel(tr("Select"), toolbar));
    m_filterBox = new QComboBox(toolbar);
    m_filterBox->setObjectName(QStringLiteral("selectionFilter"));
    m_filterBox->setToolTip(tr("What a click in the 3D view picks"));
    m_filterBox->addItem(tr("Faces, Edges, Vertices"),
                         static_cast<int>(SelectionFilter::FacesEdgesVertices));
    m_filterBox->addItem(tr("Bodies"), static_cast<int>(SelectionFilter::Bodies));
    m_filterBox->addItem(tr("Faces"), static_cast<int>(SelectionFilter::Faces));
    m_filterBox->addItem(tr("Edges"), static_cast<int>(SelectionFilter::Edges));
    m_filterBox->addItem(tr("Vertices"), static_cast<int>(SelectionFilter::Vertices));
    connect(m_filterBox, &QComboBox::currentIndexChanged, this, [this] {
        m_viewport->setSelectionFilter(
            static_cast<SelectionFilter>(m_filterBox->currentData().toInt()));
    });
    toolbar->addWidget(m_filterBox);
}

void MainWindow::createDocks() {
    m_browser = new BrowserPanel(this);
    auto* browserDock = new QDockWidget(tr("Browser"), this);
    browserDock->setObjectName(QStringLiteral("browserDock"));
    browserDock->setWidget(m_browser);
    addDockWidget(Qt::LeftDockWidgetArea, browserDock);

    auto* timeline = new QLabel(tr("No features yet"), this);
    timeline->setObjectName(QStringLiteral("timeline"));
    timeline->setAlignment(Qt::AlignCenter);
    timeline->setEnabled(false);
    timeline->setMinimumHeight(48);
    timeline->setToolTip(
        tr("Sketches and features will be listed here in the order they were made."));
    auto* timelineDock = new QDockWidget(tr("Timeline"), this);
    timelineDock->setObjectName(QStringLiteral("timelineDock"));
    timelineDock->setWidget(timeline);
    addDockWidget(Qt::BottomDockWidgetArea, timelineDock);
    // The browser runs the full height of the window; the timeline sits under the viewport.
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);

    m_toolDock = new QDockWidget(this);
    m_toolDock->setObjectName(QStringLiteral("toolDock"));
    m_toolDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, m_toolDock);
    m_toolDock->hide();

    QMenu* view = m_menuBar->findChild<QMenu*>(QStringLiteral("menu.view"));
    view->addSeparator();
    view->addActions({browserDock->toggleViewAction(), timelineDock->toggleViewAction()});
}

QString MainWindow::documentName() const {
    return m_filePath.isEmpty() ? tr("Untitled") : QFileInfo(m_filePath).completeBaseName();
}

void MainWindow::setDocument(std::unique_ptr<document::Document> document, const QString& path) {
    cancelCommand();
    // Nothing may still be watching the old document when it goes.
    m_viewport->setDocument(nullptr);
    m_browser->setDocument(nullptr, {});
    m_document = std::move(document);
    m_filePath = path;
    m_listener = m_document->addListener([this](const document::Change& change) {
        if (change.kind == document::Change::Kind::HistoryChanged) {
            updateDocumentState();
        } else {
            updateSelectionState();
        }
    });
    m_viewport->setStandardView(StandardView::Home);
    m_viewport->setDocument(m_document.get());
    m_browser->setDocument(m_document.get(), documentName());
    updateDocumentState();
    updateSelectionState();
}

void MainWindow::updateDocumentState() {
    const QString undoLabel = QString::fromStdString(m_document->undoLabel());
    const QString redoLabel = QString::fromStdString(m_document->redoLabel());
    m_undoAction->setEnabled(m_document->canUndo());
    m_undoAction->setText(undoLabel.isEmpty() ? tr("&Undo") : tr("&Undo %1").arg(undoLabel));
    m_redoAction->setEnabled(m_document->canRedo());
    m_redoAction->setText(redoLabel.isEmpty() ? tr("&Redo") : tr("&Redo %1").arg(redoLabel));

    const QString name = documentName();
    setWindowTitle(QStringLiteral("%1[*] - LCAD").arg(name));
    setWindowModified(m_document->isModified());
    m_documentLabel->setText(m_document->isModified() ? tr("%1 (unsaved changes)").arg(name)
                                                      : name);
    m_browser->setDocumentName(name);
}

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

void MainWindow::updateSelectionState() {
    m_deleteAction->setEnabled(!selectedBodies().empty());
    m_selectionLabel->setText(selectionSummary());
}

QString MainWindow::selectionSummary() const {
    const std::vector<SelectedItem> picked = m_viewport->selection();
    const std::vector<document::ObjectId> bodies = selectedBodies();
    const std::size_t parts = static_cast<std::size_t>(
        std::count_if(picked.begin(), picked.end(), [](const SelectedItem& item) {
            return item.kind != SelectedItem::Kind::Body;
        }));
    const std::size_t total = parts + bodies.size();
    if (total == 0) {
        return {};
    }
    if (total > 1) {
        return tr("%1 items selected").arg(total);
    }

    if (parts == 0) {
        const document::Body* body = m_document->findBody(bodies.front());
        if (body == nullptr) {
            return {};
        }
        return tr("%1 · volume %2 mm³ · area %3 mm²")
            .arg(QString::fromStdString(body->name), formatNumber(kernel::volume(body->shape)),
                 formatNumber(kernel::surfaceArea(body->shape)));
    }

    const auto item = std::find_if(picked.begin(), picked.end(), [](const SelectedItem& entry) {
        return entry.kind != SelectedItem::Kind::Body;
    });
    const document::Body* body = m_document->findBody(item->body);
    if (body == nullptr) {
        return {};
    }
    const QString owner = QString::fromStdString(body->name);
    try {
        switch (item->kind) {
        case SelectedItem::Kind::Face:
            return tr("Face of %1 · area %2 mm²")
                .arg(owner, formatNumber(kernel::surfaceArea(kernel::subShape(
                                body->shape, kernel::SubShapeKind::Face, item->index))));
        case SelectedItem::Kind::Edge:
            return tr("Edge of %1 · length %2 mm")
                .arg(owner, formatNumber(kernel::length(kernel::subShape(
                                body->shape, kernel::SubShapeKind::Edge, item->index))));
        case SelectedItem::Kind::Vertex: {
            const auto point = kernel::boundingBox(
                kernel::subShape(body->shape, kernel::SubShapeKind::Vertex, item->index));
            if (!point.has_value()) {
                return {};
            }
            return tr("Vertex of %1 · X %2 · Y %3 · Z %4 mm")
                .arg(owner, formatNumber(point->min.x), formatNumber(point->min.y),
                     formatNumber(point->min.z));
        }
        case SelectedItem::Kind::Body:
            break;
        }
    } catch (const std::out_of_range&) {
        // The body changed under the selection; there is nothing to say about it.
    }
    return {};
}

void MainWindow::newDocument() {
    m_loadWarnings.clear();
    setDocument(std::make_unique<document::Document>(), QString());
}

bool MainWindow::loadFrom(const QString& path, QString* error) {
    m_loadWarnings.clear();
    try {
        document::LoadedDocument loaded = document::loadDocument(toPath(path));
        for (const std::string& warning : loaded.warnings) {
            m_loadWarnings.append(QString::fromStdString(warning));
        }
        setDocument(std::move(loaded.document), QFileInfo(path).absoluteFilePath());
        return true;
    } catch (const document::FileError& failure) {
        if (error != nullptr) {
            *error = QString::fromStdString(failure.what());
        }
        return false;
    }
}

bool MainWindow::saveTo(const QString& path, QString* error) {
    try {
        document::SaveOptions options;
        options.thumbnailPng = thumbnail();
        document::saveDocument(*m_document, toPath(path), options);
    } catch (const document::FileError& failure) {
        if (error != nullptr) {
            *error = QString::fromStdString(failure.what());
        }
        return false;
    }
    m_filePath = QFileInfo(path).absoluteFilePath();
    m_document->markSaved();
    updateDocumentState();
    return true;
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

void MainWindow::startPrimitive(PrimitiveKind kind) {
    cancelCommand();
    m_panel = new PrimitivePanel(kind, m_toolDock);
    connect(m_panel, &PrimitivePanel::changed, this,
            [this] { m_viewport->setPreview(m_panel->shape()); });
    connect(m_panel, &PrimitivePanel::accepted, this, &MainWindow::acceptCommand);
    connect(m_panel, &PrimitivePanel::canceled, this, &MainWindow::cancelCommand);
    m_toolDock->setWindowTitle(tr("Create %1").arg(primitiveName(kind)));
    m_toolDock->setWidget(m_panel);
    m_toolDock->show();
    m_cancelAction->setEnabled(true);
    m_viewport->setPreview(m_panel->shape());
    m_panel->setFocus();
}

void MainWindow::acceptCommand() {
    if (m_panel == nullptr || m_panel->shape().isNull()) {
        return;
    }
    const QString name = primitiveName(m_panel->kind());
    try {
        document::Transaction transaction(*m_document, tr("Create %1").arg(name).toStdString());
        m_document->addBody(m_document->uniqueBodyName(name.toStdString()), m_panel->shape());
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
    if (m_panel == nullptr) {
        return;
    }
    m_viewport->setPreview(kernel::Shape());
    m_toolDock->hide();
    m_toolDock->setWidget(nullptr);
    // Later: this may be running inside one of the panel's own signals.
    m_panel->deleteLater();
    m_panel = nullptr;
    m_cancelAction->setEnabled(false);
    m_viewport->setFocus();
}

void MainWindow::deleteSelectedBodies() {
    const std::vector<document::ObjectId> bodies = selectedBodies();
    if (bodies.empty()) {
        return;
    }
    document::Transaction transaction(
        *m_document, bodies.size() == 1 ? tr("Delete Body").toStdString()
                                        : tr("Delete %1 Bodies").arg(bodies.size()).toStdString());
    for (const document::ObjectId id : bodies) {
        m_document->removeBody(id);
    }
    transaction.commit();
    updateSelectionState();
}

bool MainWindow::confirmDiscard() {
    if (!m_document->isModified()) {
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
    if (!confirmDiscard()) {
        return false;
    }
    newDocument();
    return true;
}

bool MainWindow::fileOpen() {
    if (!confirmDiscard()) {
        return false;
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Document"),
                                                      QFileInfo(m_filePath).absolutePath(),
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
    if (m_filePath.isEmpty()) {
        return fileSaveAs();
    }
    QString error;
    if (!saveTo(m_filePath, &error)) {
        QMessageBox::critical(this, tr("Could not save the document"), error);
        return false;
    }
    return true;
}

bool MainWindow::fileSaveAs() {
    const QString suggestion =
        m_filePath.isEmpty() ? documentName() + QStringLiteral(".lcad") : m_filePath;
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

void MainWindow::closeEvent(QCloseEvent* event) {
    if (confirmDiscard()) {
        cancelCommand();
        event->accept();
    } else {
        event->ignore();
    }
}

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

void MainWindow::showViewportError(const QString& reason) {
    QMessageBox::critical(this, tr("3D viewport unavailable"), reason);
}

} // namespace lcad::app
