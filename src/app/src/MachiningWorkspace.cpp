// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/MachiningWorkspace.h>

#include <lcad/app/Paths.h>
#include <lcad/cam/Gcode.h>
#include <lcad/cam/Planner.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Transform.h>
#include <lcad/viewport/ViewportWidget.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lcad::app {

using document::MachiningOperation;
using document::ObjectId;
using document::OperationKind;
using document::OperationTarget;
using document::Setup;
using document::Tool;
using document::ToolKind;

namespace {

QDoubleSpinBox* numberField(QWidget* parent, double low, double high, double step,
                            const QString& suffix, int decimals = 2) {
    auto* field = new QDoubleSpinBox(parent);
    field->setRange(low, high);
    field->setSingleStep(step);
    field->setDecimals(decimals);
    field->setSuffix(suffix);
    field->setKeyboardTracking(false);
    field->setMinimumWidth(70);
    return field;
}

std::vector<Tool> firstTools() {
    const auto tool = [](const char* name, ToolKind kind, double diameter, double flutes,
                         double stickout, int number) {
        Tool made;
        made.name = name;
        made.kind = kind;
        made.diameter = diameter;
        made.fluteLength = flutes;
        made.stickout = stickout;
        made.number = number;
        return made;
    };
    return {tool("6 mm flat end mill", ToolKind::FlatEndMill, 6.0, 20.0, 30.0, 1),
            tool("3 mm flat end mill", ToolKind::FlatEndMill, 3.0, 12.0, 20.0, 2),
            tool("6 mm drill", ToolKind::Drill, 6.0, 40.0, 50.0, 3),
            tool("3 mm drill", ToolKind::Drill, 3.0, 25.0, 35.0, 4)};
}

QString kindName(OperationKind kind) {
    switch (kind) {
    case OperationKind::Face:
        return MachiningWorkspace::tr("Face");
    case OperationKind::Pocket:
        return MachiningWorkspace::tr("Pocket");
    case OperationKind::Contour:
        return MachiningWorkspace::tr("Contour");
    case OperationKind::Drill:
        return MachiningWorkspace::tr("Drill");
    }
    return {};
}

QString timeText(double seconds) {
    const auto whole = static_cast<int>(std::ceil(seconds));
    if (whole < 60) {
        return MachiningWorkspace::tr("%1 s").arg(whole);
    }
    return MachiningWorkspace::tr("%1 min %2 s").arg(whole / 60).arg(whole % 60);
}

} // namespace

