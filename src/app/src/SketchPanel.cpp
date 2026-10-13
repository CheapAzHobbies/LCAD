// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/SketchPanel.h>

#include <lcad/app/Icons.h>
#include <lcad/app/PropertiesPanel.h>

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lcad::app {

namespace {

const double pi = 3.14159265358979323846;

QLabel* secondary(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("secondary"));
    label->setWordWrap(true);
    return label;
}

} // namespace

QString describeSketchState(const sketch::SolveResult& state) {
    if (state.status == sketch::SolveStatus::Conflicting) {
        const auto conflicts = static_cast<int>(state.conflicting.size());
        return conflicts == 1
                   ? SketchPanel::tr("Over-constrained: 1 constraint conflicts")
                   : SketchPanel::tr("Over-constrained: %1 constraints conflict").arg(conflicts);
    }
    if (state.degreesOfFreedom == 0) {
        return SketchPanel::tr("Fully constrained");
    }
    return state.degreesOfFreedom == 1
               ? SketchPanel::tr("1 degree of freedom")
               : SketchPanel::tr("%1 degrees of freedom").arg(state.degreesOfFreedom);
}

QString describeSketchState(const sketch::Sketch& geometry, const sketch::SolveResult& state) {
    // Only the origin: an empty sketch is fully constrained, and saying so would mislead.
    if (geometry.entities().size() <= 1) {
        return SketchPanel::tr("Nothing drawn yet");
    }
    return describeSketchState(state);
}

QString describeSketchEntity(const sketch::Sketch& geometry, sketch::EntityId id) {
    const sketch::Entity* entity = geometry.find(id);
    if (entity == nullptr) {
        return {};
    }
    const QString prefix = entity->construction ? SketchPanel::tr("Construction ") : QString();
    switch (entity->kind) {
    case sketch::EntityKind::Point:
        if (id == geometry.origin()) {
            return SketchPanel::tr("Origin");
        }
        return SketchPanel::tr("Point · X %1 · Y %2 mm")
            .arg(formatNumber(entity->x), formatNumber(entity->y));
    case sketch::EntityKind::Line: {
        const sketch::Point2 a = geometry.position(entity->start);
        const sketch::Point2 b = geometry.position(entity->end);
        return prefix + SketchPanel::tr("Line · length %1 mm")
                            .arg(formatNumber(std::hypot(b.x - a.x, b.y - a.y)));
    }
    case sketch::EntityKind::Circle:
        return prefix + SketchPanel::tr("Circle · Ø%1 mm").arg(formatNumber(2.0 * entity->radius));
    case sketch::EntityKind::Arc:
        return prefix + SketchPanel::tr("Arc · R%1 mm").arg(formatNumber(geometry.radiusOf(id)));
    }
    return {};
}

QString describeConstraint(const sketch::Constraint& constraint) {
    const QString name = viewport::SketchEditor::constraintName(constraint.kind);
    if (!sketch::isDimension(constraint.kind)) {
        return name;
    }
    if (constraint.kind == sketch::ConstraintKind::Angle) {
        return SketchPanel::tr("%1 %2°").arg(name,
                                             formatNumber(std::abs(constraint.value) * 180.0 / pi));
    }
    return SketchPanel::tr("%1 %2 mm").arg(name, formatNumber(constraint.value));
}

