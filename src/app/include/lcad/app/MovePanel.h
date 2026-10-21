// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Transform.h>

#include <QString>
#include <QWidget>

#include <array>

class QDoubleSpinBox;
class QLabel;
class QPushButton;

namespace lcad::app {

/// The form for moving a component by hand: how far along X, Y, and Z, and how far to turn
/// about each, from where it is now. The window previews the move and makes it.
class MovePanel : public QWidget {
    Q_OBJECT

public:
    /// `name` is what is being moved: "Bracket:1".
    explicit MovePanel(const QString& name, QWidget* parent = nullptr);

    /// The placement it is in now, moved as the form says: turned about its own origin first,
    /// about X, then Y, then Z of what it is in, and then moved.
    [[nodiscard]] kernel::Placement moved(const kernel::Placement& from) const;
    /// Sets one of the six fields: 0 to 2 for X, Y, Z in millimeters, 3 to 5 for the turns
    /// about them in degrees.
    void setValue(int field, double value);
    /// Why it cannot be moved, or an empty string. OK is available only without one.
    void setError(const QString& error);

signals:
    void changed();
    void accepted();
    void canceled();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    std::array<QDoubleSpinBox*, 6> m_fields{};
    QLabel* m_errorLabel;
    QPushButton* m_okButton;
};

} // namespace lcad::app
