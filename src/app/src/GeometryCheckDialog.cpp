// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/GeometryCheckDialog.h>

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace lcad::app {

namespace {

// Where in m_results an entry of the tree points: the body, and the problem or -1.
constexpr int bodyRole = Qt::UserRole;
constexpr int problemRole = Qt::UserRole + 1;

} // namespace

GeometryCheckDialog::GeometryCheckDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Check Geometry"));
    setObjectName(QStringLiteral("geometryCheck"));
    resize(520, 320);
    auto* layout = new QVBoxLayout(this);
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("geometryProblems"));
    m_tree->setHeaderHidden(true);
    m_tree->setWordWrap(true);
    m_tree->header()->setStretchLastSection(true);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &GeometryCheckDialog::selectionChanged);
    layout->addWidget(m_tree);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_repair = buttons->addButton(tr("Repair Body"), QDialogButtonBox::ActionRole);
    m_repair->setObjectName(QStringLiteral("repairBody"));
    m_repair->setToolTip(tr("Add a Repair to the timeline for the body chosen above."));
    m_repair->setEnabled(false);
    connect(m_repair, &QPushButton::clicked, this, [this] {
        const QTreeWidgetItem* item = m_tree->currentItem();
        if (item != nullptr) {
            const auto index = static_cast<std::size_t>(item->data(0, bodyRole).toInt());
            if (index < m_results.size()) {
                emit repairRequested(m_results[index].body);
            }
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
}

void GeometryCheckDialog::setResults(const std::vector<BodyCheck>& results) {
    m_results = results;
    m_tree->clear();
    for (std::size_t b = 0; b < m_results.size(); ++b) {
        const BodyCheck& check = m_results[b];
        auto* item = new QTreeWidgetItem(m_tree);
        item->setData(0, bodyRole, static_cast<int>(b));
        item->setData(0, problemRole, -1);
        if (check.problems.empty()) {
            item->setText(0, tr("%1: a sound solid.").arg(check.name));
            continue;
        }
        item->setText(0, check.problems.size() == 1
                             ? tr("%1: 1 problem").arg(check.name)
                             : tr("%1: %2 problems").arg(check.name).arg(check.problems.size()));
        for (std::size_t p = 0; p < check.problems.size(); ++p) {
            auto* problem =
                new QTreeWidgetItem(item, {QString::fromStdString(check.problems[p].description)});
            problem->setData(0, bodyRole, static_cast<int>(b));
            problem->setData(0, problemRole, static_cast<int>(p));
        }
        item->setExpanded(true);
    }
    m_repair->setEnabled(false);
}

void GeometryCheckDialog::selectionChanged() {
    const QTreeWidgetItem* item = m_tree->currentItem();
    if (item == nullptr) {
        m_repair->setEnabled(false);
        return;
    }
    const auto body = static_cast<std::size_t>(item->data(0, bodyRole).toInt());
    const int problem = item->data(0, problemRole).toInt();
    if (body >= m_results.size()) {
        return;
    }
    const BodyCheck& check = m_results[body];
    m_repair->setEnabled(!check.problems.empty());
    if (problem >= 0 && static_cast<std::size_t>(problem) < check.problems.size()) {
        const kernel::GeometryProblem& chosen = check.problems[static_cast<std::size_t>(problem)];
        emit problemChosen(check.body, chosen.faces, chosen.edges);
    } else {
        emit problemChosen(check.body, {}, {});
    }
}

} // namespace lcad::app
