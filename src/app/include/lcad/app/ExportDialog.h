// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Exchange.h>

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QWidget;

namespace lcad::app {

/// The kinds of file bodies can be exported to.
enum class ExportFormat {
    Step,    ///< exact solids, for other CAD programs
    Stl,     ///< triangles, for 3D printing
    ThreeMf, ///< triangles with names and units, for 3D printing
    Obj,     ///< triangles, for rendering and other tools
};

/// What to export, and how.
struct ExportOptions {
    ExportFormat format = ExportFormat::Step;
    /// Only the bodies that are selected, rather than all that are shown.
    bool selectedOnly = false;
    kernel::MeshSettings mesh;
    kernel::StlEncoding stlEncoding = kernel::StlEncoding::Binary;
};

/// The file name extension of a format, without the dot: "step", "stl", "3mf", "obj".
[[nodiscard]] QString extensionOf(ExportFormat format);
/// What a file dialog lists for a format: "STL files (*.stl)".
[[nodiscard]] QString filterOf(ExportFormat format);
/// Whether the format holds triangles rather than exact solids.
[[nodiscard]] bool isMesh(ExportFormat format);

/// Asks what to export: the format, which bodies, and for a mesh how finely to cut it. Only
/// the settings that go with the chosen format are shown. The file name is asked for after.
class ExportDialog : public QDialog {
    Q_OBJECT

public:
    /// `shown` and `selected` are how many bodies each choice would export.
    ExportDialog(int shown, int selected, QWidget* parent = nullptr);

    [[nodiscard]] ExportOptions options() const;
    void setOptions(const ExportOptions& options);

private:
    void presetChosen();
    void showWhatApplies();

    QComboBox* m_format;
    QComboBox* m_bodies;
    QWidget* m_meshSettings;
    QFormLayout* m_meshForm;
    QComboBox* m_resolution;
    QDoubleSpinBox* m_deviation;
    QDoubleSpinBox* m_angle;
    QCheckBox* m_text;
    QLabel* m_note;
    bool m_settingPreset = false;
};

} // namespace lcad::app