MachiningWorkspace::MachiningWorkspace(viewport::ViewportWidget* viewport, QWidget* parent)
    : QWidget(parent), m_viewport(viewport), m_setups(new QComboBox(this)),
      m_machineName(new QLineEdit(this)), m_dialect(new QComboBox(this)),
      m_origin(new QComboBox(this)), m_stockSide(numberField(this, 0.0, 1000.0, 1.0, tr(" mm"))),
      m_stockTop(numberField(this, 0.0, 1000.0, 0.5, tr(" mm"))),
      m_stockBottom(numberField(this, 0.0, 1000.0, 0.5, tr(" mm"))),
      m_travelX(numberField(this, 1.0, 10000.0, 10.0, tr(" mm"), 0)),
      m_travelY(numberField(this, 1.0, 10000.0, 10.0, tr(" mm"), 0)),
      m_travelZ(numberField(this, 1.0, 10000.0, 10.0, tr(" mm"), 0)),
      m_rapid(numberField(this, 1.0, 100000.0, 100.0, tr(" mm/min"), 0)),
      m_maxFeed(numberField(this, 1.0, 100000.0, 100.0, tr(" mm/min"), 0)),
      m_maxSpindle(numberField(this, 1.0, 100000.0, 1000.0, tr(" rpm"), 0)),
      m_operations(new QListWidget(this)), m_operationBox(new QGroupBox(tr("Operation"), this)),
      m_operationName(new QLineEdit(this)), m_tool(new QComboBox(this)),
      m_feed(numberField(this, 0.0, 100000.0, 50.0, tr(" mm/min"), 0)),
      m_plunge(numberField(this, 0.0, 100000.0, 50.0, tr(" mm/min"), 0)),
      m_spindle(numberField(this, 0.0, 100000.0, 500.0, tr(" rpm"), 0)),
      m_stepover(numberField(this, 0.0, 1000.0, 0.1, tr(" mm"))),
      m_stepdown(numberField(this, 0.0, 1000.0, 0.1, tr(" mm"))),
      m_clearance(numberField(this, 0.0, 1000.0, 1.0, tr(" mm"))),
      m_retract(numberField(this, 0.0, 1000.0, 1.0, tr(" mm"))),
      m_leave(numberField(this, 0.0, 100.0, 0.1, tr(" mm"))), m_side(new QComboBox(this)),
      m_peck(numberField(this, 0.0, 1000.0, 0.5, tr(" mm"))), m_suppressed(new QCheckBox(this)),
      m_targets(new QLabel(this)), m_slider(new QSlider(Qt::Horizontal, this)),
      m_summary(new QLabel(this)), m_problems(new QLabel(this)) {
    setObjectName(QStringLiteral("machiningPanel"));
    auto* layout = new QVBoxLayout(this);

    m_problems->setObjectName(QStringLiteral("errorText"));
    m_problems->setWordWrap(true);
    m_problems->hide();
    layout->addWidget(m_problems);

    m_setups->setObjectName(QStringLiteral("setupBox"));
    m_setups->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_setups->setMinimumContentsLength(8);
    auto* form = new QFormLayout;
    form->addRow(tr("Setup"), m_setups);
    layout->addLayout(form);

    auto* operationsLabel = new QLabel(tr("Operations"), this);
    operationsLabel->setObjectName(QStringLiteral("secondary"));
    layout->addWidget(operationsLabel);
    m_operations->setObjectName(QStringLiteral("operationList"));
    m_operations->setMaximumHeight(120);
    layout->addWidget(m_operations);

    m_operationName->setObjectName(QStringLiteral("operationName"));
    auto* operationForm = new QFormLayout(m_operationBox);
    m_operationForm = operationForm;
    operationForm->addRow(tr("Name"), m_operationName);
    m_tool->setObjectName(QStringLiteral("operationTool"));
    m_tool->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_tool->setMinimumContentsLength(8);
    operationForm->addRow(tr("Tool"), m_tool);
    m_targets->setObjectName(QStringLiteral("secondary"));
    m_targets->setWordWrap(true);
    auto* use = new QPushButton(tr("Use Selected Faces"), this);
    use->setObjectName(QStringLiteral("useFaces"));
    operationForm->addRow(m_targets);
    operationForm->addRow(use);
    m_feed->setObjectName(QStringLiteral("operationFeed"));
    operationForm->addRow(tr("Feed"), m_feed);
    operationForm->addRow(tr("Plunge"), m_plunge);
    operationForm->addRow(tr("Spindle"), m_spindle);
    operationForm->addRow(tr("Stepover"), m_stepover);
    operationForm->addRow(tr("Stepdown"), m_stepdown);
    operationForm->addRow(tr("Clearance"), m_clearance);
    operationForm->addRow(tr("Retract"), m_retract);
    operationForm->addRow(tr("Leave"), m_leave);
    m_side->addItem(tr("Outside"));
    m_side->addItem(tr("Inside"));
    operationForm->addRow(tr("Side"), m_side);
    operationForm->addRow(tr("Peck"), m_peck);
    m_suppressed->setText(tr("Leave out"));
    operationForm->addRow(m_suppressed);
    layout->addWidget(m_operationBox);

    auto* simulationBox = new QGroupBox(tr("Simulation"), this);
    auto* simulationLayout = new QVBoxLayout(simulationBox);
    m_slider->setObjectName(QStringLiteral("moveSlider"));
    m_slider->setEnabled(false);
    simulationLayout->addWidget(m_slider);
    m_summary->setObjectName(QStringLiteral("simulationSummary"));
    m_summary->setWordWrap(true);
    simulationLayout->addWidget(m_summary);
    layout->addWidget(simulationBox);
    auto* stockBox = new QGroupBox(tr("Stock"), this);
    auto* stockForm = new QFormLayout(stockBox);
    m_stockSide->setObjectName(QStringLiteral("stockSide"));
    stockForm->addRow(tr("Around"), m_stockSide);
    stockForm->addRow(tr("Above"), m_stockTop);
    stockForm->addRow(tr("Below"), m_stockBottom);
    m_origin->addItem(tr("Top corner"));
    m_origin->addItem(tr("Top center"));
    m_origin->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_origin->setMinimumContentsLength(8);
    stockForm->addRow(tr("Zero at"), m_origin);
    layout->addWidget(stockBox);

    auto* machineBox = new QGroupBox(tr("Machine"), this);
    auto* machineForm = new QFormLayout(machineBox);
    machineForm->addRow(tr("Name"), m_machineName);
    m_dialect->setObjectName(QStringLiteral("postDialect"));
    m_dialect->addItem(tr("Grbl"));
    m_dialect->addItem(tr("LinuxCNC"));
    m_dialect->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_dialect->setMinimumContentsLength(8);
    machineForm->addRow(tr("G-code for"), m_dialect);
    machineForm->addRow(tr("Travel X"), m_travelX);
    machineForm->addRow(tr("Travel Y"), m_travelY);
    machineForm->addRow(tr("Travel Z"), m_travelZ);
    machineForm->addRow(tr("Rapid"), m_rapid);
    machineForm->addRow(tr("Most feed"), m_maxFeed);
    machineForm->addRow(tr("Most spindle"), m_maxSpindle);
    layout->addWidget(machineBox);

    layout->addStretch(1);

    connect(m_setups, &QComboBox::activated, this, [this](int index) {
        m_current = ObjectId{m_setups->itemData(index).toULongLong()};
        m_selected = {};
        refresh();
    });
    connect(m_operations, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_filling || setup() == nullptr || row < 0 ||
            row >= static_cast<int>(setup()->operations.size())) {
            return;
        }
        m_selected = setup()->operations[static_cast<std::size_t>(row)].id;
        fill();
        emit stateChanged();
    });
    connect(use, &QPushButton::clicked, this, &MachiningWorkspace::targetsRequested);

    const auto setupField = [this](QDoubleSpinBox* field, const QString& label,
                                   const std::function<void(Setup&, double)>& assign) {
        connect(field, &QDoubleSpinBox::valueChanged, this, [this, label, assign](double value) {
            if (m_filling || setup() == nullptr) {
                return;
            }
            Setup changed = *setup();
            assign(changed, value);
            change(changed, label);
        });
    };
    setupField(m_stockSide, tr("Change Stock"), [](Setup& s, double v) { s.stock.side = v; });
    setupField(m_stockTop, tr("Change Stock"), [](Setup& s, double v) { s.stock.top = v; });
    setupField(m_stockBottom, tr("Change Stock"), [](Setup& s, double v) { s.stock.bottom = v; });
    setupField(m_travelX, tr("Change Machine"), [](Setup& s, double v) { s.machine.travelX = v; });
    setupField(m_travelY, tr("Change Machine"), [](Setup& s, double v) { s.machine.travelY = v; });
    setupField(m_travelZ, tr("Change Machine"), [](Setup& s, double v) { s.machine.travelZ = v; });
    setupField(m_rapid, tr("Change Machine"), [](Setup& s, double v) { s.machine.rapidRate = v; });
    setupField(m_maxFeed, tr("Change Machine"), [](Setup& s, double v) { s.machine.maxFeed = v; });
    setupField(m_maxSpindle, tr("Change Machine"),
               [](Setup& s, double v) { s.machine.maxSpindle = v; });
    connect(m_machineName, &QLineEdit::editingFinished, this, [this] {
        if (m_filling || setup() == nullptr ||
            setup()->machine.name == m_machineName->text().toStdString()) {
            return;
        }
        Setup changed = *setup();
        changed.machine.name = m_machineName->text().toStdString();
        change(changed, tr("Change Machine"));
    });
    connect(m_dialect, &QComboBox::activated, this, [this](int index) {
        if (setup() != nullptr) {
            Setup changed = *setup();
            changed.machine.dialect =
                index == 0 ? document::PostDialect::Grbl : document::PostDialect::LinuxCnc;
            change(changed, tr("Change Machine"));
        }
    });
    connect(m_origin, &QComboBox::activated, this, [this](int index) {
        if (setup() != nullptr) {
            Setup changed = *setup();
            changed.origin = index == 0 ? document::WorkOrigin::StockTopCorner
                                        : document::WorkOrigin::StockTopCenter;
            change(changed, tr("Change Work Zero"));
        }
    });

    const auto operationField = [this](QDoubleSpinBox* field, double MachiningOperation::*member) {
        connect(field, &QDoubleSpinBox::valueChanged, this, [this, member](double value) {
            editOperation(tr("Change Operation"), [member, value](MachiningOperation& operation) {
                operation.*member = value;
            });
        });
    };
    operationField(m_feed, &MachiningOperation::feed);
    operationField(m_plunge, &MachiningOperation::plungeFeed);
    operationField(m_spindle, &MachiningOperation::spindle);
    operationField(m_stepover, &MachiningOperation::stepover);
    operationField(m_stepdown, &MachiningOperation::stepdown);
    operationField(m_clearance, &MachiningOperation::clearance);
    operationField(m_retract, &MachiningOperation::retract);
    operationField(m_leave, &MachiningOperation::stockToLeave);
    operationField(m_peck, &MachiningOperation::peck);
    connect(m_operationName, &QLineEdit::editingFinished, this, [this] {
        const std::string name = m_operationName->text().trimmed().toStdString();
        if (!name.empty()) {
            editOperation(tr("Rename Operation"),
                          [name](MachiningOperation& operation) { operation.name = name; });
        }
    });
    connect(m_tool, &QComboBox::activated, this, [this](int index) {
        const ObjectId tool{m_tool->itemData(index).toULongLong()};
        editOperation(tr("Change Tool"),
                      [tool](MachiningOperation& operation) { operation.tool = tool; });
    });
    connect(m_side, &QComboBox::activated, this, [this](int index) {
        editOperation(tr("Change Operation"), [index](MachiningOperation& operation) {
            operation.side =
                index == 0 ? document::ContourSide::Outside : document::ContourSide::Inside;
        });
    });
    connect(m_suppressed, &QCheckBox::toggled, this, [this](bool on) {
        editOperation(tr("Leave Out Operation"),
                      [on](MachiningOperation& operation) { operation.suppressed = on; });
    });
    connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
        if (!m_filling && m_simulation != nullptr) {
            showMove(static_cast<std::size_t>(value));
        }
    });
    fill();
}

