// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/PropertiesPanel.h>

#include <lcad/app/SketchPanel.h>

#include <lcad/kernel/Measure.h>
#include <lcad/kernel/SubShapes.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <stdexcept>

namespace lcad::app {

using viewport::SelectedItem;

QString formatNumber(double value) {
    QString text = QString::number(value, 'f', 3);
    while (text.endsWith(QLatin1Char('0'))) {
        text.chop(1);
    }
    if (text.endsWith(QLatin1Char('.'))) {
        text.chop(1);
    }
    return text == QStringLiteral("-0") ? QStringLiteral("0") : text;
}

PropertiesPanel::PropertiesPanel(QWidget* parent)
    : QWidget(parent), m_heading(new QLabel(this)), m_form(new QFormLayout) {
    auto* layout = new QVBoxLayout(this);
    QFont bold = m_heading->font();
    bold.setBold(true);
    m_heading->setFont(bold);
    m_heading->setObjectName(QStringLiteral("propertiesHeading"));
    m_heading->setWordWrap(true);
    layout->addWidget(m_heading);
    // Each property's name on a line of its own above its value: the panel is narrow, and a
    // value beside a long name would be cut off.
    m_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    m_form->setVerticalSpacing(2);
    layout->addLayout(m_form);
    layout->addStretch(1);
    refresh(nullptr, {}, {});
}

QString PropertiesPanel::heading() const {
    return m_heading->text();
}

QStringList PropertiesPanel::rows() const {
    return m_rows;
}

void PropertiesPanel::clearRows() {
    while (m_form->rowCount() > 0) {
        m_form->removeRow(0);
    }
    m_rows.clear();
    m_name = nullptr;
    m_visible = nullptr;
    m_material = nullptr;
}

void PropertiesPanel::addRow(const QString& label, const QString& value) {
    auto* text = new QLabel(value, this);
    // Read-only, but selectable, so that a measurement can be copied.
    text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    text->setWordWrap(true);
    auto* name = new QLabel(label, this);
    name->setObjectName(QStringLiteral("secondary"));
    m_form->addRow(name, text);
    m_rows << label + QStringLiteral(": ") + value;
}

void PropertiesPanel::applyEdit(const std::function<void()>& edit) {
    m_applying = true;
    try {
        edit();
    } catch (...) {
        m_applying = false;
        m_refreshWaiting = false;
        throw;
    }
    m_applying = false;
    if (m_refreshWaiting) {
        m_refreshWaiting = false;
        // Queued: the control that made the edit is still in the middle of its own signal.
        QMetaObject::invokeMethod(
            this, [this] { refresh(m_document, m_waitingBodies, m_waitingParts); },
            Qt::QueuedConnection);
    }
}

void PropertiesPanel::refresh(document::Document* document,
                              const std::vector<document::ObjectId>& bodies,
                              const std::vector<SelectedItem>& parts) {
    if (m_applying && document == m_document) {
        m_refreshWaiting = true;
        m_waitingBodies = bodies;
        m_waitingParts = parts;
        return;
    }
    std::vector<SelectedItem> subShapes;
    for (const SelectedItem& item : parts) {
        if (item.kind != SelectedItem::Kind::Body) {
            subShapes.push_back(item);
        }
    }
    const std::size_t total = bodies.size() + subShapes.size();
    const document::Body* body =
        document != nullptr && total == 1
            ? document->findBody(bodies.size() == 1 ? bodies.front() : subShapes.front().body)
            : nullptr;

    // A body's name box keeps the keyboard while its own edits arrive back from the document.
    // The same body, with the same shape: a feature it depends on may have changed it since.
    const bool sameBody = body != nullptr && bodies.size() == 1 && m_name != nullptr &&
                          m_document == document && m_body == body->id &&
                          m_shape.impl() == body->shape.impl();
    if (sameBody) {
        const QSignalBlocker nameBlocker(m_name);
        const QSignalBlocker visibleBlocker(m_visible);
        if (!m_name->hasFocus()) {
            m_name->setText(QString::fromStdString(body->name));
        }
        m_visible->setChecked(body->visible);
        m_heading->setText(QString::fromStdString(body->name));
        // What it is made of may have changed: its mass with it.
        if (m_material != nullptr &&
            m_material->currentData().toString().toStdString() != body->material) {
            m_document = nullptr;
            refresh(document, bodies, parts);
        }
        return;
    }

    clearRows();
    m_document = document;
    m_body = {};
    m_shape = {};
    if (total == 0 || document == nullptr) {
        m_heading->setText(tr("Nothing selected"));
        if (document != nullptr) {
            addRow(tr("Bodies"), QString::number(document->bodies().size()));
        }
        return;
    }
    if (total > 1) {
        m_heading->setText(tr("%1 items selected").arg(total));
        return;
    }
    if (bodies.empty() && subShapes.front().kind == SelectedItem::Kind::Sketch) {
        const document::SketchItem* sketch = document->findSketch(subShapes.front().body);
        if (sketch == nullptr) {
            m_heading->setText(tr("Nothing selected"));
            return;
        }
        m_heading->setText(QString::fromStdString(sketch->name));
        const sketch::Sketch& geometry = sketch->geometry;
        int curves = 0;
        for (const sketch::Entity& entity : geometry.entities()) {
            curves += entity.kind != sketch::EntityKind::Point ? 1 : 0;
        }
        addRow(tr("State"), describeSketchState(geometry, geometry.state()));
        addRow(tr("Curves"), QString::number(curves));
        addRow(tr("Constraints"), QString::number(geometry.constraints().size()));
        addRow(tr("Closed outlines"), QString::number(geometry.profiles().size()));
        addRow(tr("Shown"), sketch->visible ? tr("Yes") : tr("No"));
        return;
    }
    if (body == nullptr) {
        m_heading->setText(tr("Nothing selected"));
        return;
    }

    const QString owner = QString::fromStdString(body->name);
    if (bodies.size() == 1) {
        m_body = body->id;
        m_heading->setText(owner);
        m_name = new QLineEdit(owner, this);
        m_name->setObjectName(QStringLiteral("bodyName"));
        auto* nameLabel = new QLabel(tr("Name"), this);
        nameLabel->setObjectName(QStringLiteral("secondary"));
        m_form->addRow(nameLabel, m_name);
        connect(m_name, &QLineEdit::editingFinished, this, [this] {
            const document::Body* current = m_document->findBody(m_body);
            const std::string name = m_name->text().trimmed().toStdString();
            if (current == nullptr || name == current->name) {
                return;
            }
            try {
                applyEdit([this, &name] { m_document->renameBody(m_body, name); });
            } catch (const std::invalid_argument&) {
                // An empty name: put back the one the body has.
                m_name->setText(QString::fromStdString(current->name));
            }
        });
        m_visible = new QCheckBox(tr("Shown in the 3D view"), this);
        m_visible->setObjectName(QStringLiteral("bodyVisible"));
        m_visible->setChecked(body->visible);
        m_form->addRow(m_visible);
        connect(m_visible, &QCheckBox::toggled, this, [this](bool on) {
            applyEdit([this, on] { m_document->setBodyVisible(m_body, on); });
        });

        // What it is made of, for its mass.
        m_material = new QComboBox(this);
        m_material->setObjectName(QStringLiteral("bodyMaterial"));
        m_material->addItem(tr("Not set"), QString());
        for (const document::Material& material : document::materialLibrary()) {
            m_material->addItem(QString::fromStdString(material.name),
                                QString::fromStdString(material.name));
        }
        for (const document::Material& material : document->materials()) {
            if (m_material->findData(QString::fromStdString(material.name)) < 0) {
                m_material->addItem(QString::fromStdString(material.name),
                                    QString::fromStdString(material.name));
            }
        }
        const int chosen = m_material->findData(QString::fromStdString(body->material));
        if (chosen < 0 && !body->material.empty()) {
            // A material the document does not know: kept, and said so.
            m_material->addItem(tr("%1 (unknown)").arg(QString::fromStdString(body->material)),
                                QString::fromStdString(body->material));
            m_material->setCurrentIndex(m_material->count() - 1);
        } else {
            m_material->setCurrentIndex(std::max(chosen, 0));
        }
        m_material->setToolTip(tr("What the body is made of. Its mass follows from it."));
        // The panel keeps its width whatever the longest material is called.
        m_material->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_material->setMinimumContentsLength(8);
        auto* materialLabel = new QLabel(tr("Material"), this);
        materialLabel->setObjectName(QStringLiteral("secondary"));
        m_form->addRow(materialLabel, m_material);
        connect(m_material, &QComboBox::activated, this, [this] {
            const std::string material = m_material->currentData().toString().toStdString();
            try {
                applyEdit([this, &material] { m_document->setBodyMaterial(m_body, material); });
            } catch (const std::invalid_argument&) {
            }
        });

        m_shape = body->shape;
        if (const document::Feature* maker = document->findFeature(body->madeBy)) {
            addRow(tr("Made by"), QString::fromStdString(maker->name));
        }
        if (const document::Component* component = document->findComponent(body->component)) {
            addRow(component->link.empty() ? tr("Part of") : tr("Linked from"),
                   component->link.empty() ? QString::fromStdString(component->name)
                                           : QString::fromStdString(component->link));
        }
        if (body->shape.isNull()) {
            addRow(tr("Exists"), tr("Not at this point in the timeline: the feature that makes "
                                    "it is suppressed, fails, or comes after the history "
                                    "marker."));
            return;
        }
        addRow(tr("Volume"), tr("%1 mm³").arg(formatNumber(kernel::volume(body->shape))));
        if (const auto material = document->findMaterial(body->material)) {
            if (const auto mass = kernel::massProperties(body->shape, material->density)) {
                addRow(tr("Mass"), mass->mass >= 1000.0
                                       ? tr("%1 kg").arg(formatNumber(mass->mass / 1000.0))
                                       : tr("%1 g").arg(formatNumber(mass->mass)));
            }
        }
        addRow(tr("Surface area"),
               tr("%1 mm²").arg(formatNumber(kernel::surfaceArea(body->shape))));
        if (const auto box = kernel::boundingBox(body->shape)) {
            addRow(tr("Size"), tr("%1 × %2 × %3 mm")
                                   .arg(formatNumber(box->max.x - box->min.x),
                                        formatNumber(box->max.y - box->min.y),
                                        formatNumber(box->max.z - box->min.z)));
        }
        if (const auto center = kernel::centerOfMass(body->shape)) {
            addRow(tr("Center of mass"), tr("%1, %2, %3 mm")
                                             .arg(formatNumber(center->x), formatNumber(center->y),
                                                  formatNumber(center->z)));
        }
        const kernel::TopologyCount count = kernel::countTopology(body->shape);
        addRow(tr("Faces, edges, vertices"),
               tr("%1, %2, %3").arg(count.faces).arg(count.edges).arg(count.vertices));
        return;
    }

    const SelectedItem& item = subShapes.front();
    try {
        switch (item.kind) {
        case SelectedItem::Kind::Face:
            m_heading->setText(tr("Face of %1").arg(owner));
            addRow(tr("Area"), tr("%1 mm²").arg(formatNumber(kernel::surfaceArea(kernel::subShape(
                                   body->shape, kernel::SubShapeKind::Face, item.index)))));
            break;
        case SelectedItem::Kind::Edge:
            m_heading->setText(tr("Edge of %1").arg(owner));
            addRow(tr("Length"), tr("%1 mm").arg(formatNumber(kernel::length(kernel::subShape(
                                     body->shape, kernel::SubShapeKind::Edge, item.index)))));
            break;
        case SelectedItem::Kind::Vertex: {
            m_heading->setText(tr("Vertex of %1").arg(owner));
            const auto point = kernel::boundingBox(
                kernel::subShape(body->shape, kernel::SubShapeKind::Vertex, item.index));
            if (point.has_value()) {
                addRow(tr("X"), tr("%1 mm").arg(formatNumber(point->min.x)));
                addRow(tr("Y"), tr("%1 mm").arg(formatNumber(point->min.y)));
                addRow(tr("Z"), tr("%1 mm").arg(formatNumber(point->min.z)));
            }
            break;
        }
        case SelectedItem::Kind::Body:
        case SelectedItem::Kind::Sketch:
            break;
        }
    } catch (const std::out_of_range&) {
        // The body changed under the selection; there is nothing to say about it.
        m_heading->setText(tr("Nothing selected"));
    }
}

void PropertiesPanel::showOccurrence(document::Document* document, document::ObjectId occurrence) {
    clearRows();
    m_document = document;
    m_body = {};
    m_shape = {};
    const document::Occurrence* found =
        document != nullptr ? document->findOccurrence(occurrence) : nullptr;
    if (found == nullptr) {
        m_heading->setText(tr("Nothing selected"));
        return;
    }
    const document::Component* component = document->findComponent(found->component);
    m_heading->setText(QString::fromStdString(document->occurrenceName(occurrence)));
    addRow(tr("Component"), QString::fromStdString(component->name));
    if (!component->link.empty()) {
        addRow(tr("Linked file"), QString::fromStdString(component->link));
    }
    addRow(tr("Appears"), tr("%n time(s)", nullptr,
                             static_cast<int>(document->occurrencesOf(found->component).size())));
    const kernel::Placement where = document->placementOf(occurrence);
    addRow(tr("Origin"), tr("%1, %2, %3 mm")
                             .arg(formatNumber(where.origin.x), formatNumber(where.origin.y),
                                  formatNumber(where.origin.z)));
    QStringList holding;
    for (const document::Joint& joint : document->joints()) {
        for (const document::JointSide* side : {&joint.moving, &joint.fixed}) {
            if (!side->path.empty() && side->path.front() == occurrence) {
                holding << QString::fromStdString(joint.name);
                break;
            }
        }
    }
    addRow(tr("Held by"), holding.isEmpty() ? tr("No joint: it stays where it is put")
                                            : holding.join(QStringLiteral(", ")));
    addRow(tr("Grounded"), found->grounded ? tr("Yes") : tr("No"));
    if (found->suppressed) {
        addRow(tr("Suppressed"), tr("Yes: left out of the assembly"));
    }
    int bodies = 0;
    for (const document::Body& body : document->bodies()) {
        bodies += body.component == found->component ? 1 : 0;
    }
    addRow(tr("Bodies"), QString::number(bodies));
}

void PropertiesPanel::showJoint(document::Document* document, document::ObjectId joint) {
    clearRows();
    m_document = document;
    m_body = {};
    m_shape = {};
    const document::Joint* found = document != nullptr ? document->findJoint(joint) : nullptr;
    if (found == nullptr) {
        m_heading->setText(tr("Nothing selected"));
        return;
    }
    m_heading->setText(QString::fromStdString(found->name));
    addRow(tr("Type"), QString::fromLatin1(document::toString(found->kind)));
    const auto sideName = [document](const document::JointSide& side) {
        return side.path.empty()
                   ? tr("The assembly itself")
                   : QString::fromStdString(document->occurrenceName(side.path.front()));
    };
    addRow(tr("Moves"), sideName(found->moving));
    addRow(tr("Onto"), sideName(found->fixed));
    const std::vector<document::JointMotion> motions = document::motionsOf(found->kind);
    for (std::size_t i = 0; i < motions.size(); ++i) {
        QString value = motions[i].turns ? tr("%1°").arg(formatNumber(found->values[i]))
                                         : tr("%1 mm").arg(formatNumber(found->values[i]));
        const document::JointLimits& limits = found->limits[i];
        if (limits.minimum.has_value() || limits.maximum.has_value()) {
            value += tr(" (from %1 to %2)")
                         .arg(limits.minimum.has_value() ? formatNumber(*limits.minimum)
                                                         : QStringLiteral("…"),
                              limits.maximum.has_value() ? formatNumber(*limits.maximum)
                                                         : QStringLiteral("…"));
        }
        addRow(tr(motions[i].name), value);
    }
    const std::string error = document->jointError(joint);
    addRow(tr("Holds"), found->suppressed ? tr("No: it is suppressed")
                        : error.empty()   ? tr("Yes")
                                          : tr("No. %1").arg(QString::fromStdString(error)));
}

} // namespace lcad::app
