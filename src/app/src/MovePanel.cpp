// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/MovePanel.h>

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace lcad::app {

MovePanel::MovePanel(const QString& name, QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* heading = new QLabel(tr("Move %1").arg(name), this);
    heading->setObjectName(QStringLiteral("moveHeading"));
    QFont bold = heading->font();
    bold.setBold(true);
    heading->setFont(bold);
    heading->setWordWrap(true);
    layout->addWidget(heading);
    auto* hint = new QLabel(tr("From where it is now. It turns about its own origin."), this);
    hint->setObjectName(QStringLiteral("secondary"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    const std::array<QString, 6> labels{tr("Move X"),       tr("Move Y"),       tr("Move Z"),
                                        tr("Turn about X"), tr("Turn about Y"), tr("Turn about Z")};
    const std::array<const char*, 6> names{"moveX", "moveY", "moveZ", "turnX", "turnY", "turnZ"};
    for (std::size_t i = 0; i < m_fields.size(); ++i) {
        auto* box = new QDoubleSpinBox(this);
        box->setObjectName(QLatin1String(names[i]));
        const bool turn = i >= 3;
        box->setRange(turn ? -360.0 : -100000.0, turn ? 360.0 : 100000.0);
        box->setDecimals(3);
        box->setSuffix(turn ? tr("°") : tr(" mm"));
        box->setKeyboardTracking(false);
        connect(box, &QDoubleSpinBox::valueChanged, this, &MovePanel::changed);
        form->addRow(labels[i], box);
        m_fields[i] = box;
    }
    m_errorLabel = new QLabel(this);
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
    connect(buttons, &QDialogButtonBox::rejected, this, &MovePanel::canceled);
    layout->addWidget(buttons);
    layout->addStretch(1);
}

kernel::Placement MovePanel::moved(const kernel::Placement& from) const {
    constexpr double degrees = 3.14159265358979323846 / 180.0;
    const kernel::Point origin = from.origin;
    // Turned about its own origin, along the axes of what it is in.
    kernel::Placement turned = from;
    turned.origin = {};
    const std::array<kernel::Vector, 3> axes{kernel::Vector{1.0, 0.0, 0.0},
                                             kernel::Vector{0.0, 1.0, 0.0},
                                             kernel::Vector{0.0, 0.0, 1.0}};
    for (std::size_t i = 0; i < 3; ++i) {
        turned = kernel::rotation(kernel::Axis{{0.0, 0.0, 0.0}, axes[i]},
                                  m_fields[i + 3]->value() * degrees) *
                 turned;
    }
    turned.origin = {origin.x + m_fields[0]->value(), origin.y + m_fields[1]->value(),
                     origin.z + m_fields[2]->value()};
    return kernel::normalized(turned);
}

void MovePanel::setValue(int field, double value) {
    if (field >= 0 && field < static_cast<int>(m_fields.size())) {
        m_fields[static_cast<std::size_t>(field)]->setValue(value);
    }
}

void MovePanel::setError(const QString& error) {
    m_errorLabel->setText(error);
    m_errorLabel->setVisible(!error.isEmpty());
    m_okButton->setEnabled(error.isEmpty());
}

void MovePanel::keyPressEvent(QKeyEvent* event) {
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