MachiningWorkspace::~MachiningWorkspace() {
    if (m_document != nullptr) {
        m_document->removeListener(m_listener);
    }
}

void MachiningWorkspace::setDocument(document::Document* document) {
    if (m_document != nullptr) {
        m_document->removeListener(m_listener);
    }
    m_document = document;
    m_current = {};
    m_selected = {};
    m_simulation.reset();
    m_simulated.reset();
    if (document != nullptr) {
        m_listener = document->addListener(
            [this](const document::Change& change) { onDocumentChanged(change); });
        if (!document->setups().empty()) {
            m_current = document->setups().front().id;
        }
    }
    refresh();
}

void MachiningWorkspace::setShown(bool shown) {
    m_shown = shown;
    if (shown) {
        refresh();
    } else {
        endSimulation();
        m_viewport->setToolpaths({});
    }
}

const Setup* MachiningWorkspace::setup() const {
    return m_document != nullptr ? m_document->findSetup(m_current) : nullptr;
}

const MachiningOperation* MachiningWorkspace::operation() const {
    if (setup() == nullptr) {
        return nullptr;
    }
    for (const MachiningOperation& each : setup()->operations) {
        if (each.id == m_selected) {
            return &each;
        }
    }
    return nullptr;
}

void MachiningWorkspace::onDocumentChanged(const document::Change& change) {
    using Kind = document::Change::Kind;
    switch (change.kind) {
    case Kind::BodyAdded:
    case Kind::BodyRemoved:
    case Kind::BodyVisibilityChanged:
    case Kind::BodyGeometryChanged:
    case Kind::PlacementsChanged:
    case Kind::AssemblyChanged:
    case Kind::MachiningChanged:
        if (setup() == nullptr) {
            m_current = m_document->setups().empty() ? ObjectId{} : m_document->setups().front().id;
        }
        endSimulation();
        refresh();
        break;
    default:
        break;
    }
}

