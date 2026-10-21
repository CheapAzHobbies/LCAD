// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/JointPanel.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace lcad::app {

namespace {

const std::array<document::JointKind, 7> kinds{
    document::JointKind::Rigid,       document::JointKind::Revolute, document::JointKind::Slider,
    document::JointKind::Cylindrical, document::JointKind::PinSlot,  document::JointKind::Planar,
    document::JointKind::Ball,
};

} // namespace

QString jointKindName(document::JointKind kind) {
    switch (kind) {
    case document::JointKind::Rigid:
        return JointPanel::tr("Rigid");
    case document::JointKind::Revolute:
        return JointPanel::tr("Revolute");
    case document::JointKind::Slider:
        return JointPanel::tr("Slider");
    case document::JointKind::Cylindrical:
        return JointPanel::tr("Cylindrical");
    case document::JointKind::PinSlot:
        return JointPanel::tr("Pin-Slot");
    case document::JointKind::Planar:
        return JointPanel::tr("Planar");
    case document::JointKind::Ball:
        return JointPanel::tr("Ball");
    }
    return {};
}

JointPanel::JointPanel(QWidget* parent) : QWidget(parent) {
    m_filling = true;
    auto* layout = new QVBoxLayout(this);
    auto* heading = new QLabel(tr("Joint"), this);
    heading->setObjectName(QStringLiteral("jointHeading"));
    QFont bold = heading->font();
    bold.setBold(true);
    heading->setFont(bold);
    layout->addWidget(heading);
    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("secondary"));
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);

    auto* form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    layout->addLayout(form);
    m_kind = new QComboBox(this);
    m_kind->setObjectName(QStringLiteral("jointKind"));
    for (const document::JointKind kind : kinds) {
        m_kind->addItem(jointKindName(kind), static_cast<int>(kind));
    }
    m_kind->setToolTip(tr("How the part may move against the other. LCAD suggests one from what "
                          "is picked; choose another to override it."));
    connect(m_kind, &QComboBox::activated, this, [this] {
        m_kindChosen = true;
        showMotions();
        emit changed();
    });
    form->addRow(tr("Type"), m_kind);

    const std::array<QString, 2> sideLabels{tr("Moves"), tr("Onto")};
    for (std::size_t i = 0; i < 2; ++i) {
        auto* row = new QWidget(this);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        m_sideText[i] = new QLabel(row);
        m_sideText[i]->setObjectName(i == 0 ? QStringLiteral("jointMoving")
                                            : QStringLiteral("jointFixed"));
        m_sideText[i]->setWordWrap(true);
        rowLayout->addWidget(m_sideText[i], 1);
        m_repick[i] = new QPushButton(tr("Pick"), row);
        m_repick[i]->setToolTip(tr("Pick this one again in the 3D view."));
        connect(m_repick[i], &QPushButton::clicked, this, [this, i] {
            m_picking = static_cast<int>(i);
            updateHint();
        });
        rowLayout->addWidget(m_repick[i]);
        form->addRow(sideLabels[i], row);
        m_snap[i] = new QComboBox(this);
        m_snap[i]->setObjectName(i == 0 ? QStringLiteral("movingSnap")
                                        : QStringLiteral("fixedSnap"));
        m_snap[i]->addItem(tr("Middle"), static_cast<int>(kernel::FrameSnap::Center));
        m_snap[i]->addItem(tr("Start"), static_cast<int>(kernel::FrameSnap::Start));
        m_snap[i]->addItem(tr("End"), static_cast<int>(kernel::FrameSnap::End));
        m_snap[i]->setToolTip(tr("Where along a straight edge or a round face the joint sits."));
        connect(m_snap[i], &QComboBox::activated, this, [this, i] {
            m_sides[i].snap = static_cast<kernel::FrameSnap>(m_snap[i]->currentData().toInt());
            emit changed();
        });
        form->addRow(tr("At"), m_snap[i]);
    }

    const auto spin = [this](const QString& name, double minimum, double maximum,
                             const QString& suffix) {
        auto* box = new QDoubleSpinBox(this);
        box->setObjectName(name);
        box->setRange(minimum, maximum);
        box->setDecimals(3);
        box->setSuffix(suffix);
        box->setKeyboardTracking(false);
        connect(box, &QDoubleSpinBox::valueChanged, this, [this] {
            if (!m_filling) {
                emit changed();
            }
        });
        return box;
    };
    m_offset = spin(QStringLiteral("jointOffset"), -100000.0, 100000.0, tr(" mm"));
    m_offset->setToolTip(tr("How far the part stands off along the joint's axis."));
    form->addRow(tr("Offset"), m_offset);
    m_angle = spin(QStringLiteral("jointAngle"), -360.0, 360.0, tr("°"));
    m_angle->setToolTip(tr("How far the part is turned about the joint's axis."));
    form->addRow(tr("Angle"), m_angle);
    m_flipped = new QCheckBox(tr("Flip"), this);
    m_flipped->setObjectName(QStringLiteral("jointFlipped"));
    m_flipped->setToolTip(tr("Turn the part over: the two faces point the same way instead of "
                             "at each other."));
    connect(m_flipped, &QCheckBox::toggled, this, [this] {
        if (!m_filling) {
            emit changed();
        }
    });
    form->addRow(QString(), m_flipped);

    // The ways it moves, in the same form, so that their names line up with the others.
    m_motionForm = form;
    for (std::size_t i = 0; i < 3; ++i) {
        m_motionLabels[i] = new QLabel(this);
        m_values[i] = spin(QStringLiteral("jointValue%1").arg(i), -100000.0, 100000.0, QString());
        m_motionForm->addRow(m_motionLabels[i], m_values[i]);
        auto* limits = new QWidget(this);
        auto* limitsLayout = new QHBoxLayout(limits);
        limitsLayout->setContentsMargins(0, 0, 0, 0);
        m_limited[i] = new QCheckBox(tr("Limits"), limits);
        m_limited[i]->setObjectName(QStringLiteral("jointLimited%1").arg(i));
        // Narrower than the other boxes: the two share a line with the check box.
        m_minimum[i] = spin(QStringLiteral("jointMinimum%1").arg(i), -9999.0, 9999.0, {});
        m_maximum[i] = spin(QStringLiteral("jointMaximum%1").arg(i), -9999.0, 9999.0, {});
        m_minimum[i]->setDecimals(2);
        m_maximum[i]->setDecimals(2);
        // They can be narrower than the longest number they take; the panel is narrow.
        m_minimum[i]->setMinimumWidth(56);
        m_maximum[i]->setMinimumWidth(56);
        m_minimum[i]->setToolTip(tr("The least it may move to."));
        m_maximum[i]->setToolTip(tr("The most it may move to."));
        limitsLayout->addWidget(m_limited[i]);
        limitsLayout->addWidget(m_minimum[i], 1);
        limitsLayout->addWidget(m_maximum[i], 1);
        connect(m_limited[i], &QCheckBox::toggled, this, [this, i](bool on) {
            m_minimum[i]->setEnabled(on);
            m_maximum[i]->setEnabled(on);
            if (!m_filling) {
                emit changed();
            }
        });
        m_minimum[i]->setEnabled(false);
        m_maximum[i]->setEnabled(false);
        // Across the whole panel, under the value they limit.
        m_motionForm->addRow(limits);
        m_limitRows[i] = limits;
    }
    m_animate = new QPushButton(tr("Animate"), this);
    m_animate->setObjectName(QStringLiteral("animateJoint"));
    m_animate->setToolTip(tr("Show the joint moving through its range in the 3D view."));
    connect(m_animate, &QPushButton::clicked, this, &JointPanel::animateRequested);
    layout->addWidget(m_animate);

    m_errorLabel = new QLabel(this);
    // "errorText" is what the theme's style sheet colors as an error.
    m_errorLabel->setObjectName(QStringLiteral("errorText"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->hide();
    layout->addWidget(m_errorLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_okButton = buttons->button(QDialogButtonBox::Ok);
    m_okButton->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (m_okButton->isEnabled()) {
            emit accepted();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &JointPanel::canceled);
    layout->addWidget(buttons);
    layout->addStretch(1);

    m_filling = false;
    showMotions();
    updateHint();
    setError({});
}

document::Joint JointPanel::joint() const {
    document::Joint joint;
    joint.id = m_id;
    joint.name = m_name;
    joint.suppressed = m_suppressed;
    joint.kind = static_cast<document::JointKind>(m_kind->currentData().toInt());
    joint.moving = m_sides[0];
    joint.fixed = m_sides[1];
    joint.offset = m_offset->value();
    joint.angle = m_angle->value();
    joint.flipped = m_flipped->isChecked();
    const std::size_t motions = document::motionsOf(joint.kind).size();
    for (std::size_t i = 0; i < motions; ++i) {
        joint.values[i] = m_values[i]->value();
        if (m_limited[i]->isChecked()) {
            joint.limits[i].minimum = std::min(m_minimum[i]->value(), m_maximum[i]->value());
            joint.limits[i].maximum = std::max(m_minimum[i]->value(), m_maximum[i]->value());
        }
    }
    return joint;
}

void JointPanel::setJoint(const document::Joint& joint, const QString& movingText,
                          const QString& fixedText) {
    m_filling = true;
    m_id = joint.id;
    m_name = joint.name;
    m_suppressed = joint.suppressed;
    m_kind->setCurrentIndex(m_kind->findData(static_cast<int>(joint.kind)));
    m_kindChosen = true;
    m_sides = {joint.moving, joint.fixed};
    m_picked = {true, true};
    m_picking = -1;
    m_sideText[0]->setText(movingText);
    m_sideText[1]->setText(fixedText);
    for (std::size_t i = 0; i < 2; ++i) {
        m_snap[i]->setCurrentIndex(m_snap[i]->findData(static_cast<int>(m_sides[i].snap)));
    }
    m_offset->setValue(joint.offset);
    m_angle->setValue(joint.angle);
    m_flipped->setChecked(joint.flipped);
    for (std::size_t i = 0; i < 3; ++i) {
        m_values[i]->setValue(joint.values[i]);
        const document::JointLimits& limits = joint.limits[i];
        m_limited[i]->setChecked(limits.minimum.has_value() || limits.maximum.has_value());
        m_minimum[i]->setValue(limits.minimum.value_or(joint.values[i]));
        m_maximum[i]->setValue(limits.maximum.value_or(joint.values[i]));
    }
    m_filling = false;
    showMotions();
    updateHint();
}

void JointPanel::setSide(int which, const document::JointSide& side, const QString& description) {
    if (which < 0 || which > 1) {
        return;
    }
    const auto i = static_cast<std::size_t>(which);
    const kernel::FrameSnap snap = m_sides[i].snap;
    m_sides[i] = side;
    m_sides[i].snap = m_picked[i] ? snap : side.snap;
    m_picked[i] = true;
    m_sideText[i]->setText(description);
    {
        const QSignalBlocker blocker(m_snap[i]);
        m_snap[i]->setCurrentIndex(m_snap[i]->findData(static_cast<int>(m_sides[i].snap)));
    }
    const int other = 1 - which;
    m_picking = m_picked[static_cast<std::size_t>(other)] ? -1 : other;
    updateHint();
    emit changed();
}

bool JointPanel::hasSide(int which) const {
    return which >= 0 && which <= 1 && m_picked[static_cast<std::size_t>(which)];
}

void JointPanel::suggestKind(document::JointKind kind) {
    if (m_kindChosen) {
        return;
    }
    m_kind->setCurrentIndex(m_kind->findData(static_cast<int>(kind)));
    showMotions();
}

void JointPanel::setKind(document::JointKind kind) {
    m_kind->setCurrentIndex(m_kind->findData(static_cast<int>(kind)));
    m_kindChosen = true;
    showMotions();
    emit changed();
}

void JointPanel::setError(const QString& error) {
    m_error = error;
    m_errorLabel->setText(error);
    m_errorLabel->setVisible(!error.isEmpty());
    m_okButton->setEnabled(error.isEmpty() && m_picked[0] && m_picked[1]);
    m_animate->setEnabled(
        error.isEmpty() && m_picked[0] && m_picked[1] &&
        !document::motionsOf(static_cast<document::JointKind>(m_kind->currentData().toInt()))
             .empty());
}

QString JointPanel::hint() const {
    return m_hint->text();
}

void JointPanel::updateHint() {
    for (std::size_t i = 0; i < 2; ++i) {
        if (!m_picked[i]) {
            m_sideText[i]->setText(m_picking == static_cast<int>(i) ? tr("Click it in the 3D view")
                                                                    : tr("Not picked yet"));
        }
        m_repick[i]->setDown(m_picking == static_cast<int>(i));
    }
    switch (m_picking) {
    case 0:
        m_hint->setText(tr("Click a face or an edge of the part that is to move: a round edge or "
                           "face joins at its center and axis, a flat face at its middle."));
        break;
    case 1:
        m_hint->setText(tr("Now click a face or an edge of the part it goes onto."));
        break;
    default:
        m_hint->setText(tr("Choose how it may move, and how it lines up. Pick again to change "
                           "where it joins."));
        break;
    }
}

void JointPanel::showMotions() {
    const auto kind = static_cast<document::JointKind>(m_kind->currentData().toInt());
    const std::vector<document::JointMotion> motions = document::motionsOf(kind);
    for (std::size_t i = 0; i < 3; ++i) {
        const bool used = i < motions.size();
        m_motionForm->setRowVisible(m_values[i], used);
        m_motionForm->setRowVisible(m_limitRows[i], used);
        if (!used) {
            continue;
        }
        const QString name = tr(motions[i].name);
        const QString suffix = motions[i].turns ? tr("°") : tr(" mm");
        m_motionLabels[i]->setText(name);
        for (QDoubleSpinBox* box : {m_values[i], m_minimum[i], m_maximum[i]}) {
            box->setSuffix(suffix);
        }
    }
    setError(m_error);
}

void JointPanel::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_okButton->isEnabled()) {
            emit accepted();
        }
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        emit canceled();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace lcad::app
