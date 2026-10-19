// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/app/ExportDialog.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <array>
#include <cmath>

namespace lcad::app {

namespace {

struct Preset {
    const char* name;
    double deviation;
    double angle;
};

// How finely a mesh is cut: the most a triangle may stray from the surface, and the largest
// angle between neighboring triangles.
constexpr std::array<Preset, 3> presets{{
    {QT_TRANSLATE_NOOP("lcad::app::ExportDialog", "Coarse"), 0.1, 30.0},
    {QT_TRANSLATE_NOOP("lcad::app::ExportDialog", "Medium"), 0.05, 15.0},
    {QT_TRANSLATE_NOOP("lcad::app::ExportDialog", "Fine"), 0.01, 5.0},
}};

} // namespace

QString extensionOf(ExportFormat format) {
    switch (format) {
    case ExportFormat::Step:
        return QStringLiteral("step");
    case ExportFormat::Stl:
        return QStringLiteral("stl");
    case ExportFormat::ThreeMf:
        return QStringLiteral("3mf");
    case ExportFormat::Obj:
        return QStringLiteral("obj");
    }
    return {};
}

QString filterOf(ExportFormat format) {
    switch (format) {
    case ExportFormat::Step:
        return ExportDialog::tr("STEP files (*.step *.stp)");
    case ExportFormat::Stl:
        return ExportDialog::tr("STL files (*.stl)");
    case ExportFormat::ThreeMf:
        return ExportDialog::tr("3MF files (*.3mf)");
    case ExportFormat::Obj:
        return ExportDialog::tr("OBJ files (*.obj)");
    }
    return {};
}

bool isMesh(ExportFormat format) {
    return format != ExportFormat::Step;
}

ExportDialog::ExportDialog(int shown, int selected, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Export"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    layout->addLayout(form);

    m_format = new QComboBox(this);
    m_format->setObjectName(QStringLiteral("exportFormat"));
    m_format->addItem(tr("STEP: exact solids, for other CAD programs"),
                      static_cast<int>(ExportFormat::Step));
    m_format->addItem(tr("STL: triangles, for 3D printing"), static_cast<int>(ExportFormat::Stl));
    m_format->addItem(tr("3MF: triangles with names and units, for 3D printing"),
                      static_cast<int>(ExportFormat::ThreeMf));
    m_format->addItem(tr("OBJ: triangles, for rendering"), static_cast<int>(ExportFormat::Obj));
    connect(m_format, &QComboBox::currentIndexChanged, this, &ExportDialog::showWhatApplies);
    form->addRow(tr("Format"), m_format);

    m_bodies = new QComboBox(this);
    m_bodies->setObjectName(QStringLiteral("exportBodies"));
    m_bodies->addItem(shown == 1 ? tr("The body shown") : tr("The %1 bodies shown").arg(shown),
                      false);
    if (selected > 0) {
        m_bodies->addItem(selected == 1 ? tr("The body selected")
                                        : tr("The %1 bodies selected").arg(selected),
                          true);
    }
    form->addRow(tr("Export"), m_bodies);

    m_meshSettings = new QWidget(this);
    auto* mesh = new QFormLayout(m_meshSettings);
    m_meshForm = mesh;
    mesh->setContentsMargins(0, 0, 0, 0);
    m_resolution = new QComboBox(m_meshSettings);
    m_resolution->setObjectName(QStringLiteral("meshResolution"));
    for (const Preset& preset : presets) {
        m_resolution->addItem(tr(preset.name));
    }
    m_resolution->addItem(tr("Custom"));
    m_resolution->setToolTip(tr("How closely the triangles follow curved surfaces. Finer "
                                "makes larger files."));
    connect(m_resolution, &QComboBox::activated, this, &ExportDialog::presetChosen);
    mesh->addRow(tr("Resolution"), m_resolution);
    m_deviation = new QDoubleSpinBox(m_meshSettings);
    m_deviation->setObjectName(QStringLiteral("meshDeviation"));
    m_deviation->setRange(0.001, 10.0);
    m_deviation->setDecimals(3);
    m_deviation->setSingleStep(0.01);
    m_deviation->setSuffix(tr(" mm"));
    m_deviation->setToolTip(tr("The farthest a triangle may lie from the surface."));
    mesh->addRow(tr("Deviation"), m_deviation);
    m_angle = new QDoubleSpinBox(m_meshSettings);
    m_angle->setObjectName(QStringLiteral("meshAngle"));
    m_angle->setRange(1.0, 90.0);
    m_angle->setDecimals(1);
    m_angle->setSuffix(tr("°"));
    m_angle->setToolTip(tr("The largest angle between neighboring triangles on a curve."));
    mesh->addRow(tr("Angle"), m_angle);
    const auto custom = [this] {
        if (!m_settingPreset) {
            m_resolution->setCurrentIndex(m_resolution->count() - 1);
        }
    };
    connect(m_deviation, &QDoubleSpinBox::valueChanged, this, custom);
    connect(m_angle, &QDoubleSpinBox::valueChanged, this, custom);
    m_text = new QCheckBox(tr("Write as text (readable, several times larger)"), m_meshSettings);
    m_text->setObjectName(QStringLiteral("stlText"));
    mesh->addRow(QString(), m_text);
    layout->addWidget(m_meshSettings);

    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("exportNote"));
    m_note->setWordWrap(true);
    layout->addWidget(m_note);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Export…"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    setOptions(ExportOptions{});
}