void MachiningWorkspace::refresh() {
    replan();
    fill();
    emit stateChanged();
}

void MachiningWorkspace::replan() {
    m_program = {};
    const Setup* current = setup();
    if (current != nullptr && m_document != nullptr) {
        m_program = cam::planSetup(*m_document, *current);
    }
    if (!m_shown) {
        return;
    }
    viewport::ToolpathLines lines;
    const kernel::BoundingBox& box = m_program.work.stock;
    if (current != nullptr && box.max.z > box.min.z) {
        const auto ring = [&box](double z) {
            return std::vector<kernel::Point>{{box.min.x, box.min.y, z},
                                              {box.max.x, box.min.y, z},
                                              {box.max.x, box.max.y, z},
                                              {box.min.x, box.max.y, z},
                                              {box.min.x, box.min.y, z}};
        };
        lines.stock.push_back(ring(box.min.z));
        lines.stock.push_back(ring(box.max.z));
        for (const auto& corner : ring(box.min.z)) {
            lines.stock.push_back({corner, {corner.x, corner.y, box.max.z}});
        }
    }
    // Runs of moves of one kind, each line starting where the last move ended.
    for (const cam::Toolpath& path : m_program.paths) {
        kernel::Point at{path.moves.front().to.x, path.moves.front().to.y, m_program.safeZ};
        std::vector<kernel::Point> line{at};
        bool rapid = true;
        const auto flush = [&lines, &line, &rapid] {
            if (line.size() > 1) {
                (rapid ? lines.rapids : lines.cuts).push_back(line);
            }
        };
        for (const cam::Move& move : path.moves) {
            const bool moveIsRapid = move.kind == cam::MoveKind::Rapid;
            if (moveIsRapid != rapid) {
                flush();
                line = {at};
                rapid = moveIsRapid;
            }
            line.push_back(move.to);
            at = move.to;
        }
        flush();
    }
    m_viewport->setToolpaths(lines);
}

