// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/SketchPanel.h>

#include <lcad/app/Icons.h>
#include <lcad/app/PropertiesPanel.h>
#include <lcad/sketch/Edit.h>

#include <QCheckBox>
#include <QEnterEvent>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QMouseEvent>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace lcad::app {

namespace {

const double pi = 3.14159265358979323846;

QLabel* secondary(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("secondary"));
    label->setWordWrap(true);
    return label;
}

QString capitalized(QString text) {
    if (!text.isEmpty()) {
        text[0] = text.at(0).toUpper();
    }
    return text;
}

// The icon a kind of constraint goes by on the toolbar.
QString iconOf(sketch::ConstraintKind kind) {
    using Kind = sketch::ConstraintKind;
    switch (kind) {
    case Kind::Coincident:
    case Kind::PointOnLine:
    case Kind::PointOnCircle:
        return QStringLiteral("c-coincident");
    case Kind::Horizontal:
        return QStringLiteral("c-horizontal");
    case Kind::Vertical:
        return QStringLiteral("c-vertical");
    case Kind::Parallel:
        return QStringLiteral("c-parallel");
    case Kind::Perpendicular:
        return QStringLiteral("c-perpendicular");
    case Kind::Tangent:
        return QStringLiteral("c-tangent");
    case Kind::Equal:
        return QStringLiteral("c-equal");
    case Kind::Concentric:
        return QStringLiteral("c-concentric");
    case Kind::Midpoint:
        return QStringLiteral("c-midpoint");
    case Kind::Symmetric:
        return QStringLiteral("c-symmetric");
    case Kind::Collinear:
        return QStringLiteral("c-collinear");
    case Kind::Fix:
        return QStringLiteral("c-fix");
    default:
        return QStringLiteral("dimension");
    }
}

// A line of the constraint list, which says when the pointer comes onto it and when it is clicked.
class ConstraintRow : public QFrame {
public:
    ConstraintRow(QWidget* parent, std::function<void(bool)> hovered, std::function<void()> clicked)
        : QFrame(parent), m_hovered(std::move(hovered)), m_clicked(std::move(clicked)) {
        setObjectName(QStringLiteral("constraintRow"));
    }

protected:
    void enterEvent(QEnterEvent* event) override {
        QFrame::enterEvent(event);
        m_hovered(true);
    }
    void leaveEvent(QEvent* event) override {
        QFrame::leaveEvent(event);
        m_hovered(false);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        QFrame::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
            m_clicked();
        }
    }

private:
    std::function<void(bool)> m_hovered;
    std::function<void()> m_clicked;
};

