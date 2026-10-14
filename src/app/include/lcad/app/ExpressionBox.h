// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDoubleSpinBox>
#include <QString>

#include <functional>

namespace lcad::app {

/// A box for a value that takes an expression of parameters too: "width / 2 + 3". While it
/// holds an expression it shows it, and value() is what the expression comes to.
class ExpressionBox : public QDoubleSpinBox {
    Q_OBJECT

public:
    /// What an expression comes to. Throws std::invalid_argument, with a sentence that says
    /// why, if it cannot be worked out.
    using Evaluate = std::function<double(const QString& expression)>;

    explicit ExpressionBox(Evaluate evaluate, QWidget* parent = nullptr);

    /// The expression, or an empty string while the box holds a plain value.
    [[nodiscard]] QString expression() const { return m_expression; }
    /// Makes the box hold an expression, or with an empty one, the plain value it has.
    void setExpression(const QString& expression);

    void stepBy(int steps) override;

protected:
    [[nodiscard]] QValidator::State validate(QString& text, int& position) const override;
    [[nodiscard]] double valueFromText(const QString& text) const override;
    [[nodiscard]] QString textFromValue(double value) const override;

private:
    [[nodiscard]] QString withoutSuffix(const QString& text) const;

    Evaluate m_evaluate;
    // Set as the text is read, which Qt does in a const function.
    mutable QString m_expression;
};

} // namespace lcad::app