void MachiningWorkspace::fill() {
    m_filling = true;
    const Setup* current = setup();
    {
        const QSignalBlocker blocker(m_setups);
        m_setups->clear();
        if (m_document != nullptr) {
            for (const Setup& each : m_document->setups()) {
                m_setups->addItem(QString::fromStdString(each.name),
                                  QVariant::fromValue<qulonglong>(each.id.value));
                if (each.id == m_current) {
                    m_setups->setCurrentIndex(m_setups->count() - 1);
                }
            }
        }
    }
    {
        const QSignalBlocker blocker(m_operations);
        m_operations->clear();
        if (current != nullptr) {
            for (const MachiningOperation& each : current->operations) {
                auto* item = new QListWidgetItem(QString::fromStdString(each.name));
                if (each.suppressed) {
                    QFont font = item->font();
                    font.setStrikeOut(true);
                    item->setFont(font);
                }
                m_operations->addItem(item);
                if (each.id == m_selected) {
                    m_operations->setCurrentItem(item);
                }
            }
        }
    }
    if (current != nullptr) {
        const QSignalBlocker a(m_stockSide);
        const QSignalBlocker b(m_stockTop);
        const QSignalBlocker c(m_stockBottom);
        const QSignalBlocker d(m_travelX);
        const QSignalBlocker e(m_travelY);
        const QSignalBlocker f(m_travelZ);
        const QSignalBlocker g(m_rapid);
        const QSignalBlocker h(m_maxFeed);
        const QSignalBlocker i(m_maxSpindle);
        m_stockSide->setValue(current->stock.side);
        m_stockTop->setValue(current->stock.top);
        m_stockBottom->setValue(current->stock.bottom);
        m_machineName->setText(QString::fromStdString(current->machine.name));
        m_dialect->setCurrentIndex(current->machine.dialect == document::PostDialect::Grbl ? 0 : 1);
        m_origin->setCurrentIndex(current->origin == document::WorkOrigin::StockTopCorner ? 0 : 1);
        m_travelX->setValue(current->machine.travelX);
        m_travelY->setValue(current->machine.travelY);
        m_travelZ->setValue(current->machine.travelZ);
        m_rapid->setValue(current->machine.rapidRate);
        m_maxFeed->setValue(current->machine.maxFeed);
        m_maxSpindle->setValue(current->machine.maxSpindle);
    }

    const MachiningOperation* selected = operation();
    m_operationBox->setVisible(selected != nullptr);
    if (selected != nullptr) {
        const QSignalBlocker a(m_feed);
        const QSignalBlocker b(m_plunge);
        const QSignalBlocker c(m_spindle);
        const QSignalBlocker d(m_stepover);
        const QSignalBlocker e(m_stepdown);
        const QSignalBlocker f(m_clearance);
        const QSignalBlocker g(m_retract);
        const QSignalBlocker h(m_leave);
        const QSignalBlocker i(m_peck);
        const QSignalBlocker j(m_suppressed);
        m_operationName->setText(QString::fromStdString(selected->name));
        m_tool->clear();
        for (const Tool& tool : m_document->tools()) {
            m_tool->addItem(QString::fromStdString(tool.name),
                            QVariant::fromValue<qulonglong>(tool.id.value));
            if (tool.id == selected->tool) {
                m_tool->setCurrentIndex(m_tool->count() - 1);
            }
        }
        m_feed->setValue(selected->feed);
        m_plunge->setValue(selected->plungeFeed);
        m_spindle->setValue(selected->spindle);
        m_stepover->setValue(selected->stepover);
        m_stepdown->setValue(selected->stepdown);
        m_clearance->setValue(selected->clearance);
        m_retract->setValue(selected->retract);
        m_leave->setValue(selected->stockToLeave);
        m_side->setCurrentIndex(selected->side == document::ContourSide::Outside ? 0 : 1);
        m_peck->setValue(selected->peck);
        m_suppressed->setChecked(selected->suppressed);
        const auto faces = static_cast<int>(selected->targets.size());
        // What this kind of operation has.
        const bool drilling = selected->kind == OperationKind::Drill;
        m_operationForm->setRowVisible(m_stepover, selected->kind == OperationKind::Face ||
                                                       selected->kind == OperationKind::Pocket);
        m_operationForm->setRowVisible(m_stepdown, !drilling);
        m_operationForm->setRowVisible(m_leave, !drilling);
        m_operationForm->setRowVisible(m_side, selected->kind == OperationKind::Contour);
        m_operationForm->setRowVisible(m_peck, drilling);
        m_targets->setText(selected->kind == OperationKind::Face ? tr("Works on the stock's top")
                           : faces == 1                          ? tr("Works on 1 face")
                                        : tr("Works on %1 faces").arg(faces));
    }

    QStringList problems;
    for (const cam::Problem& problem : m_program.problems) {
        problems.append(QString::fromStdString(problem.text));
    }
    if (m_simulation != nullptr) {
        for (const cam::Problem& problem : m_report.problems) {
            problems.append(QString::fromStdString(problem.text));
        }
    }
    m_problems->setText(problems.join(QLatin1Char('\n')));
    m_problems->setVisible(!problems.isEmpty());
    m_summary->setText(summaryText());
    m_filling = false;
}