SketchPanel::SketchPanel(viewport::SketchEditor* editor, QWidget* parent)
    : QWidget(parent), m_editor(editor), m_heading(new QLabel(this)), m_state(new QLabel(this)),
      m_profiles(secondary(QString(), this)), m_selection(new QLabel(this)),
      m_inference(new QCheckBox(tr("Pick up constraints while drawing"), this)),
      m_construction(new QCheckBox(tr("Draw construction geometry"), this)),
      m_sides(new QSpinBox(this)), m_constraintList(new QVBoxLayout),
      m_finish(new QPushButton(tr("Finish Sketch"), this)) {
    auto* layout = new QVBoxLayout(this);

    QFont bold = m_heading->font();
    bold.setBold(true);
    m_heading->setFont(bold);
    m_heading->setObjectName(QStringLiteral("sketchHeading"));
    m_state->setObjectName(QStringLiteral("sketchState"));
    m_state->setWordWrap(true);
    m_state->setToolTip(tr("A sketch is fully constrained when nothing in it can still move. "
                           "Geometry that can still move is drawn in blue."));
    layout->addWidget(m_heading);
    layout->addWidget(m_state);
    layout->addWidget(m_profiles);
    layout->addSpacing(8);

    layout->addWidget(secondary(tr("Drawing"), this));
    m_inference->setObjectName(QStringLiteral("sketchInference"));
    m_inference->setToolTip(tr("Ends that land on existing points or curves stay there, and "
                               "nearly level or upright lines become exactly so."));
    m_construction->setObjectName(QStringLiteral("sketchConstruction"));
    m_construction->setToolTip(
        tr("Construction geometry guides the sketch and is left out of its outlines."));
    layout->addWidget(m_inference);
    layout->addWidget(m_construction);
    auto* form = new QFormLayout;
    m_sides->setObjectName(QStringLiteral("polygonSides"));
    m_sides->setRange(3, 64);
    form->addRow(secondary(tr("Polygon sides"), this), m_sides);
    layout->addLayout(form);
    layout->addSpacing(8);

    layout->addWidget(secondary(tr("Selection"), this));
    m_selection->setObjectName(QStringLiteral("sketchSelection"));
    m_selection->setWordWrap(true);
    layout->addWidget(m_selection);
    layout->addLayout(m_constraintList);
    layout->addStretch(1);

    m_finish->setObjectName(QStringLiteral("finishSketchButton"));
    m_finish->setDefault(true);
    layout->addWidget(m_finish);

    connect(m_inference, &QCheckBox::toggled, this,
            [this](bool on) { m_editor->setInference(on); });
    connect(m_construction, &QCheckBox::toggled, this,
            [this](bool on) { m_editor->setConstructionMode(on); });
    connect(m_sides, &QSpinBox::valueChanged, this,
            [this](int sides) { m_editor->setPolygonSides(sides); });
    connect(m_finish, &QPushButton::clicked, this, &SketchPanel::finishRequested);
}

QString SketchPanel::heading() const {
    return m_heading->text();
}

QString SketchPanel::stateText() const {
    return m_state->text();
}

QString SketchPanel::selectionText() const {
    return m_selection->text();
}

void SketchPanel::refresh(const QString& name) {
    if (!m_editor->isActive()) {
        return;
    }
    const sketch::Sketch& geometry = m_editor->geometry();
    const sketch::SolveResult& state = m_editor->state();
    m_heading->setText(name);
    m_state->setText(describeSketchState(geometry, state));
    const char* kind = state.status == sketch::SolveStatus::Conflicting ? "conflict"
                       : state.degreesOfFreedom == 0                    ? "full"
                                                                        : "free";

    if (m_state->property("state").toString() != QLatin1String(kind)) {
        m_state->setProperty("state", QLatin1String(kind));
        m_state->style()->unpolish(m_state);
        m_state->style()->polish(m_state);
    }
    const auto profiles = static_cast<int>(geometry.profiles().size());
    m_profiles->setText(profiles == 0   ? tr("No closed outline yet")
                        : profiles == 1 ? tr("1 closed outline")
                                        : tr("%1 closed outlines").arg(profiles));
    {
        const QSignalBlocker a(m_inference);
        const QSignalBlocker b(m_construction);
        const QSignalBlocker c(m_sides);
        m_inference->setChecked(m_editor->hasInference());
        m_construction->setChecked(m_editor->constructionMode());
        m_sides->setValue(m_editor->polygonSides());
    }

    const std::vector<sketch::EntityId> entities = m_editor->selectedEntities();
    const std::vector<sketch::ConstraintId> constraints = m_editor->selectedConstraints();
    const std::size_t total = entities.size() + constraints.size();
    if (total == 0) {
        m_selection->setText(tr("Nothing selected"));
    } else if (total > 1) {
        m_selection->setText(tr("%1 items selected").arg(total));
    } else if (!entities.empty()) {
        m_selection->setText(describeSketchEntity(geometry, entities.front()));
    } else if (const sketch::Constraint* constraint =
                   geometry.findConstraint(constraints.front())) {
        m_selection->setText(describeConstraint(*constraint));
    }
    rebuildConstraints();
}

