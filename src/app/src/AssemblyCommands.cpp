// SPDX-License-Identifier: GPL-3.0-or-later

// The main window's assembly tools: components, joints, moving and animating them, checking
// for overlaps, weighing, linked files, and the copies of unsaved work kept against a crash.

#include <lcad/app/MainWindow.h>

#include <lcad/app/AssemblyDialogs.h>
#include <lcad/app/BrowserPanel.h>
#include <lcad/app/Commands.h>
#include <lcad/app/JointPanel.h>
#include <lcad/app/MovePanel.h>
#include <lcad/app/Paths.h>
#include <lcad/app/PropertiesPanel.h>
#include <lcad/app/Recovery.h>
#include <lcad/document/File.h>
#include <lcad/document/Links.h>
#include <lcad/kernel/Error.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/SubShapes.h>
#include <lcad/kernel/Transform.h>

#include <QApplication>
#include <QDateTime>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QTreeWidget>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace lcad::app {

namespace {

using viewport::SelectedItem;
using viewport::SelectionFilter;

const char* const partDirectoryKey = "files/lastPartDirectory";

bool startsWith(const std::vector<document::ObjectId>& path,
                const std::vector<document::ObjectId>& prefix) {
    return path.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), path.begin());
}

std::vector<document::ObjectId> joined(std::vector<document::ObjectId> path,
                                       const std::vector<document::ObjectId>& more) {
    path.insert(path.end(), more.begin(), more.end());
    return path;
}

// The face or edge a joint's side is on, in its body's coordinates, or a null shape.
kernel::Shape partOf(const document::Document& document, const document::JointSide& side) {
    const document::Body* body = document.findBody(side.body);
    if (body == nullptr || body->shape.isNull() || side.kind == document::JointSide::Kind::Origin) {
        return {};
    }
    const kernel::NamedShape named{body->shape, body->faceNames};
    const bool face = side.kind == document::JointSide::Kind::Face;
    const int index =
        face ? kernel::findFace(named, side.name) : kernel::findEdge(named, side.name);
    if (index < 0) {
        return {};
    }
    return kernel::subShape(body->shape,
                            face ? kernel::SubShapeKind::Face : kernel::SubShapeKind::Edge, index);
}

