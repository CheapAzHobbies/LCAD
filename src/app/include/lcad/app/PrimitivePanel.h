// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Shape.h>

#include <QString>
#include <QWidget>

class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QPushButton;

namespace lcad::app {

enum class PrimitiveKind { Box, Cylinder, Sphere, Cone, Torus };

/// The name a primitive goes by in menus and as a new body: "Box", "Cylinder".
[[nodiscard]] QString primitiveName(PrimitiveKind kind);

/// The form for creating a primitive solid: its sizes and where it goes, in millimeters.
///
/// The panel does not touch the document. It reports every change so that the window can show
/// a preview, and says when the user accepts or cancels.
class PrimitivePanel : public QWidget {
    Q_OBJECT

public:
    explicit PrimitivePanel(PrimitiveKind kind, QWidget* parent = nullptr);

    [[nodiscard]] PrimitiveKind kind() const { return m_kind; }

    /// The solid the current values describe, at its position. Null when the values do not
    /// describe a solid, in which case errorText() says why.
    [[nodiscard]] kernel::Shape shape() const { return m_shape; }
    [[nodiscard]] QString errorText() const { return m_error; }

    /// The value of a field, by the name of its input box: "sizeX", "sizeY", "sizeZ",
    /// "diameter", "height", "baseDiameter", "topDiameter", "ringDiameter", "tubeDiameter",
    /// "positionX", "positionY", "positionZ". Which of them exist depends on the kind.
    [[nodiscard]] double value(const QString& field) const;
    void setValue(const QString& field, double value);

signals:
    /// A value changed; shape() and errorText() are up to date.
    void changed();
    void accepted();
    void canceled();

private:
    QDoubleSpinBox* addField(const QString& name, const QString& label, double value,
                             double minimum);
    void rebuild();

    PrimitiveKind m_kind;
    kernel::Shape m_shape;
    QString m_error;
    QLabel* m_errorLabel = nullptr;
    QPushButton* m_okButton = nullptr;
    QFormLayout* m_form = nullptr;
};

} // namespace lcad::app
