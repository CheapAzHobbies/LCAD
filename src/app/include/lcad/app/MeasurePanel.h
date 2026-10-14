// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QStringList>
#include <QWidget>

class QFormLayout;
class QLabel;

namespace lcad::app {

/// What the Measure command has found: a heading, and rows of a name and a value. The window
/// works the measurements out; the panel shows them, and says when the user is done.
class MeasurePanel : public QWidget {
    Q_OBJECT

public:
    explicit MeasurePanel(QWidget* parent = nullptr);

    /// Shows a heading and rows. Each row is a name and a value.
    void show(const QString& heading, const QList<QPair<QString, QString>>& rows);
    [[nodiscard]] QString heading() const;
    /// The rows as "Name: value".
    [[nodiscard]] QStringList rows() const { return m_rows; }

signals:
    void closed();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    QLabel* m_heading;
    QLabel* m_hint;
    QFormLayout* m_form;
    QStringList m_rows;
};

} // namespace lcad::app