// What pressing D does to these: "Press D to give it a length". Empty if it does nothing.
QString dimensionPrompt(const sketch::Sketch& geometry,
                        const std::vector<sketch::EntityId>& entities) {
    const auto choice = sketch::dimensionFor(geometry, entities);
    if (!choice.has_value()) {
        return {};
    }
    switch (choice->kind) {
    case sketch::ConstraintKind::Distance:
        return entities.size() == 1 ? SketchPanel::tr("Press D to give it a length")
                                    : SketchPanel::tr("Press D to give the distance between them");
    case sketch::ConstraintKind::Diameter:
        return SketchPanel::tr("Press D to give it a diameter");
    case sketch::ConstraintKind::Radius:
        return SketchPanel::tr("Press D to give it a radius");
    case sketch::ConstraintKind::Angle:
        return SketchPanel::tr("Press D to give the angle between them");
    default:
        return SketchPanel::tr("Press D to give the distance between them");
    }
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
    case sketch::EntityKind::Ellipse: {
        const sketch::Point2 c = geometry.position(entity->center);
        const sketch::Point2 m = geometry.position(entity->start);
        return prefix + SketchPanel::tr("Ellipse · R%1 × r%2 mm")
                            .arg(formatNumber(std::hypot(m.x - c.x, m.y - c.y)),
                                 formatNumber(entity->radius));
    }
    case sketch::EntityKind::Spline:
        return prefix + SketchPanel::tr("Spline · %1 control points").arg(entity->points.size());
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

QString describeConstraint(const sketch::Sketch& geometry, const sketch::Constraint& constraint) {
    using Kind = sketch::ConstraintKind;
    const sketch::Entity* first = geometry.find(constraint.first);
    const sketch::Entity* second = geometry.find(constraint.second);
    // The length of a line is a distance between its own ends: that is what the user calls it.
    if (constraint.kind == Kind::Distance && first != nullptr && second != nullptr &&
        first->owner != 0 && first->owner == second->owner) {
        return SketchPanel::tr("Length %1 mm · %2")
            .arg(formatNumber(constraint.value),
                 viewport::SketchEditor::entityName(geometry, first->owner));
    }
    QStringList names;
    for (const sketch::EntityId id : {constraint.first, constraint.second, constraint.third}) {
        if (geometry.find(id) != nullptr) {
            names << viewport::SketchEditor::entityName(geometry, id);
        }
    }
    return SketchPanel::tr("%1 · %2").arg(describeConstraint(constraint),
                                          names.join(QStringLiteral(", ")));
}

SketchPanel::SketchPanel(viewport::SketchEditor* editor, QWidget* parent)
    : QWidget(parent), m_editor(editor), m_heading(new QLabel(this)), m_state(new QLabel(this)),
      m_profiles(secondary(QString(), this)), m_prompt(new QLabel(this)),
      m_selection(new QLabel(this)),
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
    m_prompt->setObjectName(QStringLiteral("sketchPrompt"));
    m_prompt->setWordWrap(true);
    m_prompt->setTextFormat(Qt::RichText);
    m_prompt->hide();
    layout->addWidget(m_prompt);

    layout->addWidget(secondary(tr("Drawing"), this));
    m_inference->setObjectName(QStringLiteral("sketchInference"));
    m_inference->setToolTip(tr("Ends that land on existing points or curves stay there, and "
                               "nearly level or upright lines become exactly so."));
    m_construction->setObjectName(QStringLiteral("sketchConstruction"));
    m_construction->setToolTip(
        tr("Construction geometry guides the sketch and is left out of its outlines."));
    layout->addWidget(m_inference);
    layout->addWidget(m_construction);
    m_sidesForm = new QFormLayout;
    m_sides->setObjectName(QStringLiteral("polygonSides"));
    m_sides->setRange(3, 64);
    m_sidesForm->addRow(secondary(tr("Polygon sides"), this), m_sides);
    layout->addLayout(m_sidesForm);
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
    connect(m_editor, &viewport::SketchEditor::hoverChanged, this, [this] {
        if (m_editor->isActive()) {
            rebuildConstraints();
            markRows();
        }
    });
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
    m_sidesForm->setRowVisible(m_sides, m_editor->tool() == viewport::SketchTool::Polygon);

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
    // What the tool waits for, said where the eye is while working: "Dimension: click a line",
    // shown with the tool's name above what it waits for. With nothing else to say, what the
    // selection can be given.
    QString prompt = m_editor->hint();
    if (prompt.isEmpty() && constraints.empty()) {
        prompt = dimensionPrompt(geometry, entities);
    }
    if (prompt != m_promptText) {
        m_promptText = prompt;
        const qsizetype colon = prompt.indexOf(QStringLiteral(": "));
        m_prompt->setText(colon < 0 ? prompt.toHtmlEscaped()
                                    : QStringLiteral("<b>%1</b><br>%2")
                                          .arg(prompt.left(colon).toHtmlEscaped(),
                                               capitalized(prompt.mid(colon + 2)).toHtmlEscaped()));
        m_prompt->setVisible(!prompt.isEmpty());
    }
    rebuildConstraints();
}