// The kind of joint two places suggest: round things turn, balls turn any way, straight edges
// slide, and flat faces hold fast.
document::JointKind suggestedKind(const document::Document& document, const document::JointSide& a,
                                  const document::JointSide& b) {
    bool round = false;
    bool ball = false;
    bool straight = false;
    for (const document::JointSide* side : {&a, &b}) {
        switch (kernel::partKind(partOf(document, *side))) {
        case kernel::PartKind::Spherical:
            ball = true;
            break;
        case kernel::PartKind::Cylindrical:
        case kernel::PartKind::Conical:
        case kernel::PartKind::Toroidal:
        case kernel::PartKind::Circular:
            round = true;
            break;
        case kernel::PartKind::Straight:
            straight = true;
            break;
        default:
            break;
        }
    }
    return ball       ? document::JointKind::Ball
           : round    ? document::JointKind::Revolute
           : straight ? document::JointKind::Slider
                      : document::JointKind::Rigid;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Commands

void MainWindow::defineAssemblyCommands() {
    const QString assemble = tr("Assemble");
    const QString inspect = tr("Inspect");
    const QString file = tr("File");
    const auto make = [](const char* id, const QString& text, const QString& group,
                         const QString& description) {
        Command command;
        command.id = QString::fromLatin1(id);
        command.text = text;
        command.group = group;
        command.description = description;
        return command;
    };
    const auto oneOccurrence = [this] { return selectedOccurrences().size() == 1; };
    const auto notSketching = [this] { return !isSketching(); };

    Command c = make("assembly.newComponent", tr("New &Component"), assemble,
                     tr("Make an empty component inside the one being worked on, and work on it"));
    c.iconName = QStringLiteral("new-component");
    c.aliases = {tr("part"), tr("subassembly"), tr("assembly")};
    c.run = [this](bool) { newComponent(); };
    c.isEnabled = notSketching;
    m_commands->add(std::move(c));

    c = make("assembly.makeComponent", tr("Component from &Bodies"), assemble,
             tr("Make the selected bodies, with the features that make them, a component"));
    c.iconName = QStringLiteral("component");
    c.aliases = {tr("group"), tr("make part")};
    c.run = [this](bool) {
        QString error;
        if (!makeComponentFromSelection(&error)) {
            QMessageBox::warning(this, tr("Could not make a component"), error);
        }
    };
    c.isEnabled = [this] { return !isSketching() && !selectedBodies().empty(); };
    m_commands->add(std::move(c));

    c = make("assembly.insert", tr("&Insert Part…"), assemble,
             tr("Add another LCAD document as a linked part, changed in its own file"));
    c.iconName = QStringLiteral("insert-part");
    c.aliases = {tr("linked component"), tr("external part"), tr("reference"), tr("xref")};
    c.run = [this](bool) {
        const QString directory = QSettings().value(QLatin1String(partDirectoryKey)).toString();
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Insert Part"), directory, tr("LCAD documents (*.lcad);;All files (*)"));
        if (path.isEmpty()) {
            return;
        }
        QSettings().setValue(QLatin1String(partDirectoryKey), QFileInfo(path).absolutePath());
        QString error;
        if (!insertLinkedPart(path, &error)) {
            QMessageBox::warning(this, tr("Could not insert the part"), error);
        }
    };
    c.isEnabled = notSketching;
    m_commands->add(std::move(c));

    c = make("assembly.joint", tr("&Joint"), assemble,
             tr("Join two components at a face or an edge of each, so that one moves with the "
                "other"));
    c.iconName = QStringLiteral("joint");
    c.shortcuts = {QKeySequence(Qt::Key_J)};
    c.aliases = {tr("mate"), tr("constraint"), tr("revolute"), tr("hinge"), tr("slider")};
    c.run = [this](bool) { startJoint(); };
    c.isEnabled = notSketching;
    m_commands->add(std::move(c));

    c = make("assembly.editJoint", tr("&Edit Joint"), assemble, tr("Change the selected joint"));
    c.iconName = QStringLiteral("joint");
    c.run = [this](bool) {
        if (const auto joints = m_browser->selectedJoints(); joints.size() == 1) {
            startJoint(joints.front());
        }
    };
    c.isEnabled = [this] { return !isSketching() && m_browser->selectedJoints().size() == 1; };
    m_commands->add(std::move(c));

    c = make("assembly.animate", tr("&Animate Joint"), assemble,
             tr("Show the selected joint moving through its range"));
    c.iconName = QStringLiteral("animate");
    c.aliases = {tr("motion"), tr("drive"), tr("play")};
    c.run = [this](bool) {
        if (const auto joints = m_browser->selectedJoints(); joints.size() == 1) {
            animateJoint(joints.front());
        }
    };
    c.isEnabled = [this] {
        const auto joints = m_browser->selectedJoints();
        const document::Joint* joint =
            joints.size() == 1 ? document()->findJoint(joints.front()) : nullptr;
        return joint != nullptr && !document::motionsOf(joint->kind).empty();
    };
    m_commands->add(std::move(c));

    c = make("assembly.move", tr("&Move Component"), assemble,
             tr("Move or turn the selected component by hand"));
    c.iconName = QStringLiteral("move-component");
    c.aliases = {tr("position"), tr("place"), tr("rotate component")};
    c.run = [this](bool) { startMoveComponent(); };
    c.isEnabled = [this, oneOccurrence] { return !isSketching() && oneOccurrence(); };
    m_commands->add(std::move(c));

    c = make("assembly.ground", tr("&Ground"), assemble,
             tr("Hold the selected component where it is: joints move the other side instead"));
    c.iconName = QStringLiteral("ground");
    c.checkable = true;
    c.aliases = {tr("fix"), tr("anchor"), tr("pin")};
    c.run = [this](bool on) {
        const auto paths = selectedOccurrences();
        if (paths.empty()) {
            return;
        }
        document::Transaction transaction(*document(),
                                          (on ? tr("Ground") : tr("Unground")).toStdString());
        for (const auto& path : paths) {
            document()->setOccurrenceGrounded(path.back(), on);
        }
        transaction.commit();
    };
    c.isEnabled = [this] { return !isSketching() && !selectedOccurrences().empty(); };
    c.isChecked = [this] {
        const auto paths = selectedOccurrences();
        return !paths.empty() && std::all_of(paths.begin(), paths.end(), [this](const auto& path) {
            const document::Occurrence* occurrence = document()->findOccurrence(path.back());
            return occurrence != nullptr && occurrence->grounded;
        });
    };
    m_commands->add(std::move(c));

    c = make("assembly.activate", tr("&Work on Component"), assemble,
             tr("Make the selected component the one new bodies and sketches go into"));
    c.aliases = {tr("activate"), tr("edit in place"), tr("edit in context")};
    c.run = [this](bool) {
        if (const auto paths = selectedOccurrences(); paths.size() == 1) {
            activateOccurrence(paths.front());
        }
    };
    c.isEnabled = [this, oneOccurrence] { return !isSketching() && oneOccurrence(); };
    m_commands->add(std::move(c));

    c = make("assembly.activateRoot", tr("Work on the &Whole Document"), assemble,
             tr("Make the document itself the place new bodies and sketches go into"));
    c.aliases = {tr("activate root"), tr("top level")};
    c.run = [this](bool) { activateOccurrence({}); };
    c.isEnabled = [this] { return !isSketching() && !activePath().empty(); };
    m_commands->add(std::move(c));

    c = make("assembly.duplicate", tr("&Duplicate"), assemble,
             tr("Add another occurrence of the selected component: a change to one is a change "
                "to every one"));
    c.aliases = {tr("instance"), tr("linked copy"), tr("copy paste")};
    c.run = [this](bool) {
        const auto paths = selectedOccurrences();
        if (paths.size() != 1) {
            return;
        }
        const document::Occurrence* occurrence = document()->findOccurrence(paths.front().back());
        // Beside the original, by a little more than its size.
        double step = 20.0;
        for (const document::Body& body : document()->bodies()) {
            if (body.component == occurrence->component) {
                if (const auto box = kernel::boundingBox(body.shape)) {
                    step = std::max(step, (box->max.x - box->min.x) * 1.2);
                }
            }
        }
        kernel::Placement placement = document()->placementOf(occurrence->id);
        placement.origin.x += step;
        const document::ObjectId added =
            document()->addOccurrence(occurrence->component, occurrence->parent, placement);
        std::vector<document::ObjectId> path = paths.front();
        path.back() = added;
        m_browser->setAssemblySelection({joined(activePath(), path)}, {});
    };
    c.isEnabled = [this, oneOccurrence] { return !isSketching() && oneOccurrence(); };
    m_commands->add(std::move(c));

    c = make("assembly.copy", tr("Make &Independent Copy"), assemble,
             tr("Copy the selected component into a new one that is changed on its own"));
    c.aliases = {tr("paste new"), tr("clone"), tr("copy component")};
    c.run = [this](bool) {
        const auto paths = selectedOccurrences();
        if (paths.size() != 1) {
            return;
        }
        try {
            document()->copyComponent(paths.front().back());
        } catch (const std::invalid_argument& failure) {
            QMessageBox::warning(this, tr("Could not copy the component"),
                                 QString::fromUtf8(failure.what()));
        }
    };
    c.isEnabled = [this, oneOccurrence] { return !isSketching() && oneOccurrence(); };
    m_commands->add(std::move(c));

    c = make("assembly.suppress", tr("&Suppress Component"), assemble,
             tr("Leave the selected component out of the assembly, or bring it back"));
    c.checkable = true;
    c.run = [this](bool on) {
        const auto paths = selectedOccurrences();
        if (paths.empty()) {
            return;
        }
        document::Transaction transaction(*document(),
                                          (on ? tr("Suppress") : tr("Restore")).toStdString());
        for (const auto& path : paths) {
            document()->setOccurrenceSuppressed(path.back(), on);
        }
        transaction.commit();
    };
    c.isEnabled = [this] { return !isSketching() && !selectedOccurrences().empty(); };
    c.isChecked = [this] {
        const auto paths = selectedOccurrences();
        return !paths.empty() && std::all_of(paths.begin(), paths.end(), [this](const auto& path) {
            const document::Occurrence* occurrence = document()->findOccurrence(path.back());
            return occurrence != nullptr && occurrence->suppressed;
        });
    };
    m_commands->add(std::move(c));

    // Linked parts
    const auto linkedSelected = [this]() -> const document::Component* {
        const auto paths = selectedOccurrences();
        if (paths.size() != 1) {
            return nullptr;
        }
        const document::Occurrence* occurrence = document()->findOccurrence(paths.front().back());
        const document::Component* component =
            occurrence != nullptr ? document()->findComponent(occurrence->component) : nullptr;
        return component != nullptr && !component->link.empty() ? component : nullptr;
    };
    c = make("assembly.openLinked", tr("&Open Linked File"), assemble,
             tr("Open the file of the selected linked part, to change it there"));
    c.iconName = QStringLiteral("open");
    c.aliases = {tr("edit linked"), tr("edit part")};
    c.run = [this, linkedSelected](bool) {
        const document::Component* component = linkedSelected();
        if (component == nullptr) {
            return;
        }
        const std::filesystem::path linked = document::resolveLink(
            component->link, filePath().isEmpty() ? std::filesystem::path()
                                                  : toPath(QFileInfo(filePath()).absolutePath()));
        QString error;
        if (!loadFrom(fromPath(linked), &error)) {
            QMessageBox::warning(this, tr("Could not open the linked file"), error);
        }
    };
    c.isEnabled = [linkedSelected] { return linkedSelected() != nullptr; };
    m_commands->add(std::move(c));

    c = make("assembly.relink", tr("&Find Linked File…"), assemble,
             tr("Say where the file of the selected linked part is now"));
    c.aliases = {tr("relink"), tr("repath"), tr("missing file")};
    c.run = [this, linkedSelected](bool) {
        if (const document::Component* component = linkedSelected()) {
            relink(component->id);
        }
    };
    c.isEnabled = [linkedSelected] { return linkedSelected() != nullptr; };
    m_commands->add(std::move(c));

    c = make("file.links", tr("&Linked Files…"), file,
             tr("List the files this document links to, whether each is found and up to date"));
    c.iconName = QStringLiteral("links");
    c.aliases = {tr("dependencies"), tr("dependency report"), tr("references")};
    c.run = [this](bool) { showLinks(); };
    m_commands->add(std::move(c));

    c = make("file.updateLinks", tr("&Update Linked Parts"), file,
             tr("Read again the linked files that changed since they were last read"));
    c.aliases = {tr("refresh links"), tr("reload parts")};
    c.run = [this](bool) {
        const QStringList problems = updateLinks();
        if (!problems.isEmpty()) {
            QMessageBox::warning(this, tr("Some linked parts could not be updated"),
                                 problems.join(QLatin1Char('\n')));
        }
    };
    c.isEnabled = [this] {
        const auto& components = document()->components();
        return !isSketching() &&
               std::any_of(components.begin(), components.end(),
                           [](const document::Component& each) { return !each.link.empty(); });
    };
    m_commands->add(std::move(c));

    c = make("file.package", tr("&Package Project…"), file,
             tr("Gather the document and every file it links to into one folder, to move or "
                "share"));
    c.iconName = QStringLiteral("package");
    c.aliases = {tr("pack and go"), tr("archive"), tr("collect files")};
    c.run = [this](bool) { filePackage(); };
    m_commands->add(std::move(c));

    // Inspecting
    c = make("inspect.interference", tr("Check &Interference"), inspect,
             tr("Find where parts that are shown overlap"));
    c.iconName = QStringLiteral("interference");
    c.aliases = {tr("collision"), tr("clash"), tr("overlap")};
    c.run = [this](bool) { checkInterference(); };
    c.isEnabled = notSketching;
    m_commands->add(std::move(c));

    c = make("inspect.physical", tr("&Physical Properties"), inspect,
             tr("The mass, center of mass, and inertia of the selection, or of everything "
                "shown"));
    c.iconName = QStringLiteral("mass");
    c.aliases = {tr("mass"), tr("weight"), tr("center of gravity"), tr("inertia")};
    c.run = [this](bool) { showPhysicalProperties(); };
    c.isEnabled = notSketching;
    m_commands->add(std::move(c));

    c = make("inspect.materials", tr("&Materials…"), inspect,
             tr("The materials bodies can be made of, and the document's own"));
    c.iconName = QStringLiteral("material");
    c.aliases = {tr("density"), tr("physical material")};
    c.run = [this](bool) { showMaterials(); };
    m_commands->add(std::move(c));
}

// ---------------------------------------------------------------------------------------------
// Working on a component

std::vector<document::ObjectId> MainWindow::activePath() const {
    return current().activePath;
}

document::ObjectId MainWindow::activeComponent() const {
    const std::vector<document::ObjectId>& path = current().activePath;
    const document::Occurrence* occurrence =
        path.empty() ? nullptr : document()->findOccurrence(path.back());
    return occurrence != nullptr ? occurrence->component : document::ObjectId{};
}

void MainWindow::activateOccurrence(const std::vector<document::ObjectId>& path) {
    std::vector<document::ObjectId> wanted;
    const auto& instances = document()->instances();
    const auto found =
        std::find_if(instances.begin(), instances.end(),
                     [&path](const document::Instance& i) { return i.path == path; });
    if (found != instances.end()) {
        wanted = path;
    }
    if (!wanted.empty()) {
        const document::Component* component = document()->findComponent(found->component);
        if (component != nullptr && !component->link.empty()) {
            statusBar()->showMessage(tr("%1 is a linked part: open its file to change it.")
                                         .arg(QString::fromStdString(component->name)),
                                     9000);
            return;
        }
    }
    current().activePath = wanted;
    m_viewport->setEditPath(wanted);
    m_browser->setActivePath(wanted);
    if (wanted.empty()) {
        statusBar()->showMessage(tr("Working on the whole document."), 6000);
    } else {
        statusBar()->showMessage(
            tr("Working on %1: new bodies and sketches go into it. Double-click the document "
               "in the browser to work on the whole again.")
                .arg(QString::fromStdString(document()->occurrenceName(wanted.back()))),
            9000);
    }
    m_commands->refresh();
}

void MainWindow::ensureActive(document::ObjectId component,
                              const std::vector<document::ObjectId>& path) {
    if (activeComponent() == component) {
        return;
    }
    if (!component.isValid()) {
        activateOccurrence({});
        return;
    }
    for (const document::Instance& instance : document()->instances()) {
        if (instance.component == component && (path.empty() || instance.path == path)) {
            activateOccurrence(instance.path);
            return;
        }
    }
    for (const document::Instance& instance : document()->instances()) {
        if (instance.component == component) {
            activateOccurrence(instance.path);
            return;
        }
    }
}

std::vector<std::vector<document::ObjectId>> MainWindow::selectedOccurrences() const {
    std::vector<std::vector<document::ObjectId>> paths = m_browser->selectedOccurrences();
    const std::vector<document::ObjectId> active = current().activePath;
    for (const SelectedItem& item : m_viewport->selection()) {
        if (item.path.empty() || item.kind == SelectedItem::Kind::Sketch) {
            continue;
        }
        // The occurrence directly inside the component being worked on, that holds what was
        // clicked; or the outermost, for something outside it.
        std::vector<document::ObjectId> path =
            startsWith(item.path, active) && item.path.size() > active.size()
                ? std::vector<document::ObjectId>(
                      item.path.begin(),
                      item.path.begin() + static_cast<std::ptrdiff_t>(active.size() + 1))
                : std::vector<document::ObjectId>{item.path.front()};
        if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
            paths.push_back(path);
        }
    }
    return paths;
}

