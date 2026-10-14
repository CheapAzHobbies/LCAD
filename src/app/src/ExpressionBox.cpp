// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/ExpressionBox.h>

#include <lcad/core/Expression.h>

#include <QLineEdit>
#include <QLocale>

#include <stdexcept>

namespace lcad::app {

ExpressionBox::ExpressionBox(Evaluate evaluate, QWidget* parent)
    : QDoubleSpinBox(parent), m_evaluate(std::move(evaluate)) {
    setToolTip(tr("A value, or an expression of parameters such as width / 2."));
}

QString ExpressionBox::withoutSuffix(const QString& text) const {
    QString bare = text;
    if (!suffix().isEmpty() && bare.endsWith(suffix())) {
        bare.chop(suffix().size());
    }
    return bare.trimmed();
}

void ExpressionBox::setExpression(const QString& expression) {
    m_expression = expression.trimmed();
    if (m_expression.isEmpty()) {
        setValue(value());
        lineEdit()->setText(textFromValue(value()));
        return;
    }
    try {
        setValue(m_evaluate(m_expression));
    } catch (const std::invalid_argument&) {
        // Shown as it is; what it comes to is up to the caller to say.
    }
    lineEdit()->setText(textFromValue(value()) + suffix());
}

void ExpressionBox::stepBy(int steps) {
    // An arrow key makes it a plain value again.
    m_expression.clear();
    QDoubleSpinBox::stepBy(steps);
}

QValidator::State ExpressionBox::validate(QString& text, int& position) const {
    const QString bare = withoutSuffix(text);
    if (bare.isEmpty()) {
        return QValidator::Intermediate;
    }
    if (core::isPlainNumber(bare.toStdString())) {
        bool ok = false;
        const double number = QLocale::c().toDouble(bare, &ok);
        if (ok) {
            return number >= minimum() && number <= maximum() ? QValidator::Acceptable
                                                              : QValidator::Intermediate;
        }
        return QDoubleSpinBox::validate(text, position);
    }
    try {
        const double result = m_evaluate(bare);
        return result >= minimum() && result <= maximum() ? QValidator::Acceptable
                                                          : QValidator::Intermediate;
    } catch (const std::invalid_argument&) {
        return QValidator::Intermediate;
    }
}

double ExpressionBox::valueFromText(const QString& text) const {
    const QString bare = withoutSuffix(text);
    if (!bare.isEmpty() && !core::isPlainNumber(bare.toStdString())) {
        try {
            const double result = m_evaluate(bare);
            m_expression = bare;
            return result;
        } catch (const std::invalid_argument&) {
            return value();
        }
    }
    m_expression.clear();
    bool ok = false;
    const double number = QLocale::c().toDouble(bare, &ok);
    if (ok) {
        return number;
    }
    // A plain number with a unit: "40 mm".
    try {
        return core::evaluate(bare.toStdString());
    } catch (const core::ExpressionError&) {
        return QDoubleSpinBox::valueFromText(text);
    }
}

QString ExpressionBox::textFromValue(double value) const {
    if (!m_expression.isEmpty()) {
        return m_expression;
    }
    return QDoubleSpinBox::textFromValue(value);
}

} // namespace lcad::app