QString MachiningWorkspace::problemsText() const {
    return m_problems->text();
}

QString MachiningWorkspace::summaryText() const {
    if (m_simulation == nullptr) {
        return setup() == nullptr ? tr("No setup yet") : tr("Not simulated yet");
    }
    return tr("%1 of cutting and %2 of rapids; %3 cm³ removed")
        .arg(timeText(m_report.cuttingSeconds), timeText(m_report.rapidSeconds))
        .arg(m_report.removedVolume / 1000.0, 0, 'f', 1);
}

void MachiningWorkspace::change(const Setup& changed, const QString& label) {
    m_document->changeSetup(changed, label.toStdString());
}

void MachiningWorkspace::editOperation(const QString& label,
                                       const std::function<void(MachiningOperation&)>& edit) {
    if (m_filling || setup() == nullptr || operation() == nullptr) {
        return;
    }
    Setup changed = *setup();
    for (MachiningOperation& each : changed.operations) {
        if (each.id == m_selected) {
            edit(each);
        }
    }
    change(changed, label);
}

void MachiningWorkspace::newSetup() {
    if (m_document == nullptr) {
        return;
    }
    if (m_document->tools().empty()) {
        for (const Tool& tool : firstTools()) {
            m_document->addTool(tool);
        }
    }
    Setup fresh;
    fresh.name = m_document->uniqueSetupName("Setup");
    m_current = m_document->addSetup(std::move(fresh));
    m_selected = {};
    refresh();
}