document::ObjectId MainWindow::newComponent() {
    if (isSketching()) {
        finishSketch();
    }
    cancelCommand();
    const document::ObjectId parent = activeComponent();
    const document::ObjectId component =
        document()->addComponent(document()->uniqueComponentName("Component"), parent);
    std::vector<document::ObjectId> path = activePath();
    path.push_back(document()->occurrencesOf(component).front());
    activateOccurrence(path);
    m_browser->setAssemblySelection({path}, {});
    return component;
}

bool MainWindow::makeComponentFromSelection(QString* error) {
    const std::vector<document::ObjectId> bodies = selectedBodies();
    if (bodies.empty()) {
        if (error != nullptr) {
            *error = tr("Select the bodies that are to make up the component.");
        }
        return false;
    }
    cancelCommand();
    try {
        const document::Body* first = document()->findBody(bodies.front());
        std::string name = first != nullptr && bodies.size() == 1
                               ? first->name
                               : document()->uniqueComponentName("Component");
        const document::ObjectId component = document()->makeComponent(name, bodies);
        m_viewport->clearSelection();
        for (const document::Instance& instance : document()->instances()) {
            if (instance.component == component) {
                m_browser->setAssemblySelection({instance.path}, {});
                break;
            }
        }
        statusBar()->showMessage(tr("Made the component %1.").arg(QString::fromStdString(name)),
                                 6000);
    } catch (const std::invalid_argument& failure) {
        if (error != nullptr) {
            *error = QString::fromUtf8(failure.what());
        }
        return false;
    }
    updateSelectionState();
    return true;
}

