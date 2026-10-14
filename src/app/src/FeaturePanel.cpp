// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/FeaturePanel.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lcad::app {

namespace {

using document::FeatureKind;
using document::Operation;

// Far beyond any part LCAD is meant for, and small enough that the input boxes stay narrow.
constexpr double largestInput = 1.0e6;

enum Extent { OneSide = 0, Symmetric = 1, TwoSides = 2 };

} // namespace

QString featureKindName(FeatureKind kind) {
    switch (kind) {
    case FeatureKind::Pull:
        return FeaturePanel::tr("Pull");
    case FeatureKind::Revolve:
        return FeaturePanel::tr("Revolve");
    case FeatureKind::Fillet:
        return FeaturePanel::tr("Fillet");
    case FeatureKind::Chamfer:
        return FeaturePanel::tr("Chamfer");
    case FeatureKind::Shell:
        return FeaturePanel::tr("Shell");
    }
    return {};
}

FeaturePanel::FeaturePanel(FeatureKind kind, const document::Document* document, QWidget* parent)
    : QWidget(parent), m_document(document) {
    m_feature.kind = kind;
    // The boxes report their first values as they are filled in; that is not a change yet.
    m_filling = true;
    auto* layout = new QVBoxLayout(this);

    // What is picked, and how to pick it.
    m_picked = new QLabel(this);
    m_picked->setObjectName(QStringLiteral("featureInput"));
    m_picked->setWordWrap(true);
    layout->addWidget(m_picked);

    m_form = new QFormLayout;
    layout->addLayout(m_form);
    const bool fromSketch = kind == FeatureKind::Pull || kind == FeatureKind::Revolve;
    if (fromSketch) {
        m_operation = new QComboBox(this);
        m_operation->setObjectName(QStringLiteral("operation"));
        m_operation->addItem(tr("New Body"), static_cast<int>(Operation::NewBody));
        m_operation->addItem(tr("Join"), static_cast<int>(Operation::Join));
        m_operation->addItem(tr("Cut"), static_cast<int>(Operation::Cut));
        m_operation->addItem(tr("Intersect"), static_cast<int>(Operation::Intersect));
        m_operation->setToolTip(tr("What becomes of the solid: a body of its own, added to a "
                                   "body, taken out of a body, or only what it shares with one."));
        connect(m_operation, &QComboBox::activated, this, [this] {
            m_operationChosen = true;
            readFields();
        });
        m_form->addRow(tr("Operation"), m_operation);

        m_body = new QComboBox(this);
        m_body->setObjectName(QStringLiteral("body"));
        m_body->setToolTip(tr("The body to join, cut, or intersect with."));
        connect(m_body, &QComboBox::activated, this, [this] { readFields(); });
        m_form->addRow(tr("Body"), m_body);
        fillBodies();
        // Without a body to work on, a new body is what it can be.
        m_operation->setCurrentIndex(m_body->count() == 0 ? 0 : 1);
    }
    switch (kind) {
    case FeatureKind::Pull: {
        m_extent = new QComboBox(this);
        m_extent->setObjectName(QStringLiteral("extent"));
        m_extent->addItems({tr("One Side"), tr("Symmetric"), tr("Two Sides")});
        m_extent->setToolTip(tr("Pull to one side of the sketch, by the same amount to both "
                                "sides, or by a different amount to each."));
        connect(m_extent, &QComboBox::activated, this, [this] { readFields(); });
        m_form->addRow(tr("Direction"), m_extent);
        m_distance = addField(QStringLiteral("distance"), tr("Distance"), -largestInput,
                              largestInput, tr(" mm"));
        m_distance->setValue(10.0);
        m_distance->setToolTip(tr("How far to pull. A negative distance pulls the other way."));
        m_distanceBack = addField(QStringLiteral("distanceBack"), tr("Other side"), 0.0,
                                  largestInput, tr(" mm"));
        m_distanceBack->setValue(10.0);
        m_flip = new QPushButton(tr("Flip Direction"), this);
        m_flip->setObjectName(QStringLiteral("flip"));
        m_flip->setToolTip(tr("Pull the other way."));
        connect(m_flip, &QPushButton::clicked, this,
                [this] { m_distance->setValue(-m_distance->value()); });
        m_form->addRow(QString(), m_flip);
        break;
    }
    case FeatureKind::Revolve:
        m_axis = new QComboBox(this);
        m_axis->setObjectName(QStringLiteral("axis"));
        m_axis->setToolTip(tr("The line of the sketch to turn about. Draw it as construction "
                              "geometry if it is not part of the outline."));
        connect(m_axis, &QComboBox::activated, this, [this] { readFields(); });
        m_form->addRow(tr("Axis"), m_axis);
        m_angle = addField(QStringLiteral("angle"), tr("Angle"), -360.0, 360.0, tr("°"));
        m_angle->setValue(360.0);
        break;
    case FeatureKind::Fillet:
        m_size = addField(QStringLiteral("size"), tr("Radius"), 0.0, largestInput, tr(" mm"));
        m_size->setValue(2.0);
        break;
    case FeatureKind::Chamfer:
        m_size = addField(QStringLiteral("size"), tr("Distance"), 0.0, largestInput, tr(" mm"));
        m_size->setValue(1.0);
        break;
    case FeatureKind::Shell:
        m_size =
            addField(QStringLiteral("size"), tr("Wall thickness"), 0.0, largestInput, tr(" mm"));
        m_size->setValue(2.0);
        break;
    }

    m_errorLabel = new QLabel(this);
    // "errorText" is what the theme's style sheet colors as an error.
    m_errorLabel->setObjectName(QStringLiteral("errorText"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->hide();
    layout->addWidget(m_errorLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_okButton = buttons->button(QDialogButtonBox::Ok);
    m_okButton->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (m_okButton->isEnabled()) {
            emit accepted();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &FeaturePanel::canceled);
    layout->addWidget(buttons);
    layout->addStretch(1);

    m_filling = false;
    readFields();
}

QDoubleSpinBox* FeaturePanel::addField(const QString& name, const QString& label, double minimum,
                                       double maximum, const QString& suffix) {
    auto* box = new QDoubleSpinBox(this);
    box->setObjectName(name);
    box->setRange(minimum, maximum);
    box->setDecimals(3);
    box->setSuffix(suffix);
    box->setKeyboardTracking(false);
    connect(box, &QDoubleSpinBox::valueChanged, this, [this] { readFields(); });
    m_form->addRow(label, box);
    return box;
}

void FeaturePanel::fillBodies() {
    if (m_body == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_body);
    const QVariant was = m_body->currentData();
    m_body->clear();
    for (const document::Body& body : m_document->bodies()) {
        // Only a body that exists at this point in the timeline can be worked on, and not the
        // one the feature itself makes.
        if (!body.shape.isNull() &&
            !(m_feature.operation == Operation::NewBody && body.id == m_feature.body)) {
            m_body->addItem(QString::fromStdString(body.name),
                            QVariant::fromValue<qulonglong>(body.id.value));
        }
    }
    const int index = m_body->findData(was);
    m_body->setCurrentIndex(index >= 0 ? index : 0);
}

void FeaturePanel::fillAxes() {
    if (m_axis == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_axis);
    m_axis->clear();
    const document::SketchItem* sketch = m_document->findSketch(m_feature.sketch);
    if (sketch == nullptr) {
        return;
    }
    // The lines of the sketch, construction lines first: those are drawn to be axes.
    std::vector<const sketch::Entity*> lines;
    for (const sketch::Entity& entity : sketch->geometry.entities()) {
        if (entity.kind == sketch::EntityKind::Line) {
            lines.push_back(&entity);
        }
    }
    std::stable_sort(lines.begin(), lines.end(),
                     [](const sketch::Entity* a, const sketch::Entity* b) {
                         return a->construction && !b->construction;
                     });
    int number = 0;
    for (const sketch::Entity* line : lines) {
        const sketch::Point2 a = sketch->geometry.position(line->start);
        const sketch::Point2 b = sketch->geometry.position(line->end);
        QString direction;
        if (std::abs(a.y - b.y) < 1e-9) {
            direction = tr("horizontal");
        } else if (std::abs(a.x - b.x) < 1e-9) {
            direction = tr("vertical");
        } else {
            direction = tr("slanted");
        }
        const double length = std::hypot(b.x - a.x, b.y - a.y);
        QString text = tr("Line %1 · %2 · %3 mm")
                           .arg(++number)
                           .arg(direction)
                           .arg(QLocale().toString(length, 'g', 6));
        if (line->construction) {
            text += tr(" · construction");
        }
        m_axis->addItem(text, QVariant::fromValue<uint>(line->id));
    }
    const int index = m_axis->findData(QVariant::fromValue<uint>(m_feature.axis));
    m_axis->setCurrentIndex(index >= 0 ? index : 0);
}

void FeaturePanel::setFeature(const document::Feature& feature) {
    m_filling = true;
    m_feature = feature;
    // An existing feature's operation is what the user chose.
    m_operationChosen = true;
    if (m_operation != nullptr) {
        m_operation->setCurrentIndex(m_operation->findData(static_cast<int>(feature.operation)));
        fillBodies();
        const int index = m_body->findData(QVariant::fromValue<qulonglong>(feature.body.value));
        if (index >= 0) {
            m_body->setCurrentIndex(index);
        }
    }
    if (m_extent != nullptr) {
        m_extent->setCurrentIndex(feature.symmetric             ? Symmetric
                                  : feature.distanceBack != 0.0 ? TwoSides
                                                                : OneSide);
        m_distance->setValue(feature.distance);
        if (feature.distanceBack != 0.0) {
            m_distanceBack->setValue(feature.distanceBack);
        }
    }
    if (m_angle != nullptr) {
        m_angle->setValue(feature.angle);
    }
    if (m_size != nullptr) {
        m_size->setValue(feature.size);
    }
    fillAxes();
    m_filling = false;
    readFields();
}

void FeaturePanel::setProfiles(document::ObjectId sketch,
                               const std::vector<std::vector<sketch::EntityId>>& outlines) {
    const bool newSketch = sketch != m_feature.sketch;
    m_feature.sketch = sketch;
    m_feature.regions = outlines;
    for (std::vector<sketch::EntityId>& outline : m_feature.regions) {
        std::sort(outline.begin(), outline.end());
    }
    if (newSketch) {
        fillAxes();
    }
    readFields();
}

void FeaturePanel::setEdges(document::ObjectId body, const std::vector<std::string>& edges) {
    m_feature.body = body;
    m_feature.edges = edges;
    readFields();
}

void FeaturePanel::setFaces(document::ObjectId body, const std::vector<std::string>& faces) {
    m_feature.body = body;
    m_feature.faces = faces;
    readFields();
}

bool FeaturePanel::hasInput() const {
    switch (m_feature.kind) {
    case FeatureKind::Pull:
        return m_feature.sketch.isValid() && !m_feature.regions.empty();
    case FeatureKind::Revolve:
        return m_feature.sketch.isValid() && !m_feature.regions.empty() && m_feature.axis != 0;
    case FeatureKind::Fillet:
    case FeatureKind::Chamfer:
        return m_feature.body.isValid() && !m_feature.edges.empty();
    case FeatureKind::Shell:
        return m_feature.body.isValid();
    }
    return false;
}

void FeaturePanel::setError(const QString& error) {
    m_error = error;
    m_errorLabel->setText(error);
    m_errorLabel->setVisible(!error.isEmpty());
    updateOk();
}

void FeaturePanel::updateOk() {
    m_okButton->setEnabled(hasInput() && m_error.isEmpty());
}

double FeaturePanel::value(const QString& field) const {
    const auto* box = findChild<QDoubleSpinBox*>(field);
    return box != nullptr ? box->value() : 0.0;
}

void FeaturePanel::setValue(const QString& field, double value) {
    if (auto* box = findChild<QDoubleSpinBox*>(field)) {
        box->setValue(value);
    }
}

void FeaturePanel::suggestOperation(Operation operation) {
    if (m_operation == nullptr || m_operationChosen || m_feature.operation == operation ||
        (operation != Operation::NewBody && m_body->count() == 0)) {
        return;
    }
    m_operation->setCurrentIndex(m_operation->findData(static_cast<int>(operation)));
    readFields();
}

void FeaturePanel::setChoice(const QString& list, int index) {
    auto* box = findChild<QComboBox*>(list);
    if (box == nullptr || index < 0 || index >= box->count()) {
        return;
    }
    box->setCurrentIndex(index);
    if (box == m_operation) {
        m_operationChosen = true;
    }
    readFields();
}

void FeaturePanel::readFields() {
    if (m_filling) {
        return;
    }
    if (m_operation != nullptr) {
        const auto operation = static_cast<Operation>(m_operation->currentData().toInt());
        if (operation == Operation::NewBody) {
            if (m_feature.operation != Operation::NewBody) {
                // The body is the one the feature will make.
                m_feature.body = {};
            }
        } else {
            m_feature.body = document::ObjectId{m_body->currentData().toULongLong()};
        }
        m_feature.operation = operation;
        const bool needsBody = operation != Operation::NewBody;
        m_body->setVisible(needsBody);
        m_form->labelForField(m_body)->setVisible(needsBody);
    }
    if (m_extent != nullptr) {
        const int extent = m_extent->currentIndex();
        m_feature.symmetric = extent == Symmetric;
        m_feature.distance = m_distance->value();
        m_feature.distanceBack = extent == TwoSides ? m_distanceBack->value() : 0.0;
        m_distanceBack->setVisible(extent == TwoSides);
        m_form->labelForField(m_distanceBack)->setVisible(extent == TwoSides);
        qobject_cast<QLabel*>(m_form->labelForField(m_distance))
            ->setText(extent == Symmetric ? tr("Total distance") : tr("Distance"));
    }
    if (m_axis != nullptr) {
        m_feature.axis = m_axis->count() > 0 ? m_axis->currentData().toUInt() : 0;
    }
    if (m_angle != nullptr) {
        m_feature.angle = m_angle->value();
    }
    if (m_size != nullptr) {
        m_feature.size = m_size->value();
    }
    updatePicked();
    updateOk();
    emit changed();
}

void FeaturePanel::updatePicked() {
    QString text;
    switch (m_feature.kind) {
    case FeatureKind::Pull:
    case FeatureKind::Revolve: {
        const document::SketchItem* sketch = m_document->findSketch(m_feature.sketch);
        const std::size_t count = m_feature.regions.size();
        if (sketch == nullptr || count == 0) {
            text = tr("Click the closed areas of a sketch in the 3D view to pick them.");
        } else {
            text =
                (count == 1 ? tr("1 area of %1 picked.") : tr("%1 areas of %2 picked.").arg(count))
                    .arg(QString::fromStdString(sketch->name)) +
                QLatin1Char(' ') + tr("Click an area to pick it or let it go.");
        }
        if (m_feature.kind == FeatureKind::Revolve && m_axis != nullptr && m_axis->count() == 0 &&
            sketch != nullptr) {
            text += QLatin1Char(' ') + tr("The sketch has no line to turn about.");
        }
        break;
    }
    case FeatureKind::Fillet:
    case FeatureKind::Chamfer: {
        const document::Body* body = m_document->findBody(m_feature.body);
        if (body == nullptr || m_feature.edges.empty()) {
            text = tr("Click the edges to %1 in the 3D view. Shift-click picks more.")
                       .arg(m_feature.kind == FeatureKind::Fillet ? tr("round") : tr("bevel"));
        } else {
            const std::size_t count = m_feature.edges.size();
            text =
                (count == 1 ? tr("1 edge of %1 picked.") : tr("%1 edges of %2 picked.").arg(count))
                    .arg(QString::fromStdString(body->name)) +
                QLatin1Char(' ') + tr("Shift-click picks more, or lets one go.");
        }
        break;
    }
    case FeatureKind::Shell: {
        const document::Body* body = m_document->findBody(m_feature.body);
        if (body == nullptr) {
            text = tr("Click the faces to leave open in the 3D view, or a body to hollow out "
                      "closed.");
        } else if (m_feature.faces.empty()) {
            text = tr("%1, closed all round. Click faces to leave open.")
                       .arg(QString::fromStdString(body->name));
        } else {
            const std::size_t count = m_feature.faces.size();
            text = (count == 1 ? tr("1 face of %1 left open.")
                               : tr("%1 faces of %2 left open.").arg(count))
                       .arg(QString::fromStdString(body->name));
        }
        break;
    }
    }
    m_picked->setText(text);
}

void FeaturePanel::keyPressEvent(QKeyEvent* event) {
    // Enter in one of the input boxes arrives here once the box has taken in what was typed.
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_okButton->isEnabled()) {
            emit accepted();
        }
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        emit canceled();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace lcad::app
