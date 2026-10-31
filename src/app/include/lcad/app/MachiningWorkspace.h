// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/cam/Simulation.h>
#include <lcad/cam/Toolpath.h>
#include <lcad/document/Document.h>

#include <QObject>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;

namespace lcad::viewport {
class ViewportWidget;
}

namespace lcad::app {

/// The Manufacture workspace: a setup, its tools and operations in the panel on the right, and the
/// toolpaths in the 3D view. It keeps the toolpaths in step with the document. G-code is written
/// only for a program that has come through the checks, the simulation, and being read back.
class MachiningWorkspace : public QWidget {
    Q_OBJECT

public:
    MachiningWorkspace(viewport::ViewportWidget* viewport, QWidget* parent);
    ~MachiningWorkspace() override;

    void setDocument(document::Document* document);
    /// Whether the workspace is showing, so that it works out toolpaths.
    void setShown(bool shown);

    /// The setup shown, or nullptr.
    [[nodiscard]] const document::Setup* setup() const;
    /// Adds a setup, with a first set of tools if the document has none, and shows it.
    void newSetup();
    /// Adds an operation of a kind to the setup, on these faces, and selects it.
    void addOperation(document::OperationKind kind,
                      const std::vector<document::OperationTarget>& targets);
    /// The operation selected, or nullptr.
    [[nodiscard]] const document::MachiningOperation* operation() const;
    void selectOperation(document::ObjectId operation);
    /// Removes the operation selected. Returns false if none is.
    bool removeOperation();
    /// Gives the operation selected these faces to work on.
    void setTargets(const std::vector<document::OperationTarget>& targets);

    /// What the setup comes to now.
    [[nodiscard]] const cam::Program& program() const { return m_program; }

    /// Runs the whole program on the stock, shows what it leaves, and says how it went.
    void simulate();
    /// Goes to the end of a move of the simulation.
    void showMove(std::size_t move);
    /// Leaves the simulation, and shows the bodies again.
    void endSimulation();
    [[nodiscard]] bool isSimulating() const { return m_simulation != nullptr; }
    [[nodiscard]] const cam::SimulationReport& report() const { return m_report; }

    /// The G-code of the setup for a file, or the reason it cannot be had, in `error`. Only for
    /// a program with no errors, that the simulation finds nothing wrong with, and whose G-code
    /// reads back as the same moves.
    [[nodiscard]] std::optional<std::string> gcode(QString* error);
    /// Writes gcode() to a file.
    bool exportTo(const QString& path, QString* error = nullptr);

    [[nodiscard]] QString problemsText() const;
    [[nodiscard]] QString summaryText() const;
    [[nodiscard]] QListWidget* operationList() const { return m_operations; }
    [[nodiscard]] QComboBox* dialectBox() const { return m_dialect; }
    [[nodiscard]] QDoubleSpinBox* feedField() const { return m_feed; }
    [[nodiscard]] QDoubleSpinBox* stockSideField() const { return m_stockSide; }
    [[nodiscard]] QSlider* moveSlider() const { return m_slider; }

signals:
    /// What can be done changed.
    void stateChanged();
    void message(const QString& text);
    /// Wants the faces selected in the 3D view, for the operation selected.
    void targetsRequested();

private:
    void onDocumentChanged(const document::Change& change);
    void refresh();
    void replan();
    void fill();
    void change(const document::Setup& setup, const QString& label);
    void editOperation(const QString& label,
                       const std::function<void(document::MachiningOperation&)>& edit);
    void showSimulationAt(std::size_t move);
    void showTool();
    /// The part as the simulation holds the tool to.
    [[nodiscard]] kernel::Mesh partMesh() const;

    viewport::ViewportWidget* m_viewport;
    document::Document* m_document = nullptr;
    document::Document::ListenerId m_listener = 0;
    document::ObjectId m_current;
    document::ObjectId m_selected;
    bool m_shown = false;
    bool m_filling = false;
    cam::Program m_program;
    std::unique_ptr<cam::Simulation> m_simulation;
    /// The program the simulation runs: it has to outlast m_program being worked out anew.
    std::unique_ptr<cam::Program> m_simulated;
    kernel::Mesh m_mesh;
    cam::SimulationReport m_report;

    QComboBox* m_setups;
    QLineEdit* m_machineName;
    QComboBox* m_dialect;
    QComboBox* m_origin;
    QDoubleSpinBox* m_stockSide;
    QDoubleSpinBox* m_stockTop;
    QDoubleSpinBox* m_stockBottom;
    QDoubleSpinBox* m_travelX;
    QDoubleSpinBox* m_travelY;
    QDoubleSpinBox* m_travelZ;
    QDoubleSpinBox* m_rapid;
    QDoubleSpinBox* m_maxFeed;
    QDoubleSpinBox* m_maxSpindle;
    QListWidget* m_operations;
    QGroupBox* m_operationBox;
    QFormLayout* m_operationForm = nullptr;
    QLineEdit* m_operationName;
    QComboBox* m_tool;
    QDoubleSpinBox* m_feed;
    QDoubleSpinBox* m_plunge;
    QDoubleSpinBox* m_spindle;
    QDoubleSpinBox* m_stepover;
    QDoubleSpinBox* m_stepdown;
    QDoubleSpinBox* m_clearance;
    QDoubleSpinBox* m_retract;
    QDoubleSpinBox* m_leave;
    QComboBox* m_side;
    QDoubleSpinBox* m_peck;
    QDoubleSpinBox* m_ramp;
    QDoubleSpinBox* m_tabs;
    QDoubleSpinBox* m_tabWidth;
    QDoubleSpinBox* m_tabHeight;
    QCheckBox* m_suppressed;
    QLabel* m_targets;
    QSlider* m_slider;
    QLabel* m_summary;
    QLabel* m_problems;
};

} // namespace lcad::app