bool MainWindow::insertLinkedPart(const QString& path, QString* error) {
    const auto fail = [error](const QString& why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (!filePath().isEmpty() && absolute == QFileInfo(filePath()).absoluteFilePath()) {
        return fail(tr("A document cannot be a part of itself."));
    }
    if (isSketching()) {
        finishSketch();
    }
    cancelCommand();
    const std::filesystem::path folder = filePath().isEmpty()
                                             ? std::filesystem::path()
                                             : toPath(QFileInfo(filePath()).absolutePath());
    // The same file inserted again is the same component, appearing again.
    for (const document::Component& component : document()->components()) {
        if (!component.link.empty() &&
            std::filesystem::absolute(document::resolveLink(component.link, folder))
                    .lexically_normal() == toPath(absolute).lexically_normal()) {
            document()->addOccurrence(component.id, activeComponent(), {});
            statusBar()->showMessage(
                tr("Inserted %1 again.").arg(QString::fromStdString(component.name)), 6000);
            return true;
        }
    }
    document::LinkedContent content;
    try {
        content = document::readLinkedContent(toPath(absolute));
    } catch (const document::FileError& failure) {
        return fail(QString::fromStdString(failure.what()));
    }
    if (content.bodies.empty()) {
        return fail(tr("%1 has no bodies shown in it to insert.").arg(QFileInfo(path).fileName()));
    }
    const std::string name = QFileInfo(path).completeBaseName().toStdString();
    try {
        document()->addLinkedComponent(name, document::makeLink(toPath(absolute), folder),
                                       std::move(content), activeComponent(), {});
    } catch (const std::invalid_argument& failure) {
        return fail(QString::fromUtf8(failure.what()));
    }
    statusBar()->showMessage(
        tr("Inserted %1 as a linked part. Change it in its own file; Update Linked Parts brings "
           "the change here.")
            .arg(QString::fromStdString(name)),
        9000);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Joints

namespace {

QString describeSide(const document::Document& document, const document::JointSide& side,
                     document::ObjectId component) {
    QString where;
    if (!side.path.empty()) {
        where = QString::fromStdString(document.occurrenceName(side.path.back()));
    } else if (const document::Component* found = document.findComponent(component)) {
        where = QString::fromStdString(found->name);
    } else {
        where = MainWindow::tr("the assembly");
    }
    switch (side.kind) {
    case document::JointSide::Kind::Origin:
        return MainWindow::tr("Origin of %1").arg(where);
    case document::JointSide::Kind::Face:
        return MainWindow::tr("Face of %1").arg(where);
    case document::JointSide::Kind::Edge:
        return MainWindow::tr("Edge of %1").arg(where);
    }
    return where;
}

} // namespace

void MainWindow::startJoint(document::ObjectId joint) {
    if (isSketching()) {
        finishSketch();
    }
    cancelCommand();
    const document::Joint* edited = document()->findJoint(joint);
    if (edited != nullptr) {
        ensureActive(edited->component);
    }
    m_editedJoint = edited != nullptr ? joint : document::ObjectId{};
    m_jointPanel = new JointPanel(m_inspectorStack);
    m_jointPanel->setObjectName(QStringLiteral("jointPanel"));
    m_inspectorStack->addWidget(m_jointPanel);
    connect(m_jointPanel, &JointPanel::changed, this, &MainWindow::updateJointPreview);
    connect(m_jointPanel, &JointPanel::accepted, this, &MainWindow::acceptJoint);
    connect(m_jointPanel, &JointPanel::canceled, this, &MainWindow::cancelCommand);
    connect(m_jointPanel, &JointPanel::animateRequested, this, [this] { animateJoint({}); });
    if (edited != nullptr) {
        m_jointPanel->setJoint(*edited,
                               describeSide(*document(), edited->moving, edited->component),
                               describeSide(*document(), edited->fixed, edited->component));
    }
    // Faces and edges are what joints are placed on.
    m_filterBeforeFeature = m_viewport->selectionFilter();
    m_viewport->setSelectionFilter(SelectionFilter::FacesEdgesVertices);
    m_settingPicks = true;
    m_viewport->clearSelection();
    m_settingPicks = false;
    showInspector();
    m_inspectorDock->show();
    m_jointPanel->setFocus();
    updateJointPreview();
    m_commands->refresh();
    statusBar()->showMessage(m_jointPanel->hint(), 9000);
}

void MainWindow::takeJointPick() {
    if (m_jointPanel == nullptr) {
        return;
    }
    const std::vector<SelectedItem> picked = m_viewport->selection();
    const auto letGo = [this] {
        m_settingPicks = true;
        m_viewport->clearSelection();
        m_settingPicks = false;
    };
    if (picked.empty()) {
        return;
    }
    const int which = m_jointPanel->pickingSide();
    const SelectedItem item = picked.back();
    if (which < 0) {
        statusBar()->showMessage(tr("Both places are picked. Click Pick beside one to pick it "
                                    "again."),
                                 6000);
        letGo();
        return;
    }
    if (item.kind != SelectedItem::Kind::Face && item.kind != SelectedItem::Kind::Edge) {
        statusBar()->showMessage(tr("Click a face or an edge."), 6000);
        letGo();
        return;
    }
    // A joint joins parts of the component being worked on.
    const std::vector<document::ObjectId> active = activePath();
    if (!startsWith(item.path, active)) {
        const document::Component* component = document()->findComponent(activeComponent());
        statusBar()->showMessage(
            tr("That is not part of %1, which is being worked on. Joints join the parts of one "
               "component.")
                .arg(component != nullptr ? QString::fromStdString(component->name)
                                          : tr("the document")),
            9000);
        letGo();
        return;
    }
    document::JointSide side;
    side.path.assign(item.path.begin() + static_cast<std::ptrdiff_t>(active.size()),
                     item.path.end());
    side.kind = item.kind == SelectedItem::Kind::Face ? document::JointSide::Kind::Face
                                                      : document::JointSide::Kind::Edge;
    side.body = item.body;
    side.name = nameOf(item);
    letGo();
    if (side.name.empty()) {
        return;
    }
    const document::ObjectId component = m_editedJoint.isValid()
                                             ? document()->findJoint(m_editedJoint)->component
                                             : activeComponent();
    m_jointPanel->setSide(which, side, describeSide(*document(), side, component));
    statusBar()->showMessage(m_jointPanel->hint(), 9000);
}

void MainWindow::updateJointPreview() {
    if (m_jointPanel == nullptr) {
        return;
    }
    if (isAnimating()) {
        m_animationTimer->stop();
    }
    document::Joint joint = m_jointPanel->joint();
    const document::Joint* edited = document()->findJoint(m_editedJoint);
    joint.component = edited != nullptr ? edited->component : activeComponent();
    if (m_jointPanel->hasSide(0) && m_jointPanel->hasSide(1)) {
        m_jointPanel->suggestKind(suggestedKind(*document(), joint.moving, joint.fixed));
        joint.kind = m_jointPanel->joint().kind;
    }
    // Where the picked places are: arrows at each.
    std::vector<document::Instance> instances = document()->instances();
    if (m_jointPanel->hasSide(0) && m_jointPanel->hasSide(1)) {
        const document::JointTrial trial = document()->tryJoint(joint);
        instances = trial.instances;
        m_viewport->showInstances(trial.instances);
        m_jointPanel->setError(QString::fromStdString(trial.error));
    } else {
        m_viewport->showInstances({});
        m_jointPanel->setError({});
    }
    std::vector<document::ObjectId> base;
    for (const document::Instance& instance : document()->instances()) {
        if (instance.component == joint.component) {
            base = instance.path;
            break;
        }
    }
    if (joint.component == activeComponent()) {
        base = activePath();
    }
    std::vector<kernel::Placement> frames;
    for (int i = 0; i < 2; ++i) {
        if (!m_jointPanel->hasSide(i)) {
            continue;
        }
        const document::JointSide& side = i == 0 ? joint.moving : joint.fixed;
        const kernel::Shape part = partOf(*document(), side);
        const std::optional<kernel::Placement> frame = kernel::frameOn(part, side.snap);
        if (!frame.has_value()) {
            continue;
        }
        const std::vector<document::ObjectId> where = joined(base, side.path);
        for (const document::Instance& instance : instances) {
            if (instance.path == where) {
                frames.push_back(instance.placement * *frame);
                break;
            }
        }
    }
    m_viewport->setFrameMarkers(frames);
}

void MainWindow::acceptJoint() {
    if (m_jointPanel == nullptr || !m_jointPanel->hasSide(0) || !m_jointPanel->hasSide(1) ||
        !m_jointPanel->errorText().isEmpty()) {
        return;
    }
    document::Joint joint = m_jointPanel->joint();
    const document::ObjectId editing = m_editedJoint;
    const document::Joint* edited = document()->findJoint(editing);
    joint.component = edited != nullptr ? edited->component : activeComponent();
    try {
        if (edited != nullptr) {
            document()->changeJoint(editing, joint);
        } else {
            const document::ObjectId id = document()->addJoint(joint);
            statusBar()->showMessage(
                tr("Added %1.").arg(QString::fromStdString(document()->findJoint(id)->name)), 6000);
        }
    } catch (const std::invalid_argument& failure) {
        QMessageBox::warning(this, tr("Could not add the joint"),
                             QString::fromUtf8(failure.what()));
        return;
    }
    closeJointPanel();
    updateSelectionState();
}

void MainWindow::closeJointPanel() {
    if (m_jointPanel == nullptr) {
        return;
    }
    if (isAnimating()) {
        m_animationTimer->stop();
    }
    m_viewport->showInstances({});
    m_viewport->setFrameMarkers({});
    if (m_filterBeforeFeature.has_value()) {
        m_viewport->setSelectionFilter(*m_filterBeforeFeature);
        m_filterBeforeFeature.reset();
    }
    m_inspectorStack->removeWidget(m_jointPanel);
    m_jointPanel->deleteLater();
    m_jointPanel = nullptr;
    m_editedJoint = {};
    showInspector();
}

void MainWindow::animateJoint(document::ObjectId id) {
    if (isAnimating()) {
        m_animationTimer->stop();
    }
    document::Joint joint;
    if (const document::Joint* found = document()->findJoint(id)) {
        joint = *found;
    } else if (m_jointPanel != nullptr) {
        joint = m_jointPanel->joint();
        const document::Joint* edited = document()->findJoint(m_editedJoint);
        joint.component = edited != nullptr ? edited->component : activeComponent();
    } else {
        return;
    }
    if (document::motionsOf(joint.kind).empty()) {
        statusBar()->showMessage(tr("A rigid joint does not move."), 6000);
        return;
    }
    m_animatedJoint = joint;
    m_animationStep = 0;
    if (m_animationTimer == nullptr) {
        m_animationTimer = new QTimer(this);
        m_animationTimer->setInterval(33);
        connect(m_animationTimer, &QTimer::timeout, this, &MainWindow::stepAnimation);
    }
    m_animationTimer->start();
    statusBar()->showMessage(
        tr("Showing %1 in motion.")
            .arg(joint.name.empty() ? tr("the joint") : QString::fromStdString(joint.name)),
        4000);
}

void MainWindow::stepAnimation() {
    // Three seconds: out to one end of the range and back, or once round.
    constexpr int steps = 90;
    if (m_animationStep > steps) {
        stopAnimation();
        return;
    }
    const double t = static_cast<double>(m_animationStep) / steps;
    const double there = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * t);
    document::Joint moved = m_animatedJoint;
    const std::vector<document::JointMotion> motions = document::motionsOf(moved.kind);
    for (std::size_t i = 0; i < motions.size(); ++i) {
        const document::JointLimits& limits = moved.limits[i];
        const double now = m_animatedJoint.values[i];
        if (limits.minimum.has_value() && limits.maximum.has_value()) {
            moved.values[i] = *limits.minimum + (*limits.maximum - *limits.minimum) * there;
        } else if (motions[i].turns) {
            moved.values[i] = now + 360.0 * t;
        } else {
            moved.values[i] = now + 20.0 * std::sin(2.0 * 3.14159265358979323846 * t);
        }
    }
    m_viewport->showInstances(document()->tryJoint(moved).instances);
    ++m_animationStep;
}

void MainWindow::stopAnimation() {
    if (!isAnimating()) {
        return;
    }
    m_animationTimer->stop();
    if (m_jointPanel != nullptr) {
        updateJointPreview();
    } else {
        m_viewport->showInstances({});
    }
}

bool MainWindow::isAnimating() const {
    return m_animationTimer != nullptr && m_animationTimer->isActive();
}

// ---------------------------------------------------------------------------------------------
// Moving a component by hand

void MainWindow::startMoveComponent() {
    const auto paths = selectedOccurrences();
    if (paths.size() != 1) {
        return;
    }
    if (isSketching()) {
        finishSketch();
    }
    cancelCommand();
    const document::ObjectId occurrence = paths.front().back();
    m_movedOccurrence = occurrence;
    m_movePanel = new MovePanel(QString::fromStdString(document()->occurrenceName(occurrence)),
                                m_inspectorStack);
    m_movePanel->setObjectName(QStringLiteral("movePanel"));
    m_inspectorStack->addWidget(m_movePanel);
    connect(m_movePanel, &MovePanel::changed, this, &MainWindow::updateMovePreview);
    connect(m_movePanel, &MovePanel::canceled, this, &MainWindow::cancelCommand);
    connect(m_movePanel, &MovePanel::accepted, this, [this] {
        const document::Occurrence* moved = document()->findOccurrence(m_movedOccurrence);
        if (moved == nullptr) {
            cancelCommand();
            return;
        }
        document()->setOccurrencePlacement(m_movedOccurrence, m_movePanel->moved(moved->placement));
        closeMovePanel();
        updateSelectionState();
    });
    // A component a joint holds goes where the joint puts it, whatever is typed here.
    const document::Occurrence* found = document()->findOccurrence(occurrence);
    for (const document::Joint& joint : document()->joints()) {
        for (const document::JointSide* side : {&joint.moving, &joint.fixed}) {
            if (!side->path.empty() && side->path.front() == occurrence && !found->grounded &&
                !joint.suppressed) {
                m_movePanel->setError(tr("%1 holds it in place. Change the joint, or delete it, "
                                         "to move it by hand.")
                                          .arg(QString::fromStdString(joint.name)));
            }
        }
    }
    showInspector();
    m_inspectorDock->show();
    m_movePanel->setFocus();
}

void MainWindow::updateMovePreview() {
    const document::Occurrence* moved = document()->findOccurrence(m_movedOccurrence);
    if (m_movePanel == nullptr || moved == nullptr) {
        return;
    }
    // Every appearance of it, and of what is inside it, goes as far as it does.
    std::vector<document::Instance> instances = document()->instances();
    const kernel::Placement now = moved->placement;
    const kernel::Placement then = m_movePanel->moved(now);
    for (const document::Instance& instance : document()->instances()) {
        if (instance.path.empty() || instance.path.back() != m_movedOccurrence) {
            continue;
        }
        const kernel::Placement delta =
            instance.placement * kernel::inverse(now) * then * kernel::inverse(instance.placement);
        for (document::Instance& shown : instances) {
            if (startsWith(shown.path, instance.path)) {
                shown.placement = kernel::normalized(delta * shown.placement);
            }
        }
    }
    m_viewport->showInstances(instances);
}

void MainWindow::closeMovePanel() {
    if (m_movePanel == nullptr) {
        return;
    }
    m_viewport->showInstances({});
    m_inspectorStack->removeWidget(m_movePanel);
    m_movePanel->deleteLater();
    m_movePanel = nullptr;
    m_movedOccurrence = {};
    showInspector();
}

// ---------------------------------------------------------------------------------------------
// Inspecting

std::vector<MainWindow::PlacedBody> MainWindow::placedBodies(bool selectedOnly) const {
    std::vector<PlacedBody> placed;
    std::vector<SelectedItem> selected = m_viewport->selection();
    for (const SelectedItem& item : m_browser->selectedItems()) {
        selected.push_back(item);
    }
    std::vector<std::vector<document::ObjectId>> occurrences;
    for (const auto& path : m_browser->selectedOccurrences()) {
        occurrences.push_back(path);
    }
    const auto isSelected = [&](const document::Body& body, const document::Instance& instance) {
        for (const auto& path : occurrences) {
            if (startsWith(instance.path, path)) {
                return true;
            }
        }
        return std::any_of(selected.begin(), selected.end(), [&](const SelectedItem& item) {
            return item.kind != SelectedItem::Kind::Sketch && item.body == body.id &&
                   (item.path == instance.path || (item.path.empty() && !body.component.isValid()));
        });
    };
    for (const document::Instance& instance : document()->instances()) {
        if (!instance.visible && !selectedOnly) {
            continue;
        }
        for (const document::Body& body : document()->bodies()) {
            if (body.component != instance.component || body.shape.isNull()) {
                continue;
            }
            if (selectedOnly ? !isSelected(body, instance) : !body.visible) {
                continue;
            }
            PlacedBody entry;
            entry.body = body.id;
            entry.path = instance.path;
            entry.shape = kernel::placed(body.shape, instance.placement);
            entry.name =
                instance.path.empty()
                    ? QString::fromStdString(body.name)
                    : tr("%1 / %2").arg(
                          QString::fromStdString(document()->occurrenceName(instance.path.back())),
                          QString::fromStdString(body.name));
            placed.push_back(std::move(entry));
        }
    }
    return placed;
}

void MainWindow::checkInterference() {
    const std::vector<PlacedBody> bodies = placedBodies(false);
    std::vector<Interference> found;
    QStringList problems;
    int pairs = 0;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        for (std::size_t j = i + 1; j < bodies.size(); ++j) {
            // The bodies of one part are its own business; those of the document itself are
            // loose parts, and checked against each other.
            if (bodies[i].path == bodies[j].path && !bodies[i].path.empty()) {
                continue;
            }
            ++pairs;
            try {
                if (const auto overlap = kernel::overlapOf(bodies[i].shape, bodies[j].shape)) {
                    found.push_back(
                        {bodies[i].name, bodies[j].name, overlap->volume, overlap->shape});
                }
            } catch (const kernel::Error& error) {
                problems << tr("%1 and %2 could not be checked against each other (%3).")
                                .arg(bodies[i].name, bodies[j].name,
                                     QString::fromStdString(error.reason()));
            }
        }
    }
    QApplication::restoreOverrideCursor();
    std::sort(found.begin(), found.end(),
              [](const Interference& a, const Interference& b) { return a.volume > b.volume; });
    m_overlaps.clear();
    for (const Interference& entry : found) {
        m_overlaps.push_back(entry.overlap);
    }
    if (m_interferenceDialog == nullptr) {
        m_interferenceDialog = new InterferenceDialog(this);
        connect(m_interferenceDialog, &InterferenceDialog::interferenceChosen, this,
                [this](int index) {
                    if (index >= 0 && index < static_cast<int>(m_overlaps.size())) {
                        m_viewport->setHighlights({m_overlaps[static_cast<std::size_t>(index)]});
                    } else {
                        m_viewport->setHighlights({});
                    }
                });
        connect(m_interferenceDialog, &QDialog::finished, this,
                [this] { m_viewport->setHighlights({}); });
    }
    m_interferenceDialog->setResults(found, pairs, problems);
    // All of them at once to begin with; choosing one shows that one.
    m_viewport->setHighlights(m_overlaps);
    m_interferenceDialog->show();
    m_interferenceDialog->raise();
    statusBar()->showMessage(found.empty()       ? tr("No parts overlap.")
                             : found.size() == 1 ? tr("1 overlap found.")
                                                 : tr("%1 overlaps found.").arg(found.size()),
                             6000);
}

