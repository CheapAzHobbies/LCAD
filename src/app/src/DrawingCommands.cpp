// SPDX-License-Identifier: GPL-3.0-or-later

// The workspaces, and the commands of the Drawing workspace.

#include <lcad/app/MainWindow.h>

#include <lcad/app/Commands.h>
#include <lcad/app/DrawingWorkspace.h>
#include <lcad/app/Icons.h>
#include <lcad/app/Theme.h>

#include <QComboBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>

#include <array>
#include <tuple>

namespace lcad::app {

void MainWindow::setWorkspace(Workspace workspace) {
    if (workspace == m_workspace) {
        return;
    }
    cancelCommand();
    if (isSketching()) {
        finishSketch();
    }
    m_workspace = workspace;
    const bool drawing = workspace == Workspace::Drawing;
    m_drawings->setTool(DrawingWorkspace::Tool::Select);
    m_drawings->setShown(drawing);
    m_center->setCurrentWidget(drawing ? static_cast<QWidget*>(m_drawings->view()) : m_viewport);
    m_workspaceBox->setCurrentIndex(m_workspaceBox->findData(static_cast<int>(workspace)));
    updateMode();
    showInspector();
    fitToolbar();
    m_commands->refresh();
    if (drawing && m_drawings->drawing() == nullptr) {
        // A first look at a document's drawings: there is one to look at, if there is a model.
        if (!document()->bodies().empty()) {
            m_drawings->newDrawing();
        }
    }
}

QWidget* MainWindow::inspectorPanel() const {
    return m_inspectorStack->currentWidget();
}

void MainWindow::defineDrawingCommands() {
    const QString group = tr("Drawing");
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
    const auto hasSheet = [this] { return m_drawings->drawing() != nullptr; };

    // What belongs to one workspace cannot be started from another.
    m_commands->setAvailability([this](const QString& id) {
        if (id.startsWith(QStringLiteral("workspace.")) || id.startsWith(QStringLiteral("file.")) ||
            id.startsWith(QStringLiteral("edit.")) || id.startsWith(QStringLiteral("window.")) ||
            id.startsWith(QStringLiteral("help."))) {
            return true;
        }
        const bool sheetCommand = id.startsWith(QStringLiteral("drawing."));
        return (m_workspace == Workspace::Drawing) == sheetCommand;
    });

    {
        Command c = make("workspace.design", tr("&Design"), tr("Model parts and assemblies"),
                         "new-component");
        c.aliases = {tr("workspace"), tr("model")};
        c.run = [this](bool) { setWorkspace(Workspace::Design); };
        m_commands->add(std::move(c));

        c = make("workspace.drawing", tr("&Drawing"),
                 tr("Make engineering drawings of the model"), "drawing");
        c.aliases = {tr("workspace"), tr("sheet"), tr("blueprint"), tr("2d")};
        c.run = [this](bool) { setWorkspace(Workspace::Drawing); };
        m_commands->add(std::move(c));

        c = make("drawing.new", tr("&New Drawing"),
                 tr("Start a sheet with standard views of the model"), "drawing");
        c.aliases = {tr("sheet"), tr("blueprint")};
        c.run = [this](bool) { m_drawings->newDrawing(); };
        m_commands->add(std::move(c));

        c = make("drawing.delete", tr("Delete &Drawing"), tr("Delete the sheet shown"), "delete");
        c.run = [this](bool) { document()->removeDrawing(m_drawings->drawing()->id); };
        c.isEnabled = hasSheet;
        m_commands->add(std::move(c));

        const std::array<std::tuple<const char*, QString, document::ViewKind>, 7> views{{
            {"drawing.view.front", tr("&Front"), document::ViewKind::Front},
            {"drawing.view.back", tr("&Back"), document::ViewKind::Back},
            {"drawing.view.top", tr("&Top"), document::ViewKind::Top},
            {"drawing.view.bottom", tr("B&ottom"), document::ViewKind::Bottom},
            {"drawing.view.left", tr("&Left"), document::ViewKind::Left},
            {"drawing.view.right", tr("&Right"), document::ViewKind::Right},
            {"drawing.view.isometric", tr("&Isometric"), document::ViewKind::Isometric},
        }};
        for (const auto& [id, text, kind] : views) {
            c = make(id, text, tr("Add a view of the model from this side"), "view");
            c.aliases = {tr("view"), tr("projection")};
            c.run = [this, kind](bool) { m_drawings->addView(kind); };
            c.isEnabled = hasSheet;
            m_commands->add(std::move(c));
        }

        const auto toolCommand = [&](const char* id, const QString& text,
                                     const QString& description, const char* iconName,
                                     DrawingWorkspace::Tool tool) {
            Command command = make(id, text, description, iconName);
            command.checkable = true;
            command.exclusiveGroup = QStringLiteral("drawingTool");
            command.run = [this, tool](bool) { m_drawings->setTool(tool); };
            command.isEnabled = hasSheet;
            command.isChecked = [this, tool] { return m_drawings->tool() == tool; };
            return command;
        };

        c = toolCommand("drawing.sectionUp", tr("Section, &Up"),
                        tr("Cut a view through with a line up it"), "section",
                        DrawingWorkspace::Tool::SectionUp);
        c.aliases = {tr("section"), tr("cut"), tr("hatch")};
        m_commands->add(std::move(c));

        c = toolCommand("drawing.sectionAcross", tr("Section, &Across"),
                        tr("Cut a view through with a line across it"), "section",
                        DrawingWorkspace::Tool::SectionAcross);
        c.aliases = {tr("section"), tr("cut"), tr("hatch")};
        m_commands->add(std::move(c));

        c = toolCommand("drawing.detail", tr("De&tail"),
                        tr("Show a round part of a view larger"), "detail",
                        DrawingWorkspace::Tool::Detail);
        c.aliases = {tr("zoom"), tr("enlarge")};
        m_commands->add(std::move(c));

        c = toolCommand("drawing.dimension", tr("&Dimension"),
                        tr("Give a line, a circle, or two points their measure"), "dimension",
                        DrawingWorkspace::Tool::Dimension);
        c.shortcuts = {QKeySequence(Qt::Key_D)};
        c.aliases = {tr("measure"), tr("size"), tr("length")};
        m_commands->add(std::move(c));

        c = toolCommand("drawing.note", tr("N&ote"), tr("Put text on the sheet"), "note",
                        DrawingWorkspace::Tool::Note);
        c.aliases = {tr("text"), tr("label")};
        m_commands->add(std::move(c));

        const auto exportTo = [this](const QString& filter, const QString& suffix) {
            const QString base =
                QString::fromStdString(m_drawings->drawing()->name) + QLatin1Char('.') + suffix;
            QString path = QFileDialog::getSaveFileName(this, tr("Export Drawing"), base, filter);
            if (path.isEmpty()) {
                return;
            }
            if (QFileInfo(path).suffix().isEmpty()) {
                path += QLatin1Char('.') + suffix;
            }
            QString error;
            if (!m_drawings->exportTo(path, &error)) {
                QMessageBox::critical(this, tr("Could not export the drawing"), error);
                return;
            }
            statusBar()->showMessage(tr("Exported %1").arg(QFileInfo(path).fileName()), 4000);
        };

        c = make("drawing.exportPdf", tr("Export &PDF…"), tr("Write the sheet to a PDF file"),
                 "export");
        c.aliases = {tr("print"), tr("pdf")};
        c.run = [exportTo, this](bool) { exportTo(tr("PDF files (*.pdf)"), QStringLiteral("pdf")); };
        c.isEnabled = hasSheet;
        m_commands->add(std::move(c));

        c = make("drawing.exportDxf", tr("Export DX&F…"),
                 tr("Write the sheet to a DXF file, for other CAD programs and cutters"), "export");
        c.aliases = {tr("dxf"), tr("autocad")};
        c.run = [exportTo, this](bool) { exportTo(tr("DXF files (*.dxf)"), QStringLiteral("dxf")); };
        c.isEnabled = hasSheet;
        m_commands->add(std::move(c));
    }
}

void MainWindow::createDrawingToolbar() {
    const auto action = [this](const char* id) { return m_commands->action(QLatin1String(id)); };

    QToolBar* toolbar = addToolBar(tr("Drawing"));
    m_drawingToolbar = toolbar;
    toolbar->setObjectName(QStringLiteral("drawingToolbar"));
    toolbar->setProperty("tools", true);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->setContextMenuPolicy(Qt::PreventContextMenu);
    const int iconSize = Theme::instance().tokens().iconSize;
    toolbar->setIconSize(QSize(iconSize, iconSize));

    toolbar->addAction(action("drawing.new"));
    auto* views = new QToolButton(toolbar);
    views->setObjectName(QStringLiteral("viewsButton"));
    views->setText(tr("View"));
    views->setToolTip(tr("Add a view of the model"));
    views->setIcon(icon(QStringLiteral("view")));
    views->setPopupMode(QToolButton::InstantPopup);
    views->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    views->setMenu(m_menuBar->findChild<QMenu*>(QStringLiteral("menu.addView")));
    toolbar->addWidget(views);
    toolbar->addSeparator();
    for (const char* id : {"drawing.sectionUp", "drawing.sectionAcross", "drawing.detail",
                           "drawing.dimension", "drawing.note"}) {
        toolbar->addAction(action(id));
    }
    toolbar->addSeparator();
    toolbar->addAction(action("drawing.exportPdf"));
    toolbar->addAction(action("drawing.exportDxf"));
    toolbar->hide();
}

} // namespace lcad::app