void MachiningWorkspace::addOperation(OperationKind kind,
                                      const std::vector<OperationTarget>& targets) {
    if (setup() == nullptr) {
        newSetup();
    }
    if (setup() == nullptr) {
        return;
    }
    Setup changed = *setup();
    MachiningOperation fresh;
    fresh.id = m_document->newId();
    fresh.kind = kind;
    int number = 1;
    for (const MachiningOperation& each : changed.operations) {
        number += each.kind == kind ? 1 : 0;
    }
    fresh.name = (kindName(kind) + QStringLiteral(" %1").arg(number)).toStdString();
    fresh.targets = targets;
    // The first tool of the kind it takes.
    const auto suits = [kind](const Tool& tool) {
        return kind == OperationKind::Drill ? tool.kind == ToolKind::Drill
                                            : tool.kind == ToolKind::FlatEndMill;
    };
    const auto& tools = m_document->tools();
    const auto tool = std::find_if(tools.begin(), tools.end(), suits);
    if (tool != tools.end()) {
        fresh.tool = tool->id;
        fresh.stepover = tool->diameter * 0.4;
        fresh.stepdown = tool->diameter * 0.5;
    }
    if (kind == OperationKind::Drill) {
        fresh.feed = 200.0;
        fresh.plungeFeed = 150.0;
        fresh.spindle = std::min(6000.0, changed.machine.maxSpindle);
    }
    fresh.spindle = std::min(fresh.spindle, changed.machine.maxSpindle);
    changed.operations.push_back(fresh);
    m_selected = fresh.id;
    change(changed, tr("Add %1").arg(kindName(kind)));
    fill();
}

void MachiningWorkspace::selectOperation(ObjectId id) {
    m_selected = id;
    fill();
    emit stateChanged();
}

bool MachiningWorkspace::removeOperation() {
    if (operation() == nullptr) {
        return false;
    }
    Setup changed = *setup();
    const ObjectId gone = m_selected;
    std::erase_if(changed.operations,
                  [gone](const MachiningOperation& each) { return each.id == gone; });
    m_selected = {};
    change(changed, tr("Delete Operation"));
    return true;
}

void MachiningWorkspace::setTargets(const std::vector<OperationTarget>& targets) {
    editOperation(tr("Change Faces"),
                  [&targets](MachiningOperation& each) { each.targets = targets; });
}

kernel::Mesh MachiningWorkspace::partMesh() const {
    std::vector<kernel::Shape> shapes;
    for (const document::Instance& instance : m_document->instances()) {
        if (!instance.visible) {
            continue;
        }
        for (const document::Body& body : m_document->bodies()) {
            if (body.component == instance.component && !body.shape.isNull() && body.visible) {
                shapes.push_back(kernel::placed(body.shape, instance.placement));
            }
        }
    }
    return kernel::triangulate(kernel::makeCompound(shapes), {0.02, 10.0});
}

