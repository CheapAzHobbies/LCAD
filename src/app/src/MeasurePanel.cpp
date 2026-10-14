// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/MeasurePanel.h>

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace lcad::app {

MeasurePanel::MeasurePanel(QWidget* parent)
    : QWidget(parent), m_heading(new QLabel(this)), m_hint(new QLabel(this)),
      m_form(new QFormLayout) {
    auto* layout = new QVBoxLayout(this);
    m_heading->setObjectName(QStringLiteral("measureHeading"));
    QFont bold = m_heading->font();
    bold.setBold(true);
    m_heading->setFont(bold);
    m_heading->setWordWrap(true);
    layout->addWidget(m_heading);
    layout->addLayout(m_form);
    m_hint->setObjectName(QStringLiteral("secondary"));
    m_hint->setWordWrap(true);
    m_hint->setText(tr("Click something in the 3D view, then something else, to measure between "
                       "them. A third click starts again."));
    layout->addWidget(m_hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &MeasurePanel::closed);
    layout->addWidget(buttons);
    layout->addStretch(1);
    show(tr("Nothing picked yet"), {});
}

void MeasurePanel::show(const QString& heading, const QList<QPair<QString, QString>>& rows) {
    m_heading->setText(heading);
    while (m_form->rowCount() > 0) {
        m_form->removeRow(0);
    }
    m_rows.clear();
    for (const auto& [name, value] : rows) {
        auto* label = new QLabel(name, this);
        label->setObjectName(QStringLiteral("secondary"));
        auto* text = new QLabel(value, this);
        // Selectable, so that a measurement can be copied.
        text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        m_form->addRow(label, text);
        m_rows << name + QStringLiteral(": ") + value;
    }
}

QString MeasurePanel::heading() const {
    return m_heading->text();
}

void MeasurePanel::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        emit closed();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace lcad::app