void SketchPanel::rebuildConstraints() {
    const sketch::Sketch& geometry = m_editor->geometry();
    const std::vector<sketch::EntityId> entities = m_editor->selectedEntities();
    const std::vector<sketch::ConstraintId> chosen = m_editor->selectedConstraints();
    // The constraints that are selected, and those on anything that is: on a curve, also those
    // on its points.
    std::vector<sketch::EntityId> about = entities;
    for (const sketch::EntityId id : entities) {
        for (const sketch::Entity& entity : geometry.entities()) {
            if (entity.owner == id) {
                about.push_back(entity.id);
            }
        }
    }
    const auto touches = [&about](const sketch::Constraint& c) {
        return std::any_of(about.begin(), about.end(), [&c](sketch::EntityId id) {
            return c.first == id || c.second == id || c.third == id;
        });
    };
    std::vector<sketch::Constraint> listed;
    QString signature;
    for (const sketch::Constraint& constraint : geometry.constraints()) {
        if (std::find(chosen.begin(), chosen.end(), constraint.id) != chosen.end() ||
            touches(constraint)) {
            listed.push_back(constraint);
            signature += QStringLiteral("%1:%2;").arg(constraint.id).arg(constraint.value);
        }
    }
    if (signature == m_signature) {
        return;
    }
    m_signature = signature;
    while (QLayoutItem* item = m_constraintList->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            // Out of sight at once. It is deleted later because this may be running inside a
            // click on one of its own buttons.
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    m_rows.clear();
    m_listed.clear();
    for (const sketch::Constraint& constraint : listed) {
        auto* row = new QWidget(this);
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        const QString text = describeConstraint(constraint);
        line->addWidget(new QLabel(text, row), 1);
        auto* remove = new QToolButton(row);
        remove->setObjectName(QStringLiteral("removeConstraint"));
        remove->setAutoRaise(true);
        remove->setIcon(icon(QStringLiteral("delete")));
        remove->setToolTip(tr("Remove this constraint"));
        remove->setAccessibleName(tr("Remove %1").arg(text));
        const sketch::ConstraintId id = constraint.id;
        connect(remove, &QToolButton::clicked, this, [this, id] {
            m_editor->setSelection({}, {id});
            m_editor->deleteSelection();
        });
        line->addWidget(remove);
        m_constraintList->addWidget(row);
        // Shown now, not when the event loop next gets round to it.
        row->show();
        m_rows << text;
        m_listed.push_back(id);
    }
}

void SketchPanel::removeConstraint(int row) {
    if (row < 0 || row >= static_cast<int>(m_listed.size())) {
        return;
    }
    m_editor->setSelection({}, {m_listed[static_cast<std::size_t>(row)]});
    m_editor->deleteSelection();
}

SketchPlanePanel::SketchPlanePanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* text = new QLabel(tr("Click a plane in the 3D view, or a flat face of a body."), this);
    text->setObjectName(QStringLiteral("sketchPlaneHint"));
    text->setWordWrap(true);
    layout->addWidget(text);
    layout->addSpacing(8);
    layout->addWidget(secondary(tr("Or choose here"), this));
    const QString names[3] = {tr("XY Plane (seen from the top)"),
                              tr("XZ Plane (seen from the front)"),
                              tr("YZ Plane (seen from the right)")};
    const char* const objectNames[3] = {"sketchPlaneXY", "sketchPlaneXZ", "sketchPlaneYZ"};
    for (int i = 0; i < 3; ++i) {
        m_buttons[i] = new QPushButton(names[i], this);
        m_buttons[i]->setObjectName(QLatin1String(objectNames[i]));
        // A swatch in the color the plane has in the 3D view, to tell which is which.
        // With a clear strip on its right, to stand a little off the words.
        QPixmap swatch(20, 14);
        swatch.fill(Qt::transparent);
        {
            QPainter painter(&swatch);
            painter.fillRect(
                0, 0, 14, 14,
                viewport::ViewportWidget::originColor(static_cast<viewport::OriginItem>(i)));
        }
        m_buttons[i]->setIcon(QIcon(swatch));
        m_buttons[i]->setIconSize(swatch.size());
        layout->addWidget(m_buttons[i]);
        connect(m_buttons[i], &QPushButton::clicked, this, [this, i] { emit planeChosen(i); });
    }
    layout->addSpacing(8);
    auto* cancel = new QPushButton(tr("Cancel"), this);
    cancel->setObjectName(QStringLiteral("sketchPlaneCancel"));
    layout->addWidget(cancel);
    connect(cancel, &QPushButton::clicked, this, &SketchPlanePanel::canceled);
    layout->addStretch(1);
    setFocusProxy(m_buttons[0]);
}

void SketchPlanePanel::setHoveredPlane(int plane) {
    m_hovered = plane >= 0 && plane < 3 ? plane : -1;
    for (int i = 0; i < 3; ++i) {
        m_buttons[i]->setProperty("planeHovered", i == m_hovered);
        m_buttons[i]->style()->unpolish(m_buttons[i]);
        m_buttons[i]->style()->polish(m_buttons[i]);
    }
}

QPushButton* SketchPlanePanel::planeButton(int plane) const {
    return plane >= 0 && plane < 3 ? m_buttons[plane] : nullptr;
}

void SketchPlanePanel::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        emit canceled();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace lcad::app