ExportOptions ExportDialog::options() const {
    ExportOptions options;
    options.format = static_cast<ExportFormat>(m_format->currentData().toInt());
    options.selectedOnly = m_bodies->currentData().toBool();
    options.mesh.deviation = m_deviation->value();
    options.mesh.angle = m_angle->value();
    options.stlEncoding =
        m_text->isChecked() ? kernel::StlEncoding::Text : kernel::StlEncoding::Binary;
    return options;
}

void ExportDialog::setOptions(const ExportOptions& options) {
    m_format->setCurrentIndex(m_format->findData(static_cast<int>(options.format)));
    const int bodies = m_bodies->findData(options.selectedOnly);
    m_bodies->setCurrentIndex(bodies >= 0 ? bodies : 0);
    m_settingPreset = true;
    m_deviation->setValue(options.mesh.deviation);
    m_angle->setValue(options.mesh.angle);
    m_settingPreset = false;
    int preset = m_resolution->count() - 1;
    for (std::size_t i = 0; i < presets.size(); ++i) {
        if (std::abs(presets[i].deviation - options.mesh.deviation) < 1e-9 &&
            std::abs(presets[i].angle - options.mesh.angle) < 1e-9) {
            preset = static_cast<int>(i);
        }
    }
    m_resolution->setCurrentIndex(preset);
    m_text->setChecked(options.stlEncoding == kernel::StlEncoding::Text);
    showWhatApplies();
}

void ExportDialog::presetChosen() {
    const auto index = static_cast<std::size_t>(m_resolution->currentIndex());
    if (index >= presets.size()) {
        return;
    }
    m_settingPreset = true;
    m_deviation->setValue(presets[index].deviation);
    m_angle->setValue(presets[index].angle);
    m_settingPreset = false;
}

void ExportDialog::showWhatApplies() {
    const auto format = static_cast<ExportFormat>(m_format->currentData().toInt());
    m_meshSettings->setVisible(isMesh(format));
    // The row as a whole, so that it leaves no gap.
    m_meshForm->setRowVisible(m_text, format == ExportFormat::Stl);
    m_note->setText(
        isMesh(format)
            ? tr("A mesh is made of flat triangles: it comes close to curved surfaces but is not "
                 "them, and other CAD programs cannot edit it as a solid. The LCAD document stays "
                 "as it is.")
            : tr("STEP holds the exact solids, without the timeline: other programs get the "
                 "shapes, not how they were made."));
    // The layouts first, so that the size fits what is shown now.
    m_meshForm->activate();
    layout()->activate();
    adjustSize();
}

} // namespace lcad::app
