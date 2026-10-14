// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/FeaturePanel.h>

#include <lcad/app/ExpressionBox.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lcad::app {

namespace {

using document::FeatureKind;
using document::Operation;

// Far beyond any part LCAD is meant for, and small enough that the input boxes stay narrow.
constexpr double largestInput = 1.0e6;

enum Extent { OneSide = 0, Symmetric = 1, TwoSides = 2, ToFace = 3 };

bool copies(FeatureKind kind) {
    return kind == FeatureKind::Mirror || kind == FeatureKind::RectangularPattern ||
           kind == FeatureKind::CircularPattern;
}

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
    case FeatureKind::Mirror:
        return FeaturePanel::tr("Mirror");
    case FeatureKind::RectangularPattern:
        return FeaturePanel::tr("Rectangular Pattern");
    case FeatureKind::CircularPattern:
        return FeaturePanel::tr("Circular Pattern");
    case FeatureKind::Plane:
        return FeaturePanel::tr("Plane");
    case FeatureKind::Hole:
        return FeaturePanel::tr("Hole");
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
    if (copies(kind)) {
        m_body = new QComboBox(this);
        m_body->setObjectName(QStringLiteral("body"));
        m_body->setToolTip(tr("The body whose features are copied, or that is copied."));
        connect(m_body, &QComboBox::activated, this, [this] {
            m_feature.sources.clear();
            m_pickedName.clear();
            readFields();
        });
        m_form->addRow(tr("Body"), m_body);
        fillBodies();
        m_sources = new QListWidget(this);
        m_sources->setObjectName(QStringLiteral("sources"));
        m_sources->setToolTip(tr("The features to copy. With none checked, the whole body is "
                                 "copied."));
        m_sources->setMaximumHeight(110);
        connect(m_sources, &QListWidget::itemChanged, this, [this] { readFields(); });
        m_form->addRow(tr("Copy"), m_sources);
    }
    if (kind == FeatureKind::Hole) {
        m_sketch = new QComboBox(this);
        m_sketch->setObjectName(QStringLiteral("sketch"));
        m_sketch->setToolTip(tr("The sketch whose points say where the holes go. Click a sketch "
                                "in the 3D view to pick it."));
        connect(m_sketch, &QComboBox::activated, this, [this] { readFields(); });
        m_form->addRow(tr("Points of"), m_sketch);
        m_body = new QComboBox(this);
        m_body->setObjectName(QStringLiteral("body"));
        m_body->setToolTip(tr("The body to drill into."));
        connect(m_body, &QComboBox::activated, this, [this] { readFields(); });
        m_form->addRow(tr("Body"), m_body);
        fillBodies();
        fillSketches();
    }
    if (copies(kind) || kind == FeatureKind::Plane) {
        m_reference = new QComboBox(this);
        m_reference->setObjectName(QStringLiteral("reference"));
        connect(m_reference, &QComboBox::activated, this, [this] { readFields(); });
    }
    switch (kind) {
    case FeatureKind::Pull: {
        m_extent = new QComboBox(this);
        m_extent->setObjectName(QStringLiteral("extent"));
        m_extent->addItems({tr("One Side"), tr("Symmetric"), tr("Two Sides"), tr("To Face")});
        m_extent->setToolTip(tr("Pull to one side of the sketch, by the same amount to both "
                                "sides, by a different amount to each, or up to a flat face."));
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
        m_extentFace = new QLabel(this);
        m_extentFace->setObjectName(QStringLiteral("extentFace"));
        m_extentFace->setWordWrap(true);
        m_form->addRow(tr("Up to"), m_extentFace);
        m_taper = addField(QStringLiteral("taper"), tr("Taper"), -88.0, 88.0, tr("°"));
        m_taper->setValue(0.0);
        m_taper->setToolTip(tr("How far the sides lean in as the pull goes out. Negative leans "
                               "them out."));
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
    case FeatureKind::Mirror:
        m_reference->setToolTip(tr("The plane to mirror in. Click a flat face in the 3D view to "
                                   "mirror in that."));
        m_form->addRow(tr("Plane"), m_reference);
        break;
    case FeatureKind::RectangularPattern:
        m_reference->setToolTip(tr("The direction of the first row. Click an edge or a face in "
                                   "the 3D view to go along that."));
        m_form->addRow(tr("Direction"), m_reference);
        m_count = addCount(QStringLiteral("count"), tr("Count"), 3);
        m_spacing = addField(QStringLiteral("spacing"), tr("Spacing"), -largestInput, largestInput,
                             tr(" mm"));
        m_spacing->setValue(20.0);
        m_reference2 = new QComboBox(this);
        m_reference2->setObjectName(QStringLiteral("reference2"));
        m_reference2->setToolTip(tr("The direction of the columns, or none for one row."));
        connect(m_reference2, &QComboBox::activated, this, [this] { readFields(); });
        m_form->addRow(tr("Second direction"), m_reference2);
        m_count2 = addCount(QStringLiteral("count2"), tr("Count"), 2);
        m_spacing2 = addField(QStringLiteral("spacing2"), tr("Spacing"), -largestInput,
                              largestInput, tr(" mm"));
        m_spacing2->setValue(20.0);
        break;
    case FeatureKind::CircularPattern:
        m_reference->setToolTip(tr("The axis to repeat around. Click a round edge or face, or a "
                                   "straight edge, in the 3D view to go around that."));
        m_form->addRow(tr("Axis"), m_reference);
        m_count = addCount(QStringLiteral("count"), tr("Count"), 4);
        m_angle = addField(QStringLiteral("angle"), tr("Angle"), -360.0, 360.0, tr("°"));
        m_angle->setValue(360.0);
        m_angle->setToolTip(tr("The angle they are spread over. At 360° they go evenly all "
                               "the way round."));
        break;
    case FeatureKind::Hole:
        m_holeKind = new QComboBox(this);
        m_holeKind->setObjectName(QStringLiteral("holeKind"));
        m_holeKind->addItem(tr("Simple"), static_cast<int>(document::HoleKind::Simple));
        m_holeKind->addItem(tr("Counterbore"), static_cast<int>(document::HoleKind::Counterbore));
        m_holeKind->addItem(tr("Countersink"), static_cast<int>(document::HoleKind::Countersink));
        connect(m_holeKind, &QComboBox::activated, this, [this] { readFields(); });
        m_form->addRow(tr("Type"), m_holeKind);
        m_size = addField(QStringLiteral("size"), tr("Diameter"), 0.0, largestInput, tr(" mm"));
        m_size->setValue(5.0);
        m_throughAll = new QCheckBox(tr("Through all"), this);
        m_throughAll->setObjectName(QStringLiteral("throughAll"));
        m_throughAll->setToolTip(tr("Right through the body, with a flat end."));
        connect(m_throughAll, &QCheckBox::toggled, this, [this] { readFields(); });
        m_form->addRow(QString(), m_throughAll);
        m_distance =
            addField(QStringLiteral("distance"), tr("Depth"), 0.0, largestInput, tr(" mm"));
        m_distance->setValue(10.0);
        m_tipAngle = addField(QStringLiteral("tipAngle"), tr("Tip angle"), 1.0, 180.0, tr("°"));
        m_tipAngle->setValue(118.0);
        m_tipAngle->setToolTip(tr("The point a drill leaves at the bottom. 180° is flat."));
        m_headDiameter = addField(QStringLiteral("headDiameter"), tr("Head diameter"), 0.0,
                                  largestInput, tr(" mm"));
        m_headDiameter->setValue(10.0);
        m_headDepth =
            addField(QStringLiteral("headDepth"), tr("Head depth"), 0.0, largestInput, tr(" mm"));
        m_headDepth->setValue(3.0);
        m_countersinkAngle = addField(QStringLiteral("countersinkAngle"), tr("Countersink angle"),
                                      1.0, 179.0, tr("°"));
        m_countersinkAngle->setValue(90.0);
        m_flip = new QPushButton(tr("Flip Direction"), this);
        m_flip->setObjectName(QStringLiteral("flip"));
        m_flip->setToolTip(tr("Drill the other way: toward the side the sketch is seen from."));
        connect(m_flip, &QPushButton::clicked, this, [this] {
            m_feature.flipped = !m_feature.flipped;
            readFields();
        });
        m_form->addRow(QString(), m_flip);
        break;
    case FeatureKind::Plane:
        m_reference->setToolTip(tr("What the plane is placed against. Click a flat face in the "
                                   "3D view to place it against that."));
        m_form->addRow(tr("From"), m_reference);
        m_distance = addField(QStringLiteral("distance"), tr("Offset"), -largestInput, largestInput,
                              tr(" mm"));
        m_distance->setValue(20.0);
        m_distance->setToolTip(tr("How far from it, along its normal. Negative goes the other "
                                  "way."));
        break;
    }
    if (copies(kind) || kind == FeatureKind::Plane) {
        fillReferences();
        fillSources();
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
    // Each value can be worked out from parameters.
    auto* box = new ExpressionBox(
        [this](const QString& expression) {
            return m_document->evaluate(expression.toStdString());
        },
        this);
    box->setObjectName(name);
    box->setRange(minimum, maximum);
    box->setDecimals(3);
    box->setSuffix(suffix);
    box->setKeyboardTracking(false);
    connect(box, &QDoubleSpinBox::valueChanged, this, [this] { readFields(); });
    m_form->addRow(label, box);
    return box;
}

QSpinBox* FeaturePanel::addCount(const QString& name, const QString& label, int value) {
    auto* box = new QSpinBox(this);
    box->setObjectName(name);
    box->setRange(1, 1000);
    box->setValue(value);
    box->setKeyboardTracking(false);
    connect(box, &QSpinBox::valueChanged, this, [this] { readFields(); });
    m_form->addRow(label, box);
    return box;
}

void FeaturePanel::fillSources() {
    if (m_sources == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_sources);
    m_sources->clear();
    const document::ObjectId body{m_body->currentData().toULongLong()};
    // The pulls and revolves that make or change the body, in the order they are applied, up
    // to the one being edited.
    for (const document::ObjectId id : m_document->timeline()) {
        if (id == m_feature.id && m_feature.id.isValid()) {
            break;
        }
        const document::Feature* feature = m_document->findFeature(id);
        if (feature == nullptr || feature->body != body ||
            (feature->kind != FeatureKind::Pull && feature->kind != FeatureKind::Revolve)) {
            continue;
        }
        auto* item = new QListWidgetItem(QString::fromStdString(feature->name), m_sources);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(feature->id.value));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        const bool checked = std::find(m_feature.sources.begin(), m_feature.sources.end(),
                                       feature->id) != m_feature.sources.end();
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
}

void FeaturePanel::fillReferences() {
    if (m_reference == nullptr) {
        return;
    }
    const QString was = m_reference->currentData().toString();
    const QSignalBlocker blocker(m_reference);
    m_reference->clear();
    const bool mirror =
        m_feature.kind == FeatureKind::Mirror || m_feature.kind == FeatureKind::Plane;
    if (mirror) {
        m_reference->addItem(tr("XY Plane"), QStringLiteral("XY"));
        m_reference->addItem(tr("XZ Plane"), QStringLiteral("XZ"));
        m_reference->addItem(tr("YZ Plane"), QStringLiteral("YZ"));
    } else {
        m_reference->addItem(tr("X Axis"), QStringLiteral("X"));
        m_reference->addItem(tr("Y Axis"), QStringLiteral("Y"));
        m_reference->addItem(tr("Z Axis"), QStringLiteral("Z"));
    }
    // What was picked in the 3D view, or what an existing feature refers to.
    std::string picked = m_pickedName;
    QString description = m_pickedDescription;
    const bool named = m_feature.reference.size() > 2;
    if (picked.empty() && named) {
        picked = m_feature.reference;
        description = mirror ? tr("Picked face") : tr("Picked edge or face");
    }
    if (!picked.empty()) {
        m_reference->addItem(description, QString::fromStdString(picked));
    }
    int index = m_reference->findData(was);
    if (!m_pickedName.empty()) {
        index = m_reference->count() - 1;
    } else if (index < 0) {
        index = m_reference->findData(QString::fromStdString(m_feature.reference));
    }
    const int fallback = m_feature.kind == FeatureKind::Mirror ? 2 : 0;
    m_reference->setCurrentIndex(index >= 0 ? index : fallback);
    if (m_reference2 != nullptr) {
        const QString second = m_reference2->count() > 0
                                   ? m_reference2->currentData().toString()
                                   : QString::fromStdString(m_feature.reference2);
        const QSignalBlocker blocker2(m_reference2);
        m_reference2->clear();
        m_reference2->addItem(tr("None"), QString());
        m_reference2->addItem(tr("X Axis"), QStringLiteral("X"));
        m_reference2->addItem(tr("Y Axis"), QStringLiteral("Y"));
        m_reference2->addItem(tr("Z Axis"), QStringLiteral("Z"));
        const int at = m_reference2->findData(second);
        m_reference2->setCurrentIndex(at >= 0 ? at : 0);
    }
}

void FeaturePanel::setPickedReference(document::ObjectId body, const std::string& name,
                                      const QString& description) {
    if (m_reference == nullptr) {
        return;
    }
    if (body.isValid() && m_body != nullptr) {
        const int index = m_body->findData(QVariant::fromValue<qulonglong>(body.value));
        if (index >= 0 && index != m_body->currentIndex()) {
            m_body->setCurrentIndex(index);
            m_feature.sources.clear();
        }
    }
    m_pickedName = name;
    m_pickedDescription = description;
    m_pickedBody = body;
    fillReferences();
    readFields();
}

void FeaturePanel::fillSketches() {
    if (m_sketch == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_sketch);
    m_sketch->clear();
    // Sketches with points to drill at.
    for (const document::SketchItem& sketch : m_document->sketches()) {
        const auto& entities = sketch.geometry.entities();
        const bool hasPoints =
            std::any_of(entities.begin(), entities.end(), [&sketch](const sketch::Entity& e) {
                return e.kind == sketch::EntityKind::Point && e.owner == 0 && !e.construction &&
                       e.id != sketch.geometry.origin();
            });
        if (hasPoints || sketch.id == m_feature.sketch) {
            m_sketch->addItem(QString::fromStdString(sketch.name),
                              QVariant::fromValue<qulonglong>(sketch.id.value));
        }
    }
    const int index = m_sketch->findData(QVariant::fromValue<qulonglong>(m_feature.sketch.value));
    m_sketch->setCurrentIndex(index >= 0 ? index : 0);
}

bool FeaturePanel::goesToFace() const {
    return m_extent != nullptr && m_extent->currentIndex() == ToFace;
}

void FeaturePanel::setExtentFace(document::ObjectId body, const std::string& face,
                                 const QString& description) {
    if (m_extent == nullptr) {
        return;
    }
    m_pickedBody = body;
    m_pickedName = face;
    m_pickedDescription = description;
    readFields();
}

void FeaturePanel::setSketch(document::ObjectId sketch) {
    if (m_sketch == nullptr) {
        return;
    }
    m_feature.sketch = sketch;
    fillSketches();
    // A sketch on a face is drilled into that face's body.
    if (const document::SketchItem* item = m_document->findSketch(sketch);
        item != nullptr && m_document->findBody(item->attachedTo) != nullptr) {
        const int index = m_body->findData(QVariant::fromValue<qulonglong>(item->attachedTo.value));
        if (index >= 0) {
            m_body->setCurrentIndex(index);
        }
    }
    readFields();
}

void FeaturePanel::showHoleFields() {
    if (m_holeKind == nullptr) {
        return;
    }
    const auto show = [this](QWidget* field, bool shown) {
        field->setVisible(shown);
        if (QWidget* label = m_form->labelForField(field)) {
            label->setVisible(shown);
        }
    };
    const auto kind = static_cast<document::HoleKind>(m_holeKind->currentData().toInt());
    const bool through = m_throughAll->isChecked();
    show(m_distance, !through);
    show(m_tipAngle, !through);
    show(m_headDiameter, kind != document::HoleKind::Simple);
    show(m_headDepth, kind == document::HoleKind::Counterbore);
    show(m_countersinkAngle, kind == document::HoleKind::Countersink);
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
            !(m_operation != nullptr && m_feature.operation == Operation::NewBody &&
              body.id == m_feature.body)) {
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
        m_extent->setCurrentIndex(!feature.extentFace.empty()   ? ToFace
                                  : feature.symmetric           ? Symmetric
                                  : feature.distanceBack != 0.0 ? TwoSides
                                                                : OneSide);
        m_distance->setValue(feature.distance);
        if (feature.distanceBack != 0.0) {
            m_distanceBack->setValue(feature.distanceBack);
        }
        m_taper->setValue(feature.taper);
        m_pickedBody = feature.extentBody;
        m_pickedName = feature.extentFace;
        m_pickedDescription = feature.extentFace.empty() ? QString() : tr("The face picked");
    }
    if (m_angle != nullptr) {
        m_angle->setValue(feature.angle);
    }
    if (m_size != nullptr) {
        m_size->setValue(feature.size);
    }
    if (feature.kind == FeatureKind::Plane) {
        m_pickedName.clear();
        m_pickedBody = feature.body;
        fillReferences();
        m_distance->setValue(feature.distance);
    }
    if (feature.kind == FeatureKind::Hole) {
        fillSketches();
        fillBodies();
        const int body = m_body->findData(QVariant::fromValue<qulonglong>(feature.body.value));
        if (body >= 0) {
            m_body->setCurrentIndex(body);
        }
        m_holeKind->setCurrentIndex(m_holeKind->findData(static_cast<int>(feature.holeKind)));
        m_throughAll->setChecked(feature.throughAll);
        m_distance->setValue(feature.distance);
        m_tipAngle->setValue(feature.tipAngle);
        m_headDiameter->setValue(feature.headDiameter);
        m_headDepth->setValue(feature.headDepth);
        m_countersinkAngle->setValue(feature.countersinkAngle);
    }
    if (copies(feature.kind)) {
        const int index = m_body->findData(QVariant::fromValue<qulonglong>(feature.body.value));
        if (index >= 0) {
            m_body->setCurrentIndex(index);
        }
        m_pickedName.clear();
        fillReferences();
        fillSources();
        if (m_count != nullptr) {
            m_count->setValue(feature.count);
        }
        if (m_count2 != nullptr) {
            m_count2->setValue(feature.count2);
        }
        if (m_spacing != nullptr) {
            m_spacing->setValue(feature.spacing);
            m_spacing2->setValue(feature.spacing2);
        }
    }
    fillAxes();
    for (ExpressionBox* box : findChildren<ExpressionBox*>()) {
        const auto expression = feature.expressions.find(box->objectName().toStdString());
        box->setExpression(expression == feature.expressions.end()
                               ? QString()
                               : QString::fromStdString(expression->second));
    }
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
        return m_feature.sketch.isValid() && !m_feature.regions.empty() &&
               (!goesToFace() || !m_feature.extentFace.empty());
    case FeatureKind::Revolve:
        return m_feature.sketch.isValid() && !m_feature.regions.empty() && m_feature.axis != 0;
    case FeatureKind::Fillet:
    case FeatureKind::Chamfer:
        return m_feature.body.isValid() && !m_feature.edges.empty();
    case FeatureKind::Shell:
        return m_feature.body.isValid();
    case FeatureKind::Mirror:
    case FeatureKind::RectangularPattern:
    case FeatureKind::CircularPattern:
        return m_feature.body.isValid() && !m_feature.reference.empty();
    case FeatureKind::Plane:
        return !m_feature.reference.empty();
    case FeatureKind::Hole:
        return m_feature.sketch.isValid() && m_feature.body.isValid();
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
        const auto show = [this](QWidget* field, bool shown) {
            field->setVisible(shown);
            if (QWidget* label = m_form->labelForField(field)) {
                label->setVisible(shown);
            }
        };
        m_feature.symmetric = extent == Symmetric;
        m_feature.distance = m_distance->value();
        m_feature.distanceBack = extent == TwoSides ? m_distanceBack->value() : 0.0;
        // A taper leans the sides from the sketch outward, which only works one way.
        m_feature.taper = extent == OneSide ? m_taper->value() : 0.0;
        m_feature.extentBody = extent == ToFace ? m_pickedBody : document::ObjectId{};
        m_feature.extentFace = extent == ToFace ? m_pickedName : std::string();
        show(m_distance, extent != ToFace);
        show(m_flip, extent != ToFace);
        show(m_distanceBack, extent == TwoSides);
        show(m_taper, extent == OneSide);
        show(m_extentFace, extent == ToFace);
        m_extentFace->setText(m_pickedName.empty() || extent != ToFace
                                  ? tr("Click a flat face in the 3D view.")
                                  : m_pickedDescription);
        qobject_cast<QLabel*>(m_form->labelForField(m_distance))
            ->setText(extent == Symmetric ? tr("Total distance") : tr("Distance"));
    }
    if (copies(m_feature.kind)) {
        const document::ObjectId body{m_body->currentData().toULongLong()};
        if (body != m_feature.body) {
            m_feature.body = body;
            fillSources();
        }
        m_feature.sources.clear();
        for (int i = 0; i < m_sources->count(); ++i) {
            if (m_sources->item(i)->checkState() == Qt::Checked) {
                m_feature.sources.push_back(
                    document::ObjectId{m_sources->item(i)->data(Qt::UserRole).toULongLong()});
            }
        }
        m_feature.reference = m_reference->currentData().toString().toStdString();
        if (m_reference2 != nullptr) {
            m_feature.reference2 = m_reference2->currentData().toString().toStdString();
            const bool second = !m_feature.reference2.empty();
            m_count2->setVisible(second);
            m_form->labelForField(m_count2)->setVisible(second);
            m_spacing2->setVisible(second);
            m_form->labelForField(m_spacing2)->setVisible(second);
        }
        if (m_count != nullptr) {
            m_feature.count = m_count->value();
        }
        if (m_count2 != nullptr) {
            m_feature.count2 = m_count2->value();
        }
        if (m_spacing != nullptr) {
            m_feature.spacing = m_spacing->value();
            m_feature.spacing2 = m_spacing2->value();
        }
    }
    if (m_feature.kind == FeatureKind::Hole) {
        m_feature.sketch =
            document::ObjectId{m_sketch->count() > 0 ? m_sketch->currentData().toULongLong() : 0};
        m_feature.body =
            document::ObjectId{m_body->count() > 0 ? m_body->currentData().toULongLong() : 0};
        m_feature.holeKind = static_cast<document::HoleKind>(m_holeKind->currentData().toInt());
        m_feature.throughAll = m_throughAll->isChecked();
        m_feature.distance = m_distance->value();
        m_feature.tipAngle = m_tipAngle->value();
        m_feature.headDiameter = m_headDiameter->value();
        m_feature.headDepth = m_headDepth->value();
        m_feature.countersinkAngle = m_countersinkAngle->value();
        showHoleFields();
    }
    if (m_feature.kind == FeatureKind::Plane) {
        m_feature.reference = m_reference->currentData().toString().toStdString();
        // An origin plane needs no body; a face, the body it is a face of.
        m_feature.body = m_feature.reference.size() > 2 ? m_pickedBody : document::ObjectId{};
        m_feature.distance = m_distance->value();
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
    // Values worked out from parameters, of the boxes that are in use.
    for (const ExpressionBox* box : findChildren<ExpressionBox*>()) {
        const std::string field = box->objectName().toStdString();
        if (box->expression().isEmpty() || box->isHidden()) {
            m_feature.expressions.erase(field);
        } else {
            m_feature.expressions[field] = box->expression().toStdString();
        }
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
    case FeatureKind::Mirror:
    case FeatureKind::RectangularPattern:
    case FeatureKind::CircularPattern: {
        const document::Body* body = m_document->findBody(m_feature.body);
        if (body == nullptr) {
            text = tr("There is no body to copy from.");
        } else if (m_feature.sources.empty()) {
            text = tr("All of %1 is copied. Check features below to copy only them.")
                       .arg(QString::fromStdString(body->name));
        } else {
            text = m_feature.sources.size() == 1
                       ? tr("1 feature copied.")
                       : tr("%1 features copied.").arg(m_feature.sources.size());
        }
        break;
    }
    case FeatureKind::Plane:
        text = tr("Choose an origin plane below, or click a flat face in the 3D view, and how far "
                  "from it the plane goes.");
        break;
    case FeatureKind::Hole: {
        const document::SketchItem* sketch = m_document->findSketch(m_feature.sketch);
        if (sketch == nullptr) {
            text = tr("Holes go at the points of a sketch. Draw a point where each hole goes, "
                      "on the face to drill into, then pick that sketch.");
        } else {
            int count = 0;
            for (const sketch::Entity& e : sketch->geometry.entities()) {
                count += e.kind == sketch::EntityKind::Point && e.owner == 0 && !e.construction &&
                                 e.id != sketch->geometry.origin()
                             ? 1
                             : 0;
            }
            text = (count == 1 ? tr("1 hole, at the point of %1.")
                               : tr("%1 holes, at the points of %2.").arg(count))
                       .arg(QString::fromStdString(sketch->name));
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
