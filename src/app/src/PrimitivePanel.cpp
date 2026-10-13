// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/PrimitivePanel.h>

#include <lcad/kernel/Error.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Transform.h>

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <stdexcept>

namespace lcad::app {

namespace {

// Far beyond any part LCAD is meant for, and small enough that the input boxes stay narrow.
constexpr double largestInput = 1.0e6;

} // namespace

QString primitiveName(PrimitiveKind kind) {
    switch (kind) {
    case PrimitiveKind::Box:
        return PrimitivePanel::tr("Box");
    case PrimitiveKind::Cylinder:
        return PrimitivePanel::tr("Cylinder");
    case PrimitiveKind::Sphere:
        return PrimitivePanel::tr("Sphere");
    case PrimitiveKind::Cone:
        return PrimitivePanel::tr("Cone");
    case PrimitiveKind::Torus:
        return PrimitivePanel::tr("Torus");
    }
    return {};
}

PrimitivePanel::PrimitivePanel(PrimitiveKind kind, QWidget* parent)
    : QWidget(parent), m_kind(kind) {
    auto* layout = new QVBoxLayout(this);
    m_form = new QFormLayout;
    layout->addLayout(m_form);

    switch (kind) {
    case PrimitiveKind::Box:
        addField(QStringLiteral("sizeX"), tr("Length (X)"), 50.0, 0.0);
        addField(QStringLiteral("sizeY"), tr("Width (Y)"), 50.0, 0.0);
        addField(QStringLiteral("sizeZ"), tr("Height (Z)"), 50.0, 0.0);
        break;
    case PrimitiveKind::Cylinder:
        addField(QStringLiteral("diameter"), tr("Diameter"), 50.0, 0.0);
        addField(QStringLiteral("height"), tr("Height"), 50.0, 0.0);
        break;
    case PrimitiveKind::Sphere:
        addField(QStringLiteral("diameter"), tr("Diameter"), 50.0, 0.0);
        break;
    case PrimitiveKind::Cone:
        addField(QStringLiteral("baseDiameter"), tr("Base diameter"), 50.0, 0.0);
        addField(QStringLiteral("topDiameter"), tr("Top diameter"), 0.0, 0.0);
        addField(QStringLiteral("height"), tr("Height"), 50.0, 0.0);
        break;
    case PrimitiveKind::Torus:
        addField(QStringLiteral("ringDiameter"), tr("Ring diameter"), 100.0, 0.0);
        addField(QStringLiteral("tubeDiameter"), tr("Tube diameter"), 20.0, 0.0);
        break;
    }
    const QString positionTip = kind == PrimitiveKind::Box ? tr("Where the corner of the box goes.")
                                : kind == PrimitiveKind::Sphere || kind == PrimitiveKind::Torus
                                    ? tr("Where the center goes.")
                                    : tr("Where the center of the base goes.");
    for (const auto& [name, label] : {std::pair{QStringLiteral("positionX"), tr("Position X")},
                                      std::pair{QStringLiteral("positionY"), tr("Position Y")},
                                      std::pair{QStringLiteral("positionZ"), tr("Position Z")}}) {
        addField(name, label, 0.0, -largestInput)->setToolTip(positionTip);
    }

    m_errorLabel = new QLabel(this);
    m_errorLabel->setObjectName(QStringLiteral("error"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(QStringLiteral("color: #d9534f;"));
    layout->addWidget(m_errorLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_okButton = buttons->button(QDialogButtonBox::Ok);
    m_okButton->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!m_shape.isNull()) {
            emit accepted();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &PrimitivePanel::canceled);
    layout->addWidget(buttons);
    layout->addStretch(1);

    rebuild();
}

QDoubleSpinBox* PrimitivePanel::addField(const QString& name, const QString& label, double value,
                                         double minimum) {
    auto* box = new QDoubleSpinBox(this);
    box->setObjectName(name);
    box->setRange(minimum, largestInput);
    box->setDecimals(3);
    box->setSuffix(tr(" mm"));
    box->setValue(value);
    connect(box, &QDoubleSpinBox::valueChanged, this, &PrimitivePanel::rebuild);
    m_form->addRow(label, box);
    return box;
}

void PrimitivePanel::keyPressEvent(QKeyEvent* event) {
    // Enter in one of the input boxes arrives here once the box has taken in what was typed.
    // Outside a dialog, Qt does not press the default button by itself.
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        m_okButton->click();
        return;
    }
    // The window's Cancel Command shortcut normally gets Esc first. This is for when it does
    // not, as when the panel floats in a window of its own.
    if (event->key() == Qt::Key_Escape) {
        emit canceled();
        return;
    }
    QWidget::keyPressEvent(event);
}

double PrimitivePanel::value(const QString& field) const {
    const auto* box = findChild<QDoubleSpinBox*>(field);
    return box != nullptr ? box->value() : 0.0;
}

void PrimitivePanel::setValue(const QString& field, double value) {
    if (auto* box = findChild<QDoubleSpinBox*>(field)) {
        box->setValue(value);
    }
}

void PrimitivePanel::rebuild() {
    m_shape = kernel::Shape();
    m_error.clear();
    const auto v = [this](const char* field) { return value(QString::fromLatin1(field)); };
    try {
        kernel::Shape solid;
        switch (m_kind) {
        case PrimitiveKind::Box:
            solid = kernel::makeBox(v("sizeX"), v("sizeY"), v("sizeZ"));
            break;
        case PrimitiveKind::Cylinder:
            solid = kernel::makeCylinder(v("diameter") / 2.0, v("height"));
            break;
        case PrimitiveKind::Sphere:
            solid = kernel::makeSphere(v("diameter") / 2.0);
            break;
        case PrimitiveKind::Cone:
            solid = kernel::makeCone(v("baseDiameter") / 2.0, v("topDiameter") / 2.0, v("height"));
            break;
        case PrimitiveKind::Torus:
            solid = kernel::makeTorus(v("ringDiameter") / 2.0, v("tubeDiameter") / 2.0);
            break;
        }
        m_shape = kernel::translated(solid, v("positionX"), v("positionY"), v("positionZ"));
    } catch (const std::invalid_argument&) {
        // The kernel's message names its own arguments; say it in the panel's terms.
        switch (m_kind) {
        case PrimitiveKind::Cone:
            m_error = v("height") <= 0.0
                          ? tr("The height must be greater than zero.")
                          : tr("The base and top diameters must differ, and one of them must be "
                               "greater than zero.");
            break;
        case PrimitiveKind::Torus:
            m_error = v("tubeDiameter") > 0.0 && v("ringDiameter") > 0.0
                          ? tr("The tube diameter must be smaller than the ring diameter.")
                          : tr("Every size must be greater than zero.");
            break;
        default:
            m_error = tr("Every size must be greater than zero.");
            break;
        }
    } catch (const kernel::Error& error) {
        m_error = tr("This solid cannot be built: %1").arg(QString::fromStdString(error.reason()));
    }
    m_errorLabel->setText(m_error);
    m_errorLabel->setVisible(!m_error.isEmpty());
    m_okButton->setEnabled(!m_shape.isNull());
    emit changed();
}

} // namespace lcad::app