void MainWindow::showPhysicalProperties() {
    const bool selection = !targetBodies().empty() || !m_browser->selectedOccurrences().empty();
    const std::vector<PlacedBody> bodies = placedBodies(selection);
    MassSummary summary;
    std::vector<kernel::MassProperties> parts;
    for (const PlacedBody& placed : bodies) {
        const document::Body* body = document()->findBody(placed.body);
        MassRow row;
        row.name = placed.name;
        row.material = QString::fromStdString(body->material);
        row.volume = kernel::volume(placed.shape);
        summary.volume += row.volume;
        summary.area += kernel::surfaceArea(placed.shape);
        const std::optional<document::Material> material = document()->findMaterial(body->material);
        if (material.has_value()) {
            if (const auto properties = kernel::massProperties(placed.shape, material->density)) {
                row.mass = properties->mass;
                parts.push_back(*properties);
            }
        } else {
            ++summary.unknown;
        }
        summary.rows.push_back(row);
    }
    if (!parts.empty()) {
        summary.total = kernel::combined(parts);
    }
    if (m_massDialog == nullptr) {
        m_massDialog = new MassDialog(this);
    }
    m_massDialog->setSummary(
        summary, selection            ? (bodies.size() == 1 ? tr("1 body selected.")
                                                            : tr("%1 bodies selected.").arg(bodies.size()))
                 : bodies.size() == 1 ? tr("Everything shown: 1 body.")
                                      : tr("Everything shown: %1 bodies.").arg(bodies.size()));
    m_massDialog->show();
    m_massDialog->raise();
}

