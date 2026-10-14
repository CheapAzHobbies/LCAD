// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>
#include <lcad/document/Feature.h>

#include <QString>
#include <QWidget>

#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace lcad::app {

/// The name a kind of feature goes by in menus and panels: "Pull", "Fillet".
[[nodiscard]] QString featureKindName(document::FeatureKind kind);

/// The form for a feature: what it works on, which is picked in the 3D view, and its values.
/// It serves for adding a feature and for editing one.
///
/// The panel does not touch the document. It reports every change so that the window can try
/// the feature and preview it, and shows the outcome it is given back.
class FeaturePanel : public QWidget {
    Q_OBJECT

public:
    /// `document` is read for the bodies a pull can join, and the lines a revolve can turn
    /// about. It must outlive the panel.
    FeaturePanel(document::FeatureKind kind, const document::Document* document,
                 QWidget* parent = nullptr);

    [[nodiscard]] document::FeatureKind kind() const { return m_feature.kind; }
    /// The feature as the panel describes it now.
    [[nodiscard]] document::Feature feature() const { return m_feature; }
    /// Shows a feature's values, for editing it.
    void setFeature(const document::Feature& feature);

    // What is picked in the 3D view

    /// Pull and revolve: the profiles, all of one sketch, each by the curves of its outline.
    void setProfiles(document::ObjectId sketch,
                     const std::vector<std::vector<sketch::EntityId>>& outlines);
    /// Fillet and chamfer: the edges, of one body, by name.
    void setEdges(document::ObjectId body, const std::vector<std::string>& edges);
    /// Shell: the faces to leave open, of one body, by name.
    void setFaces(document::ObjectId body, const std::vector<std::string>& faces);
    /// Hole: the sketch whose points say where the holes go.
    void setSketch(document::ObjectId sketch);
    /// Pull: the flat face, of a body, by name, that a pull up to a face ends in.
    void setExtentFace(document::ObjectId body, const std::string& face,
                       const QString& description);
    /// Pull: whether it goes up to a face, which is then picked in the 3D view.
    [[nodiscard]] bool goesToFace() const;
    /// Mirror and patterns: an edge or a face of a body, by name, picked to give the plane,
    /// the direction, or the axis. `description` says what it is, for the list.
    void setPickedReference(document::ObjectId body, const std::string& name,
                            const QString& description);

    /// Whether what the feature needs has been picked.
    [[nodiscard]] bool hasInput() const;
    /// The outcome of trying the feature: why it fails, or an empty string if it works. OK is
    /// available only while it works.
    void setError(const QString& error);
    [[nodiscard]] QString errorText() const { return m_error; }

    /// The value of a field, by the name of its input box: "distance", "distanceBack",
    /// "angle", "size", "spacing", "spacing2", "count", "count2".
    [[nodiscard]] double value(const QString& field) const;
    void setValue(const QString& field, double value);
    /// Whether the user chose the operation of a pull or a revolve. Until then the window may
    /// suggest one.
    [[nodiscard]] bool operationChosen() const { return m_operationChosen; }
    /// Shows `operation` as the operation, unless the user has chosen one.
    void suggestOperation(document::Operation operation);
    /// Picks an entry of a list, by the name of the list ("operation", "body", "extent",
    /// "axis", "reference", "reference2") and the entry's position.
    void setChoice(const QString& list, int index);

signals:
    /// A value or what is picked changed; feature() is up to date.
    void changed();
    void accepted();
    void canceled();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    QDoubleSpinBox* addField(const QString& name, const QString& label, double minimum,
                             double maximum, const QString& suffix);
    QSpinBox* addCount(const QString& name, const QString& label, int value);
    void fillBodies();
    void fillAxes();
    void fillSources();
    void fillReferences();
    /// Adds the construction planes or axes there are at this point in the timeline to a list.
    void addConstruction(QComboBox* box, document::FeatureKind kind) const;
    void fillSketches();
    /// Shows the boxes that go with the kind of hole.
    void showHoleFields();
    /// Reads the boxes into m_feature and shows or hides what depends on them.
    void readFields();
    void updatePicked();
    void updateOk();

    document::Feature m_feature;
    const document::Document* m_document;
    /// Whether the user chose the operation, or it is still LCAD's guess.
    bool m_operationChosen = false;
    bool m_filling = false;
    QString m_error;
    QLabel* m_picked = nullptr;
    QComboBox* m_operation = nullptr;
    QComboBox* m_body = nullptr;
    QComboBox* m_extent = nullptr;
    QComboBox* m_axis = nullptr;
    QDoubleSpinBox* m_distance = nullptr;
    QDoubleSpinBox* m_distanceBack = nullptr;
    QDoubleSpinBox* m_angle = nullptr;
    QDoubleSpinBox* m_size = nullptr;
    QPushButton* m_flip = nullptr;
    QDoubleSpinBox* m_taper = nullptr;
    QLabel* m_extentFace = nullptr;
    // Mirror and patterns
    QListWidget* m_sources = nullptr;
    QComboBox* m_reference = nullptr;
    QComboBox* m_reference2 = nullptr;
    QSpinBox* m_count = nullptr;
    QSpinBox* m_count2 = nullptr;
    QDoubleSpinBox* m_spacing = nullptr;
    QDoubleSpinBox* m_spacing2 = nullptr;
    /// The edge or face picked in the 3D view for the plane, direction, or axis, and what it is.
    std::string m_pickedName;
    QString m_pickedDescription;
    document::ObjectId m_pickedBody;
    // Hole
    QComboBox* m_sketch = nullptr;
    QComboBox* m_holeKind = nullptr;
    QCheckBox* m_throughAll = nullptr;
    QDoubleSpinBox* m_tipAngle = nullptr;
    QDoubleSpinBox* m_headDiameter = nullptr;
    QDoubleSpinBox* m_headDepth = nullptr;
    QDoubleSpinBox* m_countersinkAngle = nullptr;
    QLabel* m_errorLabel = nullptr;
    QPushButton* m_okButton = nullptr;
    QFormLayout* m_form = nullptr;
};

} // namespace lcad::app
