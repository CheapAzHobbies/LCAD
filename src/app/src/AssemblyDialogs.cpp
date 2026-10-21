// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/AssemblyDialogs.h>

#include <lcad/app/Paths.h>
#include <lcad/app/PropertiesPanel.h>

#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace lcad::app {

namespace {

constexpr int indexRole = Qt::UserRole;
constexpr int componentRole = Qt::UserRole + 1;
constexpr int fileRole = Qt::UserRole + 2;

QString mass(double grams) {
    return grams >= 1000.0 ? MassDialog::tr("%1 kg").arg(formatNumber(grams / 1000.0))
                           : MassDialog::tr("%1 g").arg(formatNumber(grams));
}

} // namespace

// ---------------------------------------------------------------------------------------------

InterferenceDialog::InterferenceDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Check Interference"));
    setObjectName(QStringLiteral("interferenceCheck"));
    resize(520, 320);
    auto* layout = new QVBoxLayout(this);
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("interferenceSummary"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("interferences"));
    m_tree->setHeaderLabels({tr("Parts"), tr("Overlap")});
    m_tree->setRootIsDecorated(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] {
        const QTreeWidgetItem* item = m_tree->currentItem();
        emit interferenceChosen(
            item != nullptr && item->isSelected() ? item->data(0, indexRole).toInt() : -1);
    });
    layout->addWidget(m_tree);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
}

void InterferenceDialog::setResults(const std::vector<Interference>& results, int pairs,
                                    const QStringList& problems) {
    m_tree->clear();
    for (std::size_t i = 0; i < results.size(); ++i) {
        auto* item =
            new QTreeWidgetItem(m_tree, {tr("%1 and %2").arg(results[i].first, results[i].second),
                                         tr("%1 mm³").arg(formatNumber(results[i].volume))});
        item->setData(0, indexRole, static_cast<int>(i));
    }
    const QString checked =
        pairs == 1 ? tr("1 pair of bodies checked.") : tr("%1 pairs of bodies checked.").arg(pairs);
    QString text = results.empty() ? tr("No parts overlap. %1").arg(checked)
                   : results.size() == 1
                       ? tr("1 overlap found. %1 Choose it to see it in the 3D view.").arg(checked)
                       : tr("%1 overlaps found. %2 Choose one to see it in the 3D view.")
                             .arg(results.size())
                             .arg(checked);
    if (!problems.isEmpty()) {
        text += QLatin1Char('\n') + problems.join(QLatin1Char('\n'));
    }
    m_summary->setText(text);
}

QString InterferenceDialog::summary() const {
    return m_summary->text();
}

// ---------------------------------------------------------------------------------------------

MassDialog::MassDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Physical Properties"));
    setObjectName(QStringLiteral("physicalProperties"));
    resize(560, 420);
    auto* layout = new QVBoxLayout(this);
    m_scope = new QLabel(this);
    m_scope->setWordWrap(true);
    layout->addWidget(m_scope);
    m_table = new QTableWidget(this);
    m_table->setObjectName(QStringLiteral("massTable"));
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({tr("Body"), tr("Material"), tr("Volume"), tr("Mass")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_table, 1);
    m_totalsLabel = new QLabel(this);
    m_totalsLabel->setObjectName(QStringLiteral("massTotals"));
    m_totalsLabel->setWordWrap(true);
    m_totalsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_totalsLabel);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
}

void MassDialog::setSummary(const MassSummary& summary, const QString& scope) {
    m_scope->setText(scope);
    m_table->setRowCount(static_cast<int>(summary.rows.size()));
    for (std::size_t r = 0; r < summary.rows.size(); ++r) {
        const MassRow& row = summary.rows[r];
        const auto row_ = static_cast<int>(r);
        m_table->setItem(row_, 0, new QTableWidgetItem(row.name));
        m_table->setItem(
            row_, 1, new QTableWidgetItem(row.material.isEmpty() ? tr("Not set") : row.material));
        m_table->setItem(row_, 2, new QTableWidgetItem(tr("%1 mm³").arg(formatNumber(row.volume))));
        m_table->setItem(
            row_, 3, new QTableWidgetItem(row.mass.has_value() ? mass(*row.mass) : tr("Unknown")));
    }
    m_totals.clear();
    m_totals << tr("Volume: %1 mm³").arg(formatNumber(summary.volume));
    m_totals << tr("Surface area: %1 mm²").arg(formatNumber(summary.area));
    if (summary.total.has_value() && summary.total->mass > 0.0) {
        const kernel::MassProperties& total = *summary.total;
        m_totals << tr("Mass: %1").arg(mass(total.mass));
        m_totals << tr("Center of mass: %1, %2, %3 mm")
                        .arg(formatNumber(total.center.x), formatNumber(total.center.y),
                             formatNumber(total.center.z));
        const kernel::PrincipalInertia principal = kernel::principalInertia(total);
        m_totals << tr("Principal moments of inertia: %1, %2, %3 g·mm²")
                        .arg(formatNumber(principal.moments[0]), formatNumber(principal.moments[1]),
                             formatNumber(principal.moments[2]));
        m_totals << tr("Moments about X, Y, Z through the center: %1, %2, %3 g·mm²")
                        .arg(formatNumber(total.inertia[0][0]), formatNumber(total.inertia[1][1]),
                             formatNumber(total.inertia[2][2]));
    }
    if (summary.unknown > 0) {
        m_totals << (summary.unknown == 1
                         ? tr("1 body without a material is left out of the mass. Choose a "
                              "material in the Properties panel.")
                         : tr("%1 bodies without a material are left out of the mass. Choose a "
                              "material in the Properties panel.")
                               .arg(summary.unknown));
    }
    m_totalsLabel->setText(m_totals.join(QLatin1Char('\n')));
}

