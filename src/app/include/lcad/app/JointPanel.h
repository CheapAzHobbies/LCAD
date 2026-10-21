// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Assembly.h>

#include <QString>
#include <QWidget>

#include <array>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QPushButton;

namespace lcad::app {

/// The form for a joint: the two places it joins, picked in the 3D view, what kind of joint it
/// is, how the two line up, and how far it has moved and may move. It serves for adding a joint
/// and for editing one.
///
/// The panel does not touch the document. It reports every change so that the window can try
/// the joint and preview it, and shows the outcome it is given back.
class JointPanel : public QWidget {
    Q_OBJECT

public:
    explicit JointPanel(QWidget* parent = nullptr);

    /// The joint as the panel describes it now. Its component is left for the window to say.
    [[nodiscard]] document::Joint joint() const;
    /// Shows a joint's values, for editing it.
    void setJoint(const document::Joint& joint, const QString& movingText,
                  const QString& fixedText);

    /// Which side the next pick in the 3D view is for: 0 for the part that moves, 1 for the
    /// part it goes onto, -1 when both are picked.
    [[nodiscard]] int pickingSide() const { return m_picking; }
    /// Takes a pick for a side, and moves on to the other side if that is not picked yet.
    /// `description` says what it is: "Face of Arm:1".
    void setSide(int which, const document::JointSide& side, const QString& description);
    [[nodiscard]] bool hasSide(int which) const;
    /// The kind of joint the picks suggest. Taken unless the user has chosen a kind.
    void suggestKind(document::JointKind kind);

    /// The outcome of trying the joint: why it does not hold, or an empty string. OK is
    /// available only while it holds.
    void setError(const QString& error);
    [[nodiscard]] QString errorText() const { return m_error; }
    /// What the panel says about what to do next.
    [[nodiscard]] QString hint() const;

    /// Sets the kind as the user would. For tests and the command search.
    void setKind(document::JointKind kind);

signals:
    void changed();
    void accepted();
    void canceled();
    /// The user asked to see the joint move through its range.
    void animateRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void showMotions();
    void updateHint();

    QLabel* m_hint;
    QComboBox* m_kind;
    std::array<QLabel*, 2> m_sideText{};
    std::array<QComboBox*, 2> m_snap{};
    std::array<QPushButton*, 2> m_repick{};
    QDoubleSpinBox* m_offset;
    QDoubleSpinBox* m_angle;
    QCheckBox* m_flipped;
    QFormLayout* m_motionForm;
    std::array<QLabel*, 3> m_motionLabels{};
    std::array<QDoubleSpinBox*, 3> m_values{};
    std::array<QCheckBox*, 3> m_limited{};
    std::array<QDoubleSpinBox*, 3> m_minimum{};
    std::array<QDoubleSpinBox*, 3> m_maximum{};
    std::array<QWidget*, 3> m_limitRows{};
    QPushButton* m_animate;
    QLabel* m_errorLabel;
    QPushButton* m_okButton;
    QString m_error;

    std::array<document::JointSide, 2> m_sides{};
    std::array<bool, 2> m_picked{};
    int m_picking = 0;
    bool m_kindChosen = false;
    bool m_filling = false;
    document::ObjectId m_id;
    std::string m_name;
    bool m_suppressed = false;
};

/// The name a kind of joint goes by in the interface: "Revolute", "Pin-Slot".
[[nodiscard]] QString jointKindName(document::JointKind kind);

} // namespace lcad::app
