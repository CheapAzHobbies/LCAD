// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/ParametersDialog.h>

#include <lcad/app/PropertiesPanel.h>
#include <lcad/app/Theme.h>

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

#include <stdexcept>

namespace lcad::app {

namespace {

constexpr int idRole = Qt::UserRole;

document::ObjectId idOfRow(const QTableWidget* table, int row) {
    const QTableWidgetItem* name = table->item(row, ParametersDialog::Name);
    return name == nullptr ? document::ObjectId{}
                           : document::ObjectId{name->data(idRole).toULongLong()};
}

} // namespace

ParametersDialog::ParametersDialog(QWidget* parent)
    : QDialog(parent), m_table(new QTableWidget(0, 4, this)), m_message(new QLabel(this)) {
    setObjectName(QStringLiteral("parametersDialog"));
    setWindowTitle(tr("Parameters"));
    resize(620, 360);
    auto* layout = new QVBoxLayout(this);
    auto* hint = new QLabel(tr("Name values here, then type a name instead of a number in any "
                               "value box or sketch dimension. An expression can use other "
                               "parameters, units (mm, cm, m, in, ft, deg, rad), + - * / ^, "
                               "and functions such as sqrt, sin, min, round."),
                            this);
    hint->setObjectName(QStringLiteral("secondary"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_table->setObjectName(QStringLiteral("parameterTable"));
    m_table->setHorizontalHeaderLabels({tr("Name"), tr("Expression"), tr("Value"), tr("Comment")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setColumnWidth(Name, 120);
    m_table->setColumnWidth(Expression, 180);
    m_table->setColumnWidth(Value, 150);
    connect(m_table, &QTableWidget::itemChanged, this, &ParametersDialog::onItemChanged);
    layout->addWidget(m_table, 1);

    m_problems = new QLabel(this);
    m_problems->setObjectName(QStringLiteral("errorText"));
    m_problems->setWordWrap(true);
    m_problems->hide();
    layout->addWidget(m_problems);
    m_message->setObjectName(QStringLiteral("errorText"));
    m_message->setWordWrap(true);
    m_message->hide();
    layout->addWidget(m_message);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* add = buttons->addButton(tr("&Add"), QDialogButtonBox::ActionRole);
    add->setObjectName(QStringLiteral("addParameter"));
    auto* remove = buttons->addButton(tr("&Delete"), QDialogButtonBox::ActionRole);
    remove->setObjectName(QStringLiteral("deleteParameter"));
    connect(add, &QPushButton::clicked, this, &ParametersDialog::addParameter);
    connect(remove, &QPushButton::clicked, this, &ParametersDialog::removeSelected);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
}

ParametersDialog::~ParametersDialog() {
    setDocument(nullptr);
}

void ParametersDialog::setDocument(document::Document* document) {
    if (m_document != nullptr) {
        m_document->removeListener(m_listener);
        m_listener = 0;
    }
    m_document = document;
    if (m_document != nullptr) {
        m_listener = m_document->addListener([this](const document::Change& change) {
            using Kind = document::Change::Kind;
            if (change.kind != Kind::ParametersChanged && change.kind != Kind::HistoryChanged) {
                return;
            }
            // Later: the table may be the one making the change.
            if (!m_refreshQueued) {
                m_refreshQueued = true;
                QMetaObject::invokeMethod(
                    this,
                    [this] {
                        m_refreshQueued = false;
                        refresh();
                    },
                    Qt::QueuedConnection);
            }
        });
    }
    refresh();
}

QString ParametersDialog::message() const {
    return m_message->isVisible() ? m_message->text() : QString();
}

void ParametersDialog::refresh() {
    const QSignalBlocker blocker(m_table);
    m_filling = true;
    const int current = m_table->currentRow();
    m_table->setRowCount(0);
    if (m_document != nullptr) {
        const auto& parameters = m_document->parameters();
        m_table->setRowCount(static_cast<int>(parameters.size()));
        for (int row = 0; row < static_cast<int>(parameters.size()); ++row) {
            const document::Parameter& parameter = parameters[static_cast<std::size_t>(row)];
            auto* name = new QTableWidgetItem(QString::fromStdString(parameter.name));
            name->setData(idRole, QVariant::fromValue<qulonglong>(parameter.id.value));
            m_table->setItem(row, Name, name);
            m_table->setItem(row, Expression,
                             new QTableWidgetItem(QString::fromStdString(parameter.expression)));
            auto* value = new QTableWidgetItem;
            value->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            if (const auto result = m_document->parameterValue(parameter.id)) {
                value->setText(formatNumber(*result));
            } else {
                value->setText(tr("cannot be worked out"));
                value->setForeground(Theme::instance().tokens().error);
                value->setToolTip(QString::fromStdString(m_document->parameterError(parameter.id)));
            }
            m_table->setItem(row, Value, value);
            m_table->setItem(row, Comment,
                             new QTableWidgetItem(QString::fromStdString(parameter.comment)));
        }
    }
    if (current >= 0 && current < m_table->rowCount()) {
        m_table->setCurrentCell(current, Name);
    }
    // Why the values that cannot be worked out cannot be, below the table.
    QStringList problems;
    if (m_document != nullptr) {
        for (const document::Parameter& parameter : m_document->parameters()) {
            if (const std::string why = m_document->parameterError(parameter.id); !why.empty()) {
                problems << QStringLiteral("%1: %2").arg(QString::fromStdString(parameter.name),
                                                         QString::fromStdString(why));
            }
        }
    }
    m_problems->setText(problems.join(QLatin1Char('\n')));
    m_problems->setVisible(!problems.isEmpty());
    m_filling = false;
}

void ParametersDialog::addParameter() {
    if (m_document == nullptr) {
        return;
    }
    m_document->addParameter(m_document->uniqueParameterName("param"), "0");
    refresh();
    const int row = m_table->rowCount() - 1;
    m_table->setCurrentCell(row, Name);
    m_table->editItem(m_table->item(row, Name));
}

void ParametersDialog::removeSelected() {
    if (m_document == nullptr) {
        return;
    }
    std::vector<document::ObjectId> ids;
    for (const QModelIndex& index : m_table->selectionModel()->selectedRows()) {
        ids.push_back(idOfRow(m_table, index.row()));
    }
    if (ids.empty() && m_table->currentRow() >= 0) {
        ids.push_back(idOfRow(m_table, m_table->currentRow()));
    }
    if (ids.empty()) {
        return;
    }
    document::Transaction transaction(*m_document,
                                      ids.size() == 1 ? "Delete Parameter" : "Delete Parameters");
    for (const document::ObjectId id : ids) {
        m_document->removeParameter(id);
    }
    transaction.commit();
    refresh();
}

void ParametersDialog::onItemChanged(QTableWidgetItem* item) {
    if (m_filling || m_document == nullptr || item->column() == Value) {
        return;
    }
    const document::ObjectId id = idOfRow(m_table, item->row());
    const document::Parameter* parameter = m_document->findParameter(id);
    if (parameter == nullptr) {
        return;
    }
    document::Parameter changed = *parameter;
    const std::string text = item->text().trimmed().toStdString();
    switch (item->column()) {
    case Name:
        changed.name = text;
        break;
    case Expression:
        changed.expression = text;
        break;
    case Comment:
        changed.comment = item->text().toStdString();
        break;
    default:
        return;
    }
    try {
        m_document->changeParameter(id, changed);
        m_message->hide();
    } catch (const std::invalid_argument& refused) {
        // Shown, and the table put back as the document has it.
        QString why = QString::fromUtf8(refused.what());
        why = why.mid(why.indexOf(QLatin1String(": ")) + 2);
        m_message->setText(why.left(1).toUpper() + why.mid(1));
        m_message->show();
    }
    QMetaObject::invokeMethod(this, &ParametersDialog::refresh, Qt::QueuedConnection);
}

} // namespace lcad::app