// ---------------------------------------------------------------------------------------------

MaterialsDialog::MaterialsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Materials"));
    setObjectName(QStringLiteral("materials"));
    resize(420, 480);
    auto* layout = new QVBoxLayout(this);
    auto* hint = new QLabel(tr("LCAD's materials are listed first. Add the document's own below "
                               "them; double-click one of those to rename it or change its "
                               "density."),
                            this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    m_table = new QTableWidget(this);
    m_table->setObjectName(QStringLiteral("materialTable"));
    m_table->setColumnCount(2);
    m_table->setHorizontalHeaderLabels({tr("Material"), tr("Density (g/cm³)")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (m_filling || m_document == nullptr) {
            return;
        }
        const std::string was = item->data(Qt::UserRole).toString().toStdString();
        const int row = item->row();
        document::Material changed;
        changed.name = m_table->item(row, 0)->text().trimmed().toStdString();
        bool number = false;
        changed.density = m_table->item(row, 1)->text().toDouble(&number);
        if (!number) {
            changed.density = 0.0;
        }
        // Later: the document's answer fills the table again.
        QMetaObject::invokeMethod(
            this,
            [this, was, changed] {
                try {
                    m_document->changeMaterial(was, changed);
                } catch (const std::invalid_argument&) {
                    QMessageBox::warning(this, tr("Could not change the material"),
                                         tr("A material needs a name the document's other "
                                            "materials do not have, and a density above "
                                            "zero."));
                }
                fill();
            },
            Qt::QueuedConnection);
    });
    layout->addWidget(m_table, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* add = buttons->addButton(tr("Add"), QDialogButtonBox::ActionRole);
    add->setObjectName(QStringLiteral("addMaterial"));
    connect(add, &QPushButton::clicked, this, [this] {
        if (m_document == nullptr) {
            return;
        }
        std::string name = "Material";
        for (int number = 2; m_document->findMaterial(name).has_value(); ++number) {
            name = "Material " + std::to_string(number);
        }
        m_document->addMaterial({name, 1.0});
        fill();
        m_table->editItem(m_table->item(m_table->rowCount() - 1, 0));
    });
    m_remove = buttons->addButton(tr("Remove"), QDialogButtonBox::ActionRole);
    m_remove->setObjectName(QStringLiteral("removeMaterial"));
    connect(m_remove, &QPushButton::clicked, this, [this] {
        const int row = m_table->currentRow();
        if (m_document == nullptr || row < 0) {
            return;
        }
        const QTableWidgetItem* item = m_table->item(row, 0);
        if (item == nullptr || !(item->flags() & Qt::ItemIsEditable)) {
            return;
        }
        m_document->removeMaterial(item->data(Qt::UserRole).toString().toStdString());
        fill();
    });
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row) {
        const QTableWidgetItem* item = row >= 0 ? m_table->item(row, 0) : nullptr;
        m_remove->setEnabled(item != nullptr && (item->flags() & Qt::ItemIsEditable));
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
    m_remove->setEnabled(false);
}

void MaterialsDialog::setDocument(document::Document* document) {
    if (m_document != nullptr) {
        m_document->removeListener(m_listener);
        m_listener = 0;
    }
    m_document = document;
    if (m_document != nullptr) {
        m_listener = m_document->addListener([this](const document::Change& change) {
            if (change.kind == document::Change::Kind::MaterialsChanged && !change.body.isValid()) {
                QMetaObject::invokeMethod(this, &MaterialsDialog::fill, Qt::QueuedConnection);
            }
        });
    }
    fill();
}

void MaterialsDialog::fill() {
    m_filling = true;
    m_table->setRowCount(0);
    if (m_document != nullptr) {
        const auto addRow = [this](const document::Material& material, bool own) {
            const int row = m_table->rowCount();
            m_table->insertRow(row);
            auto* name = new QTableWidgetItem(QString::fromStdString(material.name));
            auto* density = new QTableWidgetItem(QString::number(material.density, 'g', 6));
            for (QTableWidgetItem* item : {name, density}) {
                item->setData(Qt::UserRole, QString::fromStdString(material.name));
                Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
                if (own) {
                    flags |= Qt::ItemIsEditable;
                }
                item->setFlags(flags);
            }
            if (!own) {
                name->setToolTip(tr("One of LCAD's materials."));
            }
            m_table->setItem(row, 0, name);
            m_table->setItem(row, 1, density);
        };
        for (const document::Material& material : document::materialLibrary()) {
            addRow(material, false);
        }
        for (const document::Material& material : m_document->materials()) {
            addRow(material, true);
        }
    }
    m_filling = false;
}

// ---------------------------------------------------------------------------------------------

QString linkStateName(document::LinkStatus::State state) {
    switch (state) {
    case document::LinkStatus::State::UpToDate:
        return LinksDialog::tr("Up to date");
    case document::LinkStatus::State::Changed:
        return LinksDialog::tr("Changed: update to take it");
    case document::LinkStatus::State::Missing:
        return LinksDialog::tr("Missing");
    case document::LinkStatus::State::Unreadable:
        return LinksDialog::tr("Cannot be read");
    }
    return {};
}

LinksDialog::LinksDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Linked Files"));
    setObjectName(QStringLiteral("linkedFiles"));
    resize(640, 360);
    auto* layout = new QVBoxLayout(this);
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("linksSummary"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("links"));
    m_tree->setHeaderLabels({tr("Part"), tr("State"), tr("File")});
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(m_tree, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_update = buttons->addButton(tr("Update All"), QDialogButtonBox::ActionRole);
    m_update->setObjectName(QStringLiteral("updateLinks"));
    m_relink = buttons->addButton(tr("Find File…"), QDialogButtonBox::ActionRole);
    m_relink->setObjectName(QStringLiteral("relink"));
    m_open = buttons->addButton(tr("Open"), QDialogButtonBox::ActionRole);
    m_open->setObjectName(QStringLiteral("openLinked"));
    auto* package = buttons->addButton(tr("Package Project…"), QDialogButtonBox::ActionRole);
    package->setObjectName(QStringLiteral("packageProject"));
    connect(m_update, &QPushButton::clicked, this, &LinksDialog::updateRequested);
    connect(package, &QPushButton::clicked, this, &LinksDialog::packageRequested);
    connect(m_relink, &QPushButton::clicked, this, [this] {
        const QTreeWidgetItem* item = m_tree->currentItem();
        if (item != nullptr && item->parent() == nullptr) {
            emit relinkRequested(document::ObjectId{item->data(0, componentRole).toULongLong()});
        }
    });
    connect(m_open, &QPushButton::clicked, this, [this] {
        const QTreeWidgetItem* item = m_tree->currentItem();
        if (item != nullptr) {
            emit openRequested(item->data(0, fileRole).toString());
        }
    });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] {
        const QTreeWidgetItem* item = m_tree->currentItem();
        // Only the document's own links are found again from here; those of the files it links
        // to are put right in those files.
        m_relink->setEnabled(item != nullptr && item->parent() == nullptr);
        m_open->setEnabled(item != nullptr &&
                           QFileInfo::exists(item->data(0, fileRole).toString()));
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
    m_relink->setEnabled(false);
    m_open->setEnabled(false);
}

void LinksDialog::setStatuses(const std::vector<document::LinkStatus>& statuses) {
    m_tree->clear();
    int changed = 0;
    int missing = 0;
    std::function<void(QTreeWidgetItem*, const document::LinkStatus&)> add =
        [&](QTreeWidgetItem* parent, const document::LinkStatus& status) {
            const QStringList columns{QString::fromStdString(status.name),
                                      linkStateName(status.state),
                                      QDir::toNativeSeparators(fromPath(status.file))};
            auto* item = parent == nullptr ? new QTreeWidgetItem(m_tree, columns)
                                           : new QTreeWidgetItem(parent, columns);
            item->setData(0, componentRole,
                          QVariant::fromValue<qulonglong>(status.component.value));
            item->setData(0, fileRole, fromPath(status.file));
            item->setToolTip(1, QString::fromStdString(status.problem));
            changed += status.state == document::LinkStatus::State::Changed ? 1 : 0;
            missing += status.state == document::LinkStatus::State::Missing ||
                               status.state == document::LinkStatus::State::Unreadable
                           ? 1
                           : 0;
            for (const document::LinkStatus& inner : status.links) {
                add(item, inner);
            }
            item->setExpanded(true);
        };
    for (const document::LinkStatus& status : statuses) {
        add(nullptr, status);
    }
    if (statuses.empty()) {
        m_summary->setText(tr("This document links to no other files. Insert Part adds one."));
    } else if (changed == 0 && missing == 0) {
        m_summary->setText(tr("Every linked file is found, and up to date."));
    } else {
        QStringList parts;
        if (changed > 0) {
            parts << (changed == 1 ? tr("1 changed since it was last read")
                                   : tr("%1 changed since they were last read").arg(changed));
        }
        if (missing > 0) {
            parts << (missing == 1
                          ? tr("1 missing or unreadable: the copy kept in the document is shown")
                          : tr("%1 missing or unreadable: the copies kept in the document are "
                               "shown")
                                .arg(missing));
        }
        m_summary->setText(parts.join(QStringLiteral("; ")) + QLatin1Char('.'));
    }
    m_update->setEnabled(changed > 0);
}

} // namespace lcad::app