void SketchPanel::rebuildConstraints() {
    const sketch::Sketch& geometry = m_editor->geometry();
    std::vector<sketch::EntityId> entities = m_editor->selectedEntities();
    const std::vector<sketch::ConstraintId> chosen = m_editor->selectedConstraints();
    // With nothing selected, what the pointer is on says what is wanted.
    if (entities.empty() && chosen.empty() && m_editor->hoveredEntity() != 0) {
        entities = {m_editor->hoveredEntity()};
    }
    // The constraints that are selected, and those on anything that is: on a curve, also those
    // on its points; on a point, those on every point in that place, which is what holds a
    // corner together.
    std::vector<sketch::EntityId> about = entities;
    for (const sketch::EntityId id : entities) {
        const sketch::Entity* selected = geometry.find(id);
        if (selected != nullptr && selected->kind == sketch::EntityKind::Point) {
            const auto together = sketch::pointsTogether(geometry, id);
            about.insert(about.end(), together.begin(), together.end());
        }
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
            signature += QStringLiteral("%1:%2;").arg(constraint.id).arg(
                describeConstraint(geometry, constraint));
        }
    }
    if (signature == m_signature) {
        return;
    }
    m_signature = signature;
    // The row the pointer was on may be going.
    m_editor->showConstraint(0);
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
    m_rowWidgets.clear();
    for (const sketch::Constraint& constraint : listed) {
        const sketch::ConstraintId id = constraint.id;
        auto* row = new ConstraintRow(
            this,
            [this, id](bool on) {
                m_editor->showConstraint(on ? id : 0);
                markRows();
            },
            [this, id] {
                m_editor->pinConstraint(m_editor->pinnedConstraint() == id ? 0 : id);
                markRows();
            });
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(4, 2, 0, 2);
        const QString text = describeConstraint(geometry, constraint);
        const qsizetype split = text.indexOf(QStringLiteral(" · "));
        auto* symbol = new QLabel(row);
        symbol->setPixmap(icon(iconOf(constraint.kind)).pixmap(16, 16));
        line->addWidget(symbol, 0, Qt::AlignTop);
        auto* words = new QVBoxLayout;
        words->setSpacing(0);
        words->addWidget(new QLabel(text.left(split), row));
        words->addWidget(secondary(text.mid(split + 3), row));
        line->addLayout(words, 1);
        auto* remove = new QToolButton(row);
        remove->setObjectName(QStringLiteral("removeConstraint"));
        remove->setAutoRaise(true);
        remove->setIcon(icon(QStringLiteral("delete")));
        remove->setToolTip(constraint.kind == sketch::ConstraintKind::Coincident
                               ? tr("Disconnect")
                               : tr("Remove this constraint"));
        remove->setAccessibleName(tr("Remove %1").arg(text));
        connect(remove, &QToolButton::clicked, this, [this, id] {
            m_editor->removeConstraint(id);
        });
        line->addWidget(remove);
        m_constraintList->addWidget(row);
        // Shown now, not when the event loop next gets round to it.
        row->show();
        m_rows << text;
        m_listed.push_back(id);
        m_rowWidgets.push_back(row);
    }
    markRows();
}

void SketchPanel::markRows() {
    if (!m_editor->isActive()) {
        return;
    }
    const sketch::Sketch& geometry = m_editor->geometry();
    const sketch::EntityId hovered = m_editor->hoveredEntity();
    // The point the pointer is on, and every other in that place; or the curve and its points.
    std::vector<sketch::EntityId> about;
    if (hovered != 0) {
        const sketch::Entity* entity = geometry.find(hovered);
        if (entity != nullptr && entity->kind == sketch::EntityKind::Point) {
            about = sketch::pointsTogether(geometry, hovered);
        } else {
            about = {hovered};
            for (const sketch::Entity& other : geometry.entities()) {
                if (other.owner == hovered) {
                    about.push_back(other.id);
                }
            }
        }
    }
    for (std::size_t i = 0; i < m_rowWidgets.size(); ++i) {
        const sketch::Constraint* constraint = geometry.findConstraint(m_listed[i]);
        if (constraint == nullptr) {
            continue;
        }
        const bool lit = constraint->id == m_editor->hoveredConstraint() ||
                         std::any_of(about.begin(), about.end(), [constraint](sketch::EntityId id) {
                             return constraint->first == id || constraint->second == id ||
                                    constraint->third == id;
                         });
        const bool pinned = constraint->id == m_editor->pinnedConstraint();
        QWidget* row = m_rowWidgets[i];
        if (row->property("lit").toBool() != lit || row->property("pinned").toBool() != pinned) {
            row->setProperty("lit", lit);
            row->setProperty("pinned", pinned);
            row->style()->unpolish(row);
            row->style()->polish(row);
        }
    }
}

QList<int> SketchPanel::litRows() const {
    QList<int> lit;
    for (std::size_t i = 0; i < m_rowWidgets.size(); ++i) {
        if (m_rowWidgets[i]->property("lit").toBool()) {
            lit << static_cast<int>(i);
        }
    }
    return lit;
}

void SketchPanel::removeConstraint(int row) {
    if (row < 0 || row >= static_cast<int>(m_listed.size())) {
        return;
    }
    m_editor->removeConstraint(m_listed[static_cast<std::size_t>(row)]);
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