void MainWindow::showMaterials() {
    if (m_materialsDialog == nullptr) {
        m_materialsDialog = new MaterialsDialog(this);
    }
    m_materialsDialog->setDocument(document());
    m_materialsDialog->show();
    m_materialsDialog->raise();
}

// ---------------------------------------------------------------------------------------------
// Linked files

void MainWindow::showLinks() {
    if (m_linksDialog == nullptr) {
        m_linksDialog = new LinksDialog(this);
        connect(m_linksDialog, &LinksDialog::updateRequested, this, [this] {
            const QStringList problems = updateLinks();
            if (!problems.isEmpty()) {
                QMessageBox::warning(m_linksDialog, tr("Some linked parts could not be updated"),
                                     problems.join(QLatin1Char('\n')));
            }
        });
        connect(m_linksDialog, &LinksDialog::relinkRequested, this, &MainWindow::relink);
        connect(m_linksDialog, &LinksDialog::openRequested, this, [this](const QString& path) {
            QString error;
            if (!loadFrom(path, &error)) {
                QMessageBox::warning(m_linksDialog, tr("Could not open the linked file"), error);
            }
        });
        connect(m_linksDialog, &LinksDialog::packageRequested, this, &MainWindow::filePackage);
    }
    m_linksDialog->setStatuses(document::checkLinks(*document(), toPath(filePath())));
    m_linksDialog->show();
    m_linksDialog->raise();
}