void MachiningWorkspace::simulate() {
    endSimulation();
    if (setup() == nullptr || m_program.paths.empty()) {
        emit message(tr("There is nothing to simulate yet."));
        return;
    }
    m_simulated = std::make_unique<cam::Program>(m_program);
    m_mesh = partMesh();
    m_simulation = std::make_unique<cam::Simulation>(*m_simulated, m_mesh, setup()->machine);
    m_report = m_simulation->run();
    {
        const QSignalBlocker blocker(m_slider);
        m_slider->setEnabled(true);
        m_slider->setRange(0, static_cast<int>(m_simulation->moveCount()));
        m_slider->setValue(m_slider->maximum());
    }
    showSimulationAt(m_simulation->movesDone());
    fill();
    emit stateChanged();
}

void MachiningWorkspace::showMove(std::size_t move) {
    if (m_simulation == nullptr) {
        return;
    }
    if (move < m_simulation->movesDone()) {
        // The stock cannot grow back: start over.
        m_simulation = std::make_unique<cam::Simulation>(*m_simulated, m_mesh, setup()->machine);
    }
    m_simulation->runTo(move);
    showSimulationAt(move);
}

void MachiningWorkspace::showSimulationAt(std::size_t) {
    const cam::HeightField& stock = m_simulation->stock();
    const int step = std::max(1, std::max(stock.columns(), stock.rows()) / 400);
    m_viewport->setStockSurface(m_simulation->stockMesh(step));
    showTool();
}

void MachiningWorkspace::showTool() {
    const document::Tool* tool = m_simulation != nullptr ? m_simulation->tool() : nullptr;
    if (tool == nullptr) {
        m_viewport->setToolShape({});
        return;
    }
    const kernel::Point at = m_simulation->toolPosition();
    m_viewport->setToolShape(kernel::translated(
        kernel::makeCylinder(tool->diameter / 2.0, tool->stickout), at.x, at.y, at.z));
}

void MachiningWorkspace::endSimulation() {
    if (m_simulation == nullptr) {
        return;
    }
    m_simulation.reset();
    m_simulated.reset();
    m_viewport->setStockSurface({});
    m_viewport->setToolShape({});
    m_slider->setEnabled(false);
}

std::optional<std::string> MachiningWorkspace::gcode(QString* error) {
    const auto refuse = [error](const QString& why) {
        if (error != nullptr) {
            *error = why;
        }
        return std::nullopt;
    };
    const Setup* current = setup();
    if (current == nullptr || m_program.paths.empty()) {
        return refuse(tr("There are no toolpaths to write."));
    }
    if (m_program.hasErrors()) {
        for (const cam::Problem& problem : m_program.problems) {
            if (problem.severity == cam::Problem::Severity::Error) {
                return refuse(QString::fromStdString(problem.text));
            }
        }
    }
    // What it would do to the stock, every time: the model may have changed since it was run.
    const kernel::Mesh mesh = partMesh();
    cam::Simulation simulation(m_program, mesh, current->machine);
    const cam::SimulationReport report = simulation.run();
    if (report.hasErrors()) {
        return refuse(QString::fromStdString(report.problems.front().text));
    }
    std::string text = cam::writeGcode(m_program, current->machine, current->name);
    const std::vector<std::string> wrong =
        cam::verifyGcode(text, m_program, current->machine.dialect);
    if (!wrong.empty()) {
        return refuse(tr("The G-code did not read back as the same moves: %1")
                          .arg(QString::fromStdString(wrong.front())));
    }
    return text;
}

bool MachiningWorkspace::exportTo(const QString& path, QString* error) {
    const std::optional<std::string> text = gcode(error);
    if (!text.has_value()) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) {
            *error = tr("Could not write %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    file.write(text->data(), static_cast<qint64>(text->size()));
    return true;
}

} // namespace lcad::app