QStringList MainWindow::updateLinks() {
    QStringList problems;
    for (const std::string& problem : document::updateLinks(*document(), toPath(filePath()))) {
        problems << QString::fromStdString(problem);
    }
    if (m_linksDialog != nullptr) {
        m_linksDialog->setStatuses(document::checkLinks(*document(), toPath(filePath())));
    }
    statusBar()->showMessage(problems.isEmpty() ? tr("Linked parts are up to date.")
                                                : tr("Some linked parts could not be updated."),
                             6000);
    return problems;
}

void MainWindow::relink(document::ObjectId component) {
    const document::Component* found = document()->findComponent(component);
    if (found == nullptr || found->link.empty()) {
        return;
    }
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Find %1").arg(QString::fromStdString(found->name)),
                                     QSettings().value(QLatin1String(partDirectoryKey)).toString(),
                                     tr("LCAD documents (*.lcad);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    try {
        document::LinkedContent content = document::readLinkedContent(toPath(path));
        const std::filesystem::path folder = filePath().isEmpty()
                                                 ? std::filesystem::path()
                                                 : toPath(QFileInfo(filePath()).absolutePath());
        document::Transaction transaction(*document(), "Relink " + found->name);
        document()->setComponentLink(component, document::makeLink(toPath(path), folder));
        document()->updateLinkedComponent(component, std::move(content));
        transaction.commit();
    } catch (const document::FileError& failure) {
        QMessageBox::warning(this, tr("Could not read the file"),
                             QString::fromStdString(failure.what()));
        return;
    }
    if (m_linksDialog != nullptr) {
        m_linksDialog->setStatuses(document::checkLinks(*document(), toPath(filePath())));
    }
}

bool MainWindow::packageTo(const QString& folder, QString* error, QStringList* problems) {
    try {
        const document::PackageResult result =
            document::packageProject(*document(), toPath(filePath()), toPath(folder));
        if (problems != nullptr) {
            for (const std::string& problem : result.problems) {
                *problems << QString::fromStdString(problem);
            }
        }
    } catch (const document::FileError& failure) {
        if (error != nullptr) {
            *error = QString::fromStdString(failure.what());
        }
        return false;
    }
    statusBar()->showMessage(tr("Packaged the project into %1.").arg(folder), 6000);
    return true;
}

bool MainWindow::filePackage() {
    const QString folder = QFileDialog::getExistingDirectory(
        this, tr("Package Project Into Folder"),
        filePath().isEmpty() ? QSettings().value(QStringLiteral("files/lastDirectory")).toString()
                             : QFileInfo(filePath()).absolutePath());
    if (folder.isEmpty()) {
        return false;
    }
    QString error;
    QStringList problems;
    if (!packageTo(folder, &error, &problems)) {
        QMessageBox::critical(this, tr("Could not package the project"), error);
        return false;
    }
    QString text = tr("The document and the files it links to are in %1. The folder can be "
                      "moved or shared as it is.")
                       .arg(folder);
    if (!problems.isEmpty()) {
        text += QStringLiteral("\n\n") + problems.join(QLatin1Char('\n'));
    }
    QMessageBox::information(this, tr("Project packaged"), text);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Menus

void MainWindow::showAssemblyMenu(const QPoint& where) {
    m_commands->refresh();
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const auto add = [this, menu](const char* id) {
        QAction* action = m_commands->action(QLatin1String(id));
        if (action->isEnabled()) {
            menu->addAction(action);
        }
    };
    if (!m_browser->selectedJoints().empty()) {
        for (const char* id : {"assembly.editJoint", "assembly.animate", "body.rename"}) {
            add(id);
        }
    } else if (!m_browser->selectedOccurrences().empty()) {
        for (const char* id :
             {"assembly.activate", "assembly.openLinked", "assembly.relink", "body.rename",
              "assembly.move", "assembly.ground", "assembly.duplicate", "assembly.copy",
              "body.hide", "body.show", "body.isolate", "assembly.suppress", "inspect.physical"}) {
            add(id);
        }
    } else {
        // The document's own entry.
        for (const char* id :
             {"assembly.activateRoot", "assembly.newComponent", "assembly.insert"}) {
            add(id);
        }
    }
    if (!m_browser->selectedJoints().empty() || !m_browser->selectedOccurrences().empty()) {
        menu->addSeparator();
        menu->addAction(m_commands->action(QStringLiteral("edit.delete")));
    }
    menu->popup(where);
}

// ---------------------------------------------------------------------------------------------
// Autosave and recovery

void MainWindow::enableRecovery(const QString& folder, int seconds) {
    m_recovery = std::make_unique<RecoveryStore>(folder);
    if (!m_recovery->open()) {
        m_recovery.reset();
        return;
    }
    if (m_autosaveTimer == nullptr) {
        m_autosaveTimer = new QTimer(this);
        connect(m_autosaveTimer, &QTimer::timeout, this, &MainWindow::autosave);
    }
    m_autosaveTimer->start(std::max(1, seconds) * 1000);
}

void MainWindow::autosave() {
    if (m_recovery == nullptr) {
        return;
    }
    for (int i = 0; i < documentCount(); ++i) {
        Session& session = *m_sessions[static_cast<std::size_t>(i)];
        if (session.recoveryKey.isEmpty()) {
            session.recoveryKey = QString::number(m_nextRecoveryKey++);
        }
        if (!session.document->isModified()) {
            m_recovery->discard(session.recoveryKey);
            session.autosaveDue = false;
            continue;
        }
        if (!session.autosaveDue) {
            continue;
        }
        if (m_recovery->save(session.recoveryKey, *session.document, session.filePath,
                             documentName(i))) {
            session.autosaveDue = false;
        }
    }
}

int MainWindow::offerRecovery(bool ask) {
    if (m_recovery == nullptr) {
        return 0;
    }
    const std::vector<RecoveryStore::Entry> entries = m_recovery->leftBehind();
    if (entries.empty()) {
        return 0;
    }
    if (ask) {
        QStringList lines;
        for (const RecoveryStore::Entry& entry : entries) {
            lines << tr("• %1, last kept %2")
                         .arg(entry.title,
                              QLocale().toString(entry.saved.toLocalTime(), QLocale::ShortFormat));
        }
        QMessageBox box(QMessageBox::Question, tr("Restore unsaved work?"),
                        tr("LCAD did not close properly last time. It kept copies of the unsaved "
                           "changes to:\n%1\n\nRestore them? Each opens in a tab, with its changes "
                           "not yet saved.")
                            .arg(lines.join(QLatin1Char('\n'))),
                        QMessageBox::NoButton, this);
        QPushButton* restore = box.addButton(tr("Restore"), QMessageBox::AcceptRole);
        QPushButton* discard = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
        box.addButton(tr("Later"), QMessageBox::RejectRole);
        box.setDefaultButton(restore);
        box.exec();
        if (box.clickedButton() == discard) {
            for (const RecoveryStore::Entry& entry : entries) {
                m_recovery->removeSession(entry.session);
            }
            return 0;
        }
        if (box.clickedButton() != restore) {
            return 0;
        }
    }
    int restored = 0;
    QStringList failed;
    for (const RecoveryStore::Entry& entry : entries) {
        try {
            document::LoadedDocument loaded = document::loadDocument(toPath(entry.file));
            loaded.document->markModified();
            storeViewOf(m_current);
            const int index = addSession(std::move(loaded.document), entry.originalPath);
            m_sessions[static_cast<std::size_t>(index)]->autosaveDue = true;
            activate(index);
            ++restored;
        } catch (const document::FileError& failure) {
            failed << QString::fromStdString(failure.what());
        }
    }
    if (failed.isEmpty()) {
        for (const RecoveryStore::Entry& entry : entries) {
            m_recovery->removeSession(entry.session);
        }
    } else if (ask) {
        QMessageBox::warning(this, tr("Some work could not be restored"),
                             failed.join(QLatin1Char('\n')));
    }
    // Copies of what was restored are kept again, in this session, from now on.
    autosave();
    statusBar()->showMessage(
        restored == 1 ? tr("Restored 1 document. Save it to keep the changes.")
                      : tr("Restored %1 documents. Save them to keep the changes.").arg(restored),
        9000);
    return restored;
}

} // namespace lcad::app
